from pathlib import Path
from ufs1 import Ufs1
import struct
from simh_tables import load_simh, insn_length

ops = load_simh(
    Path(
        r"C:\Users\deang\OneDrive\emulators\open-simh-simh-open-simh-baseline-448-ga1f57fa"
        r"\open-simh-simh-a1f57fa\VAX"
    )
)
disk = Path(r"..\..\vVaxSdCard\disks\netbsd101-boot.dsk")
with Ufs1(disk) as fs:
    b = fs.read_file(fs.lookup("/boot"))
ph = struct.unpack_from("<IIIIIIII", b, 52)
base, off0, filesz, memsz = ph[2], ph[1], ph[4], ph[5]
fault = 0x20CDCF
rel = fault - base
img = b[off0 : off0 + filesz]
print("base", hex(base), "rel", hex(rel), "filesz", hex(filesz), "in_file", rel < filesz)

best = None
for s in range(max(0, rel - 80), rel):
    p = s
    path = []
    while p < rel + 20 and p < len(img):
        r = insn_length(ops, img, p)
        if r is None:
            break
        inst, ln = r
        path.append((p, inst, ln))
        p += ln
        if p == rel:
            best = (s, path)
            break
        if p > rel:
            break
    if best:
        break

print("sync from", hex(base + best[0]) if best else None)
if best:
    for p, inst, ln in best[1][-15:]:
        mark = " <<" if p == rel else ""
        print(f"{base+p:08X}: {ops[inst].name:8} {img[p:p+ln].hex()}{mark}")
elif rel < len(img):
    print("raw", img[rel : rel + 16].hex())

for lo, hi, name in [
    (0x7D0000, 0x7E4F18, "oldimg"),
    (0x7E4F18, 0x800000, "oldhi"),
    (0x600000, 0x7D0000, "mid"),
    (0x214F18, 0x300000, "pastnew"),
]:
    n = 0
    ex = []
    last = -4
    for off in range(16, len(b) - 3):
        if off < last + 4:
            continue
        v = struct.unpack_from("<I", b, off)[0]
        if lo <= v < hi:
            n += 1
            last = off
            if len(ex) < 10:
                ex.append((hex(off), hex(v)))
    print(name, n, ex)
