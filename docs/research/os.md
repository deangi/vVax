# VAX operating systems (guest targets)

## Preferred first guest: NetBSD/vax

- Open source; redistributable installation media.
- Documented to boot with roughly **6 MB** RAM on MicroVAX-class machines — matches the vVax RAM try.
- Uses MSCP (`ra`) disks and serial console.

## Other guests (later / legal caveats)

| OS | Notes |
|----|--------|
| Ultrix-32 | Historic DEC UNIX; media licensing varies |
| OpenVMS / VMS | Hobbyist kits have ended; **do not ship VMS kits in this repo** |
| VAXBSD / historic BSD | Niche; NetBSD is the practical path |

## Media policy

- Repo ships **config templates only** under `vVaxSdCard/`.
- Disk images and firmware/ROM blobs are user-supplied (`vVaxSdCard/disks/`, `vVaxSdCard/firmware/`).
- No DEC/HPE/VSI affiliation.
