"""Disassemble netbsd kernel PCs by seeking the disk (no full read)."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

from simh_tables import load_simh, insn_length

DISK = Path(r"..\..\vVaxSdCard\disks\netbsd101-boot.dsk")
SIMH = Path(
    r"C:\Users\deang\OneDrive\emulators\open-simh-simh-open-simh-baseline-448-ga1f57fa"
    r"\open-simh-simh-a1f57fa\VAX"
)


def find_kernel_img(f) -> tuple[bytes, int]:
    """Return (first PT_LOAD bytes, vaddr) for the kernel ELF."""
    # Scan 64KiB windows starting at 2MB; UFS file data is not at byte 0.
    needle = b"\x7fELF\x01\x01"
    pos = 0x200000
    f.seek(0, 2)
    end = f.tell()
    while pos + 64 < end and pos < 0x800000:
        f.seek(pos)
        chunk = f.read(0x10000)
        i = chunk.find(needle)
        if i >= 0:
            elf_off = pos + i
            f.seek(elf_off)
            hdr = f.read(0x80)
            if hdr[18:20] != b"\x4b\x00":  # EM_VAX = 75
                pos = elf_off + 4
                continue
            phoff = struct.unpack_from("<I", hdr, 28)[0]
            phentsize, phnum = struct.unpack_from("<HH", hdr, 42)
            f.seek(elf_off + phoff)
            ph = f.read(phentsize * phnum)
            for n in range(phnum):
                p = n * phentsize
                p_type, p_off, p_vaddr, _p_paddr, p_filesz, p_memsz = struct.unpack_from(
                    "<IIIIII", ph, p
                )
                if p_type == 1 and p_vaddr >= 0x80000000:
                    f.seek(elf_off + p_off)
                    return f.read(min(p_filesz, 8 * 1024 * 1024)), p_vaddr
            pos = elf_off + 4
            continue
        pos += 0xF000
    raise SystemExit("kernel ELF not found")


def disasm(ops, img, base, pc, before=24, after=40):
    start = pc - base
    p = max(0, start - before)
    print(f"--- {pc:08X} ---")
    while p < start + after and p < len(img):
        r = insn_length(ops, img, p)
        if not r:
            print(f"{base + p:08X}: ??? {img[p:p+8].hex()}")
            break
        inst, ln = r
        mark = " <<" if base + p == pc else ""
        print(f"{base + p:08X}: {ops[inst].name or 'OP_%02X'%inst:10} {img[p:p+ln].hex()}{mark}")
        p += ln


def main():
    pcs = [int(x, 16) for x in sys.argv[1:]] or [
        0x801822D4,
        0x801C75A8,
        0x801C0CAE,
        0x8019C9C5,
        0x8019049B,
        0x801AC253,
        0x802923CD,
        0x801AF51C,
        0x8029316A,
    ]
    ops = load_simh(SIMH)
    with DISK.open("rb") as f:
        img, base = find_kernel_img(f)
    print(f"image base={base:08X} filesz={len(img):#x}")
    for pc in pcs:
        disasm(ops, img, base, pc)


if __name__ == "__main__":
    main()
