#include "vax_cpu.h"
#include "config.h"
#include "vax_mmu.h"
#include "vax_console.h"
#include "vax_clock.h"
#include "vax_mscp.h"
#include "vax_boot.h"
#include "platform.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

namespace vax_cpu {

static uint8_t* g_ram = nullptr;
static size_t   g_ram_bytes = 0;
static State    g_st;
static bool     g_running = false;
static bool     g_logged_reloc = false;
static uint32_t g_instr_count = 0;
static uint32_t g_trace_left = 0;
static bool     g_logged_stop = false;
static uint32_t g_last_op_pc = 0;
static bool     g_mmgt_abort = false;  // instruction aborted into ACV/TNV
static uint32_t g_mmgt_log_left = 8;
static int      g_xlat_mode_ov = -1;  // CHMx dest-mode writes (SIMH acc = ACC_MASK(mode))
static uint32_t cpu_cur_mode() {
  if (g_xlat_mode_ov >= 0) return (uint32_t)g_xlat_mode_ov;
  return (g_st.psl >> 24) & 3u;
}
static bool     g_boot_elf_active = false;
static uint32_t g_irq_log_left = 8;
static bool     g_logged_s0_pc = false;
static bool     g_logged_user_rei = false;
static bool     g_logged_user_calls = false;
static bool     g_logged_user_jmp = false;
static uint32_t g_user_jmp_log_left = 8;
static bool     g_logged_user_as_kern = false;
static uint32_t g_p1_rei_log_left = 6;
static uint32_t g_repair_log_left = 8;
static uint32_t g_rei_bad_log_left = 8;
static uint32_t g_popr_log_left = 4;
static uint32_t g_xtransl_log_left = 8;  // Xtransl_v RET/POPR sandwich
static uint32_t g_acv_storm_pc = 0;
static uint32_t g_acv_storm_va = 0;
static uint16_t g_acv_storm_count = 0;
static bool     g_acv_storm_dumped = false;
static uint32_t g_chmk_log_left = 4;
static bool     g_in_ie = false;  // nested mmgt while building a frame
static bool     g_logged_wild_jsb = false;
static uint32_t g_kernel_load_end = 0;  // highest S0 VA written by /boot MOVC3
static bool     g_logged_mapen = false;
static uint32_t g_sisr = 0;           // IPR 21 — software interrupt summary (bits 1–15)
static uint32_t g_sirr_log_left = 4;
static bool     g_planted_kernel_csr = false;
#if VAX_CLOCK_WARP_DEFAULT
static uint32_t g_idle_warp_ctr = 0;
static bool     g_logged_idle_warp = false;
static uint32_t g_warp_ms = 0;
static uint32_t g_warp_in_ms = 0;
static uint32_t g_warp_holdoff_ms = 0;
#endif
static uint32_t g_warp_fires = 0;
static bool     g_rootopen_trace = false;
static uint32_t g_root_hb_ms = 0;
static uint32_t g_hb_last_ms = 0;
static uint32_t g_hb_last_instr = 0;
static uint64_t g_hb_elapsed_ms = 0;
static bool     g_hb_hold = false;  // after REI dump: stop USB hb so the log can be copied
static uint8_t  g_mscp_irq_logs = 12;
static uint8_t  g_mscp_blocked_logs = 8;
static bool     g_reset_probes = false;
// One-shot USB dump of kernel copyin/copyout into user P0/P1 (sh vs cat).
static uint8_t  g_user_copy_logs = 8;
static uint8_t  g_user_movb_logs = 16;
static uint32_t g_watch_va = 0;
static uint32_t g_watch_n = 0;
static unsigned g_watch_dst0_even = 0;
static uint8_t  g_watch_hb_left = 0;
static uint8_t  g_watch_rd_head = 0;
static uint8_t  g_watch_rd_tok = 0;
static uint8_t  g_watch_from = 0;
static uint32_t g_watch_export_off = 0;
static uint8_t  g_tok_wr = 0;
static uint32_t g_tok_last_va = 0;
static uint8_t  g_tok_bss_left = 2;
static uint32_t g_name_va = 0;
static uint8_t  g_name_n = 0;
static uint8_t  g_name_rd = 0;
static uint32_t g_name_last_va = 0;
static uint8_t  g_name_skip = 0;  // bit0=128xx strlen, bit1=247xx
static uint8_t  g_arg_wr = 0;
static uint32_t g_arg_last_va = 0;
static uint8_t  g_name_movc = 0;
static uint8_t  g_tx_log = 0;
static uint8_t  g_name_libc = 0;
static uint8_t  g_insn_dump = 0;
static uint8_t  g_case_log = 0;

static bool     pa_ok(uint32_t pa, size_t n);
static uint32_t phys_r32(uint32_t pa);
static void     phys_w32(uint32_t pa, uint32_t v);
static void     raise_mmgt(uint32_t va, bool write);
static void     raise_mchk(uint32_t pa, bool write);
static void     raise_exception(uint32_t scb);
static void     trace_user_movb(uint32_t va, uint8_t v);
static void     trace_user_movc(const char* tag, uint32_t src, uint32_t dst,
                                uint32_t n);
static void     dump_watch_buf();
static void     dump_name_buf();
static void     dump_tok_bss();
static void     dump_insn8(uint32_t pc);
static void     dump_arg16(uint32_t va);
static void     dump_code_win(uint32_t pc, uint32_t n);
static void     peek16(uint32_t va, uint32_t n, uint8_t out[16]);
static uint32_t peek_va32(uint32_t va);
static void     trace_watch_read(uint32_t va, uint8_t v);
static void     trace_tok_wr(uint32_t va, uint8_t v);
static void     trace_name_read(uint32_t va, uint8_t v);
static void     trace_arg_wr(uint32_t va, uint8_t v);

static void note_fault(uint32_t code, const char* why, uint32_t va = 0) {
  bool first = (g_st.fault == 0);
  g_st.fault = code;
  if (first) {
    LOGE("VAX fault %u (%s) PC=%08X opPC=%08X SP=%08X VA=%08X",
         (unsigned)code, why ? why : "?",
         (unsigned)g_st.r[R_PC], (unsigned)g_last_op_pc,
         (unsigned)g_st.r[R_SP], (unsigned)va);
    LOGE("  R0=%08X R1=%08X R2=%08X R3=%08X R4=%08X R5=%08X",
         (unsigned)g_st.r[0], (unsigned)g_st.r[1], (unsigned)g_st.r[2],
         (unsigned)g_st.r[3], (unsigned)g_st.r[4], (unsigned)g_st.r[5]);
    LOGE("  R6=%08X AP=%08X FP=%08X MAPEN=%u",
         (unsigned)g_st.r[6], (unsigned)g_st.r[R_AP], (unsigned)g_st.r[R_FP],
         vax_mmu::mapen() ? 1u : 0u);
    if (vax_mmu::mapen()) {
      uint32_t sbr = vax_mmu::get_ipr(vax_mmu::IPR_SBR);
      uint32_t slr = vax_mmu::get_ipr(vax_mmu::IPR_SLR);
      LOGE("  SBR=%08X SLR=%08X P0LR=%08X P1LR=%08X P0BR=%08X P1BR=%08X",
           (unsigned)sbr, (unsigned)slr,
           (unsigned)vax_mmu::get_ipr(vax_mmu::IPR_P0LR),
           (unsigned)vax_mmu::get_ipr(vax_mmu::IPR_P1LR),
           (unsigned)vax_mmu::get_ipr(vax_mmu::IPR_P0BR),
           (unsigned)vax_mmu::get_ipr(vax_mmu::IPR_P1BR));
      if (va >= 0x80000000u && va < 0xC0000000u) {
        uint32_t vpn = (va - 0x80000000u) >> 9;
        uint32_t pte_pa = sbr + vpn * 4u;
        uint32_t pte = pa_ok(pte_pa, 4) ? phys_r32(pte_pa) : 0;
        LOGE("  S0 vpn=%u pte@%08X=%08X", (unsigned)vpn, (unsigned)pte_pa,
             (unsigned)pte);
      }
    }
    uint32_t dump_pc = g_last_op_pc ? g_last_op_pc : g_st.r[R_PC];
    dump_pc &= 0x3FFFFFFFu;  // PAMASK — opPC is often an S0 VA
    if (g_ram && ((uint64_t)dump_pc + 8u) <= g_ram_bytes) {
      LOGE("  bytes@opPC: %02X %02X %02X %02X %02X %02X %02X %02X",
           g_ram[dump_pc], g_ram[dump_pc + 1], g_ram[dump_pc + 2], g_ram[dump_pc + 3],
           g_ram[dump_pc + 4], g_ram[dump_pc + 5], g_ram[dump_pc + 6], g_ram[dump_pc + 7]);
    }
  }
}

// ---- memory ----

static bool pa_ok(uint32_t pa, size_t n) {
  return g_ram && ((uint64_t)pa + n) <= g_ram_bytes;
}

// KA630 local I/O: System Identification Extension (findcpu for UV2).
// High byte 0x01 → vax_boardtype = 0x08000001 (VAX_BTYP_630).
static constexpr uint32_t KA630_SIE_PA  = 0x20040004u;
static constexpr uint32_t KA630_SIE_VAL = 0x01000000u;

// KA630 Q22: 8K I/O page at 0x20000000, map regs at 0x20088000 (8192 × 4),
// 4 MiB DMA window at 0x30000000. Do not fold 0x30000000 into the I/O page —
// NetBSD qba_attach badaddr()s that window with maps invalid and must MCHK.
static constexpr uint32_t Q22_LOCAL_BASE = 0x20000000u;
static constexpr uint32_t Q22_LOCAL_END  = 0x30000000u;
static constexpr uint32_t Q22_MAP_BASE   = 0x20088000u;
static constexpr uint32_t Q22_MAP_COUNT  = 8192u;
static constexpr uint32_t Q22_MAP_BYTES  = Q22_MAP_COUNT * 4u;
static constexpr uint32_t Q22_MEM_BASE   = 0x30000000u;
static constexpr uint32_t Q22_MEM_SIZE   = 0x400000u;  // 8192 × 512
static constexpr uint32_t Q22_MAP_V      = 0x80000000u;
static constexpr uint32_t Q22_MAP_PFN    = 0x001FFFFFu;  // NetBSD PG_PFNUM

static uint32_t* g_q22map = nullptr;
static uint32_t  g_mchk_log_left = 2;
static bool      g_logged_q22_map = false;
static bool      g_logged_q22_dma = false;

static bool q22_io_space(uint32_t phys) {
  return phys >= 0x20000000u && phys < 0x40000000u;
}

static uint32_t q22_normalize(uint32_t phys) {
  // Fold aliases of local register space only (0x20xxxxxx). The Q22
  // memory window (0x30xxxxxx) is a distinct decode.
  if (phys >= Q22_LOCAL_BASE && phys < Q22_LOCAL_END)
    return Q22_LOCAL_BASE | (phys & 0x003FFFFFu);
  return phys;
}

static bool q22_map_hit(uint32_t phys) {
  return phys >= Q22_MAP_BASE && phys < Q22_MAP_BASE + Q22_MAP_BYTES;
}

static uint8_t q22_map_r8(uint32_t phys) {
  if (!g_q22map) return 0;
  uint32_t off = phys - Q22_MAP_BASE;
  uint32_t e = g_q22map[off >> 2];
  return (uint8_t)(e >> ((off & 3u) * 8u));
}

static void q22_map_w8(uint32_t phys, uint8_t v) {
  if (!g_q22map) return;
  uint32_t off = phys - Q22_MAP_BASE;
  uint32_t i = off >> 2;
  uint32_t sh = (off & 3u) * 8u;
  g_q22map[i] = (g_q22map[i] & ~(0xFFu << sh)) | ((uint32_t)v << sh);
  if (!g_logged_q22_map && (g_q22map[i] & Q22_MAP_V)) {
    g_logged_q22_map = true;
    LOG("Q22 map[%u]=0x%08X (pfn=%u pa=%08X)",
        (unsigned)i, (unsigned)g_q22map[i],
        (unsigned)(g_q22map[i] & Q22_MAP_PFN),
        (unsigned)((g_q22map[i] & Q22_MAP_PFN) << 9));
  }
}

static bool q22_mem_hit(uint32_t phys) {
  return phys >= Q22_MEM_BASE && phys < Q22_MEM_BASE + Q22_MEM_SIZE;
}

static bool q22_window_to_host(uint32_t phys, uint32_t* host) {
  if (!g_q22map) return false;
  uint32_t off = phys - Q22_MEM_BASE;
  uint32_t e = g_q22map[off >> 9];
  if (!(e & Q22_MAP_V)) return false;
  *host = ((e & Q22_MAP_PFN) << 9) | (off & 0x1FFu);
  return true;
}

// MSCP DMA / rings: follow Q22 maps when valid; else identity (/boot).
static uint32_t mscp_ba_to_host(uint32_t ba) {
  if (g_q22map) {
    uint32_t q22 = ba & 0x003FFFFFu;
    uint32_t e = g_q22map[q22 >> 9];
    if (e & Q22_MAP_V) {
      uint32_t host = ((e & Q22_MAP_PFN) << 9) | (q22 & 0x1FFu);
      if (!g_logged_q22_dma) {
        g_logged_q22_dma = true;
        LOG("Q22 DMA map ba=%08X -> pa=%08X", (unsigned)ba, (unsigned)host);
      }
      return host;
    }
  }
  return ba & 0x3FFFFFFFu;
}

// User (sh P0 text) byte reads from the watched /etc/rc buffer. Sequential
// off=0,1,2 = packed parse; 0,2,4 = even skip → xot from export.
static void trace_watch_read(uint32_t va, uint8_t v) {
  if (g_watch_n == 0 || va < g_watch_va || va >= g_watch_va + g_watch_n)
    return;
  if (cpu_cur_mode() != 3u) return;
  if (g_last_op_pc >= 0x00400000u) return;
  const uint32_t off = va - g_watch_va;
  if (off < 24u) {
    if (g_watch_rd_head == 0) return;
    g_watch_rd_head--;
  } else if (off >= 340u && off < 380u) {
    if (g_watch_rd_tok == 0) return;
    g_watch_rd_tok--;
  } else {
    return;
  }
  LOG("copy: rd off=%u VA=%08X b=%02X PC=%08X",
      (unsigned)off, (unsigned)va, (unsigned)v, (unsigned)g_last_op_pc);
  if (off >= 340u && g_tok_wr == 0 && g_name_va == 0)
    g_tok_wr = 48;
}

static void dump_tok_bss() {
  if (g_tok_bss_left == 0 || g_watch_n == 0) return;
  g_tok_bss_left--;
  const uint32_t va = g_watch_va + g_watch_n;
  uint8_t b[16];
  peek16(va, 16, b);
  LOG("copy:  tokbss VA=%08X %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
      (unsigned)va, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
      b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  peek16(va + 16u, 16, b);
  LOG("copy:  tokbss +16 %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
      b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

// Ash USTPUTC: printable byte stores from sh text. V0.6.65 spent the budget on
// CHECKSTRSPACE NULs at a fixed BSS word and skipped dests inside the /etc/rc
// watch (in-place token). Ignore zeros; log stride + regs.
static void trace_tok_wr(uint32_t va, uint8_t v) {
  if (g_tok_wr == 0 || g_watch_n == 0) return;
  if (cpu_cur_mode() != 3u) return;
  if (g_last_op_pc < 0x1000u || g_last_op_pc >= 0x00400000u) return;
  if (v < 0x20u || v > 0x7Eu) return;
  if (g_tok_bss_left == 2)
    dump_tok_bss();
  const uint32_t prev_va = g_tok_last_va;
  const int32_t stride = (prev_va == 0) ? 0 : (int32_t)(va - prev_va);
  g_tok_last_va = va;
  g_tok_wr--;
  LOG("copy: wr tok VA=%08X b=%02X '%c' d=%d PC=%08X R0=%08X R1=%08X R6=%08X",
      (unsigned)va, (unsigned)v, (char)v, (int)stride,
      (unsigned)g_last_op_pc, (unsigned)g_st.r[0], (unsigned)g_st.r[1],
      (unsigned)g_st.r[6]);
  if (g_tok_wr == 24)
    dump_tok_bss();
  // V0.6.67 missed this: 25E64 writes 0x65 to 0x4834C between dest 'e' and 'x'.
  // Arm on the strcpy dest at 25E3F ('e' into P0 ~0x484xx).
  if (g_name_va == 0 && v == 0x65u &&
      g_last_op_pc == 0x00025E3Fu &&
      va >= 0x00048000u && va < 0x00049000u) {
    g_name_va = va;
    g_name_n = 8;   // "export\0" only; skip the following struct words
    g_name_rd = 64;
    g_name_last_va = 0;
    g_name_skip = 0;
    g_arg_wr = 40;
    g_arg_last_va = 0;
    g_name_movc = 8;
    g_tx_log = 48;
    g_name_libc = 0;
    g_insn_dump = 0;
    g_case_log = 12;
    g_tok_wr = 0;
    LOG("copy:  name arm VA=%08X PC=%08X",
        (unsigned)g_name_va, (unsigned)g_last_op_pc);
  }
}

// V0.6.68 spent the name-read budget on sh strlen/equal packed walks
// (128C7/128E1, 247DF/247F6). Log one NUL-terminated pass of each, then
// ignore those PCs so libc %s / later copies can show (incl. 7Fxxxxxx).
static bool name_read_skip_repeat(uint32_t pc, uint8_t v) {
  if (pc == 0x000128C7u || pc == 0x000128E1u) {
    if (g_name_skip & 1u) return true;
    if (v == 0) g_name_skip |= 1u;
    return false;
  }
  if (pc == 0x000247DFu || pc == 0x000247F6u) {
    if (g_name_skip & 2u) return true;
    if (v == 0) g_name_skip |= 2u;
    return false;
  }
  return false;
}

static void trace_name_read(uint32_t va, uint8_t v) {
  if (g_name_rd == 0 || g_name_n == 0) return;
  if (va < g_name_va || va >= g_name_va + g_name_n) return;
  if (cpu_cur_mode() != 3u) return;
  const uint32_t pc = g_last_op_pc;
  if (name_read_skip_repeat(pc, v)) return;
  const uint32_t off = va - g_name_va;
  const int32_t stride = (g_name_last_va == 0) ? 0
      : (int32_t)(va - g_name_last_va);
  g_name_last_va = va;
  g_name_rd--;
  const char ch = (v >= 0x20u && v <= 0x7Eu) ? (char)v : '.';
  LOG("copy: rd name off=%u VA=%08X b=%02X '%c' d=%d PC=%08X R0=%08X R1=%08X R2=%08X R6=%08X R9=%08X R10=%08X",
      (unsigned)off, (unsigned)va, (unsigned)v, ch, (int)stride,
      (unsigned)pc, (unsigned)g_st.r[0], (unsigned)g_st.r[1],
      (unsigned)g_st.r[2], (unsigned)g_st.r[6], (unsigned)g_st.r[9],
      (unsigned)g_st.r[10]);
  if (g_name_rd == 63)
    dump_name_buf();
  uint8_t bit = 0;
  if (pc == 0x0001C220u) bit = 1u;
  else if (pc == 0x0001C16Au) bit = 2u;
  else if (pc == 0x0001C19Cu) bit = 4u;
  else if (pc == 0x0001C1A7u) bit = 8u;
  else if (pc == 0x0001AB61u) bit = 16u;
  if (bit != 0 && (g_insn_dump & bit) == 0) {
    g_insn_dump |= bit;
    dump_insn8(pc);
    if (pc == 0x0001C16Au) {
      dump_code_win(0x0001C16Au, 80);
      dump_insn8(0x0001AB61u);
      dump_arg16(g_st.r[0]);
    }
  }
  // First libc/ld.so (P1 text) read of the packed name — dump again.
  if (g_name_libc == 0 && (pc & 0xC0000000u) == 0x40000000u) {
    g_name_libc = 1;
    dump_name_buf();
  }
}

static void dump_insn8(uint32_t pc) {
  uint8_t b[16];
  peek16(pc, 8, b);
  LOG("copy:  insn PC=%08X %02X %02X %02X %02X %02X %02X %02X %02X",
      (unsigned)pc, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
}

static void dump_code_win(uint32_t pc, uint32_t n) {
  uint8_t b[16];
  for (uint32_t off = 0; off < n; off += 16u) {
    peek16(pc + off, 16, b);
    LOG("copy:  code PC=%08X +%u %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
        (unsigned)pc, (unsigned)off,
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  }
}

static void dump_arg16(uint32_t va) {
  uint8_t b[16];
  const uint32_t base = va & ~0xFu;
  peek16(base, 16, b);
  LOG("copy:  arg16 VA=%08X %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
      (unsigned)base, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
      b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

// Copies of the command name after the packed strcpy (argv / hash node).
// V0.6.69: 1C19C stores odd letters (x,o,t) at dest+1,+3,+5; 1AB61 packs
// them to xot. Log CTLESC (0x81) / NULs and the store instruction bytes.
static void trace_arg_wr(uint32_t va, uint8_t v) {
  if (g_arg_wr == 0 || g_name_va == 0) return;
  if (cpu_cur_mode() != 3u) return;
  if (va < g_name_va + 8u || va >= g_name_va + 0x400u) return;
  // Skip libc PATH strcat (7Fxxxxxx); keep sh 1C19C / 1AB61.
  if ((g_last_op_pc & 0xC0000000u) == 0x40000000u) return;
  const uint32_t pc = g_last_op_pc;
  // V0.6.70 burned the budget on CLRL zeros at 0x4868x before 1C19C.
  if (v == 0 && pc != 0x0001C19Cu && pc != 0x0001AB61u) return;
  if (v != 0 && v != 0x81u && (v < 0x20u || v > 0x7Eu)) return;
  const uint32_t prev = g_arg_last_va;
  const int32_t stride = (prev == 0) ? 0 : (int32_t)(va - prev);
  g_arg_last_va = va;
  g_arg_wr--;
  const char ch = (v >= 0x20u && v <= 0x7Eu) ? (char)v
      : (v == 0x81u) ? 'E' : '.';
  LOG("copy: wr arg VA=%08X b=%02X '%c' d=%d PC=%08X R0=%08X R1=%08X R2=%08X R6=%08X R9=%08X R10=%08X",
      (unsigned)va, (unsigned)v, ch, (int)stride,
      (unsigned)pc, (unsigned)g_st.r[0], (unsigned)g_st.r[1],
      (unsigned)g_st.r[2], (unsigned)g_st.r[6], (unsigned)g_st.r[9],
      (unsigned)g_st.r[10]);
  uint8_t bit = 0;
  if (pc == 0x0001C220u) bit = 1u;
  else if (pc == 0x0001C16Au) bit = 2u;
  else if (pc == 0x0001C19Cu) bit = 4u;
  else if (pc == 0x0001C1A7u) bit = 8u;
  else if (pc == 0x0001AB61u) bit = 16u;
  else if (pc == 0x0001C166u) bit = 32u;
  if (bit != 0 && (g_insn_dump & bit) == 0) {
    g_insn_dump |= bit;
    dump_insn8(pc);
    if (pc == 0x0001C166u) {
      // V0.6.72: letters reach this CTLESC store after cvtbl+'e'+0x7E → R2=0xE3
      // (unsigned > 9) then tstl r11. Dump r11 + CALLS frame + prologue.
      LOG("copy:  gate R0=%08X R1=%08X R2=%08X R6=%08X R7=%08X R8=%08X",
          (unsigned)g_st.r[0], (unsigned)g_st.r[1], (unsigned)g_st.r[2],
          (unsigned)g_st.r[6], (unsigned)g_st.r[7], (unsigned)g_st.r[8]);
      LOG("copy:  gate R9=%08X R10=%08X R11=%08X AP=%08X FP=%08X SP=%08X PSL=%08X",
          (unsigned)g_st.r[9], (unsigned)g_st.r[10], (unsigned)g_st.r[11],
          (unsigned)g_st.r[R_AP], (unsigned)g_st.r[R_FP],
          (unsigned)g_st.r[R_SP], (unsigned)g_st.psl);
      const uint32_t fp = g_st.r[R_FP];
      const uint32_t ap = g_st.r[R_AP];
      const uint32_t wd = peek_va32(fp + 4u);
      LOG("copy:  frame wd=%08X mask=%03X calls=%u spa=%u savpc=%08X 4(ap)=%08X 8(ap)=%08X",
          (unsigned)wd, (unsigned)((wd >> 16) & 0xFFFu),
          (unsigned)((wd >> 29) & 1u), (unsigned)(wd >> 30),
          (unsigned)peek_va32(fp + 16u),
          (unsigned)peek_va32(ap + 4u), (unsigned)peek_va32(ap + 8u));
      dump_code_win(0x0001C040u, 160);
      dump_code_win(0x0001C0E0u, 208);
      dump_code_win(0x0001C1B0u, 96);
      dump_code_win(0x0001C220u, 160);
    }
  }
  if (pc == 0x0001C19Cu || pc == 0x0001AB61u)
    dump_arg16(va);
}

static uint8_t mem_r8(uint32_t pa) {
  if (g_mmgt_abort) return 0;
  uint32_t phys = pa;
  if (!vax_mmu::translate(pa, &phys, false, cpu_cur_mode())) {
    raise_mmgt(pa, false);
    return 0;
  }
  phys = q22_normalize(phys);
  if (vax_mscp::csr_hit(phys)) {
    uint16_t w = vax_mscp::csr_read(phys & ~1u);
    return (phys & 1) ? (uint8_t)(w >> 8) : (uint8_t)w;
  }
  if (phys >= KA630_SIE_PA && phys < KA630_SIE_PA + 4u) {
    unsigned sh = (unsigned)(phys - KA630_SIE_PA) * 8u;
    return (uint8_t)(KA630_SIE_VAL >> sh);
  }
  if (vax_boot::nvr_hit(phys))
    return vax_boot::nvr_read8(phys);
  if (vax_clock::toy_hit(phys))
    return vax_clock::toy_read8(phys);
  if (q22_map_hit(phys))
    return q22_map_r8(phys);
  if (q22_mem_hit(phys)) {
    uint32_t host = 0;
    if (q22_window_to_host(phys, &host) && pa_ok(host, 1))
      return g_ram[host];
    raise_mchk(phys, false);
    return 0;
  }
  if (pa_ok(phys, 1)) {
    uint8_t v = g_ram[phys];
    if ((g_watch_rd_head | g_watch_rd_tok) != 0)
      trace_watch_read(pa, v);
    if (g_name_rd != 0)
      trace_name_read(pa, v);
    return v;
  }
  // Other Q22 / KA630 local I/O: probes must not sticky-fault (SIE, QIPCR, …).
  if (q22_io_space(phys)) return 0;
  note_fault(2, "pa-r", pa);
  return 0;
}

static uint16_t mem_r16(uint32_t pa) {
  if (g_mmgt_abort) return 0;
  uint32_t phys = pa;
  if (!vax_mmu::translate(pa, &phys, false, cpu_cur_mode())) {
    raise_mmgt(pa, false);
    return 0;
  }
  phys = q22_normalize(phys);
  if (!(phys & 1) && vax_mscp::csr_hit(phys))
    return vax_mscp::csr_read(phys);
  return (uint16_t)(mem_r8(pa) | ((uint16_t)mem_r8(pa + 1) << 8));
}

static uint32_t mem_r32(uint32_t pa) {
  return (uint32_t)mem_r8(pa) |
         ((uint32_t)mem_r8(pa + 1) << 8) |
         ((uint32_t)mem_r8(pa + 2) << 16) |
         ((uint32_t)mem_r8(pa + 3) << 24);
}

// Direct physical access for MMU PTE walks (no translate recursion).
static uint32_t phys_r32(uint32_t pa) {
  if (!pa_ok(pa, 4)) return 0;
  return (uint32_t)g_ram[pa] |
         ((uint32_t)g_ram[pa + 1] << 8) |
         ((uint32_t)g_ram[pa + 2] << 16) |
         ((uint32_t)g_ram[pa + 3] << 24);
}

static void phys_w32(uint32_t pa, uint32_t v) {
  if (!pa_ok(pa, 4)) return;
  g_ram[pa]     = (uint8_t)v;
  g_ram[pa + 1] = (uint8_t)(v >> 8);
  g_ram[pa + 2] = (uint8_t)(v >> 16);
  g_ram[pa + 3] = (uint8_t)(v >> 24);
}

// /boot RPB (CALLS first arg, e.g. 0x7B26E8). locore memcpy's it to the
// uarea; pmap_bootstrap then does `*(struct rpb *)0 = *uv` *after*
// scb_init() relocates the SCB off PA 0. Do not write KERNBASE/PA 0 here:
// that page is still the SCB template (offset 84 = cmrerr vec 0x8000036D).
static bool plant_rpb_csrphy(uint32_t va) {
  uint32_t pa = va & 0x3FFFFFFFu;
  if (!g_ram || (uint64_t)pa + 104u > g_ram_bytes) return false;
  uint8_t  devtyp  = g_ram[pa + 102];
  uint32_t old_csr = phys_r32(pa + 84);
  if (devtyp != 17u && old_csr != 0x20001C68u)
    return false;
  uint32_t old_base = phys_r32(pa);
  const uint32_t csr = 0x20001468u;
  phys_w32(pa + 84, csr);
  if (devtyp != 17u)
    g_ram[pa + 102] = 17u;
  // device_register() ignores a synthetic RPB (rpb_base == -1).
  if (old_base == 0xFFFFFFFFu)
    phys_w32(pa, 0x80000000u);
  g_planted_kernel_csr = true;
  LOG("boot: RPB va=%08X pa=%08X csrphy %08X→%08X devtyp=%u unit=%u base=%08X",
      (unsigned)va, (unsigned)pa, (unsigned)old_csr, (unsigned)csr,
      (unsigned)g_ram[pa + 102],
      (unsigned)(g_ram[pa + 100] | ((uint16_t)g_ram[pa + 101] << 8)),
      (unsigned)phys_r32(pa));
  return true;
}

// MSCP ring/packet DMA — Q22 maps when valid, else identity (/boot).
static uint8_t mscp_phys_r8(uint32_t pa) {
  uint32_t host = mscp_ba_to_host(pa);
  if (!pa_ok(host, 1)) return 0;
  return g_ram[host];
}

static void mscp_phys_w8(uint32_t pa, uint8_t v) {
  uint32_t host = mscp_ba_to_host(pa);
  if (!pa_ok(host, 1)) return;
  g_ram[host] = v;
}

static void mem_w8(uint32_t pa, uint8_t v) {
  if (g_mmgt_abort) return;
  uint32_t phys = pa;
  if (!vax_mmu::translate(pa, &phys, true, cpu_cur_mode())) {
    raise_mmgt(pa, true);
    return;
  }
  phys = q22_normalize(phys);
  if (phys == CONSOLE_TX_PA) {
    vax_console::txdb_wr(v);
    return;
  }
  // MSCP CSRs are word devices; lone byte write zeros the other half.
  if (vax_mscp::csr_hit(phys)) {
    uint16_t w = (phys & 1) ? ((uint16_t)v << 8) : (uint16_t)v;
    vax_mscp::csr_write(phys & ~1u, w);
    return;
  }
  if (vax_clock::toy_hit(phys)) {
    vax_clock::toy_write8(phys, v);
    return;
  }
  if (q22_map_hit(phys)) {
    q22_map_w8(phys, v);
    return;
  }
  if (q22_mem_hit(phys)) {
    uint32_t host = 0;
    if (q22_window_to_host(phys, &host) && pa_ok(host, 1)) {
      g_ram[host] = v;
      return;
    }
    raise_mchk(phys, true);
    return;
  }
  if (pa_ok(phys, 1)) {
    g_ram[phys] = v;
    if (g_tok_wr != 0)
      trace_tok_wr(pa, v);
    if (g_arg_wr != 0)
      trace_arg_wr(pa, v);
    return;
  }
  // Ignore writes into Q22/local I/O we do not model yet (QIPCR, …).
  if (q22_io_space(phys)) return;
  note_fault(2, "pa-w", pa);
  static bool logged_pa = false;
  if (!logged_pa) {
    logged_pa = true;
    LOGE("pa-w target VA=%08X PA=%08X", (unsigned)pa, (unsigned)phys);
  }
}

static void mem_w16(uint32_t pa, uint16_t v) {
  if (g_mmgt_abort) return;
  uint32_t phys = pa;
  if (!vax_mmu::translate(pa, &phys, true, cpu_cur_mode())) {
    raise_mmgt(pa, true);
    return;
  }
  phys = q22_normalize(phys);
  if (!(phys & 1) && vax_mscp::csr_hit(phys)) {
    vax_mscp::csr_write(phys, v);
    return;
  }
  mem_w8(pa, (uint8_t)v);
  mem_w8(pa + 1, (uint8_t)(v >> 8));
}

static void mem_w32(uint32_t pa, uint32_t v) {
  mem_w8(pa, (uint8_t)v);
  mem_w8(pa + 1, (uint8_t)(v >> 8));
  mem_w8(pa + 2, (uint8_t)(v >> 16));
  mem_w8(pa + 3, (uint8_t)(v >> 24));
}

static uint8_t fetch8() {
  if (g_mmgt_abort) return 0;
  uint32_t pc = g_st.r[R_PC];
  uint8_t v = mem_r8(pc);
  if (g_mmgt_abort) return 0;
  g_st.r[R_PC] = pc + 1;
  return v;
}

static uint16_t fetch16() {
  if (g_mmgt_abort) return 0;
  uint16_t v = mem_r16(g_st.r[R_PC]);
  if (g_mmgt_abort) return 0;
  g_st.r[R_PC] += 2;
  return v;
}

static uint32_t fetch32() {
  if (g_mmgt_abort) return 0;
  uint32_t v = mem_r32(g_st.r[R_PC]);
  if (g_mmgt_abort) return 0;
  g_st.r[R_PC] += 4;
  return v;
}

// ---- PSL / CC ----

static void set_nz_long(uint32_t v) {
  if (g_mmgt_abort) return;
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V);
  if (v == 0) g_st.psl |= PSL_Z;
  if (v & 0x80000000u) g_st.psl |= PSL_N;
}

static void set_nz_word(uint16_t v) {
  if (g_mmgt_abort) return;
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V);
  if (v == 0) g_st.psl |= PSL_Z;
  if (v & 0x8000) g_st.psl |= PSL_N;
}

static void set_nz_byte(uint8_t v) {
  if (g_mmgt_abort) return;
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V);
  if (v == 0) g_st.psl |= PSL_Z;
  if (v & 0x80) g_st.psl |= PSL_N;
}

static void set_add_cc(uint32_t a, uint32_t b, uint32_t r) {
  if (g_mmgt_abort) return;
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
  if (r == 0) g_st.psl |= PSL_Z;
  if (r & 0x80000000u) g_st.psl |= PSL_N;
  if (r < a) g_st.psl |= PSL_C;  // unsigned carry
  // signed overflow
  if (((~(a ^ b) & (a ^ r)) & 0x80000000u) != 0) g_st.psl |= PSL_V;
}

static void set_sub_cc(uint32_t a, uint32_t b, uint32_t r) {
  if (g_mmgt_abort) return;
  // CMP / SUB: a - b → r
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
  if (r == 0) g_st.psl |= PSL_Z;
  if (r & 0x80000000u) g_st.psl |= PSL_N;
  if (a < b) g_st.psl |= PSL_C;
  if ((((a ^ b) & (a ^ r)) & 0x80000000u) != 0) g_st.psl |= PSL_V;
}

// ---- operand access ----

enum Acc : uint8_t { ACC_R = 0, ACC_W = 1, ACC_M = 2, ACC_A = 3 };

struct Opnd {
  bool     ok;
  bool     is_reg;
  uint8_t  reg;
  uint32_t addr;   // effective address (or register number if is_reg)
  uint32_t value;  // for read access
};

static Opnd decode_opnd(Acc acc, int size) {
  Opnd o{};
  o.ok = false;
  if (g_mmgt_abort) return o;
  uint8_t spec = fetch8();
  uint8_t mode = (uint8_t)(spec >> 4);
  uint8_t rn   = (uint8_t)(spec & 0x0F);

  // Short literal (modes 0–3): read-only
  if (mode <= 3) {
    if (acc == ACC_W || acc == ACC_M || acc == ACC_A) {
      note_fault(3, "spec");
      return o;
    }
    o.ok = true;
    o.is_reg = false;
    o.addr = 0;
    o.value = (uint32_t)(spec & 0x3F);
    return o;
  }

  if (mode == 4) {
    // Indexed: Rn is index; next specifier is base. addr = base + Rn*size
    Opnd base = decode_opnd(ACC_A, size);
    if (!base.ok || base.is_reg) {
      note_fault(3, "spec");
      return o;
    }
    o.ok = true;
    o.is_reg = false;
    o.addr = base.addr + g_st.r[rn] * (uint32_t)size;
    if (acc != ACC_A && acc != ACC_W)
      o.value = (size == 1) ? mem_r8(o.addr) :
                (size == 2) ? mem_r16(o.addr) : mem_r32(o.addr);
    return o;
  }

  if (mode == 5) {
    // Register
    if (acc == ACC_A) {
      note_fault(3, "spec");
      return o;
    }
    o.ok = true;
    o.is_reg = true;
    o.reg = rn;
    o.addr = rn;
    if (acc != ACC_W) {
      uint32_t v = g_st.r[rn];
      if (size == 1) v &= 0xFF;
      else if (size == 2) v &= 0xFFFF;
      o.value = v;
    }
    return o;
  }

  uint32_t ea = 0;
  switch (mode) {
    case 6:  // (Rn)
      ea = g_st.r[rn];
      break;
    case 7:  // -(Rn)
      g_st.r[rn] -= (uint32_t)size;
      ea = g_st.r[rn];
      break;
    case 8:  // (Rn)+
      ea = g_st.r[rn];
      g_st.r[rn] += (uint32_t)size;
      break;
    case 9:  // @(Rn)+
      ea = mem_r32(g_st.r[rn]);
      g_st.r[rn] += 4;
      break;
    case 0xA: {  // B^D(Rn)
      int8_t d = (int8_t)fetch8();
      ea = g_st.r[rn] + (int32_t)d;
      break;
    }
    case 0xB: {  // @B^D(Rn)
      int8_t d = (int8_t)fetch8();
      ea = mem_r32(g_st.r[rn] + (int32_t)d);
      break;
    }
    case 0xC: {  // W^D(Rn)
      int16_t d = (int16_t)fetch16();
      ea = g_st.r[rn] + (int32_t)d;
      break;
    }
    case 0xD: {  // @W^D(Rn)
      int16_t d = (int16_t)fetch16();
      ea = mem_r32(g_st.r[rn] + (int32_t)d);
      break;
    }
    case 0xE: {  // L^D(Rn)
      int32_t d = (int32_t)fetch32();
      ea = g_st.r[rn] + (uint32_t)d;
      break;
    }
    case 0xF: {  // @L^D(Rn)
      int32_t d = (int32_t)fetch32();
      ea = mem_r32(g_st.r[rn] + (uint32_t)d);
      break;
    }
    default:
      note_fault(3, "spec");
      return o;
  }

  o.ok = true;
  o.is_reg = false;
  o.addr = ea;
  if (acc == ACC_A) {
    o.value = ea;
    return o;
  }
  if (acc != ACC_W) {
    o.value = (size == 1) ? mem_r8(ea) :
              (size == 2) ? mem_r16(ea) : mem_r32(ea);
  }
  return o;
}

static void store_opnd(const Opnd& o, uint32_t v, int size) {
  if (!o.ok || g_mmgt_abort) return;
  if (o.is_reg) {
    if (size == 1) {
      g_st.r[o.reg] = (g_st.r[o.reg] & ~0xFFu) | (v & 0xFF);
    } else if (size == 2) {
      g_st.r[o.reg] = (g_st.r[o.reg] & ~0xFFFFu) | (v & 0xFFFF);
    } else {
      g_st.r[o.reg] = v;
    }
    return;
  }
  if (size == 1) {
    mem_w8(o.addr, (uint8_t)v);
    if (!g_mmgt_abort)
      trace_user_movb(o.addr, (uint8_t)v);
  } else if (size == 2) mem_w16(o.addr, (uint16_t)v);
  else mem_w32(o.addr, v);
}

// Variable bit-field base (VB): register → rn + R[rn]/R[rn+1]; else byte address.
struct VField {
  bool ok = false;
  bool is_mem = false;
  uint8_t rn = 0;
  uint32_t ea = 0;
  uint32_t vfldrp1 = 0;
};

static uint32_t bitfield_mask(uint32_t size) {
  if (size >= 32) return 0xFFFFFFFFu;
  return (1u << size) - 1u;
}

static VField decode_vfield_base() {
  VField f{};
  uint8_t spec = fetch8();
  uint8_t mode = (uint8_t)(spec >> 4);
  uint8_t rn = (uint8_t)(spec & 0x0F);
  if (mode <= 3) {
    note_fault(3, "vb");
    return f;
  }
  if (mode == 5) {
    f.ok = true;
    f.is_mem = false;
    f.rn = rn;
    f.vfldrp1 = g_st.r[(rn + 1u) & 0xFu];
    return f;
  }
  uint32_t ea = 0;
  if (mode == 4) {
    Opnd base = decode_opnd(ACC_A, 1);
    if (!base.ok || base.is_reg) {
      note_fault(3, "vb");
      return f;
    }
    ea = base.addr + g_st.r[rn];
  } else {
    switch (mode) {
      case 6: ea = g_st.r[rn]; break;
      case 7: g_st.r[rn] -= 1; ea = g_st.r[rn]; break;
      case 8: ea = g_st.r[rn]; g_st.r[rn] += 1; break;
      case 9: ea = mem_r32(g_st.r[rn]); g_st.r[rn] += 4; break;
      case 0xA: ea = g_st.r[rn] + (int32_t)(int8_t)fetch8(); break;
      case 0xB: ea = mem_r32(g_st.r[rn] + (int32_t)(int8_t)fetch8()); break;
      case 0xC: ea = g_st.r[rn] + (int32_t)(int16_t)fetch16(); break;
      case 0xD: ea = mem_r32(g_st.r[rn] + (int32_t)(int16_t)fetch16()); break;
      case 0xE: ea = g_st.r[rn] + fetch32(); break;
      case 0xF: ea = mem_r32(g_st.r[rn] + fetch32()); break;
      default:
        note_fault(3, "vb");
        return f;
    }
  }
  f.ok = true;
  f.is_mem = true;
  f.ea = ea;
  return f;
}

static uint32_t vfield_extract(uint32_t pos, uint32_t size, const VField& base) {
  if (size == 0) return 0;
  if (size > 32) {
    note_fault(3, "vsize");
    return 0;
  }
  if (!base.is_mem) {
    if (pos > 31) {
      note_fault(3, "vpos");
      return 0;
    }
    uint32_t wd = g_st.r[base.rn];
    uint32_t wd1 = base.vfldrp1;
    if (pos)
      wd = (wd >> pos) | (wd1 << (32 - pos));
    return wd & bitfield_mask(size);
  }
  uint32_t ba = base.ea + (pos >> 3);
  uint32_t bpos = (pos & 7u) | ((ba & 3u) << 3);
  ba &= ~3u;
  uint32_t wd = mem_r32(ba);
  uint32_t wd1 = 0;
  if (size + bpos > 32)
    wd1 = mem_r32(ba + 4);
  if (bpos)
    wd = (wd >> bpos) | (wd1 << (32 - bpos));
  return wd & bitfield_mask(size);
}

static void vfield_insert(uint32_t ins, uint32_t pos, uint32_t size, const VField& base) {
  if (size == 0) return;
  if (size > 32) {
    note_fault(3, "vsize");
    return;
  }
  if (!base.is_mem) {
    if (pos > 31) {
      note_fault(3, "vpos");
      return;
    }
    if (pos + size > 32) {
      if (base.rn >= 14) {
        note_fault(3, "vspan");
        return;
      }
      uint32_t mask = bitfield_mask(pos + size - 32);
      uint32_t val = ins >> (32 - pos);
      uint8_t rn1 = (uint8_t)((base.rn + 1u) & 0xFu);
      g_st.r[rn1] = (base.vfldrp1 & ~mask) | (val & mask);
    }
    uint32_t mask = bitfield_mask(size) << pos;
    uint32_t val = ins << pos;
    g_st.r[base.rn] = (g_st.r[base.rn] & ~mask) | (val & mask);
    return;
  }
  uint32_t ba = base.ea + (pos >> 3);
  uint32_t bpos = (pos & 7u) | ((ba & 3u) << 3);
  ba &= ~3u;
  uint32_t wd = mem_r32(ba);
  if (size + bpos > 32) {
    uint32_t wd1 = mem_r32(ba + 4);
    uint32_t mask = bitfield_mask(bpos + size - 32);
    uint32_t val = ins >> (32 - bpos);
    mem_w32(ba + 4, (wd1 & ~mask) | (val & mask));
  }
  uint32_t mask = bitfield_mask(size) << bpos;
  uint32_t val = ins << bpos;
  mem_w32(ba, (wd & ~mask) | (val & mask));
}

static uint32_t find_first_set(uint32_t wd, uint32_t size) {
  for (uint32_t i = 0; i < size; i++, wd >>= 1) {
    if (wd & 1u) return i;
  }
  return size;
}

// Quadword (.rq / .wq): Rn=lo, R[n+1]=hi; memory is little-endian lo,hi.
struct QuadOp {
  bool ok;
  bool is_reg;
  uint8_t reg;
  uint32_t addr;
  uint32_t lo;
  uint32_t hi;
};

static QuadOp fetch_quad(Acc acc) {
  QuadOp q{};
  q.ok = false;
  uint8_t spec = fetch8();
  uint8_t mode = (uint8_t)(spec >> 4);
  uint8_t rn = (uint8_t)(spec & 0x0F);

  // Short literal (modes 0–3): read-only; value zero-extended to 64 bits.
  // Study: Open SIMH VAX/vax_cpu.c SH0|RQ … opnd=spec, opnd=0
  if (mode <= 3) {
    if (acc != ACC_R) {
      note_fault(3, "spec");
      return q;
    }
    q.ok = true;
    q.is_reg = false;
    q.addr = 0;
    q.lo = (uint32_t)(spec & 0x3F);
    q.hi = 0;
    return q;
  }
  if (mode == 5) {
    if (acc == ACC_A) {
      note_fault(3, "spec");
      return q;
    }
    q.ok = true;
    q.is_reg = true;
    q.reg = rn;
    if (acc != ACC_W) {
      q.lo = g_st.r[rn];
      q.hi = (rn < 15) ? g_st.r[rn + 1] : 0;
    }
    return q;
  }

  // Address of quad — reuse longword address calc with size 8 for modify.
  // Indexed / deferred / disp: call decode by putting specifier back — instead
  // inline the same switch as decode_opnd for ea with size 8.
  uint32_t ea = 0;
  if (mode == 4) {
    Opnd base = decode_opnd(ACC_A, 8);
    if (!base.ok || base.is_reg) {
      note_fault(3, "spec");
      return q;
    }
    ea = base.addr + g_st.r[rn] * 8u;
  } else {
    switch (mode) {
      case 6: ea = g_st.r[rn]; break;
      case 7:
        g_st.r[rn] -= 8;
        ea = g_st.r[rn];
        break;
      case 8:
        ea = g_st.r[rn];
        g_st.r[rn] += 8;
        break;
      case 9:
        ea = mem_r32(g_st.r[rn]);
        g_st.r[rn] += 4;
        break;
      case 0xA: {
        int8_t d = (int8_t)fetch8();
        ea = g_st.r[rn] + (int32_t)d;
        break;
      }
      case 0xB: {
        int8_t d = (int8_t)fetch8();
        ea = mem_r32(g_st.r[rn] + (int32_t)d);
        break;
      }
      case 0xC: {
        int16_t d = (int16_t)fetch16();
        ea = g_st.r[rn] + (int32_t)d;
        break;
      }
      case 0xD: {
        int16_t d = (int16_t)fetch16();
        ea = mem_r32(g_st.r[rn] + (int32_t)d);
        break;
      }
      case 0xE: {
        int32_t d = (int32_t)fetch32();
        ea = g_st.r[rn] + (uint32_t)d;
        break;
      }
      case 0xF: {
        int32_t d = (int32_t)fetch32();
        ea = mem_r32(g_st.r[rn] + (uint32_t)d);
        break;
      }
      default:
        note_fault(3, "spec");
        return q;
    }
  }
  q.ok = true;
  q.is_reg = false;
  q.addr = ea;
  if (acc != ACC_W) {
    q.lo = mem_r32(ea);
    q.hi = mem_r32(ea + 4);
  }
  return q;
}

static void store_quad(const QuadOp& q, uint32_t lo, uint32_t hi) {
  if (!q.ok || g_mmgt_abort) return;
  if (q.is_reg) {
    g_st.r[q.reg] = lo;
    if (q.reg < 15) g_st.r[q.reg + 1] = hi;
    return;
  }
  mem_w32(q.addr, lo);
  mem_w32(q.addr + 4, hi);
}

static void branch_b(int8_t disp) {
  g_st.r[R_PC] = (uint32_t)((int32_t)g_st.r[R_PC] + (int32_t)disp);
}

static void branch_w(int16_t disp) {
  g_st.r[R_PC] = (uint32_t)((int32_t)g_st.r[R_PC] + (int32_t)disp);
}

static constexpr uint32_t PSL_IPL_MASK = 0x001F0000u;
static constexpr uint32_t PSL_IPL_SHIFT = 16;

// Per-mode stack pointers for CHMx (K/E/S/U). SP is kept coherent with STK[cur].
static uint32_t g_stk[4] = {};
static uint32_t g_isp = 0;  // IPR 4 — interrupt stack (NetBSD mtpr istack+USPACE)

static constexpr uint32_t PSL_CUR_MASK = 0x03000000u;
static constexpr uint32_t PSL_CUR_SHIFT = 24;
static constexpr uint32_t PSL_PRV_MASK = 0x00C00000u;
static constexpr uint32_t PSL_PRV_SHIFT = 22;
static constexpr uint32_t PSL_IS = 0x04000000u;  // interrupt stack
static constexpr uint32_t PSL_CM = 0x80000000u;  // compatibility mode (UV2: reserved)
static constexpr uint32_t PSW_MBZ = 0x0000FF00u;
static constexpr uint32_t PSL_MBZ = 0x30200000u | PSW_MBZ;  // bits 29:28,21,15:8
static constexpr uint32_t SCB_CHMK = 0x40;
static constexpr uint32_t SCB_PRIV = 0x10;  // privileged / reserved instruction
static constexpr uint32_t SCB_RESOP = 0x18;  // reserved operand
static constexpr uint32_t SCB_ACV  = 0x20;  // access control / length violation
static constexpr uint32_t SCB_TNV  = 0x24;  // translation not valid (V=0, prot set)
static constexpr uint32_t SCB_MCHK = 0x04;  // machine check (badaddr / NXMEM)
static constexpr uint32_t PAMASK_LW = 0x3FFFFFFCu;

static uint32_t psl_cur() { return (g_st.psl & PSL_CUR_MASK) >> PSL_CUR_SHIFT; }
static uint32_t psl_prv() { return (g_st.psl & PSL_PRV_MASK) >> PSL_PRV_SHIFT; }
static bool     psl_is()  { return (g_st.psl & PSL_IS) != 0; }

static uint32_t psl_ipl_of(uint32_t psl) {
  return (psl & PSL_IPL_MASK) >> PSL_IPL_SHIFT;
}

// SIMH/VARM: user/esu must have IPL=0 and ~IS; IS implies kernel and IPL>0; MBZ clear.
static bool psl_is_sane(uint32_t psl) {
  if (psl & (PSL_MBZ | PSL_CM)) return false;
  const uint32_t cur = (psl & PSL_CUR_MASK) >> PSL_CUR_SHIFT;
  const uint32_t ipl = psl_ipl_of(psl);
  const bool is = (psl & PSL_IS) != 0;
  if (cur != 0 && (is || ipl != 0)) return false;
  if (is && ipl == 0) return false;
  return true;
}

// SIMH op_rei rules 1–8 (UV2: compatibility mode is reserved).
static bool rei_psl_legal(uint32_t npsl, uint32_t oldpsl) {
  const uint32_t newcur = (npsl & PSL_CUR_MASK) >> PSL_CUR_SHIFT;
  const uint32_t oldcur = (oldpsl & PSL_CUR_MASK) >> PSL_CUR_SHIFT;
  const uint32_t newprv = (npsl & PSL_PRV_MASK) >> PSL_PRV_SHIFT;
  const uint32_t newipl = psl_ipl_of(npsl);
  const uint32_t oldipl = psl_ipl_of(oldpsl);
  if (npsl & (PSL_MBZ | PSL_CM)) return false;
  if (newcur < oldcur) return false;
  if (newcur != 0) {
    if (npsl & (PSL_IS | PSL_IPL_MASK)) return false;
    if (newcur > newprv) return false;
  } else {
    if ((npsl & PSL_IS) && ((oldpsl & PSL_IS) == 0 || newipl == 0))
      return false;
    if (newipl > oldipl) return false;
  }
  return true;
}

static uint32_t peek_va32(uint32_t va) {
  uint32_t pa = va;
  if (!vax_mmu::translate(va, &pa, false, 0)) return 0xFFFFFFFFu;
  if (!pa_ok(pa, 4)) return 0xFFFFFFFFu;
  return phys_r32(pa);
}

static const char* va_band(uint32_t va) {
  if (va < 0x40000000u) return "P0";
  if (va < 0x80000000u) return "P1";
  return "S0";
}

// Translate as kernel read; do not raise MMGT (dump-only).
static bool peek_va8(uint32_t va, uint8_t* b, uint32_t* pa_out) {
  uint32_t pa = va;
  if (!vax_mmu::translate(va, &pa, false, 0) || !pa_ok(pa, 1))
    return false;
  if (b) *b = g_ram[pa];
  if (pa_out) *pa_out = pa;
  return true;
}

static void peek16(uint32_t va, uint32_t n, uint8_t out[16]) {
  memset(out, 0, 16);
  uint32_t shown = n < 16u ? n : 16u;
  for (uint32_t i = 0; i < shown; i++) {
    uint8_t b = 0;
    if (!peek_va8(va + i, &b, nullptr))
      break;
    out[i] = b;
  }
}

static unsigned count_zero_parity(const uint8_t* b, uint32_t n, bool odd) {
  unsigned c = 0;
  uint32_t shown = n < 16u ? n : 16u;
  for (uint32_t i = odd ? 1u : 0u; i < shown; i += 2u) {
    if (b[i] == 0) c++;
  }
  return c;
}

static void log_peek16(const char* tag, uint32_t va, uint32_t n, uint32_t off) {
  if (off >= n) return;
  uint8_t b[16];
  peek16(va + off, n - off, b);
  LOG("copy:  %s +%u %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
      tag, (unsigned)off, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
      b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static unsigned count_va_zeros(uint32_t va, uint32_t n, bool odd) {
  unsigned c = 0;
  for (uint32_t i = odd ? 1u : 0u; i < n; i += 2u) {
    uint8_t b = 0;
    if (peek_va8(va + i, &b, nullptr) && b == 0) c++;
  }
  return c;
}

static void dump_name_buf() {
  if (g_name_va == 0 || g_name_n == 0) return;
  uint8_t nb = 0;
  if (!peek_va8(g_name_va, &nb, nullptr)) {
    LOG("copy:  name VA=%08X UNMAPPED PC=%08X",
        (unsigned)g_name_va, (unsigned)g_st.r[R_PC]);
    return;
  }
  uint8_t b[16];
  peek16(g_name_va, 16, b);
  LOG("copy:  name VA=%08X %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
      (unsigned)g_name_va, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
      b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void dump_watch_buf() {
  dump_name_buf();
  if (g_watch_n == 0 || g_watch_va == 0) return;
  uint8_t b0 = 0;
  if (!peek_va8(g_watch_va, &b0, nullptr)) {
    LOG("copy:  watch VA=%08X UNMAPPED PC=%08X",
        (unsigned)g_watch_va, (unsigned)g_st.r[R_PC]);
    return;
  }
  unsigned e = count_va_zeros(g_watch_va, g_watch_n, false);
  unsigned o = count_va_zeros(g_watch_va, g_watch_n, true);
  LOG("copy:  watch VA=%08X n=%u dst0 even/odd=%u/%u (was even %u) PC=%08X",
      (unsigned)g_watch_va, (unsigned)g_watch_n, e, o, g_watch_dst0_even,
      (unsigned)g_st.r[R_PC]);
  log_peek16("watch", g_watch_va, g_watch_n, 0);
  uint32_t page = 0x200u - (g_watch_va & 0x1FFu);
  if (page == 0) page = 0x200u;
  log_peek16("watch", g_watch_va, g_watch_n, page);
  if (g_watch_export_off != 0 && g_watch_export_off + 8u <= g_watch_n)
    log_peek16("export", g_watch_va, g_watch_n, g_watch_export_off);
}

// Kernel CUR writing into user P0 (sh BSS / ureadc). Include zeros so even
// slots that stay 0 are visible. Skip P1 — V0.6.59 showed those wr==rd.
// After a script copy, also log user (CUR=3) stores into that buffer (ash NUL compact).
static void trace_user_movb(uint32_t va, uint8_t v) {
  if (g_user_movb_logs == 0 || !vax_mmu::mapen()) return;
  const bool in_watch = (g_watch_n != 0 && va >= g_watch_va &&
                         va < g_watch_va + g_watch_n);
  if (va >= 0x40000000u) return;
  if (psl_cur() != 0 && !in_watch) return;
  g_user_movb_logs--;
  uint8_t got = 0;
  uint32_t pa = 0xFFFFFFFFu;
  bool ok = peek_va8(va, &got, &pa);
  LOG("copy: MOVB P0 VA=%08X PA=%08X wr=%02X rd=%02X%s CUR=%u PC=%08X",
      (unsigned)va, (unsigned)pa, (unsigned)v, (unsigned)got,
      (!ok || got != v) ? " MISMATCH" : "",
      (unsigned)psl_cur(), (unsigned)g_last_op_pc);
}

// Kernel copyout into P0 only. V0.6.59 spent the budget on ld.so P1 memcpy
// (src==dst, packed) before sh read /etc/rc into basebuf.
static void trace_user_movc(const char* tag, uint32_t src, uint32_t dst,
                            uint32_t n) {
  if (g_user_copy_logs == 0 || n == 0 || !vax_mmu::mapen()) return;
  if (psl_cur() != 0 || dst >= 0x40000000u) return;
  g_user_copy_logs--;
  uint8_t sb[16], db[16];
  peek16(src, n, sb);
  peek16(dst, n, db);
  uint32_t pa0 = 0xFFFFFFFFu, pa1 = 0xFFFFFFFFu;
  peek_va8(dst, nullptr, &pa0);
  peek_va8(dst + 1u, nullptr, &pa1);
  LOG("copy: %s n=%u src=%08X(%s) dst=%08X(P0) pa0=%08X pa1=%08X PC=%08X",
      tag, (unsigned)n, (unsigned)src, va_band(src), (unsigned)dst,
      (unsigned)pa0, (unsigned)pa1, (unsigned)g_last_op_pc);
  LOG("copy:  src %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
      sb[0], sb[1], sb[2], sb[3], sb[4], sb[5], sb[6], sb[7],
      sb[8], sb[9], sb[10], sb[11], sb[12], sb[13], sb[14], sb[15]);
  LOG("copy:  dst %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
      db[0], db[1], db[2], db[3], db[4], db[5], db[6], db[7],
      db[8], db[9], db[10], db[11], db[12], db[13], db[14], db[15]);
  LOG("copy:  zeros src even=%u odd=%u  dst even=%u odd=%u (first %u)",
      count_zero_parity(sb, n, false), count_zero_parity(sb, n, true),
      count_zero_parity(db, n, false), count_zero_parity(db, n, true),
      (unsigned)(n < 16u ? n : 16u));

  const bool textish = (sb[0] >= 0x09 && sb[0] <= 0x7E && n >= 32u);
  if (!textish) return;

  uint32_t lim = n > 1024u ? 1024u : n;
  unsigned src_e = 0, src_o = 0, dst_e = 0, dst_o = 0, mism = 0;
  int me = 0, mx = 0;
  uint32_t export_at = 0xFFFFFFFFu, xot_at = 0xFFFFFFFFu;
  static const char kExp[] = "export";
  static const char kXot[] = "xot";
  for (uint32_t i = 0; i < lim; i++) {
    uint8_t s = 0, d = 0;
    peek_va8(src + i, &s, nullptr);
    peek_va8(dst + i, &d, nullptr);
    if (s != d) mism++;
    if (s == 0) {
      if (i & 1u) src_o++;
      else src_e++;
    }
    if (d == 0) {
      if (i & 1u) dst_o++;
      else dst_e++;
    }
    if (d == (uint8_t)kExp[me]) {
      if (++me == 6) export_at = i - 5u;
    } else {
      me = (d == (uint8_t)kExp[0]) ? 1 : 0;
    }
    if (d == (uint8_t)kXot[mx]) {
      if (++mx == 3) xot_at = i - 2u;
    } else {
      mx = (d == (uint8_t)kXot[0]) ? 1 : 0;
    }
  }
  LOG("copy:  full scanned=%u mismatch=%u src0 even/odd=%u/%u dst0 even/odd=%u/%u export@%d xot@%d",
      (unsigned)lim, mism, src_e, src_o, dst_e, dst_o,
      export_at == 0xFFFFFFFFu ? -1 : (int)export_at,
      xot_at == 0xFFFFFFFFu ? -1 : (int)xot_at);
  uint32_t page = 0x200u - (dst & 0x1FFu);
  if (page == 0) page = 0x200u;
  log_peek16("page", dst, n, page);
  if (n > 512u) log_peek16("mid", dst, n, 512u);

  g_watch_va = dst;
  g_watch_n = lim;
  g_watch_dst0_even = dst_e;
  g_watch_hb_left = 8;
  g_watch_rd_head = 20;
  g_watch_rd_tok = 24;
  g_watch_from = 8;
  g_watch_export_off = (export_at == 0xFFFFFFFFu) ? 0 : export_at;
  g_user_movb_logs = 24;
}

// Push without moving SP until the write succeeds. CALLS/PUSHR used to
// decrement first; an ACV/TNV on the write then left a leaked longword
// under the MMGT frame (Xtransl_v success REI one long too low).
static bool stack_push32(uint32_t v) {
  const uint32_t nsp = g_st.r[R_SP] - 4u;
  mem_w32(nsp, v);
  if (g_mmgt_abort) return false;
  g_st.r[R_SP] = nsp;
  return true;
}

// NetBSD 10 GENERIC intvec.S: Xtransl_v @ 800002BC, Xaccess_v @ 800002DC.
static bool in_xtransl_v(uint32_t pc) {
  return pc >= 0x800002BCu && pc < 0x800002DCu;
}

// SIMH vax_cpu1.c op_scnspn / op_locskp (no FPD; abort retries from the start).
static void op_scnspn(bool spanc) {
  Opnd len = decode_opnd(ACC_R, 2);
  Opnd src = decode_opnd(ACC_A, 1);
  Opnd tbl = decode_opnd(ACC_A, 1);
  Opnd mask = decode_opnd(ACC_R, 1);
  if (!len.ok || !src.ok || !tbl.ok || !mask.ok) return;
  uint32_t n = len.value & 0xFFFFu;
  uint32_t addr = src.addr;
  const uint8_t m = (uint8_t)mask.value;
  for (; n != 0; n--, addr++) {
    uint8_t c = mem_r8(addr);
    if (g_mmgt_abort) return;
    uint8_t t = mem_r8(tbl.addr + c);
    if (g_mmgt_abort) return;
    const bool hit = (t & m) != 0;
    if (hit != spanc) break;
  }
  g_st.r[0] = n;
  g_st.r[1] = addr;
  g_st.r[2] = 0;
  g_st.r[3] = tbl.addr;
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
  if (n == 0) g_st.psl |= PSL_Z;
}

static void op_locskp(bool skpc) {
  Opnd ch = decode_opnd(ACC_R, 1);
  Opnd len = decode_opnd(ACC_R, 2);
  Opnd src = decode_opnd(ACC_A, 1);
  if (!ch.ok || !len.ok || !src.ok) return;
  const uint8_t match = (uint8_t)ch.value;
  uint32_t n = len.value & 0xFFFFu;
  uint32_t addr = src.addr;
  for (; n != 0; n--, addr++) {
    uint8_t c = mem_r8(addr);
    if (g_mmgt_abort) return;
    const bool hit = (c == match);
    if (hit != skpc) break;
  }
  g_st.r[0] = n;
  g_st.r[1] = addr;
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
  if (n == 0) g_st.psl |= PSL_Z;
}

// Kernel sret REI: (%sp)=PC, 4(%sp)=PSL. Dump neighbors so an off-by-8
// (trap/code not skipped, or skipped twice) is visible. Do not rewrite PSL.
static void dump_rei_frame(uint32_t npc, uint32_t npsl) {
  const uint32_t sp = g_st.r[R_SP];
  LOG("boot: REI bad nPC=%08X nPSL=%08X oldPSL=%08X opPC=%08X SP=%08X FP=%08X AP=%08X",
      (unsigned)npc, (unsigned)npsl, (unsigned)g_st.psl, (unsigned)g_last_op_pc,
      (unsigned)sp, (unsigned)g_st.r[R_FP], (unsigned)g_st.r[R_AP]);
  LOG("boot: REI stack -8=%08X -4=%08X [PC]=%08X [PSL]=%08X +8=%08X +12=%08X +16=%08X +20=%08X",
      (unsigned)peek_va32(sp - 8), (unsigned)peek_va32(sp - 4),
      (unsigned)peek_va32(sp), (unsigned)peek_va32(sp + 4),
      (unsigned)peek_va32(sp + 8), (unsigned)peek_va32(sp + 12),
      (unsigned)peek_va32(sp + 16), (unsigned)peek_va32(sp + 20));
  LOG("boot: REI low -24=%08X -20=%08X -16=%08X -12=%08X",
      (unsigned)peek_va32(sp - 24), (unsigned)peek_va32(sp - 20),
      (unsigned)peek_va32(sp - 16), (unsigned)peek_va32(sp - 12));
  // sret addl2 $8 then rei: (%sp)=tf_pc, 4(%sp)=tf_psl. One long too low:
  // (%sp)=tf_code, +4=tf_pc, +8=tf_psl. Do not rewrite SP — just show it.
  LOG("boot: REI if-4 trap=%08X code=%08X pc=%08X psl=%08X",
      (unsigned)peek_va32(sp - 4), (unsigned)peek_va32(sp),
      (unsigned)peek_va32(sp + 4), (unsigned)peek_va32(sp + 8));
  LOG("boot: REI regs R0=%08X R1=%08X R6=%08X R7=%08X R8=%08X R9=%08X R10=%08X R11=%08X",
      (unsigned)g_st.r[0], (unsigned)g_st.r[1], (unsigned)g_st.r[6],
      (unsigned)g_st.r[7], (unsigned)g_st.r[8], (unsigned)g_st.r[9],
      (unsigned)g_st.r[10], (unsigned)g_st.r[11]);
  if (g_last_op_pc >= 4u)
    LOG("boot: REI insn@opPC-4=%08X (C0085E=addl2 $8,sp  then 02=rei)",
        (unsigned)peek_va32(g_last_op_pc - 4u));
  g_hb_hold = true;
  LOG("USB: heartbeats off — copy REI dump above");
  Serial.flush();
}

static bool is_user_pc(uint32_t pc) {
  return pc >= 0x1000u && pc < 0x80000000u;
}

static bool is_kern_sp(uint32_t sp) {
  if ((sp & 0xFF000000u) == 0x80000000u) return true;
  if (sp >= 0x80480000u && sp < 0x80500000u) return true;
  if (sp >= 0x82E00000u && sp < 0x82F00000u) return true;
  return false;
}

static bool is_kern_uarea(uint32_t a) { return is_kern_sp(a); }

// Fault handlers run on KSP; botched REI/sret can leave SP on the kernel stack.
// Only SP is safe to restore from USP. Forcing AP/FP/R6 to USP made AP==FP so
// _rtld_relocate_nonplt_self's 8(ap) read saved AP (0) as relocbase → VA=0x200.
static bool scrub_kstack_sp() {
  if (!g_stk[3]) return false;
  if (!is_kern_sp(g_st.r[R_SP])) return false;
  g_st.r[R_SP] = g_stk[3];
  return true;
}

static void sanitize_user_return(uint32_t npc) {
  if (!is_user_pc(npc)) return;
  g_xlat_mode_ov = -1;
  if (g_stk[3] && is_kern_sp(g_st.r[R_SP]))
    g_st.r[R_SP] = g_stk[3];
}

// Xaccess_v runs with PSL=00C00000 (kernel CUR, user PRV). A botched sret/REI
// can resume ld.so at a user PC with that handler PSL and KSP. Repair PSL and
// SP only — never AP/FP/R6 (CALLS args / relocbase live there).
static void repair_user_exec_state() {
  if (!g_boot_elf_active || !vax_mmu::mapen()) return;
  if (((g_st.psl & PSL_IPL_MASK) >> PSL_IPL_SHIFT) != 0 || psl_is()) return;
  if (!is_user_pc(g_st.r[R_PC])) return;

  const bool need_psl = (psl_cur() == 0);
  const bool need_sp = is_kern_sp(g_st.r[R_SP]);
  if (!need_psl && !need_sp) return;

  if (need_psl)
    g_st.psl = 0x03C00000u | (g_st.psl & 0x0Fu);
  scrub_kstack_sp();

  if (g_repair_log_left) {
    g_repair_log_left--;
    LOG("boot: repair user PC=%08X PSL=%08X SP=%08X USP=%08X KSP=%08X AP=%08X FP=%08X R6=%08X",
        (unsigned)g_st.r[R_PC], (unsigned)g_st.psl, (unsigned)g_st.r[R_SP],
        (unsigned)g_stk[3], (unsigned)g_stk[0],
        (unsigned)g_st.r[R_AP], (unsigned)g_st.r[R_FP], (unsigned)g_st.r[6]);
  }
}

// SCBB/PCBB are physical (SIMH ReadLP (SCBB+vec) & PAMASK).
static uint32_t scb_read_phys(uint32_t vec) {
  return phys_r32((g_st.scbb + vec) & PAMASK_LW);
}

static void set_cmp_long(uint32_t a, uint32_t b) {
  if (g_mmgt_abort) return;
  uint32_t r = a - b;
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
  if (r == 0) g_st.psl |= PSL_Z;
  if (r & 0x80000000u) g_st.psl |= PSL_N;
  if (a < b) g_st.psl |= PSL_C;
  if ((((a ^ b) & (a ^ r)) & 0x80000000u) != 0) g_st.psl |= PSL_V;
}

static void set_cmp_word(uint16_t a, uint16_t b) {
  if (g_mmgt_abort) return;
  uint16_t r = (uint16_t)(a - b);
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
  if (r == 0) g_st.psl |= PSL_Z;
  if (r & 0x8000) g_st.psl |= PSL_N;
  if (a < b) g_st.psl |= PSL_C;
  if ((((a ^ b) & (a ^ r)) & 0x8000) != 0) g_st.psl |= PSL_V;
}

static void set_cmp_byte(uint8_t a, uint8_t b) {
  if (g_mmgt_abort) return;
  uint8_t r = (uint8_t)(a - b);
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
  if (r == 0) g_st.psl |= PSL_Z;
  if (r & 0x80) g_st.psl |= PSL_N;
  if (a < b) g_st.psl |= PSL_C;
  if ((((a ^ b) & (a ^ r)) & 0x80) != 0) g_st.psl |= PSL_V;
}

// Bit-branch base (.wb): Rn → bit in register; else byte address.
struct BbBase {
  bool ok = false;
  bool is_reg = false;
  uint8_t rn = 0;
  uint32_t ea = 0;
};

static BbBase decode_bb_base() {
  BbBase b{};
  Opnd o = decode_opnd(ACC_M, 1);
  if (!o.ok) return b;
  b.ok = true;
  if (o.is_reg) {
    b.is_reg = true;
    b.rn = o.reg;
  } else {
    b.ea = o.addr;
  }
  return b;
}

static int bb_get(uint32_t pos, const BbBase& b) {
  if (b.is_reg) {
    if (pos > 31) {
      note_fault(3, "bbpos");
      return 0;
    }
    return (int)((g_st.r[b.rn] >> pos) & 1u);
  }
  uint32_t ea = b.ea + (pos >> 3);
  uint32_t p = pos & 7u;
  return (int)((mem_r8(ea) >> p) & 1u);
}

static int bb_set(uint32_t pos, const BbBase& b, int newb) {
  if (b.is_reg) {
    if (pos > 31) {
      note_fault(3, "bbpos");
      return 0;
    }
    int bit = (int)((g_st.r[b.rn] >> pos) & 1u);
    if (newb)
      g_st.r[b.rn] |= (1u << pos);
    else
      g_st.r[b.rn] &= ~(1u << pos);
    return bit;
  }
  uint32_t ea = b.ea + (pos >> 3);
  uint32_t p = pos & 7u;
  uint8_t by = mem_r8(ea);
  int bit = (int)((by >> p) & 1u);
  if (newb)
    by = (uint8_t)(by | (1u << p));
  else
    by = (uint8_t)(by & ~(1u << p));
  mem_w8(ea, by);
  return bit;
}

static void do_casex(uint32_t sel, uint32_t base, uint32_t lim, uint32_t mask) {
  // SIMH: r = (sel - base) & BMASK/WMASK/LMASK (unsigned wrap), then r > lim.
  uint32_t r = (sel - base) & mask;
  if (g_case_log != 0 && g_name_va != 0 && cpu_cur_mode() == 3u &&
      g_last_op_pc >= 0x0001C000u && g_last_op_pc < 0x0001D000u) {
    g_case_log--;
    const uint32_t tab = g_st.r[R_PC];
    uint32_t tgt = tab + (lim + 1u) * 2u;
    if (r <= lim) {
      uint8_t lo = 0, hi = 0;
      if (peek_va8(tab + r * 2u, &lo, nullptr) &&
          peek_va8(tab + r * 2u + 1u, &hi, nullptr))
        tgt = tab + (uint32_t)(int32_t)(int16_t)(uint16_t)(lo | ((uint16_t)hi << 8));
    }
    LOG("copy: case PC=%08X sel=%08X base=%08X lim=%08X r=%u tab=%08X tgt=%08X",
        (unsigned)g_last_op_pc, (unsigned)sel, (unsigned)base, (unsigned)lim,
        (unsigned)r, (unsigned)tab, (unsigned)tgt);
  }
  set_cmp_long(r, lim);
  if (r > lim) {
    g_st.r[R_PC] += (lim + 1u) * 2u;
  } else {
    int16_t disp = (int16_t)mem_r16(g_st.r[R_PC] + r * 2u);
    branch_w(disp);
  }
}

static void do_chmx(uint8_t opc, uint32_t arg) {
  // SIMH op_chm: stack writes use destination-mode access *before* PSL/PC change.
  // User CHMK must store onto KSP with kernel PTE rights; writing as CUR=user ACV's
  // then this path used to clobber raise_mmgt's PC/PSL and resume as "kernel" at ld.so.
  uint32_t mode = opc & 3u;
  uint32_t cur = psl_cur();
  if (g_st.scbb == 0) {
    note_fault(2, "chm-scb");
    return;
  }
  uint32_t newpc = scb_read_phys(SCB_CHMK + (mode << 2));
  if (cur < mode) mode = cur;
  g_stk[cur] = g_st.r[R_SP];
  uint32_t tsp = g_stk[mode];
  if (tsp == 0) tsp = g_st.r[R_SP];
  const uint32_t old_pc = g_st.r[R_PC];
  const uint32_t old_psl = g_st.psl;
  g_xlat_mode_ov = (int)mode;
  mem_w32(tsp - 12, (uint32_t)(int16_t)(arg & 0xFFFF));
  mem_w32(tsp - 8, old_pc);
  mem_w32(tsp - 4, old_psl);
  g_xlat_mode_ov = -1;
  if (g_chmk_log_left && (old_pc & 0x80000000u) == 0) {
    g_chmk_log_left--;
    LOG("boot: CHM%c PC=%08X CUR=%u KSP=%08X USP=%08X abort=%u PSL=%08X",
        "KESU"[opc & 3u], (unsigned)old_pc, (unsigned)cur, (unsigned)tsp,
        (unsigned)g_stk[3], (unsigned)g_mmgt_abort, (unsigned)old_psl);
  }
  if (g_mmgt_abort) return;
  g_st.r[R_SP] = tsp - 12;
  g_stk[mode] = g_st.r[R_SP];
  g_st.psl = (mode << PSL_CUR_SHIFT) | (old_psl & PSL_IPL_MASK) | (cur << PSL_PRV_SHIFT);
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
  g_st.r[R_PC] = newpc & ~3u;  // SCB flag bits 1:0
}

// ---- IPR / interrupts (KA630-style) ----

static uint32_t cur_ipl() {
  return (g_st.psl & PSL_IPL_MASK) >> PSL_IPL_SHIFT;
}

static uint32_t ipr_read(uint32_t ipr) {
  switch (ipr) {
    case 0: return psl_is() ? g_stk[0] : g_st.r[R_SP];  // KSP
    case 1: return g_stk[1];
    case 2: return g_stk[2];
    case 3: return g_stk[3];
    case 4: return psl_is() ? g_st.r[R_SP] : g_isp;  // ISP
    case 8: case 9: case 10: case 11: case 12: case 13:
    case 56: case 57: case 58:
      return vax_mmu::get_ipr(ipr);
    case 16: return g_st.pcbb;
    case 17: return g_st.scbb;
    case 18: return cur_ipl();
    case 21: return g_sisr & 0xFFFEu;  // SISR (bit 0 unused)
    case 24: return vax_clock::iccs_rd();
    case 25: return vax_clock::nicr_rd();
    case 26: return vax_clock::icr_rd();
    case 27: return vax_clock::todr_rd();
    case 32: return vax_console::rxcs_rd();
    case 33: return vax_console::rxdb_rd();
    case 34: return vax_console::txcs_rd();
    case 35: return 0;  // TXDB read undefined / 0
    // SID high byte = CPU type. MicroVAX II is VAX_TYP_UV2 (8).
    // Was 0x01000001 (type 1 = 11/780) which made /boot take the
    // "fromnet" R0-device path and report "Can't open device type 24".
    case 62: return 0x08000001u;
    default: return 0;
  }
}

// pmap_bootstrap identity-maps all physmem at KERNBASE then mtpr MAPEN, but
// SP/FP/AP are still the /boot stack (~0x79xxxx, P1). P1LR=NPTEPERREG so P1
// is empty — RET would mmu-r. Those pages are valid S0 as va|0x80000000.
static uint32_t s0_alias_if_phys(uint32_t a) {
  if (a != 0 && a < g_ram_bytes) return a | 0x80000000u;
  return a;
}

static void promote_boot_stack_to_s0() {
  uint32_t old_sp = g_st.r[R_SP];
  uint32_t old_fp = g_st.r[R_FP];
  uint32_t old_ap = g_st.r[R_AP];
  if (old_sp >= g_ram_bytes && old_fp >= g_ram_bytes) return;

  g_st.r[R_SP] = s0_alias_if_phys(old_sp);
  g_st.r[R_FP] = s0_alias_if_phys(old_fp);
  g_st.r[R_AP] = s0_alias_if_phys(old_ap);
  g_stk[0] = g_st.r[R_SP];

  uint32_t fp = old_fp;
  for (int n = 0; n < 16; n++) {
    if (fp == 0 || fp + 20u > g_ram_bytes) break;
    uint32_t saved_ap = phys_r32(fp + 8);
    uint32_t saved_fp = phys_r32(fp + 12);
    if (saved_ap != 0 && saved_ap < g_ram_bytes)
      phys_w32(fp + 8, saved_ap | 0x80000000u);
    if (saved_fp != 0 && saved_fp < g_ram_bytes)
      phys_w32(fp + 12, saved_fp | 0x80000000u);
    if (saved_fp == 0 || saved_fp >= g_ram_bytes || saved_fp == fp) break;
    fp = saved_fp;
  }
  LOG("boot: MAPEN stack SP %08X→%08X FP %08X→%08X AP %08X→%08X",
      (unsigned)old_sp, (unsigned)g_st.r[R_SP],
      (unsigned)old_fp, (unsigned)g_st.r[R_FP],
      (unsigned)old_ap, (unsigned)g_st.r[R_AP]);
}

static void ipr_write(uint32_t ipr, uint32_t value) {
  if (g_boot_elf_active && !g_logged_mapen &&
      (ipr == 4 || ipr == 12 || ipr == 13 || ipr == 56)) {
    g_logged_mapen = (ipr == 56);
    LOG("boot: MTPR IPR=%u val=0x%08X PC=%08X",
        (unsigned)ipr, (unsigned)value, (unsigned)g_last_op_pc);
  }
  switch (ipr) {
    case 0:  // KSP
      if (psl_is()) g_stk[0] = value;
      else g_st.r[R_SP] = value;
      break;
    case 1: g_stk[1] = value; break;
    case 2: g_stk[2] = value; break;
    case 3: g_stk[3] = value; break;
    case 4:  // ISP — SIMH: on IS write SP, else write IS
      if (psl_is()) g_st.r[R_SP] = value;
      else g_isp = value;
      break;
    case 8: case 9: case 10: case 11: case 12: case 13:
    case 56: case 57: case 58:
      if (ipr == 56 && value != 0 && !vax_mmu::mapen()) {
        promote_boot_stack_to_s0();
        g_irq_log_left = 4;  // show first kernel IRQs (SCB is now physical)
      }
      vax_mmu::set_ipr(ipr, value);
      break;
    case 16: g_st.pcbb = value; break;
    case 17: g_st.scbb = value; break;
    case 18:
      g_st.psl = (g_st.psl & ~PSL_IPL_MASK) |
                 ((value & 0x1F) << PSL_IPL_SHIFT);
      break;
    case 20: {  // SIRR — request software IPL 1..15 (SIMH: SISR |= 1<<val)
      uint32_t lvl = value & 0xFu;
      if (lvl != 0) {
        g_sisr |= (1u << lvl);
        if (g_boot_elf_active && vax_mmu::mapen() && g_sirr_log_left > 0) {
          g_sirr_log_left--;
          LOG("boot: SIRR ipl=%u SISR=%04X PC=%08X",
              (unsigned)lvl, (unsigned)(g_sisr & 0xFFFEu),
              (unsigned)g_last_op_pc);
        }
      }
      break;
    }
    case 21:
      g_sisr = value & 0xFFFEu;
      break;
    case 24: vax_clock::iccs_wr(value); break;
    case 25: vax_clock::nicr_wr(value); break;
    case 27: vax_clock::todr_wr(value); break;
    case 32: vax_console::rxcs_wr(value); break;
    case 34: vax_console::txcs_wr(value); break;
    case 35:
      if (g_tx_log != 0) {
        const uint8_t c = (uint8_t)value;
        if (c == '\n' || c == '\r' || (c >= 0x20u && c < 0x7fu)) {
          g_tx_log--;
          const char ch = (c >= 0x20u && c < 0x7fu) ? (char)c : '.';
          LOG("copy: tx '%c' PC=%08X CUR=%u",
              ch, (unsigned)g_last_op_pc, (unsigned)cpu_cur_mode());
        }
      }
      vax_console::txdb_wr(value);
      break;
    default: break;
  }
}

// Deliver one hardware interrupt if SCBB is set and IPL allows.
// vec is the SCB byte offset (UQSSP programmable vectors go to 0x1FC).
static void try_deliver_irq(uint16_t vector, uint8_t ipl) {
  if (ipl <= cur_ipl()) return;
  if (g_st.scbb == 0) return;  // no SCB yet — leave pending in device CSR

  uint32_t handler = scb_read_phys((uint32_t)vector);
  uint32_t flags = handler & 3u;
  handler &= ~3u;
  if (handler == 0) return;

  uint32_t old_psl = g_st.psl;
  uint32_t old_pc = g_st.r[R_PC];
  uint32_t old_sp = g_st.r[R_SP];
  uint32_t old_cur = psl_cur();

  // SIMH intexc: stack writes use kernel access *before* PSL/PC change
  // (Write(..., ACC_MASK(KERN))). Switching PSL first meant a push ACV
  // saved kernel PSL with the user PC — NetBSD panics "SEGV in kernel mode".
  uint32_t newpsl;
  uint32_t newsp;
  if (psl_is()) {
    newpsl = PSL_IS;
    newsp = old_sp;
  } else {
    g_stk[old_cur & 3u] = old_sp;
    if (flags & 1u && g_isp) {
      newpsl = PSL_IS;
      newsp = g_isp;
    } else {
      newpsl = 0;
      newsp = g_stk[0] ? g_stk[0] : old_sp;
    }
  }
  g_xlat_mode_ov = 0;
  mem_w32(newsp - 4, old_psl);
  mem_w32(newsp - 8, old_pc);
  g_xlat_mode_ov = -1;
  if (g_mmgt_abort) return;

  g_st.psl = newpsl | ((uint32_t)ipl << PSL_IPL_SHIFT);
  g_st.r[R_SP] = newsp - 8;
  g_st.r[R_PC] = handler;
  g_st.irq_count++;
  if (g_irq_log_left > 0) {
    g_irq_log_left--;
    LOG("boot: IRQ vec=0x%03X -> %08X flags=%u SP %08X→%08X SCBB=%08X MAPEN=%u",
        (unsigned)vector, (unsigned)handler, (unsigned)flags,
        (unsigned)old_sp, (unsigned)g_st.r[R_SP],
        (unsigned)g_st.scbb, vax_mmu::mapen() ? 1u : 0u);
  }
}

// Fault/trap through SCB: hardware pushes PC+PSL only (NetBSD TRAPCALL adds
// code+type). Privileged HALT, reserved operand, etc. IPL is unchanged.
static void raise_exception(uint32_t scb) {
  if (g_mmgt_abort) return;
  if (g_st.scbb == 0) {
    note_fault(1, "exc-scb", scb);
    return;
  }
  uint32_t handler = scb_read_phys(scb);
  uint32_t flags = handler & 3u;
  handler &= ~3u;
  if (handler == 0) {
    note_fault(1, "exc-vec", scb);
    return;
  }

  uint32_t old_psl = g_st.psl;
  uint32_t old_cur = psl_cur();
  uint32_t old_sp = g_st.r[R_SP];
  const uint32_t fault_pc = g_last_op_pc ? g_last_op_pc : g_st.r[R_PC];

  // A reserved PSL (data word) with IS set would push this frame on USP.
  if (!psl_is_sane(old_psl) && is_user_pc(fault_pc)) {
    old_cur = 3;
    old_psl = 0x03C00000u | (old_psl & 0x0Fu);
  }

  uint32_t newsp = old_sp;
  uint32_t keep_is = 0;
  const bool on_is = psl_is_sane(g_st.psl) && psl_is();
  if (on_is) {
    keep_is = PSL_IS;
    newsp = old_sp;
  } else {
    g_stk[old_cur & 3u] = old_sp;
    if (flags & 1u && g_isp) {
      keep_is = PSL_IS;
      newsp = g_isp;
    } else if (old_cur != 0 && g_stk[0]) {
      newsp = g_stk[0];
    }
  }
  const uint32_t newpsl =
      keep_is | (on_is ? (g_st.psl & PSL_IPL_MASK) : 0) |
      (old_cur << PSL_PRV_SHIFT);

  g_xlat_mode_ov = 0;
  mem_w32(newsp - 4, old_psl);
  mem_w32(newsp - 8, fault_pc);
  g_xlat_mode_ov = -1;
  if (g_mmgt_abort) return;

  g_st.psl = newpsl;
  g_st.r[R_SP] = newsp - 8;
  g_st.r[R_PC] = handler;
  g_mmgt_abort = true;
}

// Memory-management fault. SIMH fill() checks protection before V:
//   PTE acc=0 (including a zero PTE) → ACV 0x20; V=0 with prot → TNV 0x24;
//   region length / S1 → ACV with p1 LNV bit. NetBSD Xtransl_v ORs PG_V and
//   REIs (ref-bit sim); demand paging of unmapped kmem is Xaccess_v → uvm_fault.
static void raise_mmgt(uint32_t va, bool write) {
  if (g_mmgt_abort) return;

  const uint8_t flt = vax_mmu::last_fault();
  const bool lnv = (flt == vax_mmu::FLT_LNV);
  const bool tnv = (flt == vax_mmu::FLT_TNV);
  const uint32_t p1 = (write ? 4u : 0u) | (lnv ? 1u : 0u);
  const uint32_t fault_pc = g_last_op_pc ? g_last_op_pc : g_st.r[R_PC];
  const uint32_t scb = tnv ? SCB_TNV : SCB_ACV;
  const char* tag = lnv ? "ACV LNV" : (tnv ? "TNV" : "ACV");

  if (g_st.scbb == 0) {
    note_fault(2, write ? "mmgt-w" : "mmgt-r", va);
    return;
  }

  uint32_t handler = scb_read_phys(scb);
  uint32_t flags = handler & 3u;
  handler &= ~3u;
  if (handler == 0) {
    note_fault(2, tnv ? "tnv-scb" : "acv-scb", va);
    return;
  }

  uint32_t old_psl = g_st.psl;
  uint32_t old_cur = psl_cur();
  uint32_t old_sp = g_st.r[R_SP];

  // SIMH intexc: kernel-access writes *before* PSL change. Switching first
  // meant a nested ACV saved kernel PSL with the user fault PC.
  uint32_t newsp = old_sp;
  uint32_t keep_is = old_psl & PSL_IS;
  if (!psl_is()) {
    g_stk[old_cur & 3u] = old_sp;
    if (flags & 1u && g_isp) {
      keep_is = PSL_IS;
      newsp = g_isp;
    } else if (old_cur != 0 && g_stk[0]) {
      newsp = g_stk[0];
    }
  }
  const uint32_t newpsl =
      keep_is | (old_psl & PSL_IPL_MASK) | (old_cur << PSL_PRV_SHIFT);

  if (g_in_ie) {
    g_mmgt_abort = true;
    return;
  }
  g_in_ie = true;
  g_xlat_mode_ov = 0;
  mem_w32(newsp - 4, old_psl);
  mem_w32(newsp - 8, fault_pc);
  mem_w32(newsp - 12, va);
  mem_w32(newsp - 16, p1);
  g_xlat_mode_ov = -1;
  g_in_ie = false;
  if (g_mmgt_abort) return;

  g_st.psl = newpsl;
  g_st.r[R_SP] = newsp - 16;
  g_st.r[R_PC] = handler;
  g_mmgt_abort = true;

  const bool user_pc = (fault_pc & 0x80000000u) == 0 && fault_pc >= 0x1000u;
  const bool kern_psl = ((old_psl & PSL_CUR_MASK) == 0);
  const bool kstack_ref = user_pc && is_kern_uarea(va);
  const bool suspicious = (user_pc && kern_psl) || kstack_ref;
#if VVAX_DIAG_LEVEL >= 1
  const bool routine_user = user_pc && !kern_psl && !kstack_ref;
#endif
  const uint32_t storm_tag_pc = fault_pc & ~7u;
  if (storm_tag_pc == (g_acv_storm_pc & ~7u) && va == g_acv_storm_va)
    g_acv_storm_count++;
  else {
    g_acv_storm_pc = fault_pc;
    g_acv_storm_va = va;
    g_acv_storm_count = 1;
    g_acv_storm_dumped = false;
  }
  const bool storm_active = g_acv_storm_count >= 8u;
  const bool storm = storm_active &&
                     (g_acv_storm_count == 8u || (g_acv_storm_count & 0xFFu) == 0u);
  if (storm) {
    LOG("boot: ACV storm PC=%08X VA=%08X cnt=%u R6=%08X SP=%08X PSL=%08X",
        (unsigned)fault_pc, (unsigned)va, (unsigned)g_acv_storm_count,
        (unsigned)g_st.r[6], (unsigned)old_sp, (unsigned)old_psl);
    if (!g_acv_storm_dumped) {
      g_acv_storm_dumped = true;
      LOG("boot: ACV storm regs R0=%08X R1=%08X R2=%08X R3=%08X R4=%08X R5=%08X",
          (unsigned)g_st.r[0], (unsigned)g_st.r[1], (unsigned)g_st.r[2],
          (unsigned)g_st.r[3], (unsigned)g_st.r[4], (unsigned)g_st.r[5]);
      LOG("boot: ACV storm regs R6=%08X R7=%08X R8=%08X R9=%08X R10=%08X R11=%08X",
          (unsigned)g_st.r[6], (unsigned)g_st.r[7], (unsigned)g_st.r[8],
          (unsigned)g_st.r[9], (unsigned)g_st.r[10], (unsigned)g_st.r[11]);
      LOG("boot: ACV storm regs AP=%08X FP=%08X SP=%08X USP=%08X KSP=%08X",
          (unsigned)g_st.r[R_AP], (unsigned)g_st.r[R_FP], (unsigned)g_st.r[R_SP],
          (unsigned)g_stk[3], (unsigned)g_stk[0]);
    }
    if (g_acv_storm_count >= 512u) {
      LOGE("boot: ACV storm halt PC=%08X VA=%08X cnt=%u",
           (unsigned)fault_pc, (unsigned)va, (unsigned)g_acv_storm_count);
      g_st.halt = true;
      g_running = false;
    }
  } else if (!storm_active &&
             (g_mmgt_log_left > 0 || suspicious
#if VVAX_DIAG_LEVEL >= 2
              || routine_user || va == 8u
#elif VVAX_DIAG_LEVEL >= 1
              || (routine_user && g_mmgt_log_left > 0)
#endif
              )) {
    if (g_mmgt_log_left) g_mmgt_log_left--;
    LOG("boot: %s VA=%08X wr=%u PC=%08X PSL=%08X -> %08X p1=%u SP %08X→%08X",
        tag,
        (unsigned)va, write ? 1u : 0u, (unsigned)fault_pc,
        (unsigned)old_psl, (unsigned)handler, (unsigned)p1,
        (unsigned)old_sp, (unsigned)g_st.r[R_SP]);
  }
}

// Unmapped Q22 window / NXMEM: SCB machine check. NetBSD badaddr() plants
// memtest and expects Xmcheck (cold) to REI there after skipping the
// MicroVAX param block (byte count 12 + 3 longs). Do not sticky-fault.
static void raise_mchk(uint32_t pa, bool write) {
  if (g_mmgt_abort) return;

  const uint32_t fault_pc = g_last_op_pc ? g_last_op_pc : g_st.r[R_PC];

  if (g_st.scbb == 0) {
    // No SCB yet — treat as a silent bus error (probe-safe).
    g_mmgt_abort = true;
    return;
  }

  uint32_t handler = scb_read_phys(SCB_MCHK);
  uint32_t flags = handler & 3u;
  handler &= ~3u;
  if (handler == 0) {
    g_mmgt_abort = true;
    return;
  }

  uint32_t old_psl = g_st.psl;
  uint32_t old_cur = psl_cur();
  uint32_t old_sp = g_st.r[R_SP];

  if (!psl_is()) {
    g_stk[old_cur & 3u] = g_st.r[R_SP];
    if (flags & 1u && g_isp) {
      g_st.psl |= PSL_IS;
      g_st.r[R_SP] = g_isp;
    } else if (old_cur != 0 && g_stk[0]) {
      g_st.r[R_SP] = g_stk[0];
    }
  }

  // Machine check: kernel mode, previous=old, IPL 1F, clear CC.
  g_st.psl = (g_st.psl & PSL_IS) | (0x1Fu << PSL_IPL_SHIFT) |
             (old_cur << PSL_PRV_SHIFT);

  g_st.r[R_SP] -= 4;
  mem_w32(g_st.r[R_SP], old_psl);
  g_st.r[R_SP] -= 4;
  mem_w32(g_st.r[R_SP], fault_pc);
  // SIMH MicroVAX: 12-byte param block below PC/PSL so addl2 (%sp)+,%sp
  // lands on the saved PC for memtest REI.
  g_st.r[R_SP] -= 16;
  mem_w32(g_st.r[R_SP], 12);       // byte count of extra longs
  mem_w32(g_st.r[R_SP] + 4, write ? 0x00F00000u : 0x00E00000u);
  mem_w32(g_st.r[R_SP] + 8, 0);
  mem_w32(g_st.r[R_SP] + 12, pa);

  g_st.r[R_PC] = handler;
  g_mmgt_abort = true;

  if (g_mchk_log_left > 0) {
    g_mchk_log_left--;
    LOG("boot: MCHK pa=%08X wr=%u PC=%08X -> %08X SP %08X→%08X",
        (unsigned)pa, write ? 1u : 0u, (unsigned)fault_pc,
        (unsigned)handler, (unsigned)old_sp, (unsigned)g_st.r[R_SP]);
  }
}

static void service_interrupts() {
  // Priority: clock (IPL 24), console RX/TX (IPL 20), MSCP/UQSSP (IPL 14).
  if (vax_clock::irq_clk()) {
    uint32_t before = g_st.irq_count;
    try_deliver_irq(0xC0, 24);
    if (g_st.irq_count != before)
      vax_clock::irq_clk_ack();
  }
  if (vax_console::irq_rx()) {
    uint32_t before = g_st.irq_count;
    try_deliver_irq(0xF8, 20);
    if (g_st.irq_count != before)
      vax_console::irq_rx_ack();
  }
  if (vax_console::irq_tx()) {
    uint32_t before = g_st.irq_count;
    try_deliver_irq(0xFC, 20);
    if (g_st.irq_count != before)
      vax_console::irq_tx_ack();
  }
  if (vax_mscp::irq_pending()) {
    uint16_t vec = vax_mscp::irq_vector();
    // UQSSP STEP1 vector is (SA & 0x7F)<<2; first uba programmable
    // vector is 0x1FC (uh_lastiv=0x200). An 8-bit cap dropped it, so
    // udamatch never hit scb_stray → "uda0 ... didn't interrupt".
    if (vec && vec < 0x400u) {
      if (14u <= cur_ipl() && vax_mscp::host_online_wait() &&
          g_mscp_blocked_logs) {
        g_mscp_blocked_logs--;
        LOG("MSCP irq blocked IPL=%u vec=0x%03X PC=%08X",
            (unsigned)cur_ipl(), (unsigned)vec, (unsigned)g_st.r[R_PC]);
      }
      uint32_t from_pc = g_st.r[R_PC];
      uint32_t before = g_st.irq_count;
      try_deliver_irq(vec, 14);
      if (g_st.irq_count != before) {
        vax_mscp::irq_clear();
        if (g_mscp_irq_logs) {
          g_mscp_irq_logs--;
          LOG("MSCP IRQ taken vec=0x%03X from=%08X to=%08X wait=%u ticks=%u",
              (unsigned)vec, (unsigned)from_pc, (unsigned)g_st.r[R_PC],
              vax_mscp::host_online_wait() ? 1u : 0u,
              (unsigned)vax_clock::ticks());
        }
      }
    }
  }
  // Software interrupts (SISR bits 1–15). SCB offset = 0x80 + (ipl<<2).
  // NetBSD: IPL_SOFTCLOCK=8 → vec 0xA0 (callouts / cv_timedwait).
  // Without this, hardclock runs but softclock never does — idle forever
  // after ra0/ra1 with no "boot device:" (config_finalize waits on callouts).
  uint32_t hw_ipl = cur_ipl();
  if (hw_ipl < 15u && g_sisr != 0) {
    for (int i = 15; i > (int)hw_ipl; --i) {
      if ((g_sisr & (1u << i)) == 0)
        continue;
      uint32_t before = g_st.irq_count;
      try_deliver_irq((uint16_t)(0x80u + ((uint16_t)i << 2)), (uint8_t)i);
      if (g_st.irq_count != before)
        g_sisr &= ~(1u << i);
      break;
    }
  }
}

// ---- one instruction ----

// config_finalize cv_timedwait uses getticks(); warp only on proc0 idle
// (this GENERIC: SP page 0x825DE000). Other LWPs (pffasttimo @ 825ECExx)
// must not be warped — extra IPL 24 ticks starve MSCP IPL 14 and mscp_wq.
// After MSCP/SISR, hold off. After ONLINE, skip warp until the next host cmd
// so ra_putonline's tsleep is not expired by IPL 24 before mscp_wq runs.
#if VAX_CLOCK_WARP_DEFAULT
static void maybe_idle_clock_warp() {
  if (!g_boot_elf_active || !vax_mmu::mapen()) return;
  if (cur_ipl() != 0) return;
  uint32_t sp = g_st.r[R_SP];
  if ((sp & ~0xFFFu) != 0x825DE000u) return;
  if (vax_clock::irq_clk()) return;
  uint32_t now = millis();
  if (g_sisr != 0 || vax_mscp::busy() || vax_mscp::host_online_wait()) {
    g_warp_holdoff_ms = now + 250u;
    return;
  }
  if ((int32_t)(now - g_warp_holdoff_ms) < 0) return;
  if (now != g_warp_ms) {
    g_warp_ms = now;
    g_warp_in_ms = 0;
  }
  if (g_warp_in_ms >= 10u) return;  // ≤10 extra ticks/ms (~10× wall)
  if (++g_idle_warp_ctr < 64u) return;
  g_idle_warp_ctr = 0;
  if (!g_logged_idle_warp) {
    g_logged_idle_warp = true;
    LOG("boot: idle clock warp SP=%08X (watch TFT/Telnet for boot device:)",
        (unsigned)sp);
  }
  g_warp_in_ms++;
  g_warp_fires++;
  vax_clock::force_tick();
}
#endif

// GENERIC NetBSD/vax 10.1 (/netbsd on netbsd101-boot.dsk). First hits only.
// Distinguishes: never entered raopen vs ONLINE IRQ vs wakeup-before-tsleep.
static void probe_rootopen() {
  if (!g_boot_elf_active || !vax_mmu::mapen()) return;
#if VVAX_DIAG_LEVEL == 0
  if (!vax_mscp::host_online_wait()) return;
#endif
  const uint32_t pc = g_st.r[R_PC];
  const bool waiting = vax_mscp::host_online_wait();
  // After the first sticky kprobe, scan every 4096 insns — not every insn.
  const bool scan = waiting || !g_rootopen_trace ||
                    ((g_instr_count & 0xFFFu) == 0);

  struct Probe {
    uint32_t lo;
    uint32_t sz;
    const char* name;
    uint8_t max_hits;
    uint8_t hits;
    bool sticky_trace;
  };
  static Probe probes[] = {
      {0x80019DA4u, 0x4Cu, "cpu_rootconf", 1, 0, true},
      {0x80229014u, 0x46Cu, "vfs_mountroot", 1, 0, true},
      {0x8001F0B4u, 0x19Cu, "raopen", 2, 0, true},
      {0x8001F6DEu, 0x26u, "ra_putonline", 2, 0, true},
      {0x8001E7FAu, 0x113u, "ra_putonline.body", 2, 0, true},
      {0x8001EC1Cu, 0x7Cu, "rx_putonline", 2, 0, true},
      {0x8002219Cu, 0x2Cu, "udaintr", 1, 0, false},
      {0x8001DC16u, 0x77u, "mscp_intr", 1, 0, false},
      {0x8001CF1Au, 0x73Eu, "mscp_dorsp", 1, 0, false},
      {0x8001F5C0u, 0x11Eu, "rronline", 1, 0, false},
      {0x8001D66Au, 0x53u, "mscp_worker", 1, 0, false},
      {0x801EDC2Cu, 0xF9u, "workqueue_enqueue", 1, 0, false},
      {0x801ED71Eu, 0x171u, "workqueue_worker", 1, 0, false},
      {0x801C717Au, 0xF6u, "tsleep", 1, 0, false},
      {0x801C751Eu, 0x4Fu, "wakeup", 1, 0, false},
      {0x80011128u, 0x25u, "gencngetc", 1, 0, false},
  };
  if (g_reset_probes) {
    g_reset_probes = false;
    for (Probe& p : probes) p.hits = 0;
  }

  if (scan) for (Probe& p : probes) {
    if (pc < p.lo || pc >= p.lo + p.sz) continue;
    if (p.sticky_trace) g_rootopen_trace = true;
    else if (!g_rootopen_trace)
      break;
    if (p.hits >= p.max_hits) break;
    p.hits++;
    LOG("kprobe %s PC=%08X SP=%08X IPL=%u wait=%u ticks=%u warp=%u irq=%u",
        p.name, (unsigned)pc, (unsigned)g_st.r[R_SP], (unsigned)cur_ipl(),
        vax_mscp::host_online_wait() ? 1u : 0u,
        (unsigned)vax_clock::ticks(), (unsigned)g_warp_fires,
        (unsigned)g_st.irq_count);
    break;
  }

  if (g_hb_hold) return;
  if (!g_rootopen_trace && !vax_mscp::host_online_wait()) return;
  if (g_logged_user_rei) return;
  uint32_t now = millis();
  if (now - g_root_hb_ms < 5000u) return;
  g_root_hb_ms = now;
  LOG("root hb: PC=%08X SP=%08X IPL=%u SISR=%04X wait=%u busy=%u ticks=%u warp=%u",
      (unsigned)pc, (unsigned)g_st.r[R_SP], (unsigned)cur_ipl(),
      (unsigned)(g_sisr & 0xFFFEu),
      vax_mscp::host_online_wait() ? 1u : 0u,
      vax_mscp::busy() ? 1u : 0u,
      (unsigned)vax_clock::ticks(), (unsigned)g_warp_fires);
}

static void exec_hb_if_due() {
  if (g_hb_hold) return;
  const uint32_t now = millis();
#if VVAX_DIAG_LEVEL >= 2
  const uint32_t hb_ms = 5000u;
#else
  const uint32_t hb_ms = 30000u;  // 5s made USB copy-select jump to the bottom
#endif
  if (g_hb_last_ms != 0 && now - g_hb_last_ms < hb_ms) return;

  uint32_t dt_ms = 0;
  uint32_t ips = 0;
  if (g_hb_last_ms != 0) {
    dt_ms = now - g_hb_last_ms;
    g_hb_elapsed_ms += dt_ms;
    if (dt_ms > 0)
      ips = (uint32_t)((uint64_t)(g_instr_count - g_hb_last_instr) * 1000ull /
                       dt_ms);
  }

  g_hb_last_ms = now;
  g_hb_last_instr = g_instr_count;

  LOG("hb: PC=%08X SP=%08X PSL=%08X IPL=%u instr=%u ips=%u ticks=%u uptime=%llus",
      (unsigned)g_st.r[R_PC], (unsigned)g_st.r[R_SP],
      (unsigned)g_st.psl, (unsigned)cur_ipl(),
      (unsigned)g_instr_count, (unsigned)ips, (unsigned)vax_clock::ticks(),
      (unsigned long long)(g_hb_elapsed_ms / 1000ull));
  if (g_watch_hb_left) {
    g_watch_hb_left--;
    dump_watch_buf();
  }
}

static void exec_one() {
  if (g_st.halt || g_st.fault) {
    g_running = false;
    return;
  }
  g_mmgt_abort = false;
  g_xlat_mode_ov = -1;

#if VAX_CLOCK_WARP_DEFAULT
  maybe_idle_clock_warp();
#endif
  vax_mscp::instr_tick();
  service_interrupts();
  probe_rootopen();
  repair_user_exec_state();

  if (!g_logged_reloc && g_st.r[R_PC] >= 0x100000u && g_st.r[R_PC] < 0x300000u) {
    g_logged_reloc = true;
    g_instr_count = 0;
    g_trace_left =
#if VVAX_DIAG_LEVEL >= 2
        48;
#else
        0;
#endif
    g_logged_stop = false;
    LOG("boot: executing relocated xxboot at PC=%08X", (unsigned)g_st.r[R_PC]);
  }

  g_instr_count++;

  // Kernel handoff: first fetch in S0 (KERNBASE).
  if (!g_logged_s0_pc && (g_st.r[R_PC] & 0x80000000u)) {
    g_logged_s0_pc = true;
    LOG("boot: enter S0 PC=%08X SP=%08X R6=%08X SCBB=%08X PSL=%08X",
        (unsigned)g_st.r[R_PC], (unsigned)g_st.r[R_SP],
        (unsigned)g_st.r[6], (unsigned)g_st.scbb, (unsigned)g_st.psl);
  }

  uint32_t op_pc = g_st.r[R_PC];
  g_last_op_pc = op_pc;
  uint8_t op = fetch8();
  // I-fetch ACV/TNV already vectored (PC = Xaccess_v / Xtransl_v). fetch8
  // returns 0 on abort, which is HALT — do not execute it. Next exec_one
  // runs the handler (NetBSD uvm_fault for a zero PTE / first user insn).
  if (g_mmgt_abort)
    return;
  if (g_trace_left > 0 && VVAX_DIAG_LEVEL >= 2 && op != 0x90 && op != 0xF5) {
    g_trace_left--;
    LOG("boot tr: PC=%08X op=%02X", (unsigned)op_pc, op);
  }

  switch (op) {
    case 0x00: {  // HALT — kernel only (SIMH: non-kernel → reserved-inst fault)
      const uint32_t hpc = g_last_op_pc ? g_last_op_pc : (g_st.r[R_PC] - 1);
      uint32_t hpa = hpc;
      vax_mmu::translate(hpc, &hpa, false);
      // SIMH: HALT is privileged by PSL current mode, not by PC band.
      // is_user_pc(0x102F) made the cold-boot stub HALT a false FAIL.
      const bool user_halt = (psl_cur() != 0);
      if (user_halt) {
        LOG("boot: user HALT PC=%08X PA=%08X PSL=%08X R0=%08X R6=%08X R7=%08X R8=%08X",
            (unsigned)hpc, (unsigned)hpa, (unsigned)g_st.psl,
            (unsigned)g_st.r[0], (unsigned)g_st.r[6],
            (unsigned)g_st.r[7], (unsigned)g_st.r[8]);
        LOG("boot: user HALT AP=%08X FP=%08X SP=%08X USP=%08X R9=%08X R10=%08X R11=%08X",
            (unsigned)g_st.r[R_AP], (unsigned)g_st.r[R_FP],
            (unsigned)g_st.r[R_SP], (unsigned)g_stk[3],
            (unsigned)g_st.r[9], (unsigned)g_st.r[10], (unsigned)g_st.r[11]);
        if (g_ram && pa_ok(hpa, 8)) {
          LOG("  bytes@HALT PA: %02X %02X %02X %02X %02X %02X %02X %02X",
              g_ram[hpa], g_ram[hpa + 1], g_ram[hpa + 2], g_ram[hpa + 3],
              g_ram[hpa + 4], g_ram[hpa + 5], g_ram[hpa + 6], g_ram[hpa + 7]);
        }
        if (psl_cur() == 0) {
          g_st.psl = 0x03C00000u | (g_st.psl & 0x0Fu);
          if (g_stk[3] && is_kern_sp(g_st.r[R_SP]))
            g_st.r[R_SP] = g_stk[3];
        }
        raise_exception(SCB_PRIV);
        return;
      }
      LOG("VAX HALT at PC=%08X PA=%08X MAPEN=%u R0=%08X SP=%08X",
          (unsigned)hpc, (unsigned)hpa, vax_mmu::mapen() ? 1u : 0u,
          (unsigned)g_st.r[0], (unsigned)g_st.r[R_SP]);
      if (g_ram && pa_ok(hpa, 8)) {
        LOG("  bytes@HALT PA: %02X %02X %02X %02X %02X %02X %02X %02X",
            g_ram[hpa], g_ram[hpa + 1], g_ram[hpa + 2], g_ram[hpa + 3],
            g_ram[hpa + 4], g_ram[hpa + 5], g_ram[hpa + 6], g_ram[hpa + 7]);
      }
      g_st.halt = true;
      g_running = false;
      return;
    }

    case 0x01:  // NOP
      return;

    case 0x02: {  // REI — pop PC, then PSL; switch KSP/ISP like SIMH op_rei
      uint32_t npc = mem_r32(g_st.r[R_SP]);
      uint32_t npsl = mem_r32(g_st.r[R_SP] + 4);
      if (g_mmgt_abort) return;
      uint32_t oldcur = psl_cur();
      uint32_t newcur = (npsl & PSL_CUR_MASK) >> PSL_CUR_SHIFT;
      const uint32_t newprv = (npsl & PSL_PRV_MASK) >> PSL_PRV_SHIFT;
      bool psl_bad = !rei_psl_legal(npsl, g_st.psl);
      // Reserved PSL (data as PSL, e.g. 7F55BD10) is a trapframe error.
      // Do not rewrite it to 03C00000: that would execute the wrong PC.
      if (psl_bad) {
        if (g_rei_bad_log_left) {
          g_rei_bad_log_left--;
          dump_rei_frame(npc, npsl);
        }
        raise_exception(SCB_RESOP);
        return;
      }
      // Known leftover: sret/REI after uvm_fault with handler PSL 00C00000
      // (kernel CUR, user PRV, IPL 0) at a user PC. Still a frame bug; dump it.
      // Rewrite only this pattern — not reserved/garbage PSLs.
      if (oldcur == 0 && is_user_pc(npc) && newcur == 0 && newprv == 3u &&
          !(npsl & PSL_IS) && psl_ipl_of(npsl) == 0) {
        if (g_rei_bad_log_left) {
          g_rei_bad_log_left--;
          dump_rei_frame(npc, npsl);
          LOG("boot: REI kernel PSL at user PC -> 03C00000 USP=%08X",
              (unsigned)g_stk[3]);
        }
        npsl = 0x03C00000u | (npsl & 0x0Fu);
        newcur = 3;
      }
      g_st.r[R_SP] += 8;
      if (psl_is())
        g_isp = g_st.r[R_SP];
      else
        g_stk[oldcur & 3u] = g_st.r[R_SP];
      g_st.psl = npsl;
      if (psl_is())
        g_st.r[R_SP] = g_isp;
      else
        g_st.r[R_SP] = g_stk[newcur];
      g_st.r[R_PC] = npc;
      sanitize_user_return(npc);
      const bool to_p1 = (npc & 0xFF000000u) == 0x7F000000u;
      if (to_p1 && g_p1_rei_log_left && VVAX_DIAG_LEVEL >= 1) {
        g_p1_rei_log_left--;
        LOG("boot: REI P1 PC=%08X PSL=%08X CUR=%u IS=%u SP=%08X USP=%08X KSP=%08X R6=%08X",
            (unsigned)npc, (unsigned)npsl, (unsigned)newcur,
            (unsigned)((npsl & PSL_IS) ? 1u : 0u),
            (unsigned)g_st.r[R_SP], (unsigned)g_stk[3], (unsigned)g_stk[0],
            (unsigned)g_st.r[6]);
      }
      if (!g_logged_user_rei && newcur == 3u) {
        g_logged_user_rei = true;
        LOG("boot: REI user PC=%08X PSL=%08X USP=%08X KSP=%08X SSP=%08X R6=%08X",
            (unsigned)npc, (unsigned)npsl, (unsigned)g_st.r[R_SP],
            (unsigned)g_stk[0], (unsigned)g_stk[2], (unsigned)g_st.r[6]);
        g_mmgt_log_left = 16;
        g_chmk_log_left = 8;
        g_irq_log_left = 4;
        g_user_copy_logs = 24;
        g_user_movb_logs = 32;
      }
      // Quiet after the /boot entry REI — interrupt returns flood the log.
      // hoppabort REIs to linked_base+2.
      // 0x7D0002 = stock 8 MiB; 0x7A0002 = Freenove ~8 MiB−192 KB (RAM end 0x7D0000);
      // 0x5D0002 = 6 MiB; 0x200002 = legacy (unsafe under large kernels).
      // Stock CD BOOT.;1 has e_entry=0 → npc=2; apply_cd_boot_hopp rewrites
      // that to 0x7A0002 after assembling the image (do not rewrite PSL).
      if (npc < 0x1000u && vax_boot::apply_cd_boot_hopp(&npc))
        g_st.r[R_PC] = npc;
      const bool boot_entry =
          (npc == 0x7A0002u || npc == 0x7D0002u || npc == 0x5D0002u ||
           npc == 0x200002u);
      if (boot_entry) {
        LOG("boot: REI -> PC=%08X", (unsigned)npc);
        // hoppabort leaves R11 = xxboot RPB (0xf0000). Ensure pfncnt matches RAM
        // so later /boot/kernel sizing is sane; log devtyp (expect BDEV_UDA=17).
        uint32_t rpb = g_st.r[11];
        if (g_ram && rpb && (uint64_t)rpb + 104u <= g_ram_bytes) {
          uint32_t pages = (uint32_t)(g_ram_bytes / 512u);
          g_ram[rpb + 76] = (uint8_t)pages;
          g_ram[rpb + 77] = (uint8_t)(pages >> 8);
          g_ram[rpb + 78] = (uint8_t)(pages >> 16);
          g_ram[rpb + 79] = (uint8_t)(pages >> 24);
          LOG("boot: RPB@%08X devtyp=%u unit=%u csr=%08X pfncnt=%u R0=%u AP=%08X",
              (unsigned)rpb, (unsigned)g_ram[rpb + 102],
              (unsigned)(g_ram[rpb + 100] | ((uint16_t)g_ram[rpb + 101] << 8)),
              (unsigned)((uint32_t)g_ram[rpb + 84] |
                         ((uint32_t)g_ram[rpb + 85] << 8) |
                         ((uint32_t)g_ram[rpb + 86] << 16) |
                         ((uint32_t)g_ram[rpb + 87] << 24)),
              (unsigned)pages, (unsigned)g_st.r[0], (unsigned)g_st.r[R_AP]);
        } else {
          LOG("boot: R11=%08X (expected RPB); R0=%u AP=%08X",
              (unsigned)rpb, (unsigned)g_st.r[0], (unsigned)g_st.r[R_AP]);
        }
        // Fingerprint at linked_base+0x233D (smart relocate → 0x7E).
        uint32_t fp = (npc & ~3u) + 0x233Du;
        if (g_ram && (uint64_t)fp < g_ram_bytes) {
          g_boot_elf_active = true;
          vax_boot::plant_boot_stubs(npc - 2u);
          LOG("boot: /boot fingerprint @%06X=%02X (expect 7E smart, 21=blind-corrupt)",
              (unsigned)fp, (unsigned)g_ram[fp]);
        }
      } else if (npc < 0x1000u) {
        vax_boot::log_elf_hopp(npc);
      }
      return;
    }

    case 0x04: {  // RET — frame matches SIMH/VAX: (FP)=cond, (FP)+4=SPA/S/mask/PSW
      uint32_t fp = g_st.r[R_FP];
      uint32_t spamask = mem_r32(fp + 4);
      if (g_mmgt_abort) return;
      uint16_t mask = (uint16_t)((spamask >> 16) & 0x0FFFu);
      uint32_t spa = (spamask >> 30) & 3u;
      bool calls = (spamask & (1u << 29)) != 0;
      uint32_t saved_ap = mem_r32(fp + 8);
      uint32_t saved_fp = mem_r32(fp + 12);
      uint32_t newpc = mem_r32(fp + 16);
      if (g_mmgt_abort) return;
      g_st.r[R_AP] = saved_ap;
      g_st.r[R_FP] = saved_fp;
      uint32_t sp = fp + 20;
      for (int i = 0; i <= 11; i++) {
        if (mask & (1u << i)) {
          g_st.r[i] = mem_r32(sp);
          sp += 4;
        }
      }
      sp += spa;
      uint32_t narg = 0;
      if (calls) {
        narg = mem_r32(sp) & 0xFF;
        if (g_mmgt_abort) return;
        sp += 4 + 4 * narg;
      }
      if (g_xtransl_log_left && in_xtransl_v(newpc)) {
        g_xtransl_log_left--;
        LOG("boot: RET Xtransl narg=%u mask=%03X SP %08X→%08X newpc=%08X tos=%08X +4=%08X +8=%08X +12=%08X",
            (unsigned)narg, (unsigned)mask, (unsigned)g_st.r[R_SP],
            (unsigned)sp, (unsigned)newpc,
            (unsigned)peek_va32(sp), (unsigned)peek_va32(sp + 4),
            (unsigned)peek_va32(sp + 8), (unsigned)peek_va32(sp + 12));
      }
      g_st.r[R_SP] = sp;
      g_st.psl = (g_st.psl & ~0xFFE0u) | (spamask & 0xFFE0u);
      g_st.r[R_PC] = newpc;
      return;
    }

    case 0x05: {  // RSB
      uint32_t ret = mem_r32(g_st.r[R_SP]);
      if (g_mmgt_abort) return;
      g_st.r[R_SP] += 4;
      g_st.r[R_PC] = ret;
      return;
    }

    case 0x10: {  // BSBB
      int8_t d = (int8_t)fetch8();
      if (g_mmgt_abort) return;
      if (!stack_push32(g_st.r[R_PC])) return;
      branch_b(d);
      return;
    }

    case 0x11: {  // BRB
      int8_t d = (int8_t)fetch8();
      branch_b(d);
      return;
    }

    case 0x12: {  // BNEQ / BNEQU
      int8_t d = (int8_t)fetch8();
      if (!(g_st.psl & PSL_Z)) branch_b(d);
      return;
    }

    case 0x13: {  // BEQL / BEQLU
      int8_t d = (int8_t)fetch8();
      if (g_st.psl & PSL_Z) branch_b(d);
      return;
    }

    case 0x14: {  // BGTR
      int8_t d = (int8_t)fetch8();
      if (!(g_st.psl & (PSL_N | PSL_Z))) branch_b(d);
      return;
    }

    case 0x15: {  // BLEQ
      int8_t d = (int8_t)fetch8();
      if (g_st.psl & (PSL_N | PSL_Z)) branch_b(d);
      return;
    }

    case 0x16: {  // JSB
      Opnd a = decode_opnd(ACC_A, 1);
      if (!a.ok || g_mmgt_abort) return;
      if (!stack_push32(g_st.r[R_PC])) return;
      if (a.addr == vax_boot::ROM_READ_PA) {
        vax_boot::rom_disk_read();
        uint32_t ret = mem_r32(g_st.r[R_SP]);
        g_st.r[R_SP] += 4;
        g_st.r[R_PC] = ret;
        return;
      }
      if (vax_boot::ka630_console_jsb(a.addr))
        return;
      // Kernel S0 JSB (cmn_idsptch @ 0x800006A8) is normal. Log only
      // unhandled Q22/ROM targets (smashed conspage was 0x2001xxxx).
      if (g_boot_elf_active && !g_logged_wild_jsb &&
          (a.addr & 0xFF000000u) == 0x20000000u) {
        g_logged_wild_jsb = true;
        LOG("boot: wild JSB dest=0x%08X PC=%08X R11=%08X conspage=0x%08X",
            (unsigned)a.addr, (unsigned)g_st.r[R_PC],
            (unsigned)g_st.r[11], (unsigned)vax_boot::ka630_conspage_pa());
      }
      g_st.r[R_PC] = a.addr;
      return;
    }

    case 0x17: {  // JMP — address operand size 1 (SIMH AB)
      Opnd a = decode_opnd(ACC_A, 1);
      if (!a.ok || g_mmgt_abort) return;
      if (g_user_jmp_log_left && psl_cur() == 3u && is_user_pc(a.addr)) {
        uint32_t from = g_last_op_pc;
        uint32_t dist = (a.addr >= from) ? (a.addr - from) : (from - a.addr);
        if (dist > 0x1000u) {
          g_user_jmp_log_left--;
          g_logged_user_jmp = true;
          LOG("boot: JMP user dest=%08X from PC=%08X R0=%08X R6=%08X R7=%08X R8=%08X SP=%08X",
              (unsigned)a.addr, (unsigned)from, (unsigned)g_st.r[0],
              (unsigned)g_st.r[6], (unsigned)g_st.r[7], (unsigned)g_st.r[8],
              (unsigned)g_st.r[R_SP]);
        }
      }
      g_st.r[R_PC] = a.addr;
      return;
    }

    case 0x18: {  // BGEQ
      int8_t d = (int8_t)fetch8();
      if (!(g_st.psl & PSL_N)) branch_b(d);
      return;
    }

    case 0x19: {  // BLSS
      int8_t d = (int8_t)fetch8();
      if (g_st.psl & PSL_N) branch_b(d);
      return;
    }

    case 0x1A: {  // BGTRU
      int8_t d = (int8_t)fetch8();
      if (!(g_st.psl & (PSL_C | PSL_Z))) branch_b(d);
      return;
    }

    case 0x1B: {  // BLEQU
      int8_t d = (int8_t)fetch8();
      if (g_st.psl & (PSL_C | PSL_Z)) branch_b(d);
      return;
    }

    case 0x1C: {  // BVC
      int8_t d = (int8_t)fetch8();
      if (!(g_st.psl & PSL_V)) branch_b(d);
      return;
    }

    case 0x1D: {  // BVS
      int8_t d = (int8_t)fetch8();
      if (g_st.psl & PSL_V) branch_b(d);
      return;
    }

    case 0x1E: {  // BGEQU / BCC
      int8_t d = (int8_t)fetch8();
      if (!(g_st.psl & PSL_C)) branch_b(d);
      return;
    }

    case 0x1F: {  // BLSSU / BCS
      int8_t d = (int8_t)fetch8();
      if (g_st.psl & PSL_C) branch_b(d);
      return;
    }

    case 0x28: {  // MOVC3 — simplified (no FPD); study: Open SIMH vax_cpu1.c op_movc
      Opnd len = decode_opnd(ACC_R, 2);
      Opnd src = decode_opnd(ACC_A, 1);
      Opnd dst = decode_opnd(ACC_A, 1);
      if (!len.ok || !src.ok || !dst.ok) return;
      uint32_t n = len.value & 0xFFFF;
      if (n > g_ram_bytes) n = (uint32_t)g_ram_bytes;
      if (g_boot_elf_active && (dst.addr & 0x80000000u) && n > 0) {
        uint32_t end = dst.addr + n;
        if (end > g_kernel_load_end) g_kernel_load_end = end;
        if (n >= 32768u)
          LOG("boot: MOVC3 %u bytes src=0x%08X dst=0x%08X end=0x%08X",
              (unsigned)n, (unsigned)src.addr, (unsigned)dst.addr,
              (unsigned)end);
      }
      if (src.addr < dst.addr) {
        for (uint32_t i = n; i > 0; i--) {
          mem_w8(dst.addr + i - 1, mem_r8(src.addr + i - 1));
          if (g_mmgt_abort) return;
        }
      } else {
        for (uint32_t i = 0; i < n; i++) {
          mem_w8(dst.addr + i, mem_r8(src.addr + i));
          if (g_mmgt_abort) return;
        }
      }
      if (!g_mmgt_abort) {
        trace_user_movc("MOVC3", src.addr, dst.addr, n);
        if (g_name_movc != 0 && g_name_va != 0 && psl_cur() == 3u && n > 0 &&
            n <= 64u &&
            ((src.addr >= g_name_va && src.addr < g_name_va + 8u) ||
             (dst.addr >= g_name_va && dst.addr < g_name_va + 0x400u))) {
          g_name_movc--;
          uint8_t db[16];
          peek16(dst.addr, n, db);
          LOG("copy: user MOVC3 name n=%u src=%08X dst=%08X PC=%08X",
              (unsigned)n, (unsigned)src.addr, (unsigned)dst.addr,
              (unsigned)g_last_op_pc);
          LOG("copy:  ndst %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
              db[0], db[1], db[2], db[3], db[4], db[5], db[6], db[7],
              db[8], db[9], db[10], db[11], db[12], db[13], db[14], db[15]);
        }
        if (g_watch_from && psl_cur() == 3 && g_watch_n != 0 && n > 0 &&
            n <= 64u && src.addr >= g_watch_va &&
            src.addr < g_watch_va + g_watch_n) {
          g_watch_from--;
          uint8_t db[16];
          peek16(dst.addr, n, db);
          LOG("copy: user MOVC3 n=%u src=%08X dst=%08X(%s) PC=%08X",
              (unsigned)n, (unsigned)src.addr, (unsigned)dst.addr,
              va_band(dst.addr), (unsigned)g_last_op_pc);
          LOG("copy:  udst %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
              db[0], db[1], db[2], db[3], db[4], db[5], db[6], db[7],
              db[8], db[9], db[10], db[11], db[12], db[13], db[14], db[15]);
        }
      }
      g_st.r[0] = 0;
      g_st.r[1] = src.addr + n;
      g_st.r[2] = 0;
      g_st.r[3] = dst.addr + n;
      g_st.r[4] = 0;
      g_st.r[5] = 0;
      set_nz_long(0);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x2A:  // SCANC — SIMH op_scnspn(spanc=0)
      op_scnspn(false);
      return;

    case 0x2B:  // SPANC
      op_scnspn(true);
      return;

    case 0x2C: {  // MOVC5
      Opnd srclen = decode_opnd(ACC_R, 2);
      Opnd src = decode_opnd(ACC_A, 1);
      Opnd fill = decode_opnd(ACC_R, 1);
      Opnd dstlen = decode_opnd(ACC_R, 2);
      Opnd dst = decode_opnd(ACC_A, 1);
      if (!srclen.ok || !src.ok || !fill.ok || !dstlen.ok || !dst.ok) return;
      uint32_t sl = srclen.value & 0xFFFF;
      uint32_t dl = dstlen.value & 0xFFFF;
      // Guard against runaway if lengths are wrong (keeps host responsive).
      if (sl > g_ram_bytes) sl = (uint32_t)g_ram_bytes;
      if (dl > g_ram_bytes) dl = (uint32_t)g_ram_bytes;
      uint32_t dpa = dst.addr & 0x3FFFFFFFu;
      if (g_boot_elf_active && dl >= 256u && dpa < 0x00200000u) {
        LOG("boot: MOVC5 dst=0x%08X pa=0x%08X fill=%u sl=%u dl=%u PC=%08X",
            (unsigned)dst.addr, (unsigned)dpa, (unsigned)(fill.value & 0xFF),
            (unsigned)sl, (unsigned)dl, (unsigned)g_last_op_pc);
      }
      uint32_t n = sl < dl ? sl : dl;
      for (uint32_t i = 0; i < n; i++) {
        mem_w8(dst.addr + i, mem_r8(src.addr + i));
        if (g_mmgt_abort) return;
      }
      for (uint32_t i = n; i < dl; i++) {
        mem_w8(dst.addr + i, (uint8_t)fill.value);
        if (g_mmgt_abort) return;
      }
      if (!g_mmgt_abort && n > 0)
        trace_user_movc("MOVC5", src.addr, dst.addr, n);
      // SIMH vax_cpu1.c op_movc: R3 is dest address throughout and ends at
      // dst+dstlen; R4/R2/R5=0; R0=remaining src. We had R3=remaining length
      // so NetBSD memset's next MOVC5 used dest=0xFFFF and wiped kernel text.
      g_st.r[0] = sl - n;
      g_st.r[1] = src.addr + n;
      g_st.r[2] = 0;
      g_st.r[3] = dst.addr + dl;
      g_st.r[4] = 0;
      g_st.r[5] = 0;
      {
        uint16_t a = (uint16_t)sl, b = (uint16_t)dl;
        uint16_t r = (uint16_t)(a - b);
        g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
        if (r == 0) g_st.psl |= PSL_Z;
        if (r & 0x8000) g_st.psl |= PSL_N;
        if (a < b) g_st.psl |= PSL_C;
      }
      return;
    }

    case 0x30: {  // BSBW
      int16_t d = (int16_t)fetch16();
      if (g_mmgt_abort) return;
      if (!stack_push32(g_st.r[R_PC])) return;
      branch_w(d);
      return;
    }

    case 0x31: {  // BRW
      int16_t d = (int16_t)fetch16();
      branch_w(d);
      return;
    }

    case 0x78: {  // ASHL — study: Open SIMH VAX/vax_cpu.c
      Opnd cnt = decode_opnd(ACC_R, 1);
      Opnd src = decode_opnd(ACC_R, 4);
      Opnd dst = decode_opnd(ACC_W, 4);
      if (!cnt.ok || !src.ok || !dst.ok) return;
      uint8_t sc = (uint8_t)(cnt.value & 0xFF);
      int32_t op1 = (int32_t)src.value;
      int32_t r;
      bool ov = false;
      if (sc & 0x80u) {  // right shift
        unsigned temp = 0x100u - sc;
        if (temp > 31)
          r = (op1 < 0) ? (int32_t)0xFFFFFFFFu : 0;
        else
          r = op1 >> (int)temp;
      } else {
        if (sc > 31) {
          r = 0;
          ov = (op1 != 0);
        } else {
          r = (int32_t)(((uint32_t)op1 << sc) & 0xFFFFFFFFu);
          int32_t back = r >> (int)sc;  // arithmetic shift back (SIMH)
          ov = (op1 != back);
        }
      }
      store_opnd(dst, (uint32_t)r, 4);
      set_nz_long((uint32_t)r);
      g_st.psl &= ~PSL_C;
      if (ov) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0x79: {  // ASHQ — study: Open SIMH VAX/vax_fpa.c op_ashq
      Opnd cnt = decode_opnd(ACC_R, 1);
      if (!cnt.ok) return;
      QuadOp src = fetch_quad(ACC_R);
      if (!src.ok) return;
      QuadOp dst = fetch_quad(ACC_W);
      if (!dst.ok) return;
      uint8_t sc = (uint8_t)(cnt.value & 0xFF);
      uint64_t u = ((uint64_t)src.hi << 32) | (uint64_t)src.lo;
      uint64_t ru;
      bool ov = false;
      if (sc & 0x80u) {
        // signed right: dp_rsh_s(&r, 0x100 - sc, sign)
        unsigned temp = 0x100u - sc;
        if (temp > 63)
          ru = (u & 0x8000000000000000ULL) ? ~0ULL : 0ULL;
        else
          ru = (uint64_t)((int64_t)u >> (int)temp);
      } else {
        // left: dp_lsh then overflow if arithmetic reshift != original
        if (sc > 63) {
          ru = 0;
          ov = (u != 0);
        } else {
          ru = (sc == 0) ? u : (u << sc);
          uint64_t back = (sc == 0) ? ru : (uint64_t)((int64_t)ru >> (int)sc);
          ov = (back != u);
        }
      }
      uint32_t lo = (uint32_t)ru;
      uint32_t hi = (uint32_t)(ru >> 32);
      store_quad(dst, lo, hi);
      // CC_IIZZ_Q: N from bit 63, Z if zero, V from ov, C clear
      if (hi & 0x80000000u) g_st.psl |= PSL_N; else g_st.psl &= ~PSL_N;
      if ((lo | hi) == 0) g_st.psl |= PSL_Z; else g_st.psl &= ~PSL_Z;
      g_st.psl &= ~PSL_C;
      if (ov) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0x7C: {  // CLRQ
      QuadOp d = fetch_quad(ACC_W);
      if (!d.ok) return;
      store_quad(d, 0, 0);
      set_nz_long(0);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x7D: {  // MOVQ
      QuadOp s = fetch_quad(ACC_R);
      QuadOp d = fetch_quad(ACC_W);
      if (!s.ok || !d.ok) return;
      store_quad(d, s.lo, s.hi);
      if (s.hi & 0x80000000u) g_st.psl |= PSL_N; else g_st.psl &= ~PSL_N;
      if ((s.lo | s.hi) == 0) g_st.psl |= PSL_Z; else g_st.psl &= ~PSL_Z;
      g_st.psl &= ~(PSL_V | PSL_C);
      return;
    }

    case 0x7E: {  // MOVAQ
      Opnd a = decode_opnd(ACC_A, 8);
      Opnd d = decode_opnd(ACC_W, 4);
      if (!a.ok || !d.ok) return;
      store_opnd(d, a.addr, 4);
      set_nz_long(a.addr);
      return;
    }

    case 0x7F: {  // PUSHAQ
      Opnd a = decode_opnd(ACC_A, 8);
      if (!a.ok) return;
      if (!stack_push32(a.addr)) return;
      set_nz_long(a.addr);
      return;
    }

    case 0x90: {  // MOVB
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_W, 1);
      if (!s.ok || !d.ok) return;
      store_opnd(d, s.value, 1);
      set_nz_byte((uint8_t)s.value);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x91: {  // CMPB
      Opnd s1 = decode_opnd(ACC_R, 1);
      Opnd s2 = decode_opnd(ACC_R, 1);
      if (!s1.ok || !s2.ok) return;
      uint8_t a = (uint8_t)s1.value;
      uint8_t b = (uint8_t)s2.value;
      uint8_t r = (uint8_t)(a - b);
      g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
      if (r == 0) g_st.psl |= PSL_Z;
      if (r & 0x80) g_st.psl |= PSL_N;
      if (a < b) g_st.psl |= PSL_C;
      if ((((a ^ b) & (a ^ r)) & 0x80) != 0) g_st.psl |= PSL_V;
      return;
    }

    case 0x94: {  // CLRB
      Opnd d = decode_opnd(ACC_W, 1);
      if (!d.ok) return;
      store_opnd(d, 0, 1);
      set_nz_byte(0);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x95: {  // TSTB
      Opnd s = decode_opnd(ACC_R, 1);
      if (!s.ok) return;
      set_nz_byte((uint8_t)s.value);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xB0: {  // MOVW
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_W, 2);
      if (!s.ok || !d.ok) return;
      store_opnd(d, s.value, 2);
      set_nz_word((uint16_t)s.value);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xB1: {  // CMPW
      Opnd s1 = decode_opnd(ACC_R, 2);
      Opnd s2 = decode_opnd(ACC_R, 2);
      if (!s1.ok || !s2.ok) return;
      uint16_t a = (uint16_t)s1.value;
      uint16_t b = (uint16_t)s2.value;
      uint16_t r = (uint16_t)(a - b);
      g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
      if (r == 0) g_st.psl |= PSL_Z;
      if (r & 0x8000) g_st.psl |= PSL_N;
      if (a < b) g_st.psl |= PSL_C;
      if ((((a ^ b) & (a ^ r)) & 0x8000) != 0) g_st.psl |= PSL_V;
      return;
    }

    case 0xB2: {  // MCOMW
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_W, 2);
      if (!s.ok || !d.ok) return;
      uint16_t r = (uint16_t)(~s.value);
      store_opnd(d, r, 2);
      set_nz_word(r);
      return;
    }

    case 0xB3: {  // BITW
      Opnd mask = decode_opnd(ACC_R, 2);
      Opnd src = decode_opnd(ACC_R, 2);
      if (!mask.ok || !src.ok) return;
      set_nz_word((uint16_t)(src.value & mask.value));
      return;
    }

    case 0xB4: {  // CLRW
      Opnd d = decode_opnd(ACC_W, 2);
      if (!d.ok) return;
      store_opnd(d, 0, 2);
      set_nz_word(0);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xB5: {  // TSTW
      Opnd s = decode_opnd(ACC_R, 2);
      if (!s.ok) return;
      set_nz_word((uint16_t)s.value);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xB6: {  // INCW
      Opnd d = decode_opnd(ACC_M, 2);
      if (!d.ok) return;
      uint16_t a = d.is_reg ? (uint16_t)g_st.r[d.reg] : mem_r16(d.addr);
      uint16_t r = (uint16_t)(a + 1);
      store_opnd(d, r, 2);
      set_nz_word(r);
      g_st.psl &= ~PSL_C;
      if (a == 0x7FFF) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      if (a == 0xFFFF) g_st.psl |= PSL_C;
      return;
    }

    case 0xB7: {  // DECW
      Opnd d = decode_opnd(ACC_M, 2);
      if (!d.ok) return;
      uint16_t a = d.is_reg ? (uint16_t)g_st.r[d.reg] : mem_r16(d.addr);
      uint16_t r = (uint16_t)(a - 1);
      store_opnd(d, r, 2);
      set_nz_word(r);
      if (a < 1) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if (a == 0x8000) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0xA0: {  // ADDW2
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_M, 2);
      if (!s.ok || !d.ok) return;
      uint16_t a = d.is_reg ? (uint16_t)g_st.r[d.reg] : mem_r16(d.addr);
      uint16_t b = (uint16_t)s.value;
      uint16_t r = (uint16_t)(a + b);
      store_opnd(d, r, 2);
      set_nz_word(r);
      if (r < a) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if (((~(a ^ b) & (a ^ r)) & 0x8000) != 0) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0xA1: {  // ADDW3
      Opnd s1 = decode_opnd(ACC_R, 2);
      Opnd s2 = decode_opnd(ACC_R, 2);
      Opnd d  = decode_opnd(ACC_W, 2);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint16_t a = (uint16_t)s1.value;
      uint16_t b = (uint16_t)s2.value;
      uint16_t r = (uint16_t)(a + b);
      store_opnd(d, r, 2);
      set_nz_word(r);
      if (r < a) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if (((~(a ^ b) & (a ^ r)) & 0x8000) != 0) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0xA2: {  // SUBW2
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_M, 2);
      if (!s.ok || !d.ok) return;
      uint16_t a = d.is_reg ? (uint16_t)g_st.r[d.reg] : mem_r16(d.addr);
      uint16_t b = (uint16_t)s.value;
      uint16_t r = (uint16_t)(a - b);
      store_opnd(d, r, 2);
      set_nz_word(r);
      if (a < b) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if ((((a ^ b) & (a ^ r)) & 0x8000) != 0) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0xA3: {  // SUBW3
      Opnd s1 = decode_opnd(ACC_R, 2);
      Opnd s2 = decode_opnd(ACC_R, 2);
      Opnd d  = decode_opnd(ACC_W, 2);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint16_t b = (uint16_t)s1.value;
      uint16_t a = (uint16_t)s2.value;
      uint16_t r = (uint16_t)(a - b);
      store_opnd(d, r, 2);
      set_nz_word(r);
      if (a < b) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if ((((a ^ b) & (a ^ r)) & 0x8000) != 0) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0xA8: {  // BISW2
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_M, 2);
      if (!s.ok || !d.ok) return;
      uint16_t a = d.is_reg ? (uint16_t)g_st.r[d.reg] : mem_r16(d.addr);
      uint16_t r = (uint16_t)(a | s.value);
      store_opnd(d, r, 2);
      set_nz_word(r);
      return;
    }

    case 0xA9: {  // BISW3
      Opnd s1 = decode_opnd(ACC_R, 2);
      Opnd s2 = decode_opnd(ACC_R, 2);
      Opnd d  = decode_opnd(ACC_W, 2);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint16_t r = (uint16_t)(s2.value | s1.value);
      store_opnd(d, r, 2);
      set_nz_word(r);
      return;
    }

    case 0xAA: {  // BICW2
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_M, 2);
      if (!s.ok || !d.ok) return;
      uint16_t a = d.is_reg ? (uint16_t)g_st.r[d.reg] : mem_r16(d.addr);
      uint16_t r = (uint16_t)(a & ~s.value);
      store_opnd(d, r, 2);
      set_nz_word(r);
      return;
    }

    case 0xAB: {  // BICW3
      Opnd s1 = decode_opnd(ACC_R, 2);
      Opnd s2 = decode_opnd(ACC_R, 2);
      Opnd d  = decode_opnd(ACC_W, 2);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint16_t r = (uint16_t)(s2.value & ~s1.value);
      store_opnd(d, r, 2);
      set_nz_word(r);
      return;
    }

    case 0xAC: {  // XORW2
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_M, 2);
      if (!s.ok || !d.ok) return;
      uint16_t a = d.is_reg ? (uint16_t)g_st.r[d.reg] : mem_r16(d.addr);
      uint16_t r = (uint16_t)(a ^ s.value);
      store_opnd(d, r, 2);
      set_nz_word(r);
      return;
    }

    case 0xAD: {  // XORW3
      Opnd s1 = decode_opnd(ACC_R, 2);
      Opnd s2 = decode_opnd(ACC_R, 2);
      Opnd d  = decode_opnd(ACC_W, 2);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint16_t r = (uint16_t)(s2.value ^ s1.value);
      store_opnd(d, r, 2);
      set_nz_word(r);
      return;
    }

    case 0x92: {  // MCOMB
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_W, 1);
      if (!s.ok || !d.ok) return;
      uint8_t r = (uint8_t)(~s.value);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      return;
    }

    case 0x93: {  // BITB
      Opnd mask = decode_opnd(ACC_R, 1);
      Opnd src = decode_opnd(ACC_R, 1);
      if (!mask.ok || !src.ok) return;
      set_nz_byte((uint8_t)(src.value & mask.value));
      return;
    }

    case 0x96: {  // INCB
      Opnd d = decode_opnd(ACC_M, 1);
      if (!d.ok) return;
      uint8_t a = d.is_reg ? (uint8_t)g_st.r[d.reg] : mem_r8(d.addr);
      uint8_t r = (uint8_t)(a + 1);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      if (a == 0xFF) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if (a == 0x7F) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0x97: {  // DECB
      Opnd d = decode_opnd(ACC_M, 1);
      if (!d.ok) return;
      uint8_t a = d.is_reg ? (uint8_t)g_st.r[d.reg] : mem_r8(d.addr);
      uint8_t r = (uint8_t)(a - 1);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      if (a < 1) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if (a == 0x80) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0x80: {  // ADDB2
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_M, 1);
      if (!s.ok || !d.ok) return;
      uint8_t a = d.is_reg ? (uint8_t)g_st.r[d.reg] : mem_r8(d.addr);
      uint8_t b = (uint8_t)s.value;
      uint8_t r = (uint8_t)(a + b);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      if (r < a) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if (((~(a ^ b) & (a ^ r)) & 0x80) != 0) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0x81: {  // ADDB3
      Opnd s1 = decode_opnd(ACC_R, 1);
      Opnd s2 = decode_opnd(ACC_R, 1);
      Opnd d  = decode_opnd(ACC_W, 1);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint8_t a = (uint8_t)s1.value;
      uint8_t b = (uint8_t)s2.value;
      uint8_t r = (uint8_t)(a + b);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      if (r < a) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if (((~(a ^ b) & (a ^ r)) & 0x80) != 0) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0x82: {  // SUBB2
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_M, 1);
      if (!s.ok || !d.ok) return;
      uint8_t a = d.is_reg ? (uint8_t)g_st.r[d.reg] : mem_r8(d.addr);
      uint8_t b = (uint8_t)s.value;
      uint8_t r = (uint8_t)(a - b);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      if (a < b) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if ((((a ^ b) & (a ^ r)) & 0x80) != 0) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0x83: {  // SUBB3
      Opnd s1 = decode_opnd(ACC_R, 1);
      Opnd s2 = decode_opnd(ACC_R, 1);
      Opnd d  = decode_opnd(ACC_W, 1);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint8_t b = (uint8_t)s1.value;
      uint8_t a = (uint8_t)s2.value;
      uint8_t r = (uint8_t)(a - b);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      if (a < b) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if ((((a ^ b) & (a ^ r)) & 0x80) != 0) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0x88: {  // BISB2
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_M, 1);
      if (!s.ok || !d.ok) return;
      uint8_t a = d.is_reg ? (uint8_t)g_st.r[d.reg] : mem_r8(d.addr);
      uint8_t r = (uint8_t)(a | s.value);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      return;
    }

    case 0x89: {  // BISB3
      Opnd s1 = decode_opnd(ACC_R, 1);
      Opnd s2 = decode_opnd(ACC_R, 1);
      Opnd d  = decode_opnd(ACC_W, 1);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint8_t r = (uint8_t)(s2.value | s1.value);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      return;
    }

    case 0x8A: {  // BICB2
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_M, 1);
      if (!s.ok || !d.ok) return;
      uint8_t a = d.is_reg ? (uint8_t)g_st.r[d.reg] : mem_r8(d.addr);
      uint8_t r = (uint8_t)(a & ~s.value);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      return;
    }

    case 0x8B: {  // BICB3
      Opnd s1 = decode_opnd(ACC_R, 1);
      Opnd s2 = decode_opnd(ACC_R, 1);
      Opnd d  = decode_opnd(ACC_W, 1);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint8_t r = (uint8_t)(s2.value & ~s1.value);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      return;
    }

    case 0x8C: {  // XORB2
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_M, 1);
      if (!s.ok || !d.ok) return;
      uint8_t a = d.is_reg ? (uint8_t)g_st.r[d.reg] : mem_r8(d.addr);
      uint8_t r = (uint8_t)(a ^ s.value);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      return;
    }

    case 0x8D: {  // XORB3
      Opnd s1 = decode_opnd(ACC_R, 1);
      Opnd s2 = decode_opnd(ACC_R, 1);
      Opnd d  = decode_opnd(ACC_W, 1);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint8_t r = (uint8_t)(s2.value ^ s1.value);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      return;
    }

    case 0x8E: {  // MNEGB
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_W, 1);
      if (!s.ok || !d.ok) return;
      uint8_t src = (uint8_t)s.value;
      uint8_t r = (uint8_t)(-(int8_t)src);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      if (src) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if (src == 0x80) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0xD0: {  // MOVL
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_W, 4);
      if (!s.ok || !d.ok) return;
      store_opnd(d, s.value, 4);
      set_nz_long(s.value);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xD4: {  // CLRL
      Opnd d = decode_opnd(ACC_W, 4);
      if (!d.ok) return;
      store_opnd(d, 0, 4);
      set_nz_long(0);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xD5: {  // TSTL
      Opnd s = decode_opnd(ACC_R, 4);
      if (!s.ok) return;
      set_nz_long(s.value);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xD6: {  // INCL
      Opnd d = decode_opnd(ACC_M, 4);
      if (!d.ok) return;
      uint32_t a = d.is_reg ? g_st.r[d.reg] : mem_r32(d.addr);
      uint32_t r = a + 1;
      store_opnd(d, r, 4);
      set_add_cc(a, 1, r);
      return;
    }

    case 0xD7: {  // DECL
      Opnd d = decode_opnd(ACC_M, 4);
      if (!d.ok) return;
      uint32_t a = d.is_reg ? g_st.r[d.reg] : mem_r32(d.addr);
      uint32_t r = a - 1;
      store_opnd(d, r, 4);
      set_sub_cc(a, 1, r);
      return;
    }

    case 0xD1: {  // CMPL
      Opnd s1 = decode_opnd(ACC_R, 4);
      Opnd s2 = decode_opnd(ACC_R, 4);
      if (!s1.ok || !s2.ok) return;
      uint32_t r = s1.value - s2.value;
      set_sub_cc(s1.value, s2.value, r);
      return;
    }

    case 0xD2: {  // MCOML
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_W, 4);
      if (!s.ok || !d.ok) return;
      uint32_t r = ~s.value;
      store_opnd(d, r, 4);
      set_nz_long(r);
      // C preserved (CC_IIZP)
      return;
    }

    case 0xD3: {  // BITL
      Opnd mask = decode_opnd(ACC_R, 4);
      Opnd src = decode_opnd(ACC_R, 4);
      if (!mask.ok || !src.ok) return;
      uint32_t r = src.value & mask.value;
      set_nz_long(r);
      // C preserved (CC_IIZP)
      return;
    }

    case 0xC0: {  // ADDL2
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_M, 4);
      if (!s.ok || !d.ok) return;
      uint32_t a = d.is_reg ? g_st.r[d.reg] : mem_r32(d.addr);
      uint32_t r = a + s.value;
      store_opnd(d, r, 4);
      set_add_cc(a, s.value, r);
      return;
    }

    case 0xC1: {  // ADDL3
      Opnd s1 = decode_opnd(ACC_R, 4);
      Opnd s2 = decode_opnd(ACC_R, 4);
      Opnd d  = decode_opnd(ACC_W, 4);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint32_t r = s1.value + s2.value;
      store_opnd(d, r, 4);
      set_add_cc(s1.value, s2.value, r);
      return;
    }

    case 0xC2: {  // SUBL2
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_M, 4);
      if (!s.ok || !d.ok) return;
      uint32_t a = d.is_reg ? g_st.r[d.reg] : mem_r32(d.addr);
      uint32_t r = a - s.value;
      store_opnd(d, r, 4);
      set_sub_cc(a, s.value, r);
      return;
    }

    case 0xC4: {  // MULL2 — study: Open SIMH VAX/vax_cpu.c MULL2
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_M, 4);
      if (!s.ok || !d.ok) return;
      uint32_t a = d.is_reg ? g_st.r[d.reg] : mem_r32(d.addr);
      int64_t prod = (int64_t)(int32_t)s.value * (int64_t)(int32_t)a;
      uint32_t r = (uint32_t)prod;
      int32_t rh = (int32_t)(prod >> 32);
      store_opnd(d, r, 4);
      set_nz_long(r);
      g_st.psl &= ~PSL_C;
      if (rh != ((r & 0x80000000u) ? (int32_t)-1 : 0))
        g_st.psl |= PSL_V;
      else
        g_st.psl &= ~PSL_V;
      return;
    }

    case 0xC5: {  // MULL3
      Opnd s1 = decode_opnd(ACC_R, 4);
      Opnd s2 = decode_opnd(ACC_R, 4);
      Opnd d  = decode_opnd(ACC_W, 4);
      if (!s1.ok || !s2.ok || !d.ok) return;
      int64_t prod = (int64_t)(int32_t)s1.value * (int64_t)(int32_t)s2.value;
      uint32_t r = (uint32_t)prod;
      int32_t rh = (int32_t)(prod >> 32);
      store_opnd(d, r, 4);
      set_nz_long(r);
      g_st.psl &= ~PSL_C;
      if (rh != ((r & 0x80000000u) ? (int32_t)-1 : 0))
        g_st.psl |= PSL_V;
      else
        g_st.psl &= ~PSL_V;
      return;
    }

    case 0xC6: {  // DIVL2 — study: Open SIMH VAX/vax_cpu.c DIVL2
      Opnd s = decode_opnd(ACC_R, 4);  // divisor
      Opnd d = decode_opnd(ACC_M, 4);  // dividend / dest
      if (!s.ok || !d.ok) return;
      uint32_t a = d.is_reg ? g_st.r[d.reg] : mem_r32(d.addr);
      uint32_t r;
      bool ov = false;
      if (s.value == 0) {
        r = a;
        ov = true;
        // DIVZRO trap not wired yet — set V and continue
      } else if (s.value == 0xFFFFFFFFu && a == 0x80000000u) {
        r = a;
        ov = true;
      } else {
        r = (uint32_t)((int32_t)a / (int32_t)s.value);
      }
      store_opnd(d, r, 4);
      set_nz_long(r);
      g_st.psl &= ~PSL_C;
      if (ov) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0xC7: {  // DIVL3
      Opnd s1 = decode_opnd(ACC_R, 4);  // divisor
      Opnd s2 = decode_opnd(ACC_R, 4);  // dividend
      Opnd d  = decode_opnd(ACC_W, 4);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint32_t r;
      bool ov = false;
      if (s1.value == 0) {
        r = s2.value;
        ov = true;
      } else if (s1.value == 0xFFFFFFFFu && s2.value == 0x80000000u) {
        r = s2.value;
        ov = true;
      } else {
        r = (uint32_t)((int32_t)s2.value / (int32_t)s1.value);
      }
      store_opnd(d, r, 4);
      set_nz_long(r);
      g_st.psl &= ~PSL_C;
      if (ov) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0xCA: {  // BICL2
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_M, 4);
      if (!s.ok || !d.ok) return;
      uint32_t a = d.is_reg ? g_st.r[d.reg] : mem_r32(d.addr);
      uint32_t r = a & ~s.value;
      store_opnd(d, r, 4);
      set_nz_long(r);
      // C preserved (CC_IIZP)
      return;
    }

    case 0xCB: {  // BICL3
      Opnd s1 = decode_opnd(ACC_R, 4);
      Opnd s2 = decode_opnd(ACC_R, 4);
      Opnd d  = decode_opnd(ACC_W, 4);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint32_t r = s2.value & ~s1.value;
      store_opnd(d, r, 4);
      set_nz_long(r);
      return;
    }

    case 0xC8: {  // BISL2
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_M, 4);
      if (!s.ok || !d.ok) return;
      uint32_t a = d.is_reg ? g_st.r[d.reg] : mem_r32(d.addr);
      uint32_t r = a | s.value;
      store_opnd(d, r, 4);
      set_nz_long(r);
      return;
    }

    case 0xC9: {  // BISL3
      Opnd s1 = decode_opnd(ACC_R, 4);
      Opnd s2 = decode_opnd(ACC_R, 4);
      Opnd d  = decode_opnd(ACC_W, 4);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint32_t r = s2.value | s1.value;
      store_opnd(d, r, 4);
      set_nz_long(r);
      return;
    }

    case 0xCC: {  // XORL2
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_M, 4);
      if (!s.ok || !d.ok) return;
      uint32_t a = d.is_reg ? g_st.r[d.reg] : mem_r32(d.addr);
      uint32_t r = a ^ s.value;
      store_opnd(d, r, 4);
      set_nz_long(r);
      return;
    }

    case 0xCD: {  // XORL3
      Opnd s1 = decode_opnd(ACC_R, 4);
      Opnd s2 = decode_opnd(ACC_R, 4);
      Opnd d  = decode_opnd(ACC_W, 4);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint32_t r = s2.value ^ s1.value;
      store_opnd(d, r, 4);
      set_nz_long(r);
      return;
    }

    case 0xCE: {  // MNEGL
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_W, 4);
      if (!s.ok || !d.ok) return;
      uint32_t r = (uint32_t)(-(int32_t)s.value);
      store_opnd(d, r, 4);
      set_nz_long(r);
      if (s.value) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if (s.value == 0x80000000u) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0xD8: {  // ADWC — dst = dst + src + C
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_M, 4);
      if (!s.ok || !d.ok) return;
      uint32_t op0 = s.value;
      uint32_t op1 = d.is_reg ? g_st.r[d.reg] : mem_r32(d.addr);
      uint32_t cin = (g_st.psl & PSL_C) ? 1u : 0u;
      uint32_t r = op1 + op0 + cin;
      store_opnd(d, r, 4);
      set_add_cc(op1, op0, r);
      // SIMH special case: carry out when result wraps back to op1 with nonzero addend
      if (r == op1 && op0) g_st.psl |= PSL_C;
      return;
    }

    case 0xD9: {  // SBWC — dst = dst - src - C
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_M, 4);
      if (!s.ok || !d.ok) return;
      uint32_t op0 = s.value;
      uint32_t op1 = d.is_reg ? g_st.r[d.reg] : mem_r32(d.addr);
      uint32_t cin = (g_st.psl & PSL_C) ? 1u : 0u;
      uint32_t r = op1 - op0 - cin;
      store_opnd(d, r, 4);
      set_sub_cc(op1, op0, r);
      // SIMH special case
      if (op0 == op1 && r) g_st.psl |= PSL_C;
      return;
    }

    case 0xDA: {  // MTPR
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_R, 4);  // IPR number
      if (!s.ok || !d.ok) return;
      ipr_write(d.value, s.value);
      set_nz_long(s.value);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xDB: {  // MFPR
      Opnd s = decode_opnd(ACC_R, 4);  // IPR number
      Opnd d = decode_opnd(ACC_W, 4);
      if (!s.ok || !d.ok) return;
      uint32_t v = ipr_read(s.value);
      store_opnd(d, v, 4);
      set_nz_long(v);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xDC: {  // MOVPSL
      Opnd d = decode_opnd(ACC_W, 4);
      if (!d.ok) return;
      store_opnd(d, g_st.psl, 4);
      return;
    }

    case 0xDD: {  // PUSHL
      Opnd s = decode_opnd(ACC_R, 4);
      if (!s.ok) return;
      if (!stack_push32(s.value)) return;
      set_nz_long(s.value);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x9A: {  // MOVZBL
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_W, 4);
      if (!s.ok || !d.ok) return;
      uint32_t r = s.value & 0xFF;
      store_opnd(d, r, 4);
      set_nz_long(r);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x9C: {  // ROTL — study: Open SIMH VAX/vax_cpu.c
      Opnd cnt = decode_opnd(ACC_R, 1);
      Opnd src = decode_opnd(ACC_R, 4);
      Opnd dst = decode_opnd(ACC_W, 4);
      if (!cnt.ok || !src.ok || !dst.ok) return;
      int32_t sc = (int8_t)(cnt.value & 0xFF);
      int j = (int)(sc % 32);
      if (j < 0) j += 32;
      uint32_t op1 = src.value;
      uint32_t r = j ? ((op1 << j) | (op1 >> (32 - j))) : op1;
      store_opnd(dst, r, 4);
      set_nz_long(r);
      g_st.psl &= ~PSL_V;  // C preserved (SIMH CC_IIZP)
      return;
    }

    case 0x3A:  // LOCC — SIMH op_locskp(skpc=0)
      op_locskp(false);
      return;

    case 0x3B:  // SKPC
      op_locskp(true);
      return;

    case 0x3C: {  // MOVZWL
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_W, 4);
      if (!s.ok || !d.ok) return;
      uint32_t r = s.value & 0xFFFF;
      store_opnd(d, r, 4);
      set_nz_long(r);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xC3: {  // SUBL3 — diff = minuend - subtrahend (op2 - op1)
      Opnd s1 = decode_opnd(ACC_R, 4);  // subtrahend
      Opnd s2 = decode_opnd(ACC_R, 4);  // minuend
      Opnd d  = decode_opnd(ACC_W, 4);
      if (!s1.ok || !s2.ok || !d.ok) return;
      uint32_t r = s2.value - s1.value;
      store_opnd(d, r, 4);
      set_sub_cc(s2.value, s1.value, r);
      return;
    }

    case 0x9E: {  // MOVAB
      Opnd a = decode_opnd(ACC_A, 1);
      Opnd d = decode_opnd(ACC_W, 4);
      if (!a.ok || !d.ok) return;
      store_opnd(d, a.addr, 4);
      set_nz_long(a.addr);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xDE: {  // MOVAL / MOVAD / MOVAF
      Opnd a = decode_opnd(ACC_A, 4);
      Opnd d = decode_opnd(ACC_W, 4);
      if (!a.ok || !d.ok) return;
      store_opnd(d, a.addr, 4);
      set_nz_long(a.addr);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x9F: {  // PUSHAB
      Opnd a = decode_opnd(ACC_A, 1);
      if (!a.ok) return;
      if (!stack_push32(a.addr)) return;
      set_nz_long(a.addr);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xDF: {  // PUSHAL
      Opnd a = decode_opnd(ACC_A, 4);
      if (!a.ok) return;
      if (!stack_push32(a.addr)) return;
      set_nz_long(a.addr);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xE8: {  // BLBS
      Opnd s = decode_opnd(ACC_R, 4);
      int8_t d = (int8_t)fetch8();
      if (!s.ok) return;
      if (s.value & 1u) branch_b(d);
      return;
    }

    case 0xE9: {  // BLBC
      Opnd s = decode_opnd(ACC_R, 4);
      int8_t d = (int8_t)fetch8();
      if (!s.ok) return;
      if (!(s.value & 1u)) branch_b(d);
      return;
    }

    case 0xBA: {  // POPR — VARM: bits 0–11; stop on abort before SP moves
      Opnd m = decode_opnd(ACC_R, 2);
      if (!m.ok) return;
      uint16_t mask = (uint16_t)m.value;
      int nreg = 0;
      for (int i = 0; i <= 11; i++) {
        if (mask & (1u << i)) {
          uint32_t v = mem_r32(g_st.r[R_SP]);
          if (g_mmgt_abort) return;
          g_st.r[i] = v;
          g_st.r[R_SP] += 4;
          nreg++;
        }
      }
      if (g_popr_log_left && vax_mmu::mapen() && psl_cur() == 0 &&
          (mask & 0x800u) != 0) {
        g_popr_log_left--;
        LOG("boot: POPR mask=%03X nreg=%d PC=%08X SP=%08X R11=%08X",
            (unsigned)mask, nreg, (unsigned)g_last_op_pc,
            (unsigned)g_st.r[R_SP], (unsigned)g_st.r[11]);
      }
      if (g_xtransl_log_left && in_xtransl_v(g_last_op_pc) &&
          psl_cur() == 0 && mask == 0x3Fu && nreg == 6) {
        g_xtransl_log_left--;
        LOG("boot: POPR Xtransl nreg=6 PC=%08X SP=%08X tos=%08X +4=%08X +8=%08X +12=%08X",
            (unsigned)g_last_op_pc, (unsigned)g_st.r[R_SP],
            (unsigned)peek_va32(g_st.r[R_SP]),
            (unsigned)peek_va32(g_st.r[R_SP] + 4),
            (unsigned)peek_va32(g_st.r[R_SP] + 8),
            (unsigned)peek_va32(g_st.r[R_SP] + 12));
      }
      return;
    }

    case 0xBB: {  // PUSHR
      Opnd m = decode_opnd(ACC_R, 2);
      if (!m.ok) return;
      uint16_t mask = (uint16_t)m.value;
      uint32_t tsp = g_st.r[R_SP];
      for (int i = 11; i >= 0; i--) {
        if (mask & (1u << i)) {
          tsp -= 4;
          mem_w32(tsp, g_st.r[i]);
          if (g_mmgt_abort) return;
        }
      }
      g_st.r[R_SP] = tsp;
      return;
    }

    case 0xEA: {  // FFS pos.rl, size.rb, base.vb, dst.wl
      Opnd pos = decode_opnd(ACC_R, 4);
      Opnd size = decode_opnd(ACC_R, 1);
      VField base = decode_vfield_base();
      Opnd dst = decode_opnd(ACC_W, 4);
      if (!pos.ok || !size.ok || !base.ok || !dst.ok) return;
      uint32_t field = vfield_extract(pos.value, size.value & 0xFF, base);
      if (g_st.fault) return;
      uint32_t off = find_first_set(field, size.value & 0xFF);
      store_opnd(dst, pos.value + off, 4);
      g_st.psl &= ~(PSL_N | PSL_V | PSL_C);
      if (field == 0) g_st.psl |= PSL_Z; else g_st.psl &= ~PSL_Z;
      return;
    }

    case 0xEB: {  // FFC
      Opnd pos = decode_opnd(ACC_R, 4);
      Opnd size = decode_opnd(ACC_R, 1);
      VField base = decode_vfield_base();
      Opnd dst = decode_opnd(ACC_W, 4);
      if (!pos.ok || !size.ok || !base.ok || !dst.ok) return;
      uint32_t sz = size.value & 0xFF;
      uint32_t field = vfield_extract(pos.value, sz, base);
      if (g_st.fault) return;
      field ^= bitfield_mask(sz);
      uint32_t off = find_first_set(field, sz);
      store_opnd(dst, pos.value + off, 4);
      g_st.psl &= ~(PSL_N | PSL_V | PSL_C);
      if (field == 0) g_st.psl |= PSL_Z; else g_st.psl &= ~PSL_Z;
      return;
    }

    case 0xEE: {  // EXTV — signed extract
      Opnd pos = decode_opnd(ACC_R, 4);
      Opnd size = decode_opnd(ACC_R, 1);
      VField base = decode_vfield_base();
      Opnd dst = decode_opnd(ACC_W, 4);
      if (!pos.ok || !size.ok || !base.ok || !dst.ok) return;
      uint32_t sz = size.value & 0xFF;
      uint32_t r = vfield_extract(pos.value, sz, base);
      if (g_st.fault) return;
      if (sz && (r & (1u << (sz - 1))))
        r |= ~bitfield_mask(sz);
      store_opnd(dst, r, 4);
      set_nz_long(r);
      g_st.psl &= ~PSL_V;
      return;
    }

    case 0xEF: {  // EXTZV — zero-ext extract
      Opnd pos = decode_opnd(ACC_R, 4);
      Opnd size = decode_opnd(ACC_R, 1);
      VField base = decode_vfield_base();
      Opnd dst = decode_opnd(ACC_W, 4);
      if (!pos.ok || !size.ok || !base.ok || !dst.ok) return;
      uint32_t r = vfield_extract(pos.value, size.value & 0xFF, base);
      if (g_st.fault) return;
      store_opnd(dst, r, 4);
      set_nz_long(r);
      g_st.psl &= ~PSL_V;
      return;
    }

    case 0xF0: {  // INSV src.rl, pos.rl, size.rb, base.vb
      Opnd src = decode_opnd(ACC_R, 4);
      Opnd pos = decode_opnd(ACC_R, 4);
      Opnd size = decode_opnd(ACC_R, 1);
      VField base = decode_vfield_base();
      if (!src.ok || !pos.ok || !size.ok || !base.ok) return;
      vfield_insert(src.value, pos.value, size.value & 0xFF, base);
      return;
    }

    case 0xF4: {  // SOBGEQ — study: Open SIMH VAX/vax_cpu.c
      Opnd idx = decode_opnd(ACC_M, 4);
      int8_t d = (int8_t)fetch8();
      if (!idx.ok) return;
      uint32_t a = idx.is_reg ? g_st.r[idx.reg] : mem_r32(idx.addr);
      uint32_t r = a - 1;
      store_opnd(idx, r, 4);
      set_nz_long(r);  // C preserved
      if ((((a ^ 1u) & (a ^ r)) & 0x80000000u) != 0)
        g_st.psl |= PSL_V;
      if ((int32_t)r >= 0) branch_b(d);
      return;
    }

    case 0xF5: {  // SOBGTR
      Opnd idx = decode_opnd(ACC_M, 4);
      int8_t d = (int8_t)fetch8();
      if (!idx.ok) return;
      uint32_t a = idx.is_reg ? g_st.r[idx.reg] : mem_r32(idx.addr);
      // NetBSD delay() is `sobgtr rN, .` (disp -3) with cpu_vups*ms (1e6+).
      // Only collapse that empty self-loop. copyinstr is
      //   movb (r5)+,(r4)+; beql; sobgtr r3, loop   (disp -8, MAXPATHLEN=1024).
      // Capping every SOBGTR >1000 to 1 iteration made copyinstr miss the NUL
      // and return ENAMETOOLONG (63) for /sbin/init.
      if (g_boot_elf_active && vax_mmu::mapen() && a > 1000u &&
          d < 0 && d >= -4)
        a = 1u;
      uint32_t r = a - 1;
      store_opnd(idx, r, 4);
      set_nz_long(r);
      if ((((a ^ 1u) & (a ^ r)) & 0x80000000u) != 0)
        g_st.psl |= PSL_V;
      if ((int32_t)r > 0) branch_b(d);
      return;
    }

    case 0xF6: {  // CVTLB — study: Open SIMH VAX/vax_cpu.c
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_W, 1);
      if (!s.ok || !d.ok) return;
      int32_t v = (int32_t)s.value;
      uint8_t r = (uint8_t)(v & 0xFF);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      g_st.psl &= ~PSL_C;
      if (v > 127 || v < -128) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0xF7: {  // CVTLW
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_W, 2);
      if (!s.ok || !d.ok) return;
      int32_t v = (int32_t)s.value;
      uint16_t r = (uint16_t)(v & 0xFFFF);
      store_opnd(d, r, 2);
      set_nz_word(r);
      g_st.psl &= ~PSL_C;
      if (v > 32767 || v < -32768) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0x98: {  // CVTBL
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_W, 4);
      if (!s.ok || !d.ok) return;
      uint32_t r = (uint32_t)(int32_t)(int8_t)(s.value & 0xFF);
      store_opnd(d, r, 4);
      set_nz_long(r);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x99: {  // CVTBW
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_W, 2);
      if (!s.ok || !d.ok) return;
      uint16_t r = (uint16_t)(int16_t)(int8_t)(s.value & 0xFF);
      store_opnd(d, r, 2);
      set_nz_word(r);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x32: {  // CVTWL
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_W, 4);
      if (!s.ok || !d.ok) return;
      uint32_t r = (uint32_t)(int32_t)(int16_t)(s.value & 0xFFFF);
      store_opnd(d, r, 4);
      set_nz_long(r);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x33: {  // CVTWB
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_W, 1);
      if (!s.ok || !d.ok) return;
      int16_t v = (int16_t)(s.value & 0xFFFF);
      uint8_t r = (uint8_t)(v & 0xFF);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      g_st.psl &= ~PSL_C;
      if (v > 127 || v < -128) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0xFB: {  // CALLS — SIMH/VAX frame with condition-handler longword at (FP)
      Opnd n = decode_opnd(ACC_R, 4);
      Opnd dst = decode_opnd(ACC_A, 1);
      if (!n.ok || !dst.ok || g_mmgt_abort) return;
      uint32_t narg = n.value & 0xFF;
      uint32_t entry = dst.addr;
      if (!g_logged_user_calls && psl_cur() == 3u &&
          (entry & 0x80000000u) == 0u && narg >= 2u) {
        g_logged_user_calls = true;
        uint32_t a0 = mem_r32(g_st.r[R_SP]);
        uint32_t a1 = mem_r32(g_st.r[R_SP] + 4);
        LOG("boot: CALLS user entry=%08X narg=%u R0=%08X R10=%08X 0(sp)=%08X 4(sp)=%08X AP=%08X FP=%08X SP=%08X",
            (unsigned)entry, (unsigned)narg, (unsigned)g_st.r[0],
            (unsigned)g_st.r[10], (unsigned)a0, (unsigned)a1,
            (unsigned)g_st.r[R_AP], (unsigned)g_st.r[R_FP],
            (unsigned)g_st.r[R_SP]);
        if (g_mmgt_abort) return;
      }
      // Only the kernel handoff. putchar/getsecs CALLS flooded USB-serial
      // and stalled the autoboot countdown (one log line per character).
      if (g_boot_elf_active && narg == 3 && (entry & 0x80000000u) &&
          !vax_mmu::mapen())
        LOG("boot: CALLS kernel entry=0x%08X narg=3 AP=%08X SP=%08X R6=%08X R9=%08X R11=%08X",
            (unsigned)entry, (unsigned)g_st.r[R_AP], (unsigned)g_st.r[R_SP],
            (unsigned)g_st.r[6], (unsigned)g_st.r[9], (unsigned)g_st.r[11]);
      // locore uses R9 as COMPAT MARK_END. 0 or KERNBASE makes Sysmap land
      // on the kernel image; pmap_bootstrap memset(Sysmap) then wipes text
      // (HALT at a BICL3 that has become 0x00).
      if (g_boot_elf_active && narg == 3 && entry == 0x80000584u) {
        uint32_t need = g_kernel_load_end;
        if (need < 0x80400000u)
          need = 0x80400000u;  // /netbsd filesz ~3.5 MB + BSS slack
        need = (need + 511u) & ~511u;
        if (g_st.r[9] < 0x80010000u) {
          LOG("boot: plant R9 MARK_END 0x%08X (was 0x%08X, load_end 0x%08X)",
              (unsigned)need, (unsigned)g_st.r[9], (unsigned)g_kernel_load_end);
          g_st.r[9] = need;
        }
        // /boot MSCP CSR is 0x20001C68; kernel uba maps 0172150 → 0x20001468.
        // Plant only the CALLS prpb. R11 is boothowto (3); KERNBASE is the
        // SCB until scb_init(). pmap copies the uarea RPB onto PA 0 after.
        if (!g_planted_kernel_csr && g_ram) {
          uint32_t prpb = mem_r32(g_st.r[R_SP]);
          plant_rpb_csrphy(prpb);
        }
      }
      // machdep_start: calls $0, niclose then mtpr $0x1f,$IPL. Only skip
      // that site — 0xC9CA is a different /boot helper.
      if (vax_boot::is_niclose(entry) && narg == 0 &&
          mem_r8(g_st.r[R_PC]) == 0xDAu &&
          mem_r8(g_st.r[R_PC] + 1) == 0x1Fu &&
          mem_r8(g_st.r[R_PC] + 2) == 0x12u) {
        LOG("boot: niclose elided (narg=%u) -> continue machdep_start",
            (unsigned)narg);
        g_st.psl = (g_st.psl & ~PSL_IPL_MASK) | (31u << PSL_IPL_SHIFT);
        return;
      }
      // Entry mask lives on the callee's first page. Demand-paging ACV must
      // leave raise_mmgt's PC/PSL/SP alone — committing entry+2 here skipped
      // Xaccess_v and built a CALLS frame on KSP (AP==FP, relocbase 0).
      uint16_t mask = mem_r16(entry);
      if (g_mmgt_abort) return;
      const uint32_t narg_addr = g_st.r[R_SP] - 4u;
      mem_w32(narg_addr, narg);
      if (g_mmgt_abort) return;
      uint32_t new_ap = narg_addr;
      uint32_t spa = narg_addr & 3u;
      uint32_t tsp = narg_addr & ~3u;
      for (int i = 11; i >= 0; i--) {
        if (mask & (1u << i)) {
          tsp -= 4;
          mem_w32(tsp, g_st.r[i]);
          if (g_mmgt_abort) return;
        }
      }
      mem_w32(tsp - 4, g_st.r[R_PC]);
      mem_w32(tsp - 8, g_st.r[R_FP]);
      mem_w32(tsp - 12, g_st.r[R_AP]);
      uint32_t wd = (spa << 30) | (1u << 29) |
                    (((uint32_t)mask & 0x0FFFu) << 16) | (g_st.psl & 0xFFE0u);
      mem_w32(tsp - 16, wd);
      mem_w32(tsp - 20, 0);  // condition handler
      if (g_mmgt_abort) return;
      g_st.r[R_AP] = new_ap;
      g_st.r[R_SP] = g_st.r[R_FP] = tsp - 20;
      g_st.r[R_PC] = entry + 2;
      return;
    }

    case 0xFA: {  // CALLG
      Opnd arglist = decode_opnd(ACC_A, 4);
      Opnd dst = decode_opnd(ACC_A, 1);
      if (!arglist.ok || !dst.ok || g_mmgt_abort) return;
      uint32_t new_ap = arglist.addr;
      uint32_t entry = dst.addr;
      uint16_t mask = mem_r16(entry);
      if (g_mmgt_abort) return;
      uint32_t spa = g_st.r[R_SP] & 3u;
      uint32_t tsp = g_st.r[R_SP] & ~3u;
      for (int i = 11; i >= 0; i--) {
        if (mask & (1u << i)) {
          tsp -= 4;
          mem_w32(tsp, g_st.r[i]);
          if (g_mmgt_abort) return;
        }
      }
      mem_w32(tsp - 4, g_st.r[R_PC]);
      mem_w32(tsp - 8, g_st.r[R_FP]);
      mem_w32(tsp - 12, g_st.r[R_AP]);
      uint32_t wd = (spa << 30) |
                    (((uint32_t)mask & 0x0FFFu) << 16) | (g_st.psl & 0xFFE0u);
      mem_w32(tsp - 16, wd);
      mem_w32(tsp - 20, 0);
      if (g_mmgt_abort) return;
      g_st.r[R_AP] = new_ap;
      g_st.r[R_SP] = g_st.r[R_FP] = tsp - 20;
      g_st.r[R_PC] = entry + 2;
      return;
    }

    case 0x03:  // BPT — treat as HALT for now (debugger trap)
      LOG("VAX BPT at PC=%08X", (unsigned)(g_st.r[R_PC] - 1));
      g_st.halt = true;
      g_running = false;
      return;

    case 0x06: {  // LDPCTX — SIMH vax_cpu1.c op_ldpctx (physical PCB)
      if ((g_st.psl & PSL_CUR_MASK) != 0) {
        note_fault(3, "ldpctx-mode");
        return;
      }
      uint32_t pcbpa = g_st.pcbb & 0x3FFFFFFFu;
      if (pcbpa == 0 || !pa_ok(pcbpa, 96)) {
        note_fault(2, "ldpctx", pcbpa);
        return;
      }
      g_stk[0] = phys_r32(pcbpa + 0);
      g_stk[1] = phys_r32(pcbpa + 4);
      g_stk[2] = phys_r32(pcbpa + 8);
      g_stk[3] = phys_r32(pcbpa + 12);
      for (int i = 0; i < 12; i++)
        g_st.r[i] = phys_r32(pcbpa + 16 + (uint32_t)i * 4u);
      g_st.r[R_AP] = phys_r32(pcbpa + 64);
      g_st.r[R_FP] = phys_r32(pcbpa + 68);
      uint32_t newpc = phys_r32(pcbpa + 72);
      uint32_t newpsl = phys_r32(pcbpa + 76);
      vax_mmu::set_ipr(8, phys_r32(pcbpa + 80));   // P0BR
      vax_mmu::set_ipr(9, phys_r32(pcbpa + 84));   // P0LR (+ AST)
      vax_mmu::set_ipr(10, phys_r32(pcbpa + 88));  // P1BR
      vax_mmu::set_ipr(11, phys_r32(pcbpa + 92));  // P1LR
      if (psl_is())
        g_isp = g_st.r[R_SP];
      g_st.psl &= ~PSL_IS;
      g_st.r[R_SP] = g_stk[0] - 8u;
      mem_w32(g_st.r[R_SP], newpc);
      mem_w32(g_st.r[R_SP] + 4, newpsl);
      return;
    }

    case 0x07: {  // SVPCTX — pop PC/PSL, switch to ISP, save full PCB
      if ((g_st.psl & PSL_CUR_MASK) != 0) {
        note_fault(3, "svpctx-mode");
        return;
      }
      uint32_t pcbpa = g_st.pcbb & 0x3FFFFFFFu;
      if (pcbpa == 0 || !pa_ok(pcbpa, 80)) {
        note_fault(2, "svpctx", pcbpa);
        return;
      }
      uint32_t savpc = mem_r32(g_st.r[R_SP]);
      uint32_t savpsl = mem_r32(g_st.r[R_SP] + 4);
      if (psl_is()) {
        g_st.r[R_SP] += 8;
      } else {
        g_stk[0] = g_st.r[R_SP] + 8;  // KSP after pop
        g_st.r[R_SP] = g_isp;
        if ((g_st.psl & PSL_IPL_MASK) == 0)
          g_st.psl |= (1u << PSL_IPL_SHIFT);
        g_st.psl |= PSL_IS;
      }
      phys_w32(pcbpa + 0, g_stk[0]);
      phys_w32(pcbpa + 4, g_stk[1]);
      phys_w32(pcbpa + 8, g_stk[2]);
      phys_w32(pcbpa + 12, g_stk[3]);
      for (int i = 0; i < 12; i++)
        phys_w32(pcbpa + 16 + (uint32_t)i * 4u, g_st.r[i]);
      phys_w32(pcbpa + 64, g_st.r[R_AP]);
      phys_w32(pcbpa + 68, g_st.r[R_FP]);
      phys_w32(pcbpa + 72, savpc);
      phys_w32(pcbpa + 76, savpsl);
      return;
    }

    case 0x0A: {  // INDEX subscript.rl,low.rl,high.rl,size.rl,indexin.rl,ret.wl
      Opnd sub = decode_opnd(ACC_R, 4);
      Opnd low = decode_opnd(ACC_R, 4);
      Opnd high = decode_opnd(ACC_R, 4);
      Opnd size = decode_opnd(ACC_R, 4);
      Opnd idxin = decode_opnd(ACC_R, 4);
      Opnd dst = decode_opnd(ACC_W, 4);
      if (!sub.ok || !low.ok || !high.ok || !size.ok || !idxin.ok || !dst.ok) return;
      if ((int32_t)sub.value < (int32_t)low.value || (int32_t)sub.value > (int32_t)high.value) {
        // subscript range trap — halt for visibility until trap delivery exists
        note_fault(3, "index");
        return;
      }
      uint32_t r = (sub.value + idxin.value) * size.value;
      store_opnd(dst, r, 4);
      set_nz_long(r);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x0C:  // PROBER
    case 0x0D: {  // PROBEW — simplified: succeed (Z=0) if length>0
      Opnd mode = decode_opnd(ACC_R, 1);
      Opnd len = decode_opnd(ACC_R, 2);
      Opnd base = decode_opnd(ACC_A, 1);
      if (!mode.ok || !len.ok || !base.ok) return;
      (void)mode;
      (void)base;
      g_st.psl &= ~(PSL_N | PSL_V | PSL_Z);
      if ((len.value & 0xFFFF) == 0) g_st.psl |= PSL_Z;  // lose if zero length
      // C preserved
      return;
    }

    case 0x0E: {  // INSQUE entry.ab, pred.ab
      Opnd ent = decode_opnd(ACC_A, 1);
      Opnd pred = decode_opnd(ACC_A, 1);
      if (!ent.ok || !pred.ok) return;
      uint32_t e = ent.addr, p = pred.addr;
      uint32_t s = mem_r32(p);
      mem_w32(e, s);
      mem_w32(e + 4, p);
      mem_w32(s + 4, e);
      mem_w32(p, e);
      set_cmp_long(s, p);
      return;
    }

    case 0x0F: {  // REMQUE entry.ab, dst.wl
      Opnd ent = decode_opnd(ACC_A, 1);
      Opnd dst = decode_opnd(ACC_W, 4);
      if (!ent.ok || !dst.ok) return;
      uint32_t e = ent.addr;
      uint32_t s = mem_r32(e);
      uint32_t p = mem_r32(e + 4);
      set_cmp_long(s, p);
      if (e != p) {
        mem_w32(p, s);
        mem_w32(s + 4, p);
      } else {
        g_st.psl |= PSL_V;
      }
      store_opnd(dst, e, 4);
      return;
    }

    case 0x58: {  // ADAWI add.rw, dst.mw
      Opnd src = decode_opnd(ACC_R, 2);
      Opnd dst = decode_opnd(ACC_M, 2);
      if (!src.ok || !dst.ok) return;
      uint16_t a = (uint16_t)(src.value & 0xFFFF);
      uint16_t b = dst.is_reg ? (uint16_t)(g_st.r[dst.reg] & 0xFFFF) : mem_r16(dst.addr);
      if (!dst.is_reg && (dst.addr & 1u)) {
        note_fault(3, "adawi");
        return;
      }
      uint16_t r = (uint16_t)(a + b);
      store_opnd(dst, r, 2);
      g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
      if (r == 0) g_st.psl |= PSL_Z;
      if (r & 0x8000) g_st.psl |= PSL_N;
      if (r < a) g_st.psl |= PSL_C;
      if (((~(a ^ b) & (a ^ r)) & 0x8000) != 0) g_st.psl |= PSL_V;
      return;
    }

    case 0x5C: {  // INSQHI entry.ab, header.aq
      Opnd ent = decode_opnd(ACC_A, 1);
      Opnd hdr = decode_opnd(ACC_A, 8);
      if (!ent.ok || !hdr.ok) return;
      uint32_t h = hdr.addr, d = ent.addr;
      if ((h == d) || ((h | d) & 7u)) {
        note_fault(3, "insqhi");
        return;
      }
      uint32_t a = mem_r32(h);
      if (a & 6u) {
        note_fault(3, "insqhi");
        return;
      }
      if (a & 1u) {
        g_st.psl = (g_st.psl & ~(PSL_N | PSL_Z | PSL_V)) | PSL_C;
        return;
      }
      mem_w32(h, a | 1u);
      a = a + h;
      mem_w32(a + 4, d - a);
      mem_w32(d, a - d);
      mem_w32(d + 4, h - d);
      mem_w32(h, d - h);
      g_st.psl &= ~(PSL_N | PSL_V | PSL_C);
      if (a == h) g_st.psl |= PSL_Z; else g_st.psl &= ~PSL_Z;
      return;
    }

    case 0x5D: {  // INSQTI
      Opnd ent = decode_opnd(ACC_A, 1);
      Opnd hdr = decode_opnd(ACC_A, 8);
      if (!ent.ok || !hdr.ok) return;
      uint32_t h = hdr.addr, d = ent.addr;
      if ((h == d) || ((h | d) & 7u)) {
        note_fault(3, "insqti");
        return;
      }
      uint32_t a = mem_r32(h);
      if (a == 0) {
        // empty → INSQHI path
        mem_w32(h, a | 1u);
        a = a + h;
        mem_w32(a + 4, d - a);
        mem_w32(d, a - d);
        mem_w32(d + 4, h - d);
        mem_w32(h, d - h);
        g_st.psl &= ~(PSL_N | PSL_V | PSL_C);
        g_st.psl |= PSL_Z;
        return;
      }
      if (a & 6u) {
        note_fault(3, "insqti");
        return;
      }
      if (a & 1u) {
        g_st.psl = (g_st.psl & ~(PSL_N | PSL_Z | PSL_V)) | PSL_C;
        return;
      }
      mem_w32(h, a | 1u);
      uint32_t c = mem_r32(h + 4) + h;
      if (c & 7u) {
        mem_w32(h, a);
        note_fault(3, "insqti");
        return;
      }
      mem_w32(c, d - c);
      mem_w32(d, h - d);
      mem_w32(d + 4, c - d);
      mem_w32(h + 4, d - h);
      mem_w32(h, a);
      g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
      return;
    }

    case 0x5E: {  // REMQHI header.aq, dst.wl
      Opnd hdr = decode_opnd(ACC_A, 8);
      Opnd dst = decode_opnd(ACC_W, 4);
      if (!hdr.ok || !dst.ok) return;
      uint32_t h = hdr.addr;
      if (h & 7u) {
        note_fault(3, "remqhi");
        return;
      }
      uint32_t a_rel = mem_r32(h);
      if (a_rel & 6u) {
        note_fault(3, "remqhi");
        return;
      }
      if (a_rel & 1u) {
        g_st.psl = (g_st.psl & ~(PSL_N | PSL_Z | PSL_V)) | PSL_C;
        return;
      }
      if (a_rel == 0) {
        g_st.psl = (g_st.psl & ~(PSL_N | PSL_C)) | PSL_Z | PSL_V;
        store_opnd(dst, 0, 4);
        return;
      }
      mem_w32(h, a_rel | 1u);
      uint32_t a = a_rel + h;
      uint32_t b_rel = mem_r32(a);
      uint32_t b = b_rel + a;
      mem_w32(b + 4, h - b);
      mem_w32(h, b - h);
      store_opnd(dst, a, 4);
      g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
      if (b == h) g_st.psl |= PSL_Z;
      return;
    }

    case 0x3D: {  // ACBW limit.rw, add.rw, index.mw, displ.bw
      Opnd lim = decode_opnd(ACC_R, 2);
      Opnd add = decode_opnd(ACC_R, 2);
      Opnd idx = decode_opnd(ACC_M, 2);
      int16_t d = (int16_t)fetch16();
      if (!lim.ok || !add.ok || !idx.ok) return;
      uint16_t ix = idx.is_reg ? (uint16_t)(g_st.r[idx.reg] & 0xFFFF) : mem_r16(idx.addr);
      uint16_t ad = (uint16_t)(add.value & 0xFFFF);
      uint16_t r = (uint16_t)(ix + ad);
      store_opnd(idx, r, 2);
      set_nz_word(r);
      if (((~(ix ^ ad) & (ix ^ r)) & 0x8000) != 0) g_st.psl |= PSL_V;
      int16_t sr = (int16_t)r, sl = (int16_t)(lim.value & 0xFFFF);
      if ((ad & 0x8000) ? (sr >= sl) : (sr <= sl)) branch_w(d);
      return;
    }

    case 0x3E: {  // MOVAW
      Opnd src = decode_opnd(ACC_A, 2);
      Opnd dst = decode_opnd(ACC_W, 4);
      if (!src.ok || !dst.ok) return;
      store_opnd(dst, src.addr, 4);
      set_nz_long(src.addr);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x3F: {  // PUSHAW
      Opnd src = decode_opnd(ACC_A, 2);
      if (!src.ok) return;
      if (!stack_push32(src.addr)) return;
      set_nz_long(src.addr);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x7A: {  // EMUL mplr.rl, mpcn.rl, add.rl, dst.wq
      Opnd mplr = decode_opnd(ACC_R, 4);
      Opnd mpcn = decode_opnd(ACC_R, 4);
      Opnd add = decode_opnd(ACC_R, 4);
      QuadOp dst = fetch_quad(ACC_W);
      if (!mplr.ok || !mpcn.ok || !add.ok || !dst.ok) return;
      int64_t p = (int64_t)(int32_t)mplr.value * (int64_t)(int32_t)mpcn.value;
      uint32_t lo = (uint32_t)p;
      uint32_t hi = (uint32_t)(p >> 32);
      uint32_t sum = lo + add.value;
      hi = hi + (sum < lo ? 1u : 0u) - ((add.value & 0x80000000u) ? 1u : 0u);
      store_quad(dst, sum, hi);
      g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
      if (hi & 0x80000000u) g_st.psl |= PSL_N;
      if ((sum | hi) == 0) g_st.psl |= PSL_Z;
      return;
    }

    case 0x7B: {  // EDIV dvr.rl, dvd.rq, quo.wl, rem.wl
      Opnd dvr = decode_opnd(ACC_R, 4);
      QuadOp dvd = fetch_quad(ACC_R);
      Opnd quo = decode_opnd(ACC_W, 4);
      Opnd rem = decode_opnd(ACC_W, 4);
      if (!dvr.ok || !dvd.ok || !quo.ok || !rem.ok) return;
      uint32_t q = dvd.lo;
      uint32_t r = 0;
      uint32_t flg = 0;
      if (dvr.value == 0) {
        flg = PSL_V;
        q = dvd.lo;
        r = 0;
      } else {
        int64_t dividend = ((int64_t)(int32_t)dvd.hi << 32) | (uint64_t)dvd.lo;
        int64_t divisor = (int32_t)dvr.value;
        int64_t qq = dividend / divisor;
        int64_t rr = dividend % divisor;
        if (qq != (int32_t)qq) {
          flg = PSL_V;
          q = dvd.lo;
          r = 0;
        } else {
          q = (uint32_t)qq;
          r = (uint32_t)rr;
        }
      }
      store_opnd(quo, q, 4);
      store_opnd(rem, r, 4);
      set_nz_long(q);
      g_st.psl &= ~PSL_C;
      if (flg) g_st.psl |= PSL_V;
      return;
    }

    case 0x84: {  // MULB2
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_M, 1);
      if (!s.ok || !d.ok) return;
      int8_t a = (int8_t)(s.value & 0xFF);
      int8_t b = (int8_t)(d.is_reg ? (g_st.r[d.reg] & 0xFF) : mem_r8(d.addr));
      int16_t p = (int16_t)a * (int16_t)b;
      uint8_t r = (uint8_t)(p & 0xFF);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      g_st.psl &= ~PSL_C;
      if (p != (int8_t)p) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }
    case 0x85: {  // MULB3
      Opnd s1 = decode_opnd(ACC_R, 1);
      Opnd s2 = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_W, 1);
      if (!s1.ok || !s2.ok || !d.ok) return;
      int16_t p = (int16_t)(int8_t)(s1.value & 0xFF) * (int16_t)(int8_t)(s2.value & 0xFF);
      uint8_t r = (uint8_t)(p & 0xFF);
      store_opnd(d, r, 1);
      set_nz_byte(r);
      g_st.psl &= ~PSL_C;
      if (p != (int8_t)p) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }
    case 0x86: {  // DIVB2
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_M, 1);
      if (!s.ok || !d.ok) return;
      int8_t divisor = (int8_t)(s.value & 0xFF);
      int8_t dividend = (int8_t)(d.is_reg ? (g_st.r[d.reg] & 0xFF) : mem_r8(d.addr));
      if (divisor == 0) {
        note_fault(3, "divb");
        return;
      }
      int8_t r = (int8_t)(dividend / divisor);
      store_opnd(d, (uint8_t)r, 1);
      set_nz_byte((uint8_t)r);
      g_st.psl &= ~(PSL_C | PSL_V);
      return;
    }
    case 0x87: {  // DIVB3
      Opnd s1 = decode_opnd(ACC_R, 1);
      Opnd s2 = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_W, 1);
      if (!s1.ok || !s2.ok || !d.ok) return;
      int8_t divisor = (int8_t)(s1.value & 0xFF);
      int8_t dividend = (int8_t)(s2.value & 0xFF);
      if (divisor == 0) {
        note_fault(3, "divb");
        return;
      }
      int8_t r = (int8_t)(dividend / divisor);
      store_opnd(d, (uint8_t)r, 1);
      set_nz_byte((uint8_t)r);
      g_st.psl &= ~(PSL_C | PSL_V);
      return;
    }

    case 0x8F: {  // CASEB
      Opnd sel = decode_opnd(ACC_R, 1);
      Opnd base = decode_opnd(ACC_R, 1);
      Opnd lim = decode_opnd(ACC_R, 1);
      if (!sel.ok || !base.ok || !lim.ok) return;
      do_casex(sel.value & 0xFF, base.value & 0xFF, lim.value & 0xFF, 0xFFu);
      return;
    }

    case 0x9B: {  // MOVZBW
      Opnd s = decode_opnd(ACC_R, 1);
      Opnd d = decode_opnd(ACC_W, 2);
      if (!s.ok || !d.ok) return;
      uint16_t r = (uint16_t)(s.value & 0xFF);
      store_opnd(d, r, 2);
      set_nz_word(r);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0x9D: {  // ACBB
      Opnd lim = decode_opnd(ACC_R, 1);
      Opnd add = decode_opnd(ACC_R, 1);
      Opnd idx = decode_opnd(ACC_M, 1);
      int16_t d = (int16_t)fetch16();
      if (!lim.ok || !add.ok || !idx.ok) return;
      uint8_t ix = idx.is_reg ? (uint8_t)(g_st.r[idx.reg] & 0xFF) : mem_r8(idx.addr);
      uint8_t ad = (uint8_t)(add.value & 0xFF);
      uint8_t r = (uint8_t)(ix + ad);
      store_opnd(idx, r, 1);
      set_nz_byte(r);
      if (((~(ix ^ ad) & (ix ^ r)) & 0x80) != 0) g_st.psl |= PSL_V;
      int8_t sr = (int8_t)r, sl = (int8_t)(lim.value & 0xFF);
      if ((ad & 0x80) ? (sr >= sl) : (sr <= sl)) branch_w(d);
      return;
    }

    case 0xA4: {  // MULW2
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_M, 2);
      if (!s.ok || !d.ok) return;
      int32_t p = (int32_t)(int16_t)(s.value & 0xFFFF) *
                  (int32_t)(int16_t)(d.is_reg ? (g_st.r[d.reg] & 0xFFFF) : mem_r16(d.addr));
      uint16_t r = (uint16_t)(p & 0xFFFF);
      store_opnd(d, r, 2);
      set_nz_word(r);
      g_st.psl &= ~PSL_C;
      if (p != (int16_t)p) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }
    case 0xA5: {  // MULW3
      Opnd s1 = decode_opnd(ACC_R, 2);
      Opnd s2 = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_W, 2);
      if (!s1.ok || !s2.ok || !d.ok) return;
      int32_t p = (int32_t)(int16_t)(s1.value & 0xFFFF) * (int32_t)(int16_t)(s2.value & 0xFFFF);
      uint16_t r = (uint16_t)(p & 0xFFFF);
      store_opnd(d, r, 2);
      set_nz_word(r);
      g_st.psl &= ~PSL_C;
      if (p != (int16_t)p) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }
    case 0xA6: {  // DIVW2
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_M, 2);
      if (!s.ok || !d.ok) return;
      int16_t divisor = (int16_t)(s.value & 0xFFFF);
      int16_t dividend = (int16_t)(d.is_reg ? (g_st.r[d.reg] & 0xFFFF) : mem_r16(d.addr));
      if (divisor == 0) {
        note_fault(3, "divw");
        return;
      }
      int16_t r = (int16_t)(dividend / divisor);
      store_opnd(d, (uint16_t)r, 2);
      set_nz_word((uint16_t)r);
      g_st.psl &= ~(PSL_C | PSL_V);
      return;
    }
    case 0xA7: {  // DIVW3
      Opnd s1 = decode_opnd(ACC_R, 2);
      Opnd s2 = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_W, 2);
      if (!s1.ok || !s2.ok || !d.ok) return;
      int16_t divisor = (int16_t)(s1.value & 0xFFFF);
      int16_t dividend = (int16_t)(s2.value & 0xFFFF);
      if (divisor == 0) {
        note_fault(3, "divw");
        return;
      }
      int16_t r = (int16_t)(dividend / divisor);
      store_opnd(d, (uint16_t)r, 2);
      set_nz_word((uint16_t)r);
      g_st.psl &= ~(PSL_C | PSL_V);
      return;
    }

    case 0xAE: {  // MNEGW
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_W, 2);
      if (!s.ok || !d.ok) return;
      uint16_t src = (uint16_t)(s.value & 0xFFFF);
      uint16_t r = (uint16_t)(-(int16_t)src);
      store_opnd(d, r, 2);
      set_nz_word(r);
      if (src) g_st.psl |= PSL_C; else g_st.psl &= ~PSL_C;
      if (src == 0x8000) g_st.psl |= PSL_V; else g_st.psl &= ~PSL_V;
      return;
    }

    case 0xAF: {  // CASEW
      Opnd sel = decode_opnd(ACC_R, 2);
      Opnd base = decode_opnd(ACC_R, 2);
      Opnd lim = decode_opnd(ACC_R, 2);
      if (!sel.ok || !base.ok || !lim.ok) return;
      do_casex(sel.value & 0xFFFF, base.value & 0xFFFF, lim.value & 0xFFFF, 0xFFFFu);
      return;
    }

    case 0xB8: {  // BISPSW mask.rw
      Opnd m = decode_opnd(ACC_R, 2);
      if (!m.ok || g_mmgt_abort) return;
      if (m.value & PSW_MBZ) {
        raise_exception(SCB_RESOP);
        return;
      }
      g_st.psl |= (m.value & 0xFF);
      return;
    }
    case 0xB9: {  // BICPSW
      Opnd m = decode_opnd(ACC_R, 2);
      if (!m.ok || g_mmgt_abort) return;
      if (m.value & PSW_MBZ) {
        raise_exception(SCB_RESOP);
        return;
      }
      g_st.psl &= ~(m.value & 0xFF);
      return;
    }

    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {  // CHMK/E/S/U
      Opnd arg = decode_opnd(ACC_R, 2);
      if (!arg.ok) return;
      do_chmx(op, arg.value);
      return;
    }

    case 0xCF: {  // CASEL
      Opnd sel = decode_opnd(ACC_R, 4);
      Opnd base = decode_opnd(ACC_R, 4);
      Opnd lim = decode_opnd(ACC_R, 4);
      if (!sel.ok || !base.ok || !lim.ok) return;
      do_casex(sel.value, base.value, lim.value, 0xFFFFFFFFu);
      return;
    }

    case 0xE0: {  // BBS
      Opnd pos = decode_opnd(ACC_R, 4);
      BbBase base = decode_bb_base();
      int8_t d = (int8_t)fetch8();
      if (!pos.ok || !base.ok) return;
      if (bb_get(pos.value, base)) branch_b(d);
      return;
    }
    case 0xE1: {  // BBC
      Opnd pos = decode_opnd(ACC_R, 4);
      BbBase base = decode_bb_base();
      int8_t d = (int8_t)fetch8();
      if (!pos.ok || !base.ok) return;
      if (!bb_get(pos.value, base)) branch_b(d);
      return;
    }
    case 0xE2:  // BBSS
    case 0xE6: {  // BBSSI
      Opnd pos = decode_opnd(ACC_R, 4);
      BbBase base = decode_bb_base();
      int8_t d = (int8_t)fetch8();
      if (!pos.ok || !base.ok) return;
      if (bb_set(pos.value, base, 1)) branch_b(d);
      return;
    }
    case 0xE3: {  // BBCS
      Opnd pos = decode_opnd(ACC_R, 4);
      BbBase base = decode_bb_base();
      int8_t d = (int8_t)fetch8();
      if (!pos.ok || !base.ok) return;
      if (!bb_set(pos.value, base, 1)) branch_b(d);
      return;
    }
    case 0xE4: {  // BBSC
      Opnd pos = decode_opnd(ACC_R, 4);
      BbBase base = decode_bb_base();
      int8_t d = (int8_t)fetch8();
      if (!pos.ok || !base.ok) return;
      if (bb_set(pos.value, base, 0)) branch_b(d);
      return;
    }
    case 0xE5:  // BBCC
    case 0xE7: {  // BBCCI
      Opnd pos = decode_opnd(ACC_R, 4);
      BbBase base = decode_bb_base();
      int8_t d = (int8_t)fetch8();
      if (!pos.ok || !base.ok) return;
      if (!bb_set(pos.value, base, 0)) branch_b(d);
      return;
    }

    case 0xEC: {  // CMPV
      Opnd pos = decode_opnd(ACC_R, 4);
      Opnd size = decode_opnd(ACC_R, 1);
      VField base = decode_vfield_base();
      Opnd src2 = decode_opnd(ACC_R, 4);
      if (!pos.ok || !size.ok || !base.ok || !src2.ok) return;
      uint32_t sz = size.value & 0xFF;
      uint32_t r = vfield_extract(pos.value, sz, base);
      if (g_st.fault) return;
      if (sz && (r & (1u << (sz - 1)))) r |= ~bitfield_mask(sz);
      set_cmp_long(r, src2.value);
      return;
    }
    case 0xED: {  // CMPZV
      Opnd pos = decode_opnd(ACC_R, 4);
      Opnd size = decode_opnd(ACC_R, 1);
      VField base = decode_vfield_base();
      Opnd src2 = decode_opnd(ACC_R, 4);
      if (!pos.ok || !size.ok || !base.ok || !src2.ok) return;
      uint32_t r = vfield_extract(pos.value, size.value & 0xFF, base);
      if (g_st.fault) return;
      set_cmp_long(r, src2.value);
      return;
    }

    case 0xF1: {  // ACBL
      Opnd lim = decode_opnd(ACC_R, 4);
      Opnd add = decode_opnd(ACC_R, 4);
      Opnd idx = decode_opnd(ACC_M, 4);
      int16_t d = (int16_t)fetch16();
      if (!lim.ok || !add.ok || !idx.ok) return;
      uint32_t ix = idx.is_reg ? g_st.r[idx.reg] : mem_r32(idx.addr);
      uint32_t r = ix + add.value;
      store_opnd(idx, r, 4);
      set_nz_long(r);
      if (((~(ix ^ add.value) & (ix ^ r)) & 0x80000000u) != 0) g_st.psl |= PSL_V;
      if ((add.value & 0x80000000u) ? ((int32_t)r >= (int32_t)lim.value)
                                    : ((int32_t)r <= (int32_t)lim.value))
        branch_w(d);
      return;
    }
    case 0xF2: {  // AOBLSS
      Opnd lim = decode_opnd(ACC_R, 4);
      Opnd idx = decode_opnd(ACC_M, 4);
      int8_t d = (int8_t)fetch8();
      if (!lim.ok || !idx.ok) return;
      uint32_t ix = idx.is_reg ? g_st.r[idx.reg] : mem_r32(idx.addr);
      uint32_t r = ix + 1;
      store_opnd(idx, r, 4);
      set_nz_long(r);
      if (((~(ix ^ 1u) & (ix ^ r)) & 0x80000000u) != 0) g_st.psl |= PSL_V;
      if ((int32_t)r < (int32_t)lim.value) branch_b(d);
      return;
    }
    case 0xF3: {  // AOBLEQ
      Opnd lim = decode_opnd(ACC_R, 4);
      Opnd idx = decode_opnd(ACC_M, 4);
      int8_t d = (int8_t)fetch8();
      if (!lim.ok || !idx.ok) return;
      uint32_t ix = idx.is_reg ? g_st.r[idx.reg] : mem_r32(idx.addr);
      uint32_t r = ix + 1;
      store_opnd(idx, r, 4);
      set_nz_long(r);
      if (((~(ix ^ 1u) & (ix ^ r)) & 0x80000000u) != 0) g_st.psl |= PSL_V;
      if ((int32_t)r <= (int32_t)lim.value) branch_b(d);
      return;
    }

    case 0x5F: {  // REMQTI — study: Open SIMH op_remqti (simplified)
      Opnd hdr = decode_opnd(ACC_A, 8);
      Opnd dst = decode_opnd(ACC_W, 4);
      if (!hdr.ok || !dst.ok) return;
      uint32_t h = hdr.addr;
      if (h & 7u) {
        note_fault(3, "remqti");
        return;
      }
      uint32_t a_rel = mem_r32(h);
      if (a_rel == 0) {
        g_st.psl = (g_st.psl & ~(PSL_N | PSL_C)) | PSL_Z | PSL_V;
        store_opnd(dst, 0, 4);
        return;
      }
      if (a_rel & 1u) {
        g_st.psl = (g_st.psl & ~(PSL_N | PSL_Z | PSL_V)) | PSL_C;
        return;
      }
      // One-entry → remqhi-like
      uint32_t c_rel = mem_r32(h + 4);
      if (c_rel == a_rel) {
        mem_w32(h, 0);
        mem_w32(h + 4, 0);
        store_opnd(dst, a_rel + h, 4);
        g_st.psl &= ~(PSL_N | PSL_V | PSL_C);
        g_st.psl |= PSL_Z;
        return;
      }
      uint32_t c = c_rel + h;
      uint32_t b = mem_r32(c + 4) + c;
      mem_w32(b, h - b);
      mem_w32(h + 4, b - h);
      store_opnd(dst, c, 4);
      g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
      return;
    }

    default: {
      // MicroVAX D/G/H float, packed decimal, etc. Host halt hides the guest
      // (V0.6.50 user HALT → SCB 0x10). SIMH: reserved-instruction fault.
      static uint8_t undef_logs = 8;
      if (undef_logs) {
        undef_logs--;
        LOGE("VAX reserved inst 0x%02X PC=%08X PSL=%08X SP=%08X CUR=%u",
             op, (unsigned)g_last_op_pc, (unsigned)g_st.psl,
             (unsigned)g_st.r[R_SP], (unsigned)psl_cur());
      }
      raise_exception(SCB_PRIV);
      return;
    }
  }
}

// ---- public API ----

bool init(size_t ram_bytes) {
  if (g_ram) {
    free(g_ram);
    g_ram = nullptr;
    g_ram_bytes = 0;
  }
  g_ram = (uint8_t*)heap_caps_malloc(ram_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_ram) {
    // Caller may probe several sizes; it logs the outcome.
    return false;
  }
  g_ram_bytes = ram_bytes;
  memset(g_ram, 0, g_ram_bytes);
  LOG("VAX RAM: %u bytes @ %p (PSRAM)", (unsigned)g_ram_bytes, (void*)g_ram);
  if (g_q22map) {
    free(g_q22map);
    g_q22map = nullptr;
  }
  g_q22map = (uint32_t*)heap_caps_malloc(Q22_MAP_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_q22map) {
    LOGE("Q22 map alloc failed (%u bytes)", (unsigned)Q22_MAP_BYTES);
  } else {
    memset(g_q22map, 0, Q22_MAP_BYTES);
    LOG("Q22 maps: %u entries @ %p (PSRAM)", (unsigned)Q22_MAP_COUNT, (void*)g_q22map);
  }
  vax_mmu::set_phys_ops(phys_r32, phys_w32);
  vax_mscp::set_phys_ops(mscp_phys_r8, mscp_phys_w8);
  reset();
  return true;
}

void reset() {
  memset(&g_st, 0, sizeof(g_st));
  memset(g_stk, 0, sizeof(g_stk));
  g_isp = 0;
  g_sisr = 0;
  g_mmgt_abort = false;
  g_mmgt_log_left =
#if VVAX_DIAG_LEVEL >= 2
      8;
#elif VVAX_DIAG_LEVEL >= 1
      4;
#else
      2;
#endif
  g_mchk_log_left = 2;
  g_logged_q22_map = false;
  g_logged_q22_dma = false;
  if (g_q22map)
    memset(g_q22map, 0, Q22_MAP_BYTES);
  g_st.halt = true;
  g_running = false;
  g_hb_elapsed_ms = 0;
  g_hb_last_ms = 0;
  g_hb_last_instr = 0;
  g_hb_hold = false;
  vax_mmu::reset();
}

bool load(uint32_t pa, const uint8_t* data, size_t len) {
  if (!data || !pa_ok(pa, len)) return false;
  memcpy(g_ram + pa, data, len);
  return true;
}

void step(unsigned n) {
  for (unsigned i = 0; i < n && g_running && !g_st.halt && !g_st.fault; i++)
    exec_one();
  if (g_running && !g_st.halt && !g_st.fault)
    exec_hb_if_due();
  if (!g_logged_stop && g_logged_reloc && (g_st.halt || g_st.fault || !g_running)) {
    g_logged_stop = true;
    LOGE("boot stop: halt=%u fault=%u run=%u PC=%08X SP=%08X instr=%u",
         (unsigned)g_st.halt, (unsigned)g_st.fault, (unsigned)g_running,
         (unsigned)g_st.r[R_PC], (unsigned)g_st.r[R_SP],
         (unsigned)g_instr_count);
  }
}

bool running() { return g_running && !g_st.halt && !g_st.fault; }

void request_halt() {
  g_st.halt = true;
  g_running = false;
}

uint8_t* ram() { return g_ram; }
size_t   ram_bytes() { return g_ram_bytes; }
State&   state() { return g_st; }

bool selftest() {
  // Assembled at 0x1000 — math check then print "vVax OK\r\n" via console MMIO.
  // Layout (see docs/PHASES.md Phase 2):
  static const uint8_t prog[] = {
    // 1000: CLRL R2
    0xD4, 0x52,
    // 1002: ADDL2 #5, R2
    0xC0, 0x05, 0x52,
    // 1005: ADDL2 #7, R2
    0xC0, 0x07, 0x52,
    // 1008: CMPL R2, #12
    0xD1, 0x52, 0x8F, 0x0C, 0x00, 0x00, 0x00,
    // 100F: BEQL ok_math (+2 -> 1013)
    0x13, 0x02,
    // 1011: BRB fail_math (-> 1040): from 1013 to 1040 = +45 = 0x2D
    0x11, 0x2D,
    // 1013: MOVL #msg, R0   msg=0x1048
    0xD0, 0x8F, 0x48, 0x10, 0x00, 0x00, 0x50,
    // 101A: MOVB (R0)+, R1
    0x90, 0x80, 0x51,
    // 101D: BEQL done (+9 -> 1028)
    0x13, 0x09,
    // 101F: MOVB R1, @#0x20000000
    0x90, 0x51, 0x9F, 0x00, 0x00, 0x00, 0x20,
    // 1026: BRB loop (-> 101A): from 1028 to 101A = -14 = 0xF2
    0x11, 0xF2,
    // 1028: MOVL #0x4F4B0000, R0   ('OK' in high bytes)
    0xD0, 0x8F, 0x00, 0x00, 0x4B, 0x4F, 0x50,
    // 102F: HALT
    0x00,
    // 1030–103F padding toward fail
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    // 1040: fail_math — MOVL #0xBAD00000, R0 ; HALT
    0xD0, 0x8F, 0x00, 0x00, 0xAD, 0x0B, 0x50,
    0x00,
    // 1048: msg
    'v', 'V', 'a', 'x', ' ', 'O', 'K', '\r', '\n', 0x00
  };

  reset();
  if (!load(0x1000, prog, sizeof(prog))) {
    LOGE("selftest: load failed");
    return false;
  }

  g_st.r[R_PC] = 0x1000;
  g_st.r[R_SP] = (uint32_t)(g_ram_bytes > 0x10000 ? 0x10000 : (g_ram_bytes & ~3u));
  g_st.halt = false;
  g_st.fault = 0;
  g_running = true;

  // Bound runtime: enough for string print loop.
  for (int i = 0; i < 100000 && running(); i++)
    exec_one();

  bool pass = g_st.halt && !g_st.fault && g_st.r[0] == 0x4F4B0000u;
  if (pass) {
    LOG("VAX selftest: PASS");
  } else {
    LOGE("VAX selftest: FAIL R0=%08X fault=%u PC=%08X",
         (unsigned)g_st.r[0], (unsigned)g_st.fault, (unsigned)g_st.r[R_PC]);
  }
  return pass;
}

void cold_boot() {
  reset();
  LOG("VAX cold boot");
  bool cpu_ok = selftest();
  bool mmu_ok = vax_mmu::selftest();
  bool con_ok = vax_console::selftest();
  bool clk_ok = vax_clock::selftest();
  bool mscp_ok = vax_mscp::selftest();
  g_st.halt = true;
  g_running = false;
  if (!cpu_ok) LOGE("VAX cold boot: CPU selftest failed");
  if (!mmu_ok) LOGE("VAX cold boot: MMU selftest failed");
  if (!con_ok) LOGE("VAX cold boot: console selftest failed");
  if (!clk_ok) LOGE("VAX cold boot: clock selftest failed");
  if (!mscp_ok) LOGE("VAX cold boot: MSCP selftest failed");
  // selftest briefly enables ICCS IE; clear before xxboot (/boot races SCBB vs IRQ).
  vax_clock::reset();
}

void run() {
  g_st.halt = false;
  g_st.fault = 0;
  g_running = true;
  g_logged_reloc = false;
  g_instr_count = 0;
  g_trace_left = 0;
  g_logged_stop = false;
  g_last_op_pc = 0;
  g_mmgt_abort = false;
  g_mmgt_log_left =
#if VVAX_DIAG_LEVEL >= 2
      8;
#elif VVAX_DIAG_LEVEL >= 1
      4;
#else
      2;
#endif
  g_mchk_log_left = 2;
  g_logged_q22_map = false;
  g_logged_q22_dma = false;
  g_boot_elf_active = false;
  g_irq_log_left =
#if VVAX_DIAG_LEVEL >= 2
      8;
#elif VVAX_DIAG_LEVEL >= 1
      4;
#else
      0;
#endif
  g_logged_s0_pc = false;
  g_logged_user_rei = false;
  g_logged_user_calls = false;
  g_logged_user_jmp = false;
  g_user_jmp_log_left = 8;
  g_acv_storm_pc = 0;
  g_acv_storm_va = 0;
  g_acv_storm_count = 0;
  g_acv_storm_dumped = false;
  g_logged_user_as_kern = false;
  g_p1_rei_log_left =
#if VVAX_DIAG_LEVEL >= 2
      6;
#else
      0;
#endif
  g_repair_log_left = 8;
  g_rei_bad_log_left = 8;
  g_popr_log_left = 4;
  g_xtransl_log_left = 8;
  g_chmk_log_left = 4;
  g_user_copy_logs = 12;
  g_user_movb_logs = 24;
  g_watch_va = 0;
  g_watch_n = 0;
  g_watch_dst0_even = 0;
  g_watch_hb_left = 0;
  g_watch_rd_head = 0;
  g_watch_rd_tok = 0;
  g_watch_from = 0;
  g_watch_export_off = 0;
  g_tok_wr = 0;
  g_tok_last_va = 0;
  g_tok_bss_left = 2;
  g_name_va = 0;
  g_name_n = 0;
  g_name_rd = 0;
  g_name_last_va = 0;
  g_name_skip = 0;
  g_arg_wr = 0;
  g_arg_last_va = 0;
  g_name_movc = 0;
  g_tx_log = 0;
  g_name_libc = 0;
  g_insn_dump = 0;
  g_case_log = 0;
  g_in_ie = false;
  g_xlat_mode_ov = -1;
  g_logged_wild_jsb = false;
  g_kernel_load_end = 0;
  g_logged_mapen = false;
  g_sisr = 0;
  g_sirr_log_left = 4;
  g_planted_kernel_csr = false;
#if VAX_CLOCK_WARP_DEFAULT
  g_idle_warp_ctr = 0;
  g_logged_idle_warp = false;
  g_warp_ms = 0;
  g_warp_in_ms = 0;
  g_warp_holdoff_ms = 0;
#endif
  g_warp_fires = 0;
  g_rootopen_trace = false;
  g_root_hb_ms = 0;
  g_hb_elapsed_ms = 0;
  g_hb_last_ms = 0;
  g_hb_last_instr = 0;
  g_hb_hold = false;
  g_mscp_irq_logs = 12;
  g_mscp_blocked_logs = 8;
  g_reset_probes = true;
}

}  // namespace vax_cpu
