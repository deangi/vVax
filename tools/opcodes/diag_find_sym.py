"""Print NetBSD/vax kernel symbols matching name substrings."""
from __future__ import annotations

import sys
from pathlib import Path

from ufs1 import Ufs1


def main():
    need = sys.argv[1:] or [
        "rronline_cb",
        "rronline",
        "readdisklabel",
        "udaintr",
        "mscp_worker",
        "workqueue_enqueue",
    ]
    disk = Path(r"..\..\vVaxSdCard\disks\netbsd101-boot.dsk")
    with Ufs1(disk) as fs:
        data = fs.read_file(fs.lookup("/netbsd"))
    old = sys.argv
    sys.argv = [old[0]]
    from diag_syms import parse_elf_syms
    sys.argv = old
    syms = parse_elf_syms(data)
    keys = [k.lower() for k in need]
    hits = []
    for v, sz, nm, bind, typ in syms:
        low = nm.lower()
        if any(k in low for k in keys):
            hits.append((nm, v, sz))
    hits.sort(key=lambda x: x[1])
    seen = set()
    for nm, v, sz in hits:
        if nm in seen:
            continue
        seen.add(nm)
        print(f"{v:08X} {sz:5x} {nm}")


if __name__ == "__main__":
    main()
