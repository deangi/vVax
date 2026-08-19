# Implementation phases (post-scaffold)

This document is the follow-on plan after the V0.1 host + stub devices land.
Do not treat stubs as a runnable VAX ISA yet.

## Phase 1 — Host bring-up (done in scaffold)

- Freenove sketch, host_lib VT100, SD split INI, Telnet/FTP, configurable RAM
- Dual MSCP file mounts, clock stub, eth_nat module present

## Phase 2 — CPU interpreter (in progress / landed subset)

- Integer MicroVAX subset in `vax_cpu.cpp` (MOV/ADD/SUB/CMP/branch/PUSH/… + operand modes)
- Built-in `selftest()` prints `vVax OK` to VT100 via console MMIO `0x20000000`
- `/vaxconfig.ini` `[system] ram_mb=` accepts **2, 4, 6, or 8** (default 8; OOM step-down)

## Phase 3 — MMU (landed subset)

- P0/P1/S0 region IPRs, `MAPEN`, PTE walk with V/M/PFN
- `MFPR` / `MTPR` for MMU IPRs
- `vax_mmu::selftest()` on cold boot

## Phase 4 — Console + clock devices (landed subset)

- KA630 IPRs: `RXCS/RXDB/TXCS/TXDB` (32–35), `ICCS` (24), `TODR` (27)
- Console → host VT100/Telnet; clock 100 Hz + host NTP TODR
- Interrupt delivery stub when `SCBB` set (vectors `0xC0` / `0xF8` / `0xFC`)
- Cold-boot self-tests for console + clock

## Phase 5 — MSCP ×2 (landed subset)

- Dual-unit UQSSP/MSCP (`vax_mscp`) on Q22 CSRs `0x20001C68` / `0x20001C6A`
- Chunked SD transfers; ring post always, IRQ deferred/one-shot at IPL 14
- CPU phys DMA ops + `poll()` in main loop; cold-boot UQSSP init self-test
- `[diag] mscp_dump_flags=` / `mscp_dump_count=` USB-serial tracing
- Boot from unit A; optional second pack / ISO on unit B

## Phase 6 — NetBSD try (in progress)

- Host FROM750-style bootstrap: load MSCP LBA 0–15, PC=`0x0C`, R6=ROM-read stub
- No proprietary KA630/VMB ROM; `[disks] boot=a|b` selects unit
- ISA grown for xxboot (`CALLS`/`RET`/`REI`/`MOVC5`/`PUSHR`/`JSB`/…)
- xxboot relocates and reaches `open("/boot.vax")`; ROM-read ABI is R8=LBN, `4(SP)`=dest
- `netbsd101-boot.dsk` **does** have FFS (UFS1 SB at byte 8192); same image SIMH boots via KA655 VMB
- Bug fixed: CALLS/RET omitted the condition-handler longword, so NetBSD `read750` zeroed the entry mask and left `block` as `ROM_READ_PA+1`
- Emulator still halts after 8 consecutive ROM-read fails (safety net)
- **Opcode presence:** Tier B (MicroVAX integer allowlist) complete — see `docs/research/opcode_coverage.md` and `tools/opcodes/`
- Remaining Tier A gaps are float / packed / heavy CIS (Tier C/D), not the integer gate
- `/boot` ELF: relocate from pristine. Freenove often gets **8192000** guest bytes (`0x7D0000`) — use **`0x7A0000`** (default), not stock `0x7D0000`. Full 8 MiB → `0x7D0000`; 6 MiB → `0x5D0000`. Never `0x200000`
- SID must be UV2 (`0x08……`); returning type 1 (780) makes `/boot` invent `devtyp` from R0 → `Can't open device type 24` / ENODEV 19
- KA630 SIE at `0x20040004` returns `0x01000000` so `vax_boardtype = VAX_BTYP_630`
- UV2 `/boot` calls `ka630_consinit` (ROM console). Host plants conspage in the **last VAX page of guest RAM** (not `0xF2000`) + NVR @ `0x200B8024` and JSB stubs → VT100/Telnet. `0xF2000` is inside the kernel image at PA 0; `cnputc` JSB through smashed vectors was the `PC=50D00014` fault during `loadfile`
- `/boot` reaches banner + autoboot; `raopen` starts real UQSSP. Init/doorbell must run on SA write / IP read (not only host `poll()`), or guest tight loops never see STEP2 / ring completion
- After GO, host `poll()` must not leave `g_poll` cleared before guest sets OWN; IP doorbell re-arms poll and posts responses immediately (NetBSD rings IP once)
- `/boot` above 4 MiB: `raopen` STEP3 only passes 22-bit Q22 (`0x003Bxxxx`); rings/DMA live at phys `0x007Bxxxx`. MSCP applies a +4 MiB alias when OWN is not at the truncated comm address
- `boot netbsd` loadfile writes the kernel at linked S0 (`0x80000000+`). With MAPEN off, VA→PA must apply 30-bit `PAMASK` (SIMH: `pa = va & 0x3FFFFFFF`) so KERNBASE aliases into guest RAM; full-VA identity caused `pa-w` at `0x80003F7F`
- MSCP `seq_buffer` is 32-bit: KERNBASE must DMA to PA 0 via PAMASK. The Q22 ring alias is only for 22-bit comm/descriptor addresses — applying it to `0x80000000` wrote the kernel at `0x400000`, then `machdep_start` `calls` `e_entry` and faulted (`pa-r` at a wild PC, `/boot` SP still live)
- `loadfile` reads into `/boot` bounce buffers (`0x007B…`); MOVC3 copies to **`0x80000000`** (PA 0). Disk `netbsd` **`e_entry=0x80000584`**
- Kernel reached `pmap_bootstrap` then `memset(Sysmap)` via MOVC5. vVax left **R3=remaining length** after MOVC5; SIMH/NetBSD keep **R3=dest+len**. Next chunk used dest `0xFFFF` and zeroed kernel text (`HALT` at `0x80011877`).
- `mtpr MAPEN` then `RET` from `pmap_bootstrap` used the `/boot` stack at `0x79xxxx` (P1). Kernel pmap sets `P1LR=NPTEPERREG` (empty P1); those pages are identity-mapped in S0 as `va|KERNBASE`. V0.6.9 promotes SP/FP/AP and CALLS saved FP/AP before MAPEN. P0/P1 walks match SIMH (P1LR = hole at 0x40000000; P0/P1 PTEs via S0). Faults now log VA + SBR/PTE.
- After MAPEN, `mtpr $0,$IPL` takes the clock IRQ. **SCBB is physical** (SIMH `ReadLP((SCBB+vec)&PAMASK)`). vVax was translating `SCBB+0xC0` as P0 (`mmu-r VA=0049F8FC`, `P0LR=0`). V0.6.10 reads SCB physically, honors SCB ISTACK→ISP, and REI switches KSP/ISP.
- Banner then hung on `IRQ vec=0xFC` (console TX). VARM/SIMH request TX/RX when `{IE AND DONE}` goes **0→1**; taking the IRQ clears the request. NetBSD `gencninit` sets TX IE with DONE already set; `gencntint` never clears IE. V0.6.11 latches that edge and acks on delivery (level-triggered `irq_tx()` livelocked). JSB to `0x800006A8` is `cmn_idsptch`, not a wild pointer.
- V0.6.11 reached NetBSD banner + `uba0 at mainbus0: Q22` then `pa-r VA=0x80FD0600`: S0 PTE maps to Q22 PA `0x30000600` (upper half of the 512 MiB Q22 window). V0.6.12 treats phys `0x20000000–0x3FFFFFFF` as Q22 (22-bit normalize before MSCP/console decode).
- After `dhu0 didn't interrupt`, `SVPCTX` @ `0x80000824` faulted `mmu-w VA=0x0040A000` with `P0LR=0`. PCBB is a **physical** PCB address; SIMH uses `ReadLP`/`WriteLP`, not MMU virtual. V0.6.13 uses `phys_r32`/`phys_w32` on `PCBB & PAMASK`.
- V0.6.13 HALT @ `PC=0x825F8F98` (zero-filled kmem): SVPCTX/LDPCTX used wrong PCB layout (PSL@64) and LDPCTX jumped PC directly instead of pushing savpc/savpsl for RET. V0.6.14 matches SIMH: pop PC/PSL, ISP switch, AP/FP@64/68, PC/PSL@72/76, restore P0/P1 IPRs, LDPCTX pushes return frame on KSP.
- V0.6.14 hung 30 min after the entropy warning (timestamps stuck at `[   1.0000000]`). Interval clock was level-triggered `DONE&&IE` and ICCS/ICR/NICR did not match KA630/SIMH; NetBSD `hardclock` / `vax_mfpr_get_counter` can spin until `getticks()` moves. V0.6.15: edge-latched clock IRQ + ack, NICR/ICR IPRs, kernel PC heartbeat after MAPEN.
- After ~45 min V0.6.15 stopped on `pa-w VA=FEDABABE`. That is NetBSD `CASMAGIC`, not a wild pointer: uniprocessor `cas32` in `lock_stubs.S` is a restartable sequence; an interrupt plants `CASMAGIC` in `ci_cas_addr` so the resumed store must take a **region length violation** (S1, `VA>=0xC0000000`) and `trap.c` restarts at `cas32_ras_start`. vVax identity-mapped S1 and sticky-halted. V0.6.16: S1 translate fails; deliver SCB ACV `0x20` with `p1=MM_PARAM(write,PR_LNV)`, `p2=VA`, saved PC = faulting insn; do not halt.
- V0.6.16 HALT @ `PC=007B55C0` right after first `IRQ vec=0xC0` during `/boot`: clock selftest left ICCS IE on; `poll()` latched a tick before `/boot` finished the SCB handler stub (bytes at `0x7B55C0` still zero). V0.6.17: selftest ack clears IE; `cold_boot()` calls `vax_clock::reset()` before xxboot.
- Kernel autoconf after `dhu0 didn't interrupt` hung at `PC=8001043E` (`SOBGTR` in NetBSD `delay()`: `cpu_vups * ms` probe timeouts). ESP32 runs the countdown far slower than 1 loop/µs. V0.6.18 caps kernel `SOBGTR` counts above 1000 to 1 iteration so uba/uba probes time out quickly instead of spinning billions of times.
- After entropy + working `CASMAGIC` ACV, no `uda0` line: kernel idled on a process stack (`SP=825DEFxx`) without a disk. `/boot` talks MSCP at RPB `0x20001C68`; kernel `uba` maps the 8K Q22 I/O page as `0x20000000+(0172150-0160000)` = `0x20001468`. `udamatch` writes IP=0 and waits for STEP1 — a miss returns 0 with no printf. V0.6.19 decodes both CSR aliases.
- After flash, expect one-shot `MSCP DMA S0 0x80000000 -> pa 0x00000000` on `boot netbsd`. Autoboot still tries `netbsd.vax` first (ENODEV) — use `boot netbsd`
- V0.6.23: `uda0 at uba0 csr 172150 vec 774 ipl 17` then **`ubmemalloc failed: 35`** (EAGAIN). `qba_attach` `badaddr()`s Q22 window `0x30000000` with map regs invalid; vVax folded that PA into the I/O page and returned 0, so sgmap reserved `0–0x3fdfff`. V0.6.24: KA630 map regs at `0x20088000`, Q22 window via maps, SCB MCHK `0x04` on NXMEM. Expect `mscpbus0` / `ra0` after `uda0`.
- V0.6.25: UQSSP STEP2GOOD (IE+vector) so `mscpbus0` inits. Then **`no Get Unit Status response`** while scanning unit 10: GUS+NEXTUNIT must wrap with a lower unit number when no drive remains; echoing unit=10 + OFFLINE walked 0..4095 until DELAY timed out. `ra0`/`ra1` still appeared later via workqueue (too late for a clean configure). V0.6.26: NEXTUNIT wrap (GUS 0,1,2 then stop). V0.6.26 also loosened `SOBGTR` and hung again at `PC=8001043E` (`delay()` / qe probe). V0.6.27: restore cap `>1000 → 1`.
- V0.6.27 idled for hours after `ra0`/`ra1` with clock + console RX working, no `boot device:`. Hang PCs were `idle_loop` / `uvm_idle` / `getticks` — proc 0 asleep. `MTPR SIRR` (IPR 20) was a no-op, so NetBSD `softclock` (SCB `0xA0`) never ran and `cv_timedwait`/`config_finalize` never woke. V0.6.28: SIRR/SISR + software IRQ delivery. Also plant RPB `csrphy=0x20001468` at kernel entry so `booted_ra()` matches uba `0172150`.
- V0.6.28: SIRR worked (`ipl=8`/`13`, timestamps past 1.0s) then `mmu-w VA=82E25014` with S0 PTE=0. That is kernel demand-paging, not a bug in the store. V0.6.29: invalid PTE → SCB TNV `0x24` (length still ACV `0x20`) so `uvm_fault` can map the page.
- V0.6.30 reached multiuser (`login:`) after the ACV fix, then a reboot printed **`boot device: <unknown>`** and waited at **`root device:`** (IPL 31 `cngetc` poll — console was live). The V0.6.28 RPB plant used R11/0xF0000 as host offsets and skipped S0 (`0x80000000+104 > ram`). locore `_start(prpb)` memcpy's the CALLS RPB to the uarea; `pmap_bootstrap` copies that onto PA 0 **after** `scb_init()`. V0.6.31 force-planted KERNBASE/PA 0 at CALLS and overwrote the SCB template (`csrphy 0x8000036D` was `cmrerr`). V0.6.32 plants only the `/boot` RPB (`0x7B26E8`).
- After `swwdog0` the kernel is in `idle_loop` (`PC=801ACxxx`, `SP=825DEFxx`) while `config_finalize` `cv_timedwait`s for `config_pending`. Guest hz tracks wall-clock 100 Hz, so that wait is tens of minutes with no USB line (`boot device:` is TFT/Telnet). V0.6.33 warps the interval clock while proc0 is on that sleep stack so timeouts expire.
- V0.6.33 printed `boot device: ra0` / `root on ra0a` then **`vfs_mountroot: can't open root device` error 6 (ENXIO)**. `ra_putonline` tsleeps 100 guest seconds for MSCP ONLINE; warp flooded IPL 24 so the sleep expired before the disk workqueue ran. V0.6.34: no warp while MSCP busy / SISR pending; cap extra ticks at ~10× wall.
- V0.6.34 still ENXIO: ONLINE completes (`op=9`) but `ra_putonline` never wakes. Warp still matched all `0x825xxxxx` stacks (`pffasttimo` at `SP=825ECEE4`) and resumed immediately after the MSCP ISR, so IPL 24 starved `mscp_wq`. V0.6.35 warps only idle page `0x825DE000` and holds off 250 ms after MSCP/SISR.
- V0.6.35 still idled on that page after `op=9` (`PC=8019049B` `uvmpdpol_idle`, `SP=825DEEEC`): 250 ms later warp resumed while `ra_putonline` tsleeps, so IPL 24 starved `mscp_wq` again. V0.6.36: no warp until the host queues another MSCP command after ONLINE.
- V0.6.36 still ENXIO with no `ra0: size N sectors`, so `rx_putonline` never left `DK_CLOSED`. V0.6.37 is instrumentation only (`kprobe`, `root hb`, `MSCP ONLINE`/`IRQ`). After `root on ra0a`, paste those USB lines. Success order: `rx_putonline` → `tsleep` → `udaintr`/`rronline` → `wakeup`. `wakeup` before `tsleep` is a lost wakeup.

## Phase 7 — Host UX parity (vpdp1170)

Mirror the common Freenove emulator host stack from **vpdp1170** (same `host_lib` family). Guest console Telnet/FTP/WiFi/INI already work; this phase fills the operator UX gaps.

| Feature | Goal | vpdp1170 reference | vVax today |
|---------|------|--------------------|------------|
| **Status line** | Persistent TFT band: title, IP, TEL/FTP pills, MSCP activity, KIPS/halt | `draw_status_bar`, `VPDP_STATUS_BAND_H` | Boot `tft_status` rows only |
| **Touch GUI** | Double-tap settings: drives, WiFi/config pickers, brightness, restart/reset; pause guest while open | `ui.cpp`, `touch.cpp` | `ui.cpp` stub (“later host polish”) |
| **Telnet shell** | `Esc` then `>>` host shell: FS, `drives`/`mount`/`dismount`, `set`, `reboot`/`reset`, VAX `halt`/`continue`/`regs`; `exit` → guest | `telnet_shell` + `host_lib/shell_*` | Minimal: `help`/`status`/`reset`/`exit`; shell libs not registered |
| **Guest Telnet** | Keep port 23 guest console (TFT+USB+Telnet) | `telnet_pipe` | Done |
| **FTP** | Keep SD FTP + storage guard vs mounted MSCP | `ftp` + `storage_guard` | Done |
| **boot_script** | Optional expect/reply after reboot (vpdp `boot_script`) | `host_lib/boot/boot_script.*` | `boot_input` only |
| **host_diag** | Diag stream to USB + Telnet (not only Serial) | `host_diag.*` | USB Serial dumps only |

Implementation notes:

- Prefer registering existing `host_lib/shell_*` over rewriting `telnet_shell.cpp`
- Status band geometry must match Freenove VT100 layout (`host_lib/console` + board header)
- MSCP activity hooks: pulse status pills from `vax_mscp` ring/xfer (same idea as PDP RP/RL LEDs)
- Do not block Phase 6 NetBSD bring-up; land UX in parallel once kernel handoff is stable enough to need operator controls

## Phase 8 — Ethernet NAT (secondary)

- DELQA/DEQNA CSR model
- TX → `eth_nat::on_guest_tx`; RX ← `eth_nat::pop_rx`
- INI keys already match vpdp1170 (`enabled`, `mac`, `guest_ip`, …)

## Legal reminders

- No VMS hobbyist kits in-tree
- User-supplied firmware under `vVaxSdCard/firmware/`
- Prefer NetBSD open media
