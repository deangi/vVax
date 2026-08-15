# vVax

MicroVAX II–class emulator sketch for the **Freenove ESP32-S3 2.8" Display**
board (same family as vpdp1170 / v8088 / vZ80).

**Status:** V0.3 — Phase 2 CPU self-test + Phase 3 MMU PTE walk. NetBSD install
ISO can live on MSCP B; installed `dua0.dsk` still built on desktop Open SIMH.

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
| Guest RAM | **`ram_mb`** = 2, 4, or 6 (default 6; host steps down on OOM) |
| Console | **VT100** on TFT + Telnet + USB |
| Disks | **Two MSCP** units (`a` / `b` → dua / dub) |
| Clock | Interval timer + TOY |
| Network | Secondary DELQA-class + **NAT** (vpdp1170 `eth_nat`) |

See [`docs/TARGET.md`](docs/TARGET.md) and [`docs/PHASES.md`](docs/PHASES.md).

## SD card

Copy [`vVaxSdCard/`](vVaxSdCard/) to a FAT32 card:

- `/wificonfig.ini` — WiFi, NTP, Telnet, FTP
- `/vaxconfig.ini` — RAM, console `boot_text`, disks, clock, ethernet
- `/disks/*.dsk` — user-supplied MSCP images
- `/firmware/` — user-supplied ROM blobs (not redistributed here)

## Architecture (scaffold)

```text
TFT / Telnet / USB ──► vax_console ──► (future DZ CSR) ──► CPU
PSRAM 6 MB           ◄── vax_cpu / vax_mmu (stubs)
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
