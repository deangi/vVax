# Implementation phases (post-scaffold)

This document is the follow-on plan after the V0.1 host + stub devices land.
Do not treat stubs as a runnable VAX ISA yet.

## Phase 1 — Host bring-up (done in scaffold)

- Freenove sketch, host_lib VT100, SD split INI, Telnet/FTP, configurable RAM
- Dual MSCP file mounts, clock stub, eth_nat module present

## Phase 2 — CPU interpreter (in progress / landed subset)

- Integer MicroVAX subset in `vax_cpu.cpp` (MOV/ADD/SUB/CMP/branch/PUSH/… + operand modes)
- Built-in `selftest()` prints `vVax OK` to VT100 via console MMIO `0x20000000`
- `/vaxconfig.ini` `[system] ram_mb=` accepts **2, 4, or 6**

## Phase 3 — MMU (landed subset)

- P0/P1/S0 region IPRs, `MAPEN`, PTE walk with V/M/PFN
- `MFPR` / `MTPR` for MMU IPRs
- `vax_mmu::selftest()` on cold boot

## Phase 4 — Console + clock devices

- Wire DZ/DL-style CSR to `vax_console` + interrupt vectors
- Interval timer + TOY CSRs via `vax_clock` and host NTP

## Phase 5 — MSCP ×2

- RQDX3-class MSCP packets on top of `vax_mscp` block I/O
- Boot from unit A; optional second pack on unit B

## Phase 6 — NetBSD try

- Boot NetBSD/vax install or prebuilt root on dua0
- Document OOM → `ram_mb=4` if 6 MB fails under load

## Phase 7 — Ethernet NAT (secondary)

- DELQA/DEQNA CSR model
- TX → `eth_nat::on_guest_tx`; RX ← `eth_nat::pop_rx`
- INI keys already match vpdp1170 (`enabled`, `mac`, `guest_ip`, …)

## Legal reminders

- No VMS hobbyist kits in-tree
- User-supplied firmware under `vVaxSdCard/firmware/`
- Prefer NetBSD open media
