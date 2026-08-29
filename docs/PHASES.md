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

## Phase 6 — NetBSD try (C8 closed: single-user only)

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
- The Aug 18 `login:` was **SIMH** (KA655), not a vVax V0.6.30 multiuser success. After the ACV fix a vVax reboot printed **`boot device: <unknown>`** and waited at **`root device:`** (IPL 31 `cngetc` poll — console was live). The V0.6.28 RPB plant used R11/0xF0000 as host offsets and skipped S0 (`0x80000000+104 > ram`). locore `_start(prpb)` memcpy's the CALLS RPB to the uarea; `pmap_bootstrap` copies that onto PA 0 **after** `scb_init()`. V0.6.31 force-planted KERNBASE/PA 0 at CALLS and overwrote the SCB template (`csrphy 0x8000036D` was `cmrerr`). V0.6.32 plants only the `/boot` RPB (`0x7B26E8`).
- After `swwdog0` the kernel is in `idle_loop` (`PC=801ACxxx`, `SP=825DEFxx`) while `config_finalize` `cv_timedwait`s for `config_pending`. Guest hz tracks wall-clock 100 Hz, so that wait is tens of minutes with no USB line (`boot device:` is TFT/Telnet). V0.6.33 warps the interval clock while proc0 is on that sleep stack so timeouts expire.
- V0.6.33 printed `boot device: ra0` / `root on ra0a` then **`vfs_mountroot: can't open root device` error 6 (ENXIO)**. `ra_putonline` tsleeps 100 guest seconds for MSCP ONLINE; warp flooded IPL 24 so the sleep expired before the disk workqueue ran. V0.6.34: no warp while MSCP busy / SISR pending; cap extra ticks at ~10× wall.
- V0.6.34 still ENXIO: ONLINE completes (`op=9`) but `ra_putonline` never wakes. Warp still matched all `0x825xxxxx` stacks (`pffasttimo` at `SP=825ECEE4`) and resumed immediately after the MSCP ISR, so IPL 24 starved `mscp_wq`. V0.6.35 warps only idle page `0x825DE000` and holds off 250 ms after MSCP/SISR.
- V0.6.35 still idled on that page after `op=9` (`PC=8019049B` `uvmpdpol_idle`, `SP=825DEEEC`): 250 ms later warp resumed while `ra_putonline` tsleeps, so IPL 24 starved `mscp_wq` again. V0.6.36: no warp until the host queues another MSCP command after ONLINE.
- V0.6.36 still ENXIO with no `ra0: size N sectors`, so `rx_putonline` never left `DK_CLOSED`. V0.6.37 is instrumentation only (`kprobe`, `root hb`, `MSCP ONLINE`/`IRQ`). After `root on ra0a`, paste those USB lines. Success order: `rx_putonline` → `tsleep` → `udaintr`/`rronline` → `wakeup`. `wakeup` before `tsleep` is a lost wakeup.
- V0.6.37 proved that lost wakeup: ONLINE completes inside the IP-read at `rx_putonline+0x4A` (`PC=8001EC66`), IRQ is taken on the next insn, `rronline`/`wakeup` run at IPL 23, then `tsleep` — timeout → ENXIO. V0.6.38 still posts the response ring immediately (so `/boot` polling works) but defers `latch_irq` 256 guest instructions when the doorbell IP-read is the caller. Expect `ra0: size … sectors` after `swwdog0`, then `boot device: ra0` / FFS mount. USB order should be `IRQ defer` → `tsleep` → `IRQ latch`/`udaintr`/`wakeup`.
- V0.6.38 mounted FFS then `exec /sbin/init: error 63` (and oinit/init.bak/rescue/init). 63 is `ENAMETOOLONG`. Kernel `copyinstr` (`800008A8`) is a byte loop with `SOBGTR` over `MAXPATHLEN` (1024). V0.6.18 capped every kernel `SOBGTR` count `>1000` to 1 for `delay()`; that aborted `copyinstr` after one byte. V0.6.39 still collapses empty `sobgtr rN, .` busy-waits (disp −4..−1) and leaves real loop bodies alone. TOD “preposterous” / filesystem time is expected (TOY not valid).
- V0.6.39 reached `ufs_lookup` / `uvn_findpage` / MSCP reads of init, then `VAX HALT at PC=800002DB`. That is `Xaccess_v-1`, not a real HALT: I-fetch ACV on a zero PTE (typical first user insn after exec) vectors to `Xaccess_v`, but `fetch8()` returns 0 on abort and the opcode switch treated it as HALT. V0.6.40 returns from `exec_one` when `g_mmgt_abort` is set after the opcode fetch so `uvm_fault` can run.
- V0.6.40 exec'd pid 1 (`init` / `ld.elf_so` at `PC=0x7f6e5a05`) then `panic: SEGV in kernel mode: pc 0x7f6e5a05 addr 0x8`. Trapframe had kernel FP/AP, USP `7ffffb34`, `r6=0` (ld.so uses `8(r6)`). IRQ delivery only raised IPL and left PSL CUR=user, so after `sret`/`REI` the clock handler ran as user (or user text ran as kernel). V0.6.41 matches SIMH `intexc`: interrupt PSL is kernel (CUR=0) plus IS/IPL. USB should show `REI user PC=…`.
- V0.6.41 did REI to user (`PSL=03C00000`, `PC=7F6E04F6` ld.so) then the same kernel-mode SEGV at `7f6e5a05` / `addr 0x8`. CHMK was still storing the kernel stack while CUR=user (KSP PTE is kernel-only → ACV), then overwrote `raise_mmgt`'s PC/PSL and continued as kernel at the user PC. V0.6.42 matches SIMH `op_chm`: destination-mode access for those three words, and stop if the store aborts. Expect `boot: CHMK from user … abort=0` then syscalls instead of DDB.
- V0.6.42 still panicked at `pc 0x7f6e5a05 addr 0x8` with no CHMK line in the USB clip. Trapframe matches `_rtld_start` after the two `pushl`s (`r0=_DYNAMIC`, `r10=relocbase`, USP `7ffffb34`) then a `8(r6)` fetch with `r6=0`; kernel FP/AP means Xtrap ran from kernel C. Same abort-then-clobber class as CHMK: `try_deliver_irq` switched PSL to kernel *before* pushing PC/PSL, so a push ACV saved kernel PSL with the ld.so PC. V0.6.43 writes the interrupt frame with kernel access first (SIMH `intexc`) and only then commits PSL/PC; logs `user PC as kernel` and ACV when a user PC faults under a kernel PSL. USB should show `vVax V0.6.43`, `REI user … R6=…`, then either `CHMK` / `user PC as kernel` or a user SIGSEGV instead of DDB.
- V0.6.43 still panicked at the same PC/addr. USP `7ffffb34` means `_rtld_start` ran in user mode (the two `pushl`s); `fp=82e24fd8` is a CALLS frame on KSP — the CALLS to `_rtld_relocate_nonplt_self` ran with kernel CUR. A clock/softint REI restored the ld.so PC but not `PSL_U`. V0.6.44 builds ACV frames with kernel access before changing PSL (same as IRQ), logs every REI into `0x7Fxxxxxx`, and if IPL is 0 with kernel CUR at an ld.so PC, restores `PSL_U|PSL_PREVU` and USP. Flash `V0.6.44`; USB should show `REI P1` / `user PC as kernel` around the old panic.
- V0.6.44 USB confirmed the pattern: `REI user PSL=03C00000 R6=7FFFFB44`, demand-paging ACVs with `PSL=03C00000`, then `ACV … PC=7F6E59FA PSL=00C00000 SP=82E24FD8` (handler PSL + KSP at ld.so). V0.6.44 repair never fired — it wrongly required SP bit 31, but KSP is `0x82E…`. V0.6.45 repairs any IPL-0 user PC with kernel CUR (detect KSP/ISP/uarea bands), fixes sret/REI when nPSL is `00C00000` but the target PC is user, and logs `boot: repair` / `boot: REI fix`. Expect ld.so to pass `8(r6)` or fault as user SIGSEGV, not DDB.
- V0.6.48 reached `_rtld_relocate_nonplt_self` in user mode then **ACV storm `PC=7F6E86B2 VA=0x200`** with `R1=R3=7F6E0000` (map base) but `R6=0x200`, `R10=0`, **`AP==FP`**. `_rtld_start` computes `relocbase` in R10 and `calls $2` with that as `8(ap)`; `where = relocbase + rela->r_offset` with relocbase 0 stores to file offset `0x200`. CALLS fetched the callee entry mask, took an ACV on the unmapped page, then still wrote AP/FP/PC=`entry+2` — overwriting `raise_mmgt`'s handler and building a frame on KSP. Repair then forced AP=FP=USP so `8(ap)` read saved AP (0). V0.6.49 stops CALLS/CALLG after a mask/stack abort (same commit-after-abort class as CHMK), and repair only restores PSL_U / USP — never AP/FP/R6. Flash `V0.6.49`; USB should show `CALLS user entry=… 0(sp)=7F6E59F8 4(sp)=7F6E0000` then relocate stores in `7F6Exxxx`, not `VA=0x200`.
- V0.6.49 reached `_rtld` (`PC=7F6E720D` user) then emulator HALT at `PC=7F65F470` with bytes `00 AC 6D 7F 27 05 6E 7F` (pointers, not text) and `R0=0`. Opcode `00` in user mode is a privileged-instruction fault, not a console halt (SIMH `RSVD_INST_FAULT`). V0.6.50 vectors SCB `0x10` (`Xprivinflt`) so the guest can SIGILL instead of stopping the host, and logs `user HALT` plus the first user `JMP dest=`. Flash `V0.6.50`; expect `JMP user dest=… R0=…` then either init or `user HALT` plus a guest signal, not `boot stop: halt=1`.
- V0.6.50 did start init (`Stopped in pid 99.99 (init)`) then `user HALT PC=7F65F470` with **`PSL=7F55BD10`** (data: IS, IPL 21, MBZ bits) and `REI -> PC=0` → `panic: SEGV in kernel mode: pc 0 addr 0`. REI loaded that word as PSL; `raise_exception` trusted IS and pushed the privflt frame on USP. V0.6.51 matches SIMH `op_rei` PSL checks. Sanitizing a reserved PSL to `03C00000` was a dead end: it lets the CPU run at the wrong PC (execute data / HALT) and hides the bad trapframe. V0.6.52 leaves reserved REI as a reserved-operand fault and dumps `(%sp-8)…(%sp+20)` plus GPRs so an off-by-8 sret (trap/code vs PC/PSL) is visible. Flash `V0.6.52`; paste `boot: REI bad` / `REI stack`.
- V0.6.52 dump: `SP=82E35FF4` `[PC]=7F65F470` `[PSL]=7F55BD10` `+8=03C00004` `+12=82E37E90` (uarea top `82E36000`). That is **off-by-4**, not off-by-8: `03C00004` at `82E35FFC` is `tf_psl`; `7F55BD10` is `tf_pc`; `7F65F470` is `tf_code`. V0.6.51’s PSL rewrite would REI to the fault VA (`7F65F470`) with a fake user PSL — same HALT as V0.6.50. Panic `type 2 pc 800002d4` is the reserved-operand vector, not a new guest bug. V0.6.53 dumps the if-4 slots plus `POPR mask=` (expect `0xFFF` / 12 regs). V0.6.54 waits 2 s after `Serial.begin` so USB CDC can enumerate before the version line. V0.6.55 heartbeats every 30 s, then stop after the REI dump (`USB: heartbeats off`) so the serial view can be copied.
- V0.6.54 USB: `POPR mask=FFF nreg=12 PC=80000423` is **sret** (balanced). The bad REI is **not** sret: `opPC=800002D4` is `Xtransl_v`’s success-path `rei` after `pmap_simulref` returned 0. Stack is one long low (TOS=TNV VA `7F65F470`, `+4`=user PC `7F55BD10`, `+8`=legal `03C00004`). `if-4` is the correct REI. Do not rewrite that PSL and do not blindly `SP+=4`. V0.6.56 stops CALLS/PUSHR/PUSHL/JSB/BSB from decrementing SP before the write succeeds (same abort-then-leak class as V0.6.49). V0.6.56 then **LoadProhibited** on the first ld.so TNV: `LOG("… (%sp)=%08X")` is `printf %s` and copied from VA 0/`3`. V0.6.57 uses `tos=` in those lines. Also HALT is privileged by PSL CUR only, so the `0x1000` stub prints `VAX selftest: PASS` instead of `exc-scb`. Flash `V0.6.57`.
- V0.6.57 reached user/kernel then **`undefined opcode 0x2A at PC=80204BC4`** (kernel SCANC). Host halt is wrong: SCANC/SPANC/LOCC/SKPC are MicroVAX `IG_BASE` string ops (SIMH `op_scnspn` / `op_locskp`). V0.6.58 implements them (no FPD; MMGT retries the insn). Flash `V0.6.58`.
- V0.6.58 reached single-user `sh`. TTY/`read` into ash `basebuf` keeps only odd bytes (`export`→`xot`, doubled typing); `cat` of `/etc/rc` and `cat` of the console keep every byte. V0.6.59 dumps the first kernel↔user MOVC3/MOVC5 and kernel MOVB stores (`copy:` USB lines: src vs dst hex, P0/P1, dest PA even/odd, zero counts). Flash `V0.6.59`; after `cat` vs `sh` on the console, paste the `copy:` lines.
- V0.6.59 idle then **`undefined opcode 0x6E at PC=00011B62`** (`SP=7FFFF1DC`). That is **CVTLD** (long→D_float) in user P0 text (`ps` %CPU), not a junk PC. Host halt is wrong (same class as SCANC). V0.6.60 takes SCB `0x10` reserved-instruction (guest SIGILL) instead of stopping the emulator. D-float is still unimplemented.
- V0.6.59 `copy:` dump: every MOVC3 src==dst (packed `/sbin/init`, ELF at P0 `0x24000`, `/usr/lib`); kernel MOVB to P1 wr==rd. Budget was spent on ld.so P1 memcpy before `sh` read `/etc/rc`. Arduino still flashed 0.6.59 (`21:35:48`). V0.6.61 keeps reserved-inst and logs **only kernel copies into P0** (ash `basebuf`).
- V0.6.64 `copy: rd`: sh reads `/etc/rc` with **stride 1** (`off=0,1,2…` = `#!/bin/sh`; `340–348` = `t HOME=/\n`; `349–351` = `exp`). Watch still has packed `export PATH=/sbi` when `/etc/rc: xot` prints. Same odd-index keep on other words (`umask`→`ms`, `unset`→`ne`, `unalias`→`nla`, `exit`→`xt`). V0.6.65 logs ash `USTPUTC` dest (`copy: wr tok VA=`). Flash `V0.6.65`; paste `wr tok` around the first `xot` (packed `e,x,p,o,r,t` vs stride-2 dest vs only `x,o,t`).
- V0.6.65 `wr tok` spent the 32-sample budget on CHECKSTRSPACE NULs (`PC=1E90A` writes `00 00` to `0x48352` after every `pgetc`) and skipped dests inside the watch. No lexer store of `e,x,p,o,r,t`. Nearby packed copies at `PC=25E3F` (`HOME=/` at `0x48475`) are stride 1. V0.6.66 ignores zeros, logs printable stores even in the watch (`d=` stride, R0/R1/R6), and dumps 32 bytes past the script buffer (`tokbss`). Flash `V0.6.66`; paste `wr tok` / `tokbss` from `HOME=/` through the first `xot`.
- V0.6.66: `PC=25E3F` copies packed `export` to `0x48454` (`e,x,p,o,r,t` stride 1, R0=dest+1). Watch still has packed `export PATH=`. Command printed as `xot` anyway — thinning is a later **read** of that name (odd-byte / +2 walk), not the store. `R1=7FFF0000` is `clrw`/`movw` merge (SIMH does the same). V0.6.67 arms `copy: rd name` on the packed `ex` copy. Flash `V0.6.67`; paste `name` + `rd name` through the first `xot` (`d=1` packed vs `d=2` / `off=1,3,5` = xot).
- V0.6.67 never printed `name` / `rd name`: `25E64` stores `0x65` at `0x4834C` between dest `e` and `x`, so the consecutive-`ex` arm never fired. V0.6.68 arms on `25E3F` `'e'` into `0x48xxx` and dumps that 16-byte name on heartbeats. Flash `V0.6.68`; paste `name arm` + `rd name` + heartbeat `name VA=` through the first `xot`.
- V0.6.68: name at `0x48454` is packed `export\0` (heartbeat still packed when `xot` prints). Logged reads are stride 1 (`128C7`/`128E1` strlen, `247F6`, `1C16A`). Budget died on those walks; libc `%s` is at `7Fxxxxxx` and was filtered. `/etc/rc:` prints packed, so thinning is in argv/`error`’s `%s` pointer, not a global `%s` bug. V0.6.69 skips repeat strlen PCs, allows libc name reads, logs `wr arg` copies after the name, and `copy: tx` around the error. Flash `V0.6.69`; paste `rd name` with `PC=7F…` / `d=2` / `wr arg` / `tx` through the first `xot`.
- V0.6.69: `0x48454` stays packed `export`. Thinning is sh `PC=1C19C` (stores `x`,`o`,`t` at dest+1,+3,+5 with R1 holding `e`,`p`,`r`) then `PC=1AB61` packs those to `xot` at `0x484AC`. Same for `HOME=/` → `OE/`. Libc strcpy/MOVC3 of the already-thinned name is packed. V0.6.70 dumps the 8 insn bytes at those PCs and logs CTLESC `0x81` / NUL dest slots. Flash `V0.6.70`; paste `insn` + `arg16` + `wr arg` `b=81` around the first `1C19C`.
- V0.6.71: CTLESC arm confirmed. `1C166` stores literal `0x81`, `1C19C` is `movb r10,(r0)` of `p[1]`, dest is `81 x 81 o 81 t`, `1AB61` rmescapes to `xot`. Same for `HOME=/` → `81 O 81 E 81 /` → `OE/`. Logged `copy: case` was parser `25DA8` (budget), not argstr. V0.6.72 dumps `1C0E0`–`1C210` on the first `1C166` (the dispatch into CTLESC) and only logs CASE in `1C000–1D000`. Flash `V0.6.72`; paste `code PC=0001C0E0` and any `copy: case PC=0001C… sel=`.
- V0.6.72: no CASE in argstr. Dispatch is `cmpb $0x8C` / `cmpb $0x80` / `cvtbl` / `movab 0x7E(r2)` / `cmpl $9` / `bgtru` / `tstl r11`. `'e'` yields `R2=0xE3` (unsigned > 9) so the CTL table is skipped; letters still fall into the CTLESC `p+=2` body, which means `r11` was nonzero. That register is either `quotes` (`flag & EXP_QNEEDED` = `0x80` for `EXP_FULL`) or leftover. V0.6.73 dumps `R0–R11` / CALLS mask / `4(ap)` / prologue `1C040` and CTL table `1C220` on the first `1C166`. Flash `V0.6.73`; paste `copy:  gate` + `copy:  frame` + `code PC=0001C040`.
- V0.6.73: **B** (official codegen). `r11=0x80` is `quotes = flag & EXP_QNEEDED`; `8(ap)=0x83` is `EXP_FULL|EXP_TILDE` from `eval.c`; `4(ap)=0x48454` still packed `export`; CALLS mask `0xFC0` (r6–r11) is the `1C09C` entry word `C0 0F`. Gate is `cmpb $0x8C` / `blss` → `cmpb $0x80` / `cvtbl` / `movab 0x7E(r2)` / `cmpl $9` / `bgtru` / `tstl r11` / `bneq 1C13C` → `movb $0x81,(r0)` at `1C166`. **No `cmpb $0x81` / NEEDESC test before that store.** Stock NetBSD 10.1 VAX `base.tgz` `./bin/sh` (ELF text `0x10000`) matches the guest windows byte-for-byte at `1C040` / `1C166`. C `argstr` default is `STPUTC(c)`; gcc folds every non-`0x82..0x8B` char into the CTLESC `p+=2` body when quotes is set. **Retracted:** same binary on SIMH does not print `xot`. Not a remaining CMPB/BLSS/BGTRU/CVTBL/CALLS bug on this vVax path. Do not special-case `xot` or clear `EXP_GLOB`. No sh `xot` V0.6.74 tracer.
- V0.6.74: NetBSD install-CD halt `REI -> low PC=00000002` is **C** — FROM750 `hoppabort` REIs to `e_entry+2`. Stock `BOOT.;1` has `e_entry=0` / `p_vaddr=0x7D0000`, so hopp is PC=2 (zeros → HALT). Not A: host does **not** mask `0x7D0002` (that REI would log `REI -> PC=007D0002`). Not B: `e_entry=2` would hopp to PC=4; ISO LBN 864753 is the file after the header sector. HDD `/boot` was relocated to `0x7A0000` because stock `0x7D0000` does not fit 8192000-byte RAM + last-page conspage; CD cannot be pre-patched the same way. Log only (no CD relocator): `boot: ELF hdr dest= e_entry= p_vaddr= hopp=` then `ELF load dest=` then the low-PC REI line with those fields. Flash `V0.6.74`; paste those three USB lines.
- V0.6.75: CD `/boot` relocator. Stock `BOOT.;1` `e_entry=0` / `p_vaddr=0x7D0000` (or `0x7A0000`/`0x5D0000`) does not fit 8192000-byte RAM + conspage — same reason HDD `/boot` was pre-patched; the ISO cannot be. Host remaps that PT_LOAD to **`0x7A0000`** (never `0x200000`): redirect ROM-read dest in the `p_vaddr` window (those writes currently drop), copy BOOT.;1 LBN sectors from dest=0 bounce into `0x7A0000` (ISO dir LBNs 16/129/65/169/201 are left alone), then hoppabort `e_entry+2` (PC=2) → **`0x7A0002`** (and R6). Flash `V0.6.75`; expect `boot: CD /boot reloc p_vaddr=007D0000 -> 007A0000` then `boot: REI -> PC=007A0002` then the NetBSD `/boot` banner (or a new fault if load dest was wrong). Keep the ELF hdr/load lines.
- V0.6.76: V0.6.75 copied BOOT.;1 from file offset 0, so `0x7A0000` was ELF magic (`7F 45 4C 46`) and hopp `PC=007A0002` executed `4C` (`LF`). Host now parses PT_LOAD `p_offset` / `p_filesz` from the header bounce and places those file bytes at **`0x7A0000`** (LBN 864753 is 9×512 = 4608 into the file), then applies the HDD 0x7D0000→0x7A0000 reloc on the text. dest=0 stays xxboot bounce for ISO dir LBNs. Flash `V0.6.76`; expect `REI -> PC=007A0002`, fingerprint `7E`, `/boot` banner; reserved `0x4C` at `7A0002` is ELF-header-at-load-base and should be gone. Log `p_offset` / `p_filesz` / first 8 bytes at `0x7A0000` (VAX entry mask, not `7F 45 4C 46`). **Results:** ELF hdr `p_offset=84` (not 4608) `p_filesz=75496` `p_vaddr=007D0000` `e_entry=0`; CD `/boot` reloc + ELF load dest=`007A0000`; `@7A0000`=`01 01 D0 8F 00 00 7A 00` (VAX mask + `movl $0x7A0000`, not ELF magic); fingerprint `@7A233D=7E`; `REI PC=007A0002` → NetBSD/vax `/boot` banner; `boot netbsd` (autoboot `netbsd.vax` ENOENT + `nfs_open` + `getdisklabel: no disk label` are expected on ISO) → GENERIC / MicroVAX II / root `ra1a` cd9660 → ld.so/init/sh. Then same Phase 6 failures: `/etc/rc: xot: not found` and `copy: tx` of `/etc/rc: Number out of range: 2` (ash `number()`, not sysinst). CD `xot` means this is not HDD-specific. Relocator done; remaining is UV2 `argstr` vs SIMH KA655.
- Open SIMH V4 KA655 CVAX (MicroVAX 3800/3900, 65468 KB) ran this same disk to multiuser (`boot netbsd` → FFS → full `/etc/rc`; no `xot`, no "not found", no single-user). The gcc-bytes match is real but cannot be the whole story; remaining gap is vVax KA630 UV2 vs that working SIMH (parser output vs `argstr`, or a CPU difference that makes us take `1C166` for letters when CVAX does not).

## 11/750 conversion (branch `vax-11750`)

Work lives on branch `vax-11750` and is specified by [`VAX11750.md`](VAX11750.md).
Product target in [`TARGET.md`](TARGET.md) remains MicroVAX II.

**V0.6.76 flash (Aug 23 2026 12:17:01):** C0/C1/C2 confirmed — `VAX model: KA750`,
no Q22 map alloc, clock `PASS (TODR=…)` without TOY chip line, no conspage plant,
selftests PASS, xxboot/CD `/boot` reloc still work. **R2 was still `0x20001C68`**
(Q22). Autoboot countdown at `PC=007A04A1`. Guest MSCP was dead (`csr_hit` false).

**V0.7.0:** C6 R1=`0xF30000` / R2=`0xFFF468`; Unibus UDA; maps @ `0xF30800`.
USB: selftest PASS, xxboot those bootregs, ROM-reads, `/boot` banner, RPB
`csr=00FFF468`. Then `pa-w VA=00F32FB8` (`BISL3 #0x80000000,(r2)+`).

**V0.7.1:** DW750 maps also at `0xF32800` (512); nexus writes absorbed.
VMS xxboot (not VMB) sized past SID; MCHK `0xFC0000`/`0xFC2000` then **`pa-r
VA=01000000`** abort.

**V0.7.2:** NXMEM above RAM → MCHK, not `VAX fault 2`. `[system] os=`
(`netbsd` default / `vms`) selects host boot path (not a SID lie). Flash
`vax-11750`; USB `vVax V0.7.2`. After the two FC MCHKs expect
`MCHK pa=01000000` then continue, not `fault 2 (pa-r)`. `os=vms`:
`guest running (VMS)`, no NetBSD xxboot / ELF hopp.

**V0.7.3:** Empty CMI/nexus read → MCHK, not opcode 0 (`HALT` at `0xFBA000`).
16 MB probe was likely already MCHK; V0.7.2 only logged the first two.
Flash `vax-11750`; USB `vVax V0.7.3`. Expect `MCHK pa=01000000` and
`MCHK pa=00FBA000` (no silent HALT). Still no VMB/console ROM.

**V0.7.4:** VMS (`os=vms`) xxboot ROM-reads OK, then nexus scan MCHK at
`0xF20000` (TR0) and `0xF80000`/`0xF80800`, UBA map[494], then
`%BOOT-F-Unexpected Machine Check` / HALT `PC=000004C6`. TR0 is MS750 —
must respond. `0xF32800` map alias made TR9 look like UBA1, so VMS used
UBA1 Unibus mem at `0xF80000` outside a probe. C5: 750 MCHK frame
`bcnt=0x28`. Flash `vax-11750`; USB `vVax V0.7.4`. Expect `MCTL mcsr2=…`
and no MCHK at `0xF20000` / `0xF80000`. VMS still not the product target.

**V0.7.5:** V0.7.4 found MCTL (`mcsr2=0x00011555 ram=8192000`) then HALT at
`PC=000FFEC4` (zeros). Chip-size bits were 0, so VMS treated seven slots as
256 KiB boards (~1.75 MiB) and transferred SYSBOOT to the top of 1 MiB.
CS64 + complete 1 MiB boards only (`0x01011555` = 7 × 1 MiB). Do not claim
8 MiB: Freenove RAM ends at `0x7D0000`. Flash `vVax V0.7.5`; expect
`MCTL mcsr2=0x01011555` and no HALT at `0xFFEC4`.

**V0.7.6:** V0.7.5 still HALT `PC=000FFEC4` zeros; `R1=000089D5 R2=000063EF
R3=000FFE44` (copy src/len/dst) and no JMP log — transfer via `movab …,pc`
before the copy. Host copies that range on VMS I-fetch of 0 in `0xF0000–
0x100000`. Flash `vVax V0.7.6`; expect `boot: VMS relocate … 000089D5 ->
000FFE44`.

**V0.7.15:** C9 console TU58 (IPRs 28–31) always present, never a cartridge
(RSP INIT→CONTINUE, commands END+NOC). `copy:` ash traces off (`VVAX_COPY_TRACE 0`).
Stock GENERIC still plants `ctuattach` RET — `ka750_conf` calls it before
`bufq_init`; hardware cannot skip that. Flash `vVax V0.7.15`.

**V0.7.16:** Phase 6 xot isolation. NetBSD 10 `argstr` default is `STPUTC(c)`
(no CTLESC on letters); gcc emits `cmpb $0x8C; blss`. SIMH `CC_CMP_B` sets
N from signed src<dst and never V; vVax CMPB does src−dst and sets V.
USB `xot: CMPB` / `xot: CTLESC store` on the first ash switch. Flash
`vVax V0.7.16`.
**Result:** isolated. Console is clean (`/etc/rc: xot: not found`). `'e'`/`'p'`/`'r'`
of `export` each store CTLESC `0x81` at `PC=1C166` (`R11=0x80` quotes,
`8(ap)=0x83`). CMPB `'e'` vs `$0x8C` at `1C114` is NZVC=`1011` (N=1 V=1);
SIMH would have N=0 V=0. Next insn is BLSS then still `cmpb $0x80` at `1C11D`
(so that BLSS did not skip the 0x80 arm). Later letters compare at `1C1B2`
with **BGEQ** (`next=18`): vVax N=1 does not take it; SIMH N=0 would.
Do not special-case `EXP_GLOB`. Next experiment is CMPB N = signed src<dst
(V=0), CMPB-only first.

**V0.7.17:** CMPB-only (not CMPW/CMPL): N = `(int8)src < (int8)dst`, V left 0,
C still unsigned borrow. Tracer stays on. Flash `vVax V0.7.17`; expect
`xot: CMPB` NZVC `0001` (`simhN=0`) at `1C114`. Pass = no CTLESC store /
no `/etc/rc: xot`. Fail = still `1C166` with R11=0x80.
**Result:** xot gone. CMPB `'e'`..`'t'` of `export` all at `PC=1C114`
NZVC=`0001` `simhN=0` (packed walk, not `p+=2`). No `CTLESC store`. Next
gate: `/etc/rc.subr: Number out of range: 2` then single-user
(`Enter pathname of shell`). Leave CMPW/CMPL; do not special-case sh.

**V0.7.18:** CMPW/CMPL match VARM CMP (`N ← src1 LSS src2; Z ← EQL; V ← 0;
C ← LSSU`). CMPB already did. SUB still sets V. CASE/CMPV/CMPZV/INSQUE
share `set_cmp_long`. Flash `vVax V0.7.18`; xot must stay gone. Next gate
was `/etc/rc.subr: Number out of range: 2`.
**Result:** xot still gone (packed `export` CMPB NZVC=`0001`). `/etc/rc.subr:
Number out of range: 2` is gone. `init: /bin/sh on /etc/rc terminated
abnormally` → single-user, no guest error line (likely signal; C7 D-float
next if opcode dump shows `CVTLD`/`0x6E`).

**V0.7.19:** VARM/SIMH NZVC remaining integer mismatches. CASEB/CASEW use
width-correct CMP; MOVC5 `set_cmp_word`; MOV/CLR/MOVZ/PUSH/MOVA IIZP (C
preserved; TST still `C←0`); CALLS/CALLG clear live NZVC; RET restores
saved NZVC; DIVB/DIVW set V on div0 and minint/−1. Flash `vVax V0.7.19`.

**V0.7.20:** C7 F/D execute (`vax_fpa` pack/unpack, short float literals,
arith SCB `0x34` + type p1). ACCS reads 1 (`FPA present`). EMOD/POLY stay
reserved-inst. Flash `vVax V0.7.20`. Pass = no `reserved inst 0x6E`, xot
stays gone; `/etc/rc` may reach `login:` or a later reserved (0x54/0x55/
0x74/0x75). Fail = `terminated abnormally` still, plus 0x6E.
**Result:** binary `V0.7.20` (offset 20174). dmesg `cpu0 … FPA present`.
xot stays gone (packed `export` CMPB NZVC=`0001`). No `VAX reserved inst`
and **no** `fpa: op=` lines — `/etc/rc` aborted before any F/D opcode.
Same `init: '/bin/sh' on '/etc/rc' terminated abnormally` as V0.7.18.
C7 execute is on the board; this abort is not `CVTLD`/`0x6E`. Next is the
signal that kills `sh` (still no guest error line).

**V0.7.21:** Console RX. SIMH keeps `tti_int` until RXDB is read; we acked
the 0→1 latch on vector take, so a skipped take (IPL≥20) left DONE set,
the FIFO paused, and both Telnet and USB looked dead at the single-user
prompt. Re-assert RX while `{IE AND DONE}`; harvest every 256 insns in
`step()`. TX stays edge (V0.6.11). Flash `vVax V0.7.21`. Expect `cons: rx`
when you type; RETURN at `Enter pathname of shell` should get `/bin/sh`.

**Result:** Telnet keys reached init; `/bin/sh` started (slow, no signal
abort). Single-user shell works.

**V0.7.22:** Take hot-path diagnostics out of `exec_one` / `mem_r8`. DIAG 0
drops kprobe every insn, ACV-storm USB dumps, JMP-user / high-xfer logs,
`mem_r8` copy-watch branches, and MSCP live lines. 30s `hb: ips=` stays.
Repair-user PSL/SP is unchanged (not a log). Flash when the live shell can
be dropped; compare `ips=` to V0.7.21 (~5–15k userland).
**Result:** flashed. Userland `hb: ips=` ~22k–37k (was ~5–15k). Still far
from Phase 8 (≥300 KIPS). Single-user `#` is usable over Telnet.

**Checkpoint (25 Aug 2026, still V0.7.22):** `/etc/rc` abort is **ash
SIGSEGV** while sourcing `/etc/rc.subr`, not a C7 opcode and not `-xv`.
`init` prints `single user shell terminated (b)` (`wait` status `0xb` =
signal 11). Heartbeats around the crash: ash P0 (`PC=000252A4`) then P1
(`PC=7F61A483`, ld.so/libc).

Guest splits (same `#`, no firmware change):

- Typed functions live: `f() { echo hi; }`, `g() { x=$(echo hi); }`,
  `case "${nl}$( echo foo )${nl}"`, nested `$( … | while …; case "$(…)" )`.
- `. /dev/stdin` of that tiny function lives. `. /etc/rc.subr` SIGSEGVs
  with no `set -xv`. Nested `/bin/sh` is a separate fail: `Cannot execute
  ELF binary bin/sh` (ENOEXEC / `setinputfile`).
- `/sbin/mount` (dynamic, no args) SIGSEGVs. `/rescue/mount` lists
  `root_device on / type ffs (read-only, local)` — kernel `getvfsstat` is
  fine. Root stayed ro: `/rescue/mount -u -w /` hits `/etc/fstab` “Missing
  fields” then `mount: /:` (empty `strsignal`; generic `mount` forks
  `mount_ffs`). `/rescue/mount -t ffs -u -w /dev/ra0a /` same empty error.
  Do not treat remount as the next ISA hunt until `/rescue/mount_ffs -o
  update,rw /dev/ra0a /` is tried.
- `PATH` at this prompt lacks `/sbin` (`mount: not found`).

Prefix bisect of `rc.subr` via `sed … | . /dev/stdin` was **not** run
(`1,148` / `1,209` / `1,281` / `1,589` / `1,917`). That is the next guest
step; `/tmp` is not required.

**C10** (plan only, [`VAX11750.md`](VAX11750.md)): conversational VMS on
this 750 branch via `[system] os=vms` + user-supplied pack; xxboot only
(no VMB / no `0xF20400` ROM). Last VMS firmware work is V0.7.6. Independent
of the NetBSD `rc.subr` bisect; do not swap disks unless you mean to leave C8.

**V0.7.23:** C10 after the V0.7.6 SYSBOOT memcpy plant: set MOVC3 leftovers
(`R0=0`, `R1=src+n`, `R2=0`, `R3=dst+n`) so SYSBOOT does not see the
pre-copy source/len. Bit-field `vpos`/`vsize`/`vspan` take SCB reserved-
operand (`0x18`) instead of host `fault 3`. Flash `vVax V0.7.23`; expect
`boot: VMS relocate … R1=0000EDC4 R3=00106233` (not `R1=000089D3`) and no
`VAX fault 3 (vpos)`. Next is `SYSBOOT>` or a later ISA/MCHK line.

Not C7: still no `fpa: op=` / `CVTLD` on this path. Do not re-open CMPB,
BLSS=N^V, or xot. Do not hold COM18 with `reset_monitor.py` while Telnet
is the console.

**V0.7.24:** Phase 7 host UX. Persistent TFT status band (title, run/halt,
IP, TEL/FTP/DSK pills, KIPS). Double-tap or tap the band for settings
(drives, INI variants, brightness, guest restart); guest paused while
open. Telnet `Esc >` registers `host_lib/shell_*`: `ls`/`cd`/`pwd`,
`drives`/`mount`/`dismount`, `set`, `reset`/`reboot`, `halt`/`cont`/`regs`.
USB LOG also copies to Telnet diag when the guest console is attached.
SNTP UTC → TODR/TOY when the guest has not `mtpr`'d TODR. Flash
`vVax V0.7.24`.

**V0.7.27:** Phase 7 on the board: DU grey/green/yellow, TEL grey/yellow/green,
15 ms FT6336U task (double-tap reliable). Pushed `b300ceb`.

**V0.7.28:** PROBER/PROBEW walk PTEs (TNV still accessible; ACV/LNV sets Z).
INDEX range is reserved-operand, not a host halt. Log the first 12 user
ACV/LNV (not TNV) for the ash SIGSEGV on `/etc/rc.subr`. Flash `vVax V0.7.28`.

**V0.7.29–V0.7.34 (C8 closed, 29 Aug 2026):** Host tools to capture the
`/etc/rc` abort, then UVM proof it is not an ISA miss.

- V0.7.29: reload `/vaxconfig.ini` on guest restart; quiet WiFi after STA
  timeout (this site has no `dg17`).
- V0.7.30–31: arm user-mmgt logs on P0 ash PC; 12-slot ACV/LNV ring + 30 s
  heartbeat dump.
- V0.7.32–33: USB one-shot host shell `~>>cmd` or `` `>>cmd `` (start of
  line); `dq` dumps a 128-line `LOG` ring + live mmgt. Output teed to USB.
- V0.7.34: stamp `P0LR`/`P1LR` on each ring record; keep LNV / `VA<0x1000`
  / prot ACV (zero-PTE demand page dropped).

**C8 finding (not an emulator opcode bug):** GENERIC reaches `root on ra0a`,
userland, and single-user `#`. `init: '/bin/sh' on '/etc/rc' terminated
abnormally` is ash **SIGSEGV** (`wait` `0xb`) while sourcing `/etc/rc.subr`.
Stamped faults: `ACV LNV VA=7F400000 wr=1` with `P1LR=001FA800` (P1 floor
`0x7F500000`) then `P1LR=001FA000` (1 MB downward heap grow). Same grow
again before init’s message. `PC=00022437` NULL read is earlier and not
the last fault.

Guest UVM on the `#` prompt after abort (`vmstat -s`): **4096-byte software
pages**, **801 pages managed (~3.2 MB)** — same as dmesg `avail memory =
3156 KB`. **31 free vs 32 minimum free**. **0 swap devices / 0 swap pages.**
`swapctl -l`: `no swap device configured`. `dumps on ra0b` is the dump
slice only; `swapctl -A` runs later in `/etc/rc`, after `rc.subr`. A 1 MB
grow is 256 UVM pages; it cannot succeed. First grow sometimes lands;
the second hits the reserve and SIGSEGVs.

ESP32 PSRAM cannot host more than **8 MB** guest RAM (already the INI max).
Editing `/etc/rc` / `rc.subr` is out of scope. Multiuser NetBSD 10.1 on
this 8 MB KA750 image is **not practical**. C8 stops here: single-user `#`
is the success line; `/etc/rc` is a guest memory/swap-order limit, not a
vVax ISA gate. Do not re-open CMPB/xot/C7 for this abort.

## Phase 7 — Host UX parity (vpdp1170)

Mirror the common Freenove emulator host stack from **vpdp1170** (same `host_lib` family). Guest console Telnet/FTP/WiFi/INI already work; this phase fills the operator UX gaps.

| Feature | Goal | vpdp1170 reference | vVax today |
|---------|------|--------------------|------------|
| **Status line** | Persistent TFT band: title, IP, TEL/FTP pills, MSCP activity, KIPS/halt | `draw_status_bar`, `VPDP_STATUS_BAND_H` | **V0.7.24** band at y=200 |
| **Touch GUI** | Double-tap settings: drives, WiFi/config pickers, brightness, restart/reset; pause guest while open | `ui.cpp`, `touch.cpp` | **V0.7.24** double-tap or tap band |
| **Telnet shell** | `Esc` then `>>` host shell: FS, `drives`/`mount`/`dismount`, `set`, `reboot`/`reset`, VAX `halt`/`continue`/`regs`; `exit` → guest | `telnet_shell` + `host_lib/shell_*` | **V0.7.24** `host_lib/shell_*` registered |
| **Guest Telnet** | Keep port 23 guest console (TFT+USB+Telnet) | `telnet_pipe` | Done |
| **FTP** | Keep SD FTP + storage guard vs mounted MSCP | `ftp` + `storage_guard` | Done |
| **boot_script** | Optional expect/reply after reboot (vpdp `boot_script`) | `host_lib/boot/boot_script.*` | Still `boot_input` only |
| **host_diag** | Diag stream to USB + Telnet (not only Serial) | `host_diag.*` | **V0.7.24** LOG → Telnet diag FIFO |
| **TOY / TODR / NTP** | KA630 chip @ `0x200B8000` with VRT valid; SNTP → guest wall clock; GUI/shell set/show time; optional NV persist | `host_time`, `vax_clock` | **V0.7.24** SNTP → TODR/TOY; no NV persist |

Implementation notes:

- Prefer registering existing `host_lib/shell_*` over rewriting `telnet_shell.cpp`
- Status band geometry must match Freenove VT100 layout (`host_lib/console` + board header)
- MSCP activity hooks: pulse status pills from `vax_mscp` ring/xfer (same idea as PDP RP/RL LEDs)
- Phase 7 replaces the interim TOY default with live NTP (and optional SD-backed time)
- Do not block Phase 6 NetBSD bring-up; land UX in parallel once kernel handoff is stable enough to need operator controls

## Phase 8 — Interpreter throughput

Baseline from V0.6.46 `hb ips=` (wall-clock, not guest ticks): **~50–100 KIPS**.
`/boot` loadfile sits near **100 KIPS**; kernel IPL 31 copy/page-zero **~70–80**; autoconf/uba/sysctl **~45–50**. A real MicroVAX II is ~0.9 VUP (~1 MIPS-class). At 50–100 KIPS, `boot netbsd` through mountroot is **~15 minutes** of host time.

Start after Phase 6 can reach multiuser without a fault storm. Do not trade ISA correctness for speed.

| Area | Why it is slow today | Approach |
|------|----------------------|----------|
| **MMU hot path** | Every `mem_r8`/`mem_w8` walks a PTE | Per-page translate cache (VA page → PA + prot); invalidate on MTPR MAPEN/P0/P1/SBR and PTE stores |
| **`exec_one` tax** | `repair_user_exec_state`, `instr_tick`, `service_interrupts` every insn | Run those once per `step()` batch (or every N insns); keep abort/IRQ checks cheap |
| **I-fetch** | Byte-at-a-time opcode + specifier fetch | Prefetch a small window; still honor ACV/TNV on the faulting PC |
| **Host yield** | `loop()` does `step(1000)` then `delay(1)` | Drop `delay(1)` while guest running; yield only for WiFi/Telnet/SD; raise batch size |
| **Diag I/O** | USB `Serial.printf` on MSCP rings / ACV / kprobe | Default dumps off after Phase 6; never log on the 100 kHz path |
| **IRAM / PSRAM** | Interpreter + guest RAM in PSRAM | Pin `exec_one` / `translate` / `mem_r*` in IRAM; keep RAM in PSRAM |
| **String ops** | MOVC3/MOVC5 / kernel `memset` loops | Fast-path aligned copies when both sides are already translated |

**Goals (hb `ips=`, 5 s window, diag dumps off):**

1. **3×** vs today: **≥300 KIPS** on `/boot` loadfile and kernel page-zero
2. **≥200 KIPS** during uba/MSCP autoconf (IRQ + DMA still correct)
3. Stretch: **≥500 KIPS** idle/kernel so mountroot is a few minutes, not a quarter hour

Measure with `uptime=` + `ips=` only. Do not re-enable clock warp as a substitute for KIPS.

## Phase 9 — Ethernet NAT (secondary)

- DELQA/DEQNA CSR model
- TX → `eth_nat::on_guest_tx`; RX ← `eth_nat::pop_rx`
- INI keys already match vpdp1170 (`enabled`, `mac`, `guest_ip`, …)

## Legal reminders

- No VMS hobbyist kits in-tree
- User-supplied firmware under `vVaxSdCard/firmware/`
- Prefer NetBSD open media
