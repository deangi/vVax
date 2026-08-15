#include "vax_console.h"
#include "console.h"
#include "telnet.h"
#include "fifo.h"
#include "platform.h"

#include <Arduino.h>

namespace vax_console {

#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

static constexpr size_t RX_CAP = 512;  // power of two
EXT_RAM_BSS_ATTR static uint8_t rx_storage[RX_CAP];
static Fifo rxq;
static bool inited = false;

void begin() {
  if (inited) return;
  rxq.init(rx_storage, RX_CAP);
  rxq.clear();
  inited = true;
}

void reset() {
  if (inited) rxq.clear();
}

void put_guest(uint8_t c) {
  console_feed(c);
  telnet_write(c);
  if (Serial && Serial.availableForWrite() > 0)
    Serial.write(c);
}

bool get_guest(uint8_t* c) {
  if (!c || !inited) return false;
  return rxq.pop(c);
}

void inject(uint8_t c) {
  if (inited) rxq.push(c);
}

void poll() {
  if (!inited) begin();
  while (Serial.available() > 0) {
    int v = Serial.read();
    if (v >= 0) rxq.push((uint8_t)v);
  }
  uint8_t c;
  while (telnet_in_pop(&c))
    rxq.push(c);
  while (console_key_pop(&c))
    rxq.push(c);
}

}  // namespace vax_console
