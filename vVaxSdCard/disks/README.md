# Disk images (MSCP units)

Place large images here; they are **gitignored**. Example layout:

| File | Role |
|------|------|
| `netbsd101-boot.dsk` | Installed NetBSD/vax system (MSCP A) |
| `CD-NetBSD-10.1-vax.iso` | Official install CD (MSCP B) |
| `boot-pristine.elf` | Stock `/boot` extracted from the ISO (`BOOT.;1`) |

ISO and `.dsk`/`.img` files are gitignored (large). Block size: **512 bytes**.

## NetBSD `/boot` ELF note (FROM750 / xxboot)

Stock NetBSD 10.1/vax `/boot` is ELF with `e_entry=0` and `p_vaddr=0x7d0000`
(8 MB − 192 KB). xxboot loads at `e_entry` and `hoppabort` REIs to `entry+2`,
which becomes **PC=2 → HALT** unless `e_entry` is fixed.

On Freenove, guest RAM often lands at **8192000 bytes (`0x7D0000`)** after the
64 KiB backoff — exactly the stock link address, so `/boot` has **no room**.
Default relocate base is therefore **`0x7A0000`** (192 KB below that end):

```text
python tools/opcodes/relocate_boot_elf.py --elf vVaxSdCard/disks/boot-pristine.elf ^
  vVaxSdCard/disks/netbsd101-boot.dsk
```

| Guest RAM | `--new-base` |
|-----------|----------------|
| ≈8 MiB−192 KB (`0x7D0000` bytes) | **`0x7A0000`** (default) |
| Full 8 MiB | `0x7D0000` |
| 6 MiB | `0x5D0000` |

Do **not** use `0x200000`: a ~3.5 MB kernel at phys 0 overwrites `/boot` mid-load.

Always start from a **pristine** ELF. Do not re-patch an already relocated image.
