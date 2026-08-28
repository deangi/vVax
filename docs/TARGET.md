# vVax product target

## Machine

**MicroVAX II–class** on Freenove ESP32-S3 2.8" Display.

| Feature | Target |
|---------|--------|
| Guest RAM | **`ram_mb` = 2, 4, 6, or 8** in `/vaxconfig.ini` (default 8; auto step-down on OOM) |
| Console | **VT100** TFT + Telnet + USB (host_lib from vpdp/v8088) |
| Host UX | Status line, touch settings, rich Telnet shell — **Phase 7**, mirror vpdp1170 |
| Storage | **≥2 MSCP drives** (`[disks] a=` / `b=`) |
| Clock | Interval timer + TOY (`vax_clock`) |
| Network | Secondary DELQA-class + **NAT** (`eth_nat`, vpdp1170 keys) — **Phase 9** |
| Throughput | Interpreter **Phase 8**: today **50–100 KIPS**; first target **≥300 KIPS** |

## PSRAM risk

Typical Freenove board has ~8 MB PSRAM. Allocate the guest arena **before**
WiFi/lwIP. With `ram_mb=8`, try full 8 MiB then back off **64 KiB at a time**
until alloc succeeds (floor 6 MiB), then 6/4/2. Stock `/boot` @ `0x7D0000`
needs roughly ≥ `0x7E5000` guest RAM.

Allowed values: **2, 4, 6, 8**.

## Config files (SD root)

| File | Role |
|------|------|
| `/wificonfig.ini` | WiFi, NTP, Telnet, FTP |
| `/vaxconfig.ini` | title, ram_mb, console boot_text, disks a/b, clock, ethernet, diag MSCP dump |

Template tree: [`vVaxSdCard/`](../vVaxSdCard/).

## Success for this scaffold

- Sketch builds and shows VT100 console path.
- `ram_mb` selects 2 / 4 / 6 / 8 MB with OOM step-down.
- Two MSCP mount slots.
- Clock stub present.
- Phase 2 CPU self-test prints `vVax OK` and sets R0=`OK`.
- Phase 3 MMU PTE walk self-test.
- Phase 4 console/clock IPR self-tests (`console OK`, `clock selftest: PASS`).
- Ethernet NAT module present; device CSR wired in **Phase 9**.
- Phase 7: status bar, touch GUI, full Telnet management shell (vpdp1170 parity). **V0.7.24** on `vax-11750`.
- Phase 8: interpreter KIPS (baseline 50–100; first target ≥300).
