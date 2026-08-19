"""Look up NetBSD/vax kernel symbols for a list of PCs."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

from ufs1 import Ufs1

pcs = [int(x, 16) for x in sys.argv[1:]] or [
    0x801822D7,
    0x801C75B5,
    0x801C0CB8,
    0x8019C9D4,
    0x80190542,
    0x801904A8,
    0x801AC2B9,
    0x801C0CAE,
    0x801AC253,
    0x801C0BCE,
    0x801C75A8,
    0x801822D4,
]


def parse_elf_syms(b: bytes):
    if b[:4] != b"\x7fELF":
        raise SystemExit("not ELF")
    ei_class, ei_data = b[4], b[5]
    assert ei_class == 1 and ei_data == 1  # 32-bit LE
    e_shoff = struct.unpack_from("<I", b, 32)[0]
    e_shentsize = struct.unpack_from("<H", b, 46)[0]
    e_shnum = struct.unpack_from("<H", b, 48)[0]
    e_shstrndx = struct.unpack_from("<H", b, 50)[0]
    sh = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, soff, size, link, info, addralign, entsize = struct.unpack_from(
            "<IIIIIIIIII", b, off
        )
        sh.append(
            {
                "name": name,
                "type": typ,
                "addr": addr,
                "off": soff,
                "size": size,
                "link": link,
                "entsize": entsize,
            }
        )
    strtab_sh = sh[e_shstrndx]
    shstr = b[strtab_sh["off"] : strtab_sh["off"] + strtab_sh["size"]]

    def shname(idx):
        n = sh[idx]["name"]
        return shstr[n : shstr.find(b"\0", n)].decode("ascii", "replace")

    syms = []
    for i, s in enumerate(sh):
        if s["type"] != 2:  # SHT_SYMTAB
            continue
        strs = sh[s["link"]]
        stab = b[strs["off"] : strs["off"] + strs["size"]]
        ent = s["entsize"] or 16
        for j in range(0, s["size"], ent):
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(
                "<IIIBBH", b, s["off"] + j
            )
            if st_name == 0:
                continue
            nm = stab[st_name : stab.find(b"\0", st_name)].decode("ascii", "replace")
            if not nm or nm.startswith("$"):
                continue
            bind = st_info >> 4
            typ = st_info & 0xF
            if typ not in (0, 1, 2):  # NOTYPE, OBJECT, FUNC
                continue
            if st_value == 0:
                continue
            syms.append((st_value, st_size, nm, bind, typ))
    # Also try SHT_DYNSYM (11)
    if not syms:
        for s in sh:
            if s["type"] != 11:
                continue
            strs = sh[s["link"]]
            stab = b[strs["off"] : strs["off"] + strs["size"]]
            ent = s["entsize"] or 16
            for j in range(0, s["size"], ent):
                st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(
                    "<IIIBBH", b, s["off"] + j
                )
                if st_name == 0:
                    continue
                nm = stab[st_name : stab.find(b"\0", st_name)].decode("ascii", "replace")
                if nm:
                    syms.append((st_value, st_size, nm, 0, 0))
    print("sections:", [shname(i) for i in range(min(len(sh), 30))])
    print("nsyms", len(syms))
    syms.sort()
    return syms


def lookup(syms, pc):
    best = None
    for v, sz, nm, bind, typ in syms:
        if v <= pc:
            best = (v, sz, nm)
        else:
            break
    return best


def main():
    disk = Path(r"..\..\vVaxSdCard\disks\netbsd101-boot.dsk")
    with Ufs1(disk) as fs:
        names = ["/netbsd", "/netbsd.vax", "/netbsd.gdb"]
        data = None
        used = None
        for n in names:
            try:
                ino = fs.lookup(n)
                data = fs.read_file(ino)
                used = n
                break
            except Exception as e:
                print(n, e)
        if data is None:
            raise SystemExit("no kernel")
        print("file", used, "len", hex(len(data)))
    syms = parse_elf_syms(data)
    for pc in pcs:
        b = lookup(syms, pc)
        if not b:
            print(f"{pc:08X}: <no symbol>")
            continue
        v, sz, nm = b
        extra = f" size={sz:#x}" if sz else ""
        print(f"{pc:08X}: {nm}+{pc - v:#x} (sym={v:08X}{extra})")


if __name__ == "__main__":
    main()
