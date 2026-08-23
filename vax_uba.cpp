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

static uint32_t map_index(uint32_t pa) {
  return (pa - MAP_BASE) >> 2;
}

uint8_t read8(uint32_t pa) {
  if (pa >= MAP_BASE && pa < MAP_BASE + MAP_COUNT * 4u) {
    uint32_t i = map_index(pa);
    uint32_t e = g_map[i];
    unsigned sh = (unsigned)((pa & 3u) * 8u);
    return (uint8_t)(e >> sh);
  }
  // CSR file: honest empty (no invented nexus attach). /boot needs maps.
  return 0;
}

void write8(uint32_t pa, uint8_t v) {
  if (pa >= MAP_BASE && pa < MAP_BASE + MAP_COUNT * 4u) {
    uint32_t i = map_index(pa);
    uint32_t sh = (pa & 3u) * 8u;
    g_map[i] = (g_map[i] & ~(0xFFu << sh)) | ((uint32_t)v << sh);
    if (!g_logged_map && (g_map[i] & MAP_VALID)) {
      g_logged_map = true;
      LOG("UBA map[%u]=0x%08X (pfn=%u pa=%08X)",
          (unsigned)i, (unsigned)g_map[i],
          (unsigned)(g_map[i] & MAP_PFN),
          (unsigned)((g_map[i] & MAP_PFN) << 9));
    }
    return;
  }
  // Other UBA CSRs: ignore writes (CR/SR stubs).
}

#else  // KA630 — objects exist so a stray include still links

void reset() {}
bool reg_hit(uint32_t) { return false; }
bool unibus_mem_hit(uint32_t) { return false; }
bool io_page_hit(uint32_t) { return false; }
bool wcs_hit(uint32_t) { return false; }
uint8_t read8(uint32_t) { return 0; }
void write8(uint32_t, uint8_t) {}
bool map_ba(uint32_t, uint32_t*) { return false; }
bool map_unimem(uint32_t, uint32_t*) { return false; }

#endif

}  // namespace vax_uba
