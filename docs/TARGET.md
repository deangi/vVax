# vVax product target

## Machine

**MicroVAX II–class** on Freenove ESP32-S3 2.8" Display.

| Feature | Target |
|---------|--------|
| Guest RAM | **`ram_mb` = 2, 4, or 6** in `/vaxconfig.ini` (default 6; auto step-down on OOM) |
| Console | **VT100** TFT + Telnet + USB (host_lib from vpdp/v8088) |
| Storage | **≥2 MSCP drives** (`[disks] a=` / `b=`) |
| Clock | Interval timer + TOY (`vax_clock`) |
| Network | Secondary DELQA-class + **NAT** (`eth_nat`, vpdp1170 keys) |

## PSRAM risk

Typical Freenove board has ~8 MB PSRAM. A 6 MB guest arena leaves little for:

- Telnet / console FIFOs (prefer `EXT_RAM_BSS_ATTR` but still competes)
- WiFi / lwIP / FTP
- TFT frame scratch

If boot logs show RAM alloc failure, set `ram_mb = 4` or `2` in `/vaxconfig.ini`.
Allowed values are only **2, 4, and 6**.

## Config files (SD root)

| File | Role |
|------|------|
| `/wificonfig.ini` | WiFi, NTP, Telnet, FTP |
| `/vaxconfig.ini` | title, ram_mb, console boot_text, disks a/b, clock, ethernet |

Template tree: [`vVaxSdCard/`](../vVaxSdCard/).

## Success for this scaffold

- Sketch builds and shows VT100 console path.
- `ram_mb` selects 2 / 4 / 6 MB with OOM step-down.
- Two MSCP mount slots.
- Clock stub present.
- Phase 2 CPU self-test prints `vVax OK` and sets R0=`OK`.
- Ethernet NAT module present; device CSR wired in a later phase.
