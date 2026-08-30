#include "vax_pctrace.h"

#if VVAX_PCTRACE

#include "platform.h"
#include <string.h>

#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

namespace vax_pctrace {

bool enabled = false;

struct Rec {
  uint32_t pc;
  uint32_t psl;
  uint32_t r[15];  // R0–R14 (SP); PC is Rec::pc
  uint8_t  op;
};

EXT_RAM_BSS_ATTR static Rec g_ring[DEPTH];
static unsigned g_head = 0;
static unsigned g_n = 0;
static bool     g_dumped = false;

void set_enabled(bool on) {
  enabled = on;
  if (!on) {
    g_dumped = false;
  }
}

void clear() {
  g_head = 0;
  g_n = 0;
  g_dumped = false;
}

void record(uint32_t pc, uint8_t op, const uint32_t r[16], uint32_t psl) {
  Rec& e = g_ring[g_head];
  e.pc = pc;
  e.psl = psl;
  e.op = op;
  memcpy(e.r, r, sizeof(e.r));
  g_head = (g_head + 1u) % DEPTH;
  if (g_n < DEPTH) g_n++;
}

void dump() {
  if (!enabled || g_dumped || g_n == 0) return;
  g_dumped = true;
  LOG("pctrace: last %u insn%s (oldest first)",
      g_n, g_n == 1u ? "" : "s");
  unsigned i = (g_n == DEPTH) ? g_head : 0;
  for (unsigned k = 0; k < g_n; k++) {
    const Rec& e = g_ring[i];
    LOG("pctrace %03u PC=%08X op=%02X PSL=%08X R0-5 %08X %08X %08X %08X %08X %08X",
        k, (unsigned)e.pc, (unsigned)e.op, (unsigned)e.psl,
        (unsigned)e.r[0], (unsigned)e.r[1], (unsigned)e.r[2],
        (unsigned)e.r[3], (unsigned)e.r[4], (unsigned)e.r[5]);
    LOG("           R6-11 %08X %08X %08X %08X %08X %08X AP=%08X FP=%08X SP=%08X",
        (unsigned)e.r[6], (unsigned)e.r[7], (unsigned)e.r[8],
        (unsigned)e.r[9], (unsigned)e.r[10], (unsigned)e.r[11],
        (unsigned)e.r[12], (unsigned)e.r[13], (unsigned)e.r[14]);
    i = (i + 1u) % DEPTH;
  }
}

}  // namespace vax_pctrace

#endif  // VVAX_PCTRACE
