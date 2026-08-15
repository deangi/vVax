# Open-source VAX ISA / emulator study

## Primary reference: Open SIMH (MIT)

Study (do not wholesale-vend) Open SIMH `VAX/` for:

- Instruction decode / execute (MicroVAX subset)
- PSL / modes / exceptions
- MMU / translation buffer
- MSCP / RQDX patterns
- Console and interval clock devices
- DELQA/DEQNA Ethernet CSR models

Porting goal: a trimmed interpreter suitable for ESP32-S3 dual-core + PSRAM, not a desktop SCP.

## Avoid

- Charon / commercial VAX emulators (proprietary).
- Poisoned / unclear-license forks of classic `simh` trees; prefer the **Open SIMH** MIT lineage.

## ESP32 constraints

- Guest RAM in SPIRAM (try 6 MB, fallback 4 MB).
- Host FIFOs, WiFi, Telnet, and TFT must stay lean.
- Device I/O to SD must use `HostSdGuard` / `SD_FTP_StorageGuard`.
- Ethernet secondary reuses vpdp1170 `eth_nat` NAPT onto WiFi STA.
