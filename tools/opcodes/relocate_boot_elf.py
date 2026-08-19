#!/usr/bin/env python3
"""Relocate NetBSD/vax ELF /boot on a UFS disk so FROM750 xxboot can load it.

Stock /boot links at 0x7d0000 with e_entry=0. FROM750 hoppabort REIs to
entry+2 → PC=2 → HALT unless e_entry (and absolutes) are fixed.

Freenove guest RAM often allocates as 8192000 (0x7D0000) after 64 KiB backoff —
exactly the stock base, leaving no room for the image. Default --new-base is
therefore 0x7A0000 (192 KB below that end). Full 8 MiB → 0x7D0000; 6 MiB →
0x5D0000. Never use 0x200000 (kernel load overwrites /boot).

Blind word scans over .text are unsafe: PC-relative EF displacements can look
like absolute addresses in the old window and get corrupted (classic symptom:
partial banner then fault at a PC past image end, e.g. 0x2155C3).

Safe rewrite:
  - ELF e_entry / PT_LOAD p_vaddr/p_paddr / SHDR sh_addr
  - every LE word in .rodata / .data in [old, old+memsz]
  - in .text only: 8F immediate, 9F absolute, or E0–EE longword(reg) disp
    (not EF = PC-relative)
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ufs1 import Ufs1  # noqa: E402

OLD_DEFAULT = 0x7D0000
# Freenove typically gets ~8192000 (0x7D0000) guest bytes after 64 KiB backoff.
# Link 192 KB below that end — same margin as stock 0x7D0000 on full 8 MiB.
NEW_BASE_DEFAULT = 0x7A0000


def parse_elf(data: bytes) -> tuple[int, int, int, int, int, int]:
    if data[:4] != b"\x7fELF":
        raise SystemExit("not ELF")
    e_entry = struct.unpack_from("<I", data, 24)[0]
    e_phoff = struct.unpack_from("<I", data, 28)[0]
    e_phnum = struct.unpack_from("<H", data, 44)[0]
    if e_phnum < 1:
        raise SystemExit("no PHDRs")
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from(
        "<IIIIIIII", data, e_phoff
    )
    if p_type != 1:
        raise SystemExit(f"PH0 type {p_type} not PT_LOAD")
    return e_entry, e_phoff, p_offset, p_vaddr, p_filesz, p_memsz


def iter_sections(data: bytes) -> list[tuple[str, int, int, int, int, int]]:
    e_shoff = struct.unpack_from("<I", data, 32)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 46)
    if e_shoff == 0 or e_shnum == 0:
        return []
    shstr_off = e_shoff + e_shstrndx * e_shentsize
    _n, _t, _f, _a, stroff, strsz, *_ = struct.unpack_from("<IIIIIIIIII", data, shstr_off)
    shstr = data[stroff : stroff + strsz]
    out: list[tuple[str, int, int, int, int, int]] = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        name_off, typ, flags, addr, offset, size, *_rest = struct.unpack_from(
            "<IIIIIIIIII", data, o
        )
        end = shstr.find(b"\x00", name_off)
        name = shstr[name_off:end].decode("ascii", "replace")
        out.append((name, typ, flags, addr, offset, size))
    return out


def patch_word(data: bytearray, off: int, delta: int) -> None:
    val = struct.unpack_from("<I", data, off)[0]
    struct.pack_into("<I", data, off, (val + delta) & 0xFFFFFFFF)


def patch_abs_in_span(
    data: bytearray, start: int, size: int, lo: int, hi: int, delta: int
) -> int:
    """Blind LE words in [start, start+size), non-overlapping."""
    if size < 4:
        return 0
    n = 0
    last = start - 4
    end = start + size
    for off in range(start, end - 3):
        if off < last + 4:
            continue
        val = struct.unpack_from("<I", data, off)[0]
        if lo <= val < hi:
            patch_word(data, off, delta)
            last = off
            n += 1
    return n


def patch_text_safe(
    data: bytearray, start: int, size: int, lo: int, hi: int, delta: int
) -> tuple[int, int, int]:
    """Patch .text immediates/absolutes/reg+lw-disp; never PC-relative EF."""
    imm = abs9 = disp = 0
    end = start + size
    off = start
    while off < end - 4:
        b = data[off]
        if b in (0x8F, 0x9F) or 0xE0 <= b <= 0xEE:
            val = struct.unpack_from("<I", data, off + 1)[0]
            if lo <= val < hi:
                patch_word(data, off + 1, delta)
                if b == 0x8F:
                    imm += 1
                elif b == 0x9F:
                    abs9 += 1
                else:
                    disp += 1
                off += 5
                continue
        off += 1
    return imm, abs9, disp


def patch_elf_headers(data: bytearray, new_base: int, old_base: int, memsz: int) -> None:
    delta = new_base - old_base
    e_phoff = struct.unpack_from("<I", data, 28)[0]
    struct.pack_into("<I", data, 24, new_base)  # e_entry
    struct.pack_into("<I", data, e_phoff + 8, new_base)  # p_vaddr
    struct.pack_into("<I", data, e_phoff + 12, new_base)  # p_paddr

    e_shoff = struct.unpack_from("<I", data, 32)[0]
    e_shentsize, e_shnum, _ = struct.unpack_from("<HHH", data, 46)
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        addr = struct.unpack_from("<I", data, o + 12)[0]
        if old_base <= addr < old_base + memsz + 1:
            struct.pack_into("<I", data, o + 12, (addr + delta) & 0xFFFFFFFF)


def count_abs_range(data: bytes, lo: int, hi: int) -> int:
    n = 0
    last = -4
    for off in range(16, len(data) - 3):
        if off < last + 4:
            continue
        val = struct.unpack_from("<I", data, off)[0]
        if lo <= val < hi:
            n += 1
            last = off
    return n


def relocate(data: bytearray, new_base: int, old_base: int) -> None:
    e_entry, e_phoff, p_offset, p_vaddr, p_filesz, p_memsz = parse_elf(bytes(data))
    span = max(p_memsz, p_filesz)
    delta = new_base - old_base
    print(
        f"hdr e_entry={e_entry:#x} p_vaddr={p_vaddr:#x} filesz={p_filesz} memsz={p_memsz}"
    )
    print(f"old_base={old_base:#x} new_base={new_base:#x} delta={delta:#x} span={span:#x}")

    if p_vaddr not in (old_base, new_base) and e_entry not in (0, old_base, new_base):
        raise SystemExit(
            f"unexpected link address e_entry={e_entry:#x} p_vaddr={p_vaddr:#x}; "
            "restore pristine /boot (e.g. from install ISO BOOT.;1) first"
        )

    # Already relocated to the requested base — treat as install-only.
    if p_vaddr == new_base and e_entry == new_base:
        print("image already at new_base; no further patching")
        return

    lo, hi = old_base, old_base + span + 1  # inclusive end (bss/end sentinels)
    sections = {s[0]: s for s in iter_sections(bytes(data))}

    patch_elf_headers(data, new_base, old_base, span)

    for name in (".rodata", ".data", ".eh_frame"):
        if name not in sections:
            continue
        _n, _t, _f, _addr, offset, size = sections[name]
        n = patch_abs_in_span(data, offset, size, lo, hi, delta)
        print(f"patched {n} words in {name} [{offset:#x}+{size:#x})")

    if ".text" in sections:
        _n, _t, _f, _addr, offset, size = sections[".text"]
        imm, abs9, disp = patch_text_safe(data, offset, size, lo, hi, delta)
        print(
            f"patched .text: {imm} x 8F-imm, {abs9} x 9F-abs, {disp} x E0-EE-disp "
            f"[{offset:#x}+{size:#x})"
        )
    else:
        # No SHDRs — fall back to PT_LOAD file span with text-safe rules only.
        imm, abs9, disp = patch_text_safe(data, p_offset, p_filesz, lo, hi, delta)
        print(f"patched PT_LOAD (no SHDR): {imm} imm, {abs9} abs, {disp} disp")

    leftover = count_abs_range(bytes(data), old_base, old_base + 0x20000)
    print(f"remaining words still in [{old_base:#x},{old_base + 0x20000:#x}): {leftover}")


def write_inode_file(fs: Ufs1, path: str, new_data: bytes) -> None:
    ino = fs.lookup(path)
    if ino is None:
        raise SystemExit(f"missing {path}")
    if len(new_data) != ino.size:
        raise SystemExit(f"size change not supported ({len(new_data)} vs {ino.size})")
    remaining = len(new_data)
    pos = 0

    def put(fsblk: int, chunk: bytes) -> None:
        if fsblk == 0:
            raise SystemExit("unexpected hole in /boot")
        off = fs.blk_off(fsblk)
        fs.f.seek(off)
        fs.f.write(chunk)

    for b in ino.db:
        if remaining <= 0:
            break
        n = min(fs.bsize, remaining)
        put(b, new_data[pos : pos + n])
        pos += n
        remaining -= n
    if remaining > 0 and ino.ib[0]:
        ptrs = struct.unpack(
            "<" + "I" * (fs.bsize // 4), fs._read_fsblk(ino.ib[0])
        )
        for p in ptrs:
            if remaining <= 0:
                break
            n = min(fs.bsize, remaining)
            put(p, new_data[pos : pos + n])
            pos += n
            remaining -= n
    if remaining > 0 and ino.ib[1]:
        ptrs2 = struct.unpack(
            "<" + "I" * (fs.bsize // 4), fs._read_fsblk(ino.ib[1])
        )
        for p2 in ptrs2:
            if remaining <= 0:
                break
            if p2 == 0:
                continue
            ptrs = struct.unpack(
                "<" + "I" * (fs.bsize // 4), fs._read_fsblk(p2)
            )
            for p in ptrs:
                if remaining <= 0:
                    break
                n = min(fs.bsize, remaining)
                put(p, new_data[pos : pos + n])
                pos += n
                remaining -= n
    if remaining != 0:
        raise SystemExit(f"incomplete write, left {remaining}")
    fs.f.flush()


def extract_boot_from_iso(iso_path: Path) -> bytes:
    """Pull ISO9660 BOOT.;1 (NetBSD/vax install CD standalone /boot)."""
    iso = iso_path.read_bytes()
    idx = iso.find(b"BOOT.;1")
    if idx < 0:
        raise SystemExit(f"BOOT.;1 not found in {iso_path}")
    for s in range(max(0, idx - 40), idx):
        reclen = iso[s]
        if reclen < 34:
            continue
        name_len = iso[s + 32]
        name = iso[s + 33 : s + 33 + name_len]
        if not name.startswith(b"BOOT"):
            continue
        extent = struct.unpack_from("<I", iso, s + 2)[0]
        size = struct.unpack_from("<I", iso, s + 10)[0]
        blob = iso[extent * 2048 : extent * 2048 + size]
        if blob[:4] != b"\x7fELF":
            raise SystemExit("BOOT.;1 extent is not ELF")
        return blob
    raise SystemExit("could not parse BOOT.;1 directory record")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "disk",
        type=Path,
        nargs="?",
        default=Path(__file__).resolve().parents[2]
        / "vVaxSdCard"
        / "disks"
        / "netbsd101-boot.dsk",
    )
    ap.add_argument("--new-base", type=lambda s: int(s, 0), default=NEW_BASE_DEFAULT)
    ap.add_argument("--old-base", type=lambda s: int(s, 0), default=OLD_DEFAULT)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--path", default="/boot")
    ap.add_argument(
        "--elf",
        type=Path,
        help="pristine ELF to install+relocate (preferred over disk contents)",
    )
    ap.add_argument(
        "--iso",
        type=Path,
        help="NetBSD/vax ISO; extract BOOT.;1 as pristine /boot",
    )
    args = ap.parse_args()

    if args.elf and args.iso:
        raise SystemExit("use only one of --elf / --iso")

    with Ufs1(args.disk) as fs:
        fs.f.close()
        fs.f = args.disk.open("r+b")
        ino = fs.lookup(args.path)
        if ino is None:
            raise SystemExit(f"{args.path} not found")
        if args.iso:
            data = bytearray(extract_boot_from_iso(args.iso))
            print(f"loaded pristine from ISO {args.iso} ({len(data)} bytes)")
        elif args.elf:
            data = bytearray(args.elf.read_bytes())
            print(f"loaded pristine from {args.elf} ({len(data)} bytes)")
        else:
            data = bytearray(fs.read_file(ino))
        if len(data) != ino.size:
            raise SystemExit(f"size mismatch ({len(data)} vs inode {ino.size})")
        relocate(data, args.new_base, args.old_base)
        e_entry, _, p_offset, p_vaddr, _, p_memsz = parse_elf(bytes(data))
        print(f"result e_entry={e_entry:#x} p_vaddr={p_vaddr:#x}")
        past = count_abs_range(bytes(data), args.new_base + p_memsz, args.new_base + 0x20000)
        print(f"words past image end in file: {past}")
        # Sanity: BSS end sentinel must move
        end_sym = args.old_base + p_memsz
        new_end = args.new_base + p_memsz
        if any(
            struct.unpack_from("<I", data, o)[0] == end_sym
            for o in range(len(data) - 3)
        ):
            print(f"WARNING: still contains old end sentinel {end_sym:#x}")
        else:
            print(f"end sentinel {end_sym:#x} -> {new_end:#x} OK")
        if args.dry_run:
            print("dry-run: not writing")
            return 0
        write_inode_file(fs, args.path, bytes(data))
        print(f"wrote relocated {args.path} on {args.disk}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
