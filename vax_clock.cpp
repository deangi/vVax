#include "vax_clock.h"
#include "host_lib/time/host_time.h"
#include "platform.h"

#include <Arduino.h>
#include <time.h>

namespace vax_clock {

static uint32_t g_ticks = 0;
static uint32_t g_last_ms = 0;
static uint32_t g_iccs = 0;
static uint32_t g_todr = 0;
static bool     g_todr_set = false;
static bool     g_clk_irq_latched = false;
static uint32_t g_nicr = 0xFFFFD8F0u;  // -10000 µs (NetBSD cpu_initclocks)
static int32_t  g_icr = 0;

void begin() {
  g_ticks = 0;
  g_last_ms = millis();
  g_iccs = 0;
  g_todr = 0;
  g_todr_set = false;
  g_clk_irq_latched = false;
  g_nicr = 0xFFFFD8F0u;
  g_icr = 0;
}

void reset() {
  g_ticks = 0;
  g_last_ms = millis();
  g_iccs = 0;
  g_clk_irq_latched = false;
  g_icr = 0;
  // Keep TODR across soft reset if guest had set it.
}

static void refresh_todr_from_host() {
  time_t t = time(nullptr);
  if (t < 0) t = 0;
  g_todr = (uint32_t)((uint64_t)t * 100ull);
}

static void apply_ticks(uint32_t steps) {
  if (steps == 0) return;
  g_ticks += steps;
  g_icr += (int32_t)(steps * 10000u);
  if (!g_todr_set)
    refresh_todr_from_host();
  else
    g_todr += steps;
  g_iccs |= CSR_DONE;
  if (g_iccs & CSR_IE)
    g_clk_irq_latched = true;
}

void poll() {
  uint32_t now = millis();
  uint32_t dt = now - g_last_ms;
  if (dt < 10) return;
  uint32_t steps = dt / 10;
  g_last_ms += steps * 10;
  apply_ticks(steps);
}

void force_tick() {
  apply_ticks(1);
}

uint32_t ticks() { return g_ticks; }

void get_toy(uint8_t* y, uint8_t* mon, uint8_t* d,
             uint8_t* h, uint8_t* min, uint8_t* s) {
  time_t t = time(nullptr);
  struct tm tm_utc;
  gmtime_r(&t, &tm_utc);
  if (y)   *y   = (uint8_t)(tm_utc.tm_year % 100);
  if (mon) *mon = (uint8_t)(tm_utc.tm_mon + 1);
  if (d)   *d   = (uint8_t)tm_utc.tm_mday;
  if (h)   *h   = (uint8_t)tm_utc.tm_hour;
  if (min) *min = (uint8_t)tm_utc.tm_min;
  if (s)   *s   = (uint8_t)tm_utc.tm_sec;
  (void)host_time_synced;
}

uint32_t iccs_rd() {
  // KA630: only IE is implemented in ICCS (SIMH CLKCSR_IMP).
  return g_iccs & CSR_IE;
}

void iccs_wr(uint32_t v) {
  // SIMH iccs_wr: IE is RW; IE=0 or DONE write acks the request.
  if ((v & CSR_IE) == 0)
    g_clk_irq_latched = false;
  if (v & CSR_DONE) {
    g_iccs &= ~CSR_DONE;
    g_clk_irq_latched = false;
  }
  g_iccs = (g_iccs & CSR_DONE) | (v & CSR_IE);
}

uint32_t nicr_rd() { return g_nicr; }
void nicr_wr(uint32_t v) { g_nicr = v; g_icr = (int32_t)v; }

uint32_t icr_rd() { return (uint32_t)g_icr; }

uint32_t todr_rd() {
  if (!g_todr_set)
    refresh_todr_from_host();
  return g_todr;
}

void todr_wr(uint32_t v) {
  g_todr = v;
  g_todr_set = true;
}

bool irq_clk() { return g_clk_irq_latched; }
void irq_clk_ack() { g_clk_irq_latched = false; }

bool selftest() {
  begin();
  g_last_ms = millis() - 20;
  iccs_wr(CSR_IE);
  poll();
  if (!irq_clk()) {
    LOGE("clock selftest: IRQ missing");
    return false;
  }
  iccs_wr(CSR_DONE);  // ack; leave IE off — guest /boot enables ICCS itself
  if (irq_clk()) {
    LOGE("clock selftest: IRQ stuck");
    return false;
  }
  uint32_t t0 = todr_rd();
  todr_wr(0x12345678u);
  if (todr_rd() != 0x12345678u) {
    LOGE("clock selftest: TODR rw fail");
    return false;
  }
  todr_wr(t0);
  LOG("clock selftest: PASS (TODR was %08X)", (unsigned)t0);
  return true;
}

}  // namespace vax_clock
