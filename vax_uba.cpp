#include "vax_uba.h"
#include "config.h"
#include "platform.h"

#include <string.h>

namespace vax_uba {

#if VAX_MODEL == VAX_MODEL_KA750

static uint32_t g_map[MAP_COUNT];
static uint32_t g_mcsr0 = 0;
static uint32_t g_mcsr1 = 0;
static uint32_t g_mcsr2 = 0;
static size_t   g_ram_bytes = 0;
static bool     g_logged_map = false;
static bool     g_logged_mctl = false;

// SIMH vax750_mem.c: 2 MiB → 16K chips (256 KiB boards). Above that, 64K chips
// (1 MiB boards) and MCSR2_CS64 so VMS/NetBSD do not treat the map as 256 KiB
// slots (V0.7.4 0x11555 → ~1.75 MiB → SYSBOOT at 0xFFEC4). Only complete 1 MiB
// boards: 8192000 is 7 × 1 MiB, not 8 (top of 8 MiB is past guest RAM).
static uint32_t mcsr2_from_ram(size_t bytes) {
  static constexpr uint32_t INIT = 0x00010000u;
  static constexpr uint32_t CS64 = 0x01000000u;
  static constexpr uint32_t MASK64 = 0x5555u;
  if (bytes <= (2u << 20))
    return INIT | 0xFFFFu;
  uint32_t slots = (uint32_t)(bytes / (1u << 20));
  if (slots > 8u) slots = 8u;
  if (slots == 0) slots = 1u;
  uint32_t slot_mask = (1u << (slots << 1)) - 1u;
  return INIT | CS64 | (MASK64 & slot_mask);
}

void set_ram_bytes(size_t ram_bytes) {
  g_ram_bytes = ram_bytes;
  g_mcsr2 = mcsr2_from_ram(ram_bytes);
}

void reset() {
  memset(g_map, 0, sizeof(g_map));
  g_mcsr0 = 0;
  g_mcsr1 = 0;
  g_mcsr2 = mcsr2_from_ram(g_ram_bytes ? g_ram_bytes : (8u << 20));
  g_logged_map = false;
  g_logged_mctl = false;
}

bool reg_hit(uint32_t pa) {
  if (pa >= UBA0_BASE && pa < UBA0_NEX_END)
    return true;
  return pa >= MAP_ALIAS && pa < MAP_ALIAS + MAP_COUNT * 4u;
}

bool mctl_hit(uint32_t pa) {
  return pa >= MCTL_BASE && pa < MCTL_END;
}

bool rom_hit(uint32_t pa) {
  return pa >= ROM_BASE && pa < ROM_END;
}

bool nexus_hit(uint32_t pa) {
  return pa >= NEXUS_BASE && pa < NEXUS_END;
}

bool unibus_mem_hit(uint32_t pa) {
  return pa >= UNIMEM_BASE && pa < UNIIO_BASE;
}

bool io_page_hit(uint32_t pa) {
  return pa >= UNIIO_BASE && pa < UNIIO_END;
}

bool wcs_hit(uint32_t pa) {
  return pa >= WCS_BASE && pa < WCS_END;
}

bool map_ba(uint32_t ba, uint32_t* host) {
  if (!host) return false;
  uint32_t u = ba & 0x3FFFFu;
  uint32_t i = u >> 9;
  if (i >= MAP_COUNT) return false;
  uint32_t e = g_map[i];
  if (!(e & MAP_VALID)) return false;
  *host = ((e & MAP_PFN) << 9) | (u & 0x1FFu);
  return true;
}

bool map_unimem(uint32_t pa, uint32_t* host) {
  if (!unibus_mem_hit(pa)) return false;
  return map_ba(pa - UNIMEM_BASE, host);
}

static bool map_slot(uint32_t pa, uint32_t* i) {
  if (pa >= MAP_BASE && pa < MAP_BASE + MAP_COUNT * 4u) {
    *i = (pa - MAP_BASE) >> 2;
    return true;
  }
  // /boot BISL3 #PG_V, r1, (r2)+ walked 0xF32800 (UBA+0x2800), not 0xF30800.
  if (pa >= MAP_ALIAS && pa < MAP_ALIAS + MAP_COUNT * 4u) {
    *i = (pa - MAP_ALIAS) >> 2;
    return true;
  }
  return false;
}

static uint32_t mctl_rd(uint32_t pa) {
  uint32_t ofs = (pa - MCTL_BASE) >> 2;
  uint32_t v = 0;
  if (ofs == 0) v = g_mcsr0;
  else if (ofs == 1) v = g_mcsr1;
  else if (ofs == 2) v = g_mcsr2;
  if (!g_logged_mctl) {
    g_logged_mctl = true;
    LOG("MCTL mcsr2=0x%08X ram=%u @%08X",
        (unsigned)g_mcsr2, (unsigned)g_ram_bytes, (unsigned)pa);
  }
  return v;
}

static void mctl_wr(uint32_t pa, uint32_t v, uint32_t sh) {
  uint32_t ofs = (pa - MCTL_BASE) >> 2;
  if (ofs == 0) {
    // W1C on RDS / RDSH / CRD (SIMH MCSR0_RS).
    uint32_t cur = (g_mcsr0 & ~(0xFFu << sh)) | (v << sh);
    g_mcsr0 = g_mcsr0 & ~(cur & 0xE0000000u);
  } else if (ofs == 1) {
    g_mcsr1 = (g_mcsr1 & ~(0xFFu << sh)) | (v << sh);
    g_mcsr1 &= 0x1E00007Fu;
  }
}

uint8_t read8(uint32_t pa) {
  if (mctl_hit(pa)) {
    uint32_t e = mctl_rd(pa);
    unsigned sh = (unsigned)((pa & 3u) * 8u);
    return (uint8_t)(e >> sh);
  }
  uint32_t i = 0;
  if (map_slot(pa, &i)) {
    uint32_t e = g_map[i];
    unsigned sh = (unsigned)((pa & 3u) * 8u);
    return (uint8_t)(e >> sh);
  }
  return 0;
}

void write8(uint32_t pa, uint8_t v) {
  if (mctl_hit(pa)) {
    mctl_wr(pa, (uint32_t)v, (pa & 3u) * 8u);
    return;
  }
  uint32_t i = 0;
  if (map_slot(pa, &i)) {
    uint32_t sh = (pa & 3u) * 8u;
    g_map[i] = (g_map[i] & ~(0xFFu << sh)) | ((uint32_t)v << sh);
    if (!g_logged_map && (g_map[i] & MAP_VALID)) {
      g_logged_map = true;
      LOG("UBA map[%u]=0x%08X (pfn=%u pa=%08X) @%08X",
          (unsigned)i, (unsigned)g_map[i],
          (unsigned)(g_map[i] & MAP_PFN),
          (unsigned)((g_map[i] & MAP_PFN) << 9),
          (unsigned)pa);
    }
    return;
  }
  // CSR / empty nexus slot: ignore write (SIMH WriteReg timeout, not MCHK).
}

#else

void reset() {}
void set_ram_bytes(size_t) {}
bool reg_hit(uint32_t) { return false; }
bool mctl_hit(uint32_t) { return false; }
bool rom_hit(uint32_t) { return false; }
bool nexus_hit(uint32_t) { return false; }
bool unibus_mem_hit(uint32_t) { return false; }
bool io_page_hit(uint32_t) { return false; }
bool wcs_hit(uint32_t) { return false; }
uint8_t read8(uint32_t) { return 0; }
void write8(uint32_t, uint8_t) {}
bool map_ba(uint32_t, uint32_t*) { return false; }
bool map_unimem(uint32_t, uint32_t*) { return false; }

#endif

}  // namespace vax_uba
