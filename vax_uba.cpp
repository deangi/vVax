#include "vax_uba.h"
#include "config.h"
#include "platform.h"

#include <string.h>

namespace vax_uba {

#if VAX_MODEL == VAX_MODEL_KA750

static uint32_t g_map[MAP_COUNT];
static bool     g_logged_map = false;

void reset() {
  memset(g_map, 0, sizeof(g_map));
  g_logged_map = false;
}

bool reg_hit(uint32_t pa) {
  return pa >= UBA0_BASE && pa < UBA0_END;
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

uint8_t read8(uint32_t pa) {
  uint32_t i = 0;
  if (map_slot(pa, &i)) {
    uint32_t e = g_map[i];
    unsigned sh = (unsigned)((pa & 3u) * 8u);
    return (uint8_t)(e >> sh);
  }
  return 0;
}

void write8(uint32_t pa, uint8_t v) {
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
  // CSR / empty nexus slot: ignore (do not pa-w).
}

#else

void reset() {}
bool reg_hit(uint32_t) { return false; }
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
