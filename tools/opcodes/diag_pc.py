"""Disassemble netbsd at a VA."""
import struct
import sys
from pathlib import Path
from simh_tables import load_simh, insn_length

def main():
    pc = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x80000824
    simh = Path(
        r"C:\Users\deang\OneDrive\emulators\open-simh-simh-open-simh-baseline-448-ga1f57fa"
        r"\open-simh-simh-a1f57fa\VAX"
    )
    ops = load_simh(simh)
    disk = Path(r"..\..\vVaxSdCard\disks\netbsd101-boot.dsk")
    data = disk.read_bytes()
    j = data.find(b"\x7fELF", 0x2D0000)
    phoff = struct.unpack_from("<I", data, j + 28)[0]
    p = j + phoff
    _ptype, poff, pva, _ppa, pfilesz = struct.unpack_from("<IIIII", data, p)
    img = data[j + poff : j + poff + pfilesz]
    base = 0x80000000
    start = pc - base
    p = max(0, start - 16)
    while p < start + 32 and p < len(img):
        r = insn_length(ops, img, p)
        if not r:
            print(f"{base + p:08X}: ???")
            break
        inst, ln = r
        mark = " <<" if base + p == pc else ""
        print(f"{base + p:08X}: {ops[inst].name:10} {img[p : p + ln].hex()}{mark}")
        p += ln

if __name__ == "__main__":
    main()
