#include "vax_cpu.h"
#include "vax_mmu.h"
#include "vax_console.h"
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

// ---- memory ----

static bool pa_ok(uint32_t pa, size_t n) {
  return g_ram && ((uint64_t)pa + n) <= g_ram_bytes;
}

static uint8_t mem_r8(uint32_t pa) {
  uint32_t phys = pa;
  if (!vax_mmu::translate(pa, &phys, false)) {
    g_st.fault = 2;
    return 0;
  }
  if (pa_ok(phys, 1)) return g_ram[phys];
  g_st.fault = 2;
  return 0;
}

static uint16_t mem_r16(uint32_t pa) {
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

static void mem_w8(uint32_t pa, uint8_t v) {
  uint32_t phys = pa;
  if (!vax_mmu::translate(pa, &phys, true)) {
    g_st.fault = 2;
    return;
  }
  if (phys == CONSOLE_TX_PA) {
    vax_console::put_guest(v);
    return;
  }
  if (pa_ok(phys, 1)) {
    g_ram[phys] = v;
    return;
  }
  g_st.fault = 2;
}

static void mem_w16(uint32_t pa, uint16_t v) {
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
  uint32_t pc = g_st.r[R_PC];
  uint8_t v = mem_r8(pc);
  g_st.r[R_PC] = pc + 1;
  return v;
}

static uint16_t fetch16() {
  uint16_t v = mem_r16(g_st.r[R_PC]);
  g_st.r[R_PC] += 2;
  return v;
}

static uint32_t fetch32() {
  uint32_t v = mem_r32(g_st.r[R_PC]);
  g_st.r[R_PC] += 4;
  return v;
}

// ---- PSL / CC ----

static void set_nz_long(uint32_t v) {
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V);
  if (v == 0) g_st.psl |= PSL_Z;
  if (v & 0x80000000u) g_st.psl |= PSL_N;
}

static void set_nz_word(uint16_t v) {
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V);
  if (v == 0) g_st.psl |= PSL_Z;
  if (v & 0x8000) g_st.psl |= PSL_N;
}

static void set_nz_byte(uint8_t v) {
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V);
  if (v == 0) g_st.psl |= PSL_Z;
  if (v & 0x80) g_st.psl |= PSL_N;
}

static void set_add_cc(uint32_t a, uint32_t b, uint32_t r) {
  g_st.psl &= ~(PSL_N | PSL_Z | PSL_V | PSL_C);
  if (r == 0) g_st.psl |= PSL_Z;
  if (r & 0x80000000u) g_st.psl |= PSL_N;
  if (r < a) g_st.psl |= PSL_C;  // unsigned carry
  // signed overflow
  if (((~(a ^ b) & (a ^ r)) & 0x80000000u) != 0) g_st.psl |= PSL_V;
}

static void set_sub_cc(uint32_t a, uint32_t b, uint32_t r) {
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
  uint8_t spec = fetch8();
  uint8_t mode = (uint8_t)(spec >> 4);
  uint8_t rn   = (uint8_t)(spec & 0x0F);

  // Short literal (modes 0–3): read-only
  if (mode <= 3) {
    if (acc == ACC_W || acc == ACC_M || acc == ACC_A) {
      g_st.fault = 3;
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
      g_st.fault = 3;
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
      g_st.fault = 3;
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
      g_st.fault = 3;
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
  if (!o.ok) return;
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
  if (size == 1) mem_w8(o.addr, (uint8_t)v);
  else if (size == 2) mem_w16(o.addr, (uint16_t)v);
  else mem_w32(o.addr, v);
}

static void branch_b(int8_t disp) {
  g_st.r[R_PC] = (uint32_t)((int32_t)g_st.r[R_PC] + (int32_t)disp);
}

static void branch_w(int16_t disp) {
  g_st.r[R_PC] = (uint32_t)((int32_t)g_st.r[R_PC] + (int32_t)disp);
}

// ---- one instruction ----

static void exec_one() {
  if (g_st.halt || g_st.fault) {
    g_running = false;
    return;
  }

  uint8_t op = fetch8();

  switch (op) {
    case 0x00:  // HALT
      g_st.halt = true;
      g_running = false;
      return;

    case 0x01:  // NOP
      return;

    case 0x05: {  // RSB
      uint32_t ret = mem_r32(g_st.r[R_SP]);
      g_st.r[R_SP] += 4;
      g_st.r[R_PC] = ret;
      return;
    }

    case 0x10: {  // BSBB
      int8_t d = (int8_t)fetch8();
      g_st.r[R_SP] -= 4;
      mem_w32(g_st.r[R_SP], g_st.r[R_PC]);
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
      if (!(g_st.psl & (PSL_N | PSL_Z))) branch_b(d);  // simplified: N|Z clear
      // Correct BGTR: !(N^V) && !Z — V usually 0 after CMP
      return;
    }

    case 0x15: {  // BLEQ
      int8_t d = (int8_t)fetch8();
      if (g_st.psl & (PSL_N | PSL_Z)) branch_b(d);
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

    case 0x17: {  // JMP
      Opnd a = decode_opnd(ACC_A, 4);
      if (!a.ok) return;
      g_st.r[R_PC] = a.value;
      return;
    }

    case 0x31: {  // BRW
      int16_t d = (int16_t)fetch16();
      branch_w(d);
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

    case 0xB0: {  // MOVW
      Opnd s = decode_opnd(ACC_R, 2);
      Opnd d = decode_opnd(ACC_W, 2);
      if (!s.ok || !d.ok) return;
      store_opnd(d, s.value, 2);
      set_nz_word((uint16_t)s.value);
      g_st.psl &= ~PSL_C;
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

    case 0xCA: {  // BICL2
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_M, 4);
      if (!s.ok || !d.ok) return;
      uint32_t a = d.is_reg ? g_st.r[d.reg] : mem_r32(d.addr);
      uint32_t r = a & ~s.value;
      store_opnd(d, r, 4);
      set_nz_long(r);
      g_st.psl &= ~PSL_C;
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
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xDC: {  // MNEGL
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

    case 0xDD: {  // PUSHL
      Opnd s = decode_opnd(ACC_R, 4);
      if (!s.ok) return;
      g_st.r[R_SP] -= 4;
      mem_w32(g_st.r[R_SP], s.value);
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

    case 0xDA: {  // MFPR
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_W, 4);
      if (!s.ok || !d.ok) return;
      uint32_t v = vax_mmu::get_ipr(s.value);
      // Non-MMU IPRs stubbed as 0 for now except those in vax_mmu.
      store_opnd(d, v, 4);
      set_nz_long(v);
      g_st.psl &= ~PSL_C;
      return;
    }

    case 0xDB: {  // MTPR
      Opnd s = decode_opnd(ACC_R, 4);
      Opnd d = decode_opnd(ACC_R, 4);  // IPR number
      if (!s.ok || !d.ok) return;
      vax_mmu::set_ipr(d.value, s.value);
      set_nz_long(s.value);
      g_st.psl &= ~PSL_C;
      return;
    }

    default:
      LOGE("VAX undefined opcode 0x%02X at PC=%08X", op, (unsigned)(g_st.r[R_PC] - 1));
      g_st.fault = 1;
      g_st.halt = true;
      g_running = false;
      return;
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
    LOGE("VAX RAM alloc failed: %u bytes", (unsigned)ram_bytes);
    return false;
  }
  g_ram_bytes = ram_bytes;
  memset(g_ram, 0, g_ram_bytes);
  LOG("VAX RAM: %u bytes @ %p (PSRAM)", (unsigned)g_ram_bytes, (void*)g_ram);
  vax_mmu::set_phys_ops(phys_r32, phys_w32);
  reset();
  return true;
}

void reset() {
  memset(&g_st, 0, sizeof(g_st));
  g_st.halt = true;
  g_running = false;
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
  g_st.halt = true;
  g_running = false;
  if (!cpu_ok)
    LOGE("VAX cold boot: CPU selftest failed");
  if (!mmu_ok)
    LOGE("VAX cold boot: MMU selftest failed");
}

}  // namespace vax_cpu
