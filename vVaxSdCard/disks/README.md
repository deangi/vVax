# MSCP / install media (user-supplied)

| Host path | Unit | Notes |
|-----------|------|--------|
| `/disks/dua0.dsk` | MSCP A | Installed NetBSD system (create with Open SIMH) |
| `/disks/NetBSD-10.1-vax.iso` | MSCP B | Official NetBSD 10.1/vax install ISO (~427 MB) |
| `/disks/dub0.dsk` | optional | Second data pack |

## Current template (`vaxconfig.ini`)

- `a=` empty until you have an installed `dua0.dsk`
- `b=/disks/NetBSD-10.1-vax.iso` — already copied into this folder from Downloads

## Build `dua0.dsk` on a PC

1. Open SIMH / `simh-vax` (or microvax2): blank `ra92` on `rq0`, ISO on `rq1` as `cdrom`
2. Follow https://www.netbsd.org/ports/vax/emulator-howto.html
3. Copy the installed HDD image here as `dua0.dsk`
4. Switch to `vaxconfig-netbsd.ini` (or set `a=/disks/dua0.dsk`)

ISO and `.dsk` files are gitignored (large). Block size: **512 bytes**.
