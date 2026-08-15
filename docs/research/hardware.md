# VAX hardware research (MicroVAX II target)

## Why MicroVAX II

- Contained Q22-bus story (not full BI / XMI / CI clusters).
- Subset of the VAX ISA (compatible with Open SIMH MicroVAX models).
- Typical hobby/OS target: NetBSD/vax, Ultrix-32, VMS (hobbyist kits historically).
- Fits ESP32-S3 PSRAM budgets better than VAX 11/780 or VAXstation graphics.

## Chassis notes

| Item | MicroVAX II (KA630-class) |
|------|---------------------------|
| CPU | MicroVAX 78032 + FPU companion |
| Bus | Q22 |
| RAM | Up to 16 MB historically; **vVax tries 6 MB first** |
| Console | Serial console (DL/DZ-class) → host VT100 |
| Disk | RQDX3 / MSCP (RA-compatible images) |
| Clock | Interval timer + TOY |
| Net | DELQA / DEQNA Ethernet (secondary on vVax) |

## Out of scope for v1

BI/XMI/CI, Massbus, VAXstation framebuffers, full 16 MB unless PSRAM allows later.

## References

- Open SIMH `VAX/` MicroVAX / KA630 model sources (MIT license study).
- DEC MicroVAX II technical documentation (hardware manuals; user-supplied).
