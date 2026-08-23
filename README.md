# vVax

MicroVAX II–class emulator sketch for the **Freenove ESP32-S3 2.8" Display**
board (same family as vpdp1170 / v8088 / vZ80).

**Status:** V0.6.1 — Phase 6 NetBSD xxboot/`/boot` (FROM750, no proprietary ROM).
Next: finish kernel handoff; **Phase 7** host UX; **Phase 8** interpreter KIPS
(today ~50–100). See [`docs/PHASES.md`](docs/PHASES.md).

## Board / build

| Setting | Value |
|---------|--------|
| Board | ESP32S3 Dev Module |
| USB CDC on boot | Enabled |
| PSRAM | OPI |
| Flash | 16 MB |
| Partition | Huge App (3 MB APP) — see `sketch.yaml` |
| TFT | TFT_eSPI with **FNK0104B** selected |

## Product target

| Item | Choice |
|------|--------|
| Emulated system | MicroVAX II–class (KA630-ish, Q22 story) |
| Guest RAM | **`ram_mb`** = 2, 4, 6, or 8 (default 8; host steps down on OOM) |
| Console | **VT100** on TFT + Telnet + USB |
| Disks | **Two MSCP** units (`a` / `b` → dua / dub) |
| Clock | Interval timer + TOY |
| Network | Secondary DELQA-class + **NAT** (vpdp1170 `eth_nat`) |

See [`docs/TARGET.md`](docs/TARGET.md) and [`docs/PHASES.md`](docs/PHASES.md).

## SD card

Copy [`vVaxSdCard/`](vVaxSdCard/) to a FAT32 card:

- `/wificonfig.ini` — WiFi, NTP, Telnet, FTP
- `/vaxconfig.ini` — RAM, console `boot_text`, disks, clock, ethernet, `[diag]`
- `/disks/*.dsk` — user-supplied MSCP images
- `/firmware/` — user-supplied ROM blobs (not redistributed here)

## Architecture (scaffold)

```text
TFT / Telnet / USB ──► vax_console ──► (future DZ CSR) ──► CPU
PSRAM 8 MB try        ◄── vax_cpu / vax_mmu (step-down 6/4/2 on OOM)
SD MSCP a,b          ◄── vax_mscp
host millis / NTP    ──► vax_clock
WiFi STA             ◄── eth_nat (secondary DELQA hook)
```

## host_lib

Freenove-trimmed snapshot under [`host_lib/`](host_lib/) with **VT100** default.
Wiring notes: [`host_lib/INTEGRATION.md`](host_lib/INTEGRATION.md).

## Legal

- No VMS kits or DEC firmware in this repository.
- Prefer **NetBSD/vax** open media for guest OS experiments.
- Not affiliated with DEC, HPE, or VSI.
- Open SIMH is a study reference (MIT); this tree is a separate ESP32 port.
