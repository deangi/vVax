#include "vax_clock.h"
#include "host_lib/time/host_time.h"

#include <Arduino.h>
#include <time.h>

namespace vax_clock {

static uint32_t g_ticks = 0;
static uint32_t g_last_ms = 0;

void begin() {
  g_ticks = 0;
  g_last_ms = millis();
}

void reset() {
  g_ticks = 0;
  g_last_ms = millis();
}

void poll() {
  uint32_t now = millis();
  uint32_t dt = now - g_last_ms;
  if (dt == 0) return;
  // ~100 Hz guest tick approximation for stub.
  g_ticks += dt / 10;
  g_last_ms = now - (dt % 10);
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

}  // namespace vax_clock
