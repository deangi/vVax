#include "host_log.h"

#include <string.h>
#include "esp_attr.h"

#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

volatile bool g_serial_silenced = false;
void (*g_host_log_aux)(const char* line) = nullptr;

static constexpr unsigned kLogRingDepth = 128;
static constexpr unsigned kLogRingLine = 192;

EXT_RAM_BSS_ATTR static char g_log_ring[kLogRingDepth][kLogRingLine];
static unsigned g_log_head = 0;
static unsigned g_log_n = 0;

void host_log_ring_push(const char* line) {
  if (!line) return;
  char* slot = g_log_ring[g_log_head];
  strncpy(slot, line, kLogRingLine - 1);
  slot[kLogRingLine - 1] = 0;
  g_log_head = (g_log_head + 1u) % kLogRingDepth;
  if (g_log_n < kLogRingDepth) g_log_n++;
}

unsigned host_log_ring_count() { return g_log_n; }

void host_log_ring_dump(void (*out)(const char* line)) {
  if (!out || g_log_n == 0) return;
  unsigned i = (g_log_n == kLogRingDepth) ? g_log_head : 0;
  for (unsigned k = 0; k < g_log_n; k++) {
    out(g_log_ring[i]);
    i = (i + 1u) % kLogRingDepth;
  }
}
