# Open-source VAX ISA / emulator study

## Primary reference: Open SIMH (MIT)

Local checkout (this machine):

`C:\Users\deang\OneDrive\emulators\open-simh-simh-open-simh-baseline-448-ga1f57fa\open-simh-simh-a1f57fa\VAX`

Study (do not wholesale-vend) Open SIMH `VAX/` for:

- Instruction decode / execute — especially `vax_cpu.c`, `vax_defs.h` opcode enum
- PSL / modes / exceptions
- MMU / translation buffer
- MSCP / RQDX patterns
- Console and interval clock devices
- DELQA/DEQNA Ethernet CSR models

When adding opcodes to `vax_cpu.cpp`, match Open SIMH’s handler (condition codes included) rather than guessing from secondary opcode tables.

Coverage inventory (host scripts): `tools/opcodes/run_coverage.py` → `docs/research/opcode_coverage.md`.
Tier B = MicroVAX integer/`IG_BASE` presence gate; Tier A static set comes from xxboot + `/boot` + `/netbsd` on the SD disk image.

Porting goal: a trimmed interpreter suitable for ESP32-S3 dual-core + PSRAM, not a desktop SCP.

## Avoid

- Charon / commercial VAX emulators (proprietary).
- Poisoned / unclear-license forks of classic `simh` trees; prefer the **Open SIMH** MIT lineage.

## ESP32 constraints

- Guest RAM in SPIRAM (try 6 MB, fallback 4 MB).
- Host FIFOs, WiFi, Telnet, and TFT must stay lean.
- Device I/O to SD must use `HostSdGuard` / `SD_FTP_StorageGuard`.
- Ethernet secondary reuses vpdp1170 `eth_nat` NAPT onto WiFi STA.
