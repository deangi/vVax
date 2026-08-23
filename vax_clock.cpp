#include "vax_clock.h"
#include "config.h"
#include "platform.h"

#include <Arduino.h>
#include <string.h>

namespace vax_clock {

// NetBSD KA630: chip clock at KA630CLK; TODR IPR uses TODRBASE on other models.
#if VAX_MODEL == VAX_MODEL_KA630
static constexpr uint32_t KA630_TOY_PA    = 0x200B8000u;
#endif
static constexpr uint32_t KA630_TOY_WORDS = 15u;
static constexpr uint32_t TODRBASE        = 1u << 28;

// Default wall time until Phase 7 NTP sync: 1-JAN-2026 00:00:00 UTC.
static constexpr uint8_t TOY_YR   = 56;  // 2026 - 1970
static constexpr uint8_t TOY_MON  = 1;
static constexpr uint8_t TOY_DAY  = 1;
static constexpr uint8_t TOY_WDAY = 5;   // Thursday (Sun=1 .. Thu=5)
static constexpr uint8_t TOY_HR   = 0;
static constexpr uint8_t TOY_MIN  = 0;
static constexpr uint8_t TOY_SEC  = 0;

static uint32_t g_ticks = 0;
static uint32_t g_last_ms = 0;
static uint32_t g_iccs = 0;
static uint32_t g_todr = TODRBASE;
static bool     g_todr_set = false;
static bool     g_clk_irq_latched = false;
static uint32_t g_nicr = 0xFFFFD8F0u;  // -10000 us (NetBSD cpu_initclocks)
static int32_t  g_icr = 0;
static uint16_t g_toy[KA630_TOY_WORDS];
static uint32_t g_toy_cs = 0;  // centiseconds within current second (0–99)

static void toy_set_default() {
  memset(g_toy, 0, sizeof(g_toy));
  g_toy[0]  = TOY_SEC;
  g_toy[2]  = TOY_MIN;
  g_toy[4]  = TOY_HR;
  g_toy[6]  = TOY_WDAY;
  g_toy[7]  = TOY_DAY;
  g_toy[8]  = TOY_MON;
  g_toy[9]  = TOY_YR;
  g_toy[10] = 0;     // CSRA — UIP clear
  g_toy[11] = 0x06;  // CSRB — 24-hour + binary (NetBSD CSRB_24|CSRB_DM)
  g_toy[12] = 0;     // CSRC
  g_toy[13] = 0x80;  // CSRD — VRT valid
  g_toy_cs = 0;
}

static void toy_sync_todr() {
  if (g_todr_set) return;
  const uint32_t sec =
      (uint32_t)g_toy[0] + (uint32_t)g_toy[2] * 60u +
      (uint32_t)g_toy[4] * 3600u;
  g_todr = TODRBASE + sec * 100u + g_toy_cs;
}

static void toy_tick_sec() {
  if (g_toy[11] & 0x80) return;  // SET — host is programming chip

  unsigned sec  = g_toy[0];
  unsigned min  = g_toy[2];
  unsigned hr   = g_toy[4];
  unsigned day  = g_toy[7];
  unsigned mon  = g_toy[8];
  unsigned wday = g_toy[6];

  sec++;
  if (sec >= 60) {
    sec = 0;
    min++;
    wday = (wday % 7) + 1;
  }
  if (min >= 60) {
    min = 0;
    hr++;
  }
  if (hr >= 24) {
    hr = 0;
    day++;
  }
  // Good enough for boot: January 2026 has 31 days.
  if (day > 31) {
    day = 1;
    mon++;
  }
  if (mon > 12) {
    mon = 1;
    g_toy[9] = (uint16_t)((unsigned)g_toy[9] + 1u);
  }

  g_toy[0] = (uint16_t)sec;
  g_toy[2] = (uint16_t)min;
  g_toy[4] = (uint16_t)hr;
  g_toy[6] = (uint16_t)wday;
  g_toy[7] = (uint16_t)day;
  g_toy[8] = (uint16_t)mon;
  g_toy_cs = 0;
}

void begin() {
  g_ticks = 0;
  g_last_ms = millis();
  g_iccs = 0;
  g_todr = TODRBASE;
  g_todr_set = false;
  g_clk_irq_latched = false;
  g_nicr = 0xFFFFD8F0u;
  g_icr = 0;
  toy_set_default();
  toy_sync_todr();
}

void reset() {
  g_ticks = 0;
  g_last_ms = millis();
  g_iccs = 0;
  g_clk_irq_latched = false;
  g_icr = 0;
  // Keep TODR / TOY across soft reset if guest had set them.
}

static void apply_ticks(uint32_t steps) {
  if (steps == 0) return;
  g_ticks += steps;
  g_icr += (int32_t)(steps * 10000u);
  g_toy_cs += steps;
  while (g_toy_cs >= 100u) {
    g_toy_cs -= 100u;
    toy_tick_sec();
  }
  if (g_todr_set)
    g_todr += steps;
  else
    toy_sync_todr();
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
  if (y)   *y   = (uint8_t)g_toy[9];
  if (mon) *mon = (uint8_t)g_toy[8];
  if (d)   *d   = (uint8_t)g_toy[7];
  if (h)   *h   = (uint8_t)g_toy[4];
  if (min) *min = (uint8_t)g_toy[2];
  if (s)   *s   = (uint8_t)g_toy[0];
}

bool toy_hit(uint32_t pa) {
#if VAX_MODEL != VAX_MODEL_KA630
  (void)pa;
  return false;
#else
  return pa >= KA630_TOY_PA && pa < KA630_TOY_PA + KA630_TOY_WORDS * 2u;
#endif
}

uint8_t toy_read8(uint32_t pa) {
#if VAX_MODEL != VAX_MODEL_KA630
  (void)pa;
  return 0;
#else
  if (!toy_hit(pa)) return 0;
  const unsigned off = pa - KA630_TOY_PA;
  const unsigned idx = off / 2u;
  const uint16_t w = g_toy[idx];
  return (off & 1u) ? (uint8_t)(w >> 8) : (uint8_t)w;
#endif
}

void toy_write8(uint32_t pa, uint8_t v) {
#if VAX_MODEL != VAX_MODEL_KA630
  (void)pa;
  (void)v;
#else
  if (!toy_hit(pa)) return;
  const unsigned off = pa - KA630_TOY_PA;
  const unsigned idx = off / 2u;
  if (idx >= KA630_TOY_WORDS) return;
  if (off & 1u)
    g_toy[idx] = (uint16_t)((g_toy[idx] & 0x00FFu) | ((uint16_t)v << 8));
  else
    g_toy[idx] = (uint16_t)((g_toy[idx] & 0xFF00u) | v);
  if (!g_todr_set)
    toy_sync_todr();
#endif
}

uint32_t iccs_rd() {
  return g_iccs & CSR_IE;
}

void iccs_wr(uint32_t v) {
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
    toy_sync_todr();
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
  iccs_wr(CSR_DONE);
  if (irq_clk()) {
    LOGE("clock selftest: IRQ stuck");
    return false;
  }
  if ((todr_rd() & TODRBASE) != TODRBASE) {
    LOGE("clock selftest: TODR base missing (%08X)", (unsigned)todr_rd());
    return false;
  }
#if VAX_MODEL == VAX_MODEL_KA630
  if ((g_toy[13] & 0x80u) == 0) {
    LOGE("clock selftest: TOY VRT clear");
    return false;
  }
#endif
  uint32_t t0 = todr_rd();
  todr_wr(0x12345678u);
  if (todr_rd() != 0x12345678u) {
    LOGE("clock selftest: TODR rw fail");
    return false;
  }
  todr_wr(t0);
#if VAX_MODEL == VAX_MODEL_KA630
  LOG("clock selftest: PASS (TOY 1-JAN-2026 TODR=%08X)", (unsigned)t0);
#else
  LOG("clock selftest: PASS (TODR=%08X)", (unsigned)t0);
#endif
  return true;
}

}  // namespace vax_clock
