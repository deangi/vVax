#include "vax_console.h"
#include "console.h"
#include "telnet.h"
#include "fifo.h"
#include "platform.h"

#include <Arduino.h>
#include <string.h>

namespace vax_console {

#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

static constexpr size_t RX_CAP = 512;
EXT_RAM_BSS_ATTR static uint8_t rx_storage[RX_CAP];
static Fifo rxq;
static bool inited = false;

static uint32_t rxcs = 0;
static uint32_t txcs = CSR_DONE;  // transmitter ready at reset
static uint8_t  rxdb = 0;
// VARM: interrupt is requested when {IE AND DONE} changes 0→1, not while
// it stays 1. NetBSD gencntint never clears TX IE; a level-triggered
// irq_tx() livelocks after gencninit's mtpr(GC_TIE) (port-vax 2023-12-26).
static bool rx_irq_latched = false;
static bool tx_irq_latched = false;

static bool slu_ready_ie(uint32_t csr) {
  return (csr & (CSR_DONE | CSR_IE)) == (CSR_DONE | CSR_IE);
}

static void slu_eval_edge(uint32_t csr, bool was_ready_ie, bool* latched) {
  bool now = slu_ready_ie(csr);
  if (now && !was_ready_ie)
    *latched = true;
  if (!now)
    *latched = false;
}

void begin() {
  if (inited) return;
  rxq.init(rx_storage, RX_CAP);
  rxq.clear();
  inited = true;
  reset();
}

void reset() {
  if (inited) rxq.clear();
  rxcs = 0;
  txcs = CSR_DONE;
  rxdb = 0;
  rx_irq_latched = false;
  tx_irq_latched = false;
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
  if (!inited) begin();
  rxq.push(c);
}

static void harvest_keys() {
  if (!inited) begin();
  while (Serial.available() > 0) {
    int v = Serial.read();
    if (v >= 0) rxq.push((uint8_t)v);
  }
  uint8_t c;
  while (telnet_in_pop(&c)) rxq.push(c);
  while (console_key_pop(&c)) rxq.push(c);

  if (!(rxcs & CSR_DONE) && !rxq.empty()) {
    uint8_t b = 0;
    if (rxq.pop(&b)) {
      rxdb = b;
      bool was = slu_ready_ie(rxcs);
      rxcs |= CSR_DONE;
      slu_eval_edge(rxcs, was, &rx_irq_latched);
    }
  }
}

void poll() {
  harvest_keys();
  // Instant UART: TX completes on the next poll if still busy.
  if (!(txcs & CSR_DONE)) {
    bool was = slu_ready_ie(txcs);
    txcs |= CSR_DONE;
    slu_eval_edge(txcs, was, &tx_irq_latched);
  }
}

uint32_t rxcs_rd() {
  harvest_keys();
  return rxcs & (CSR_DONE | CSR_IE);
}

void rxcs_wr(uint32_t v) {
  bool was = slu_ready_ie(rxcs);
  rxcs = (rxcs & CSR_DONE) | (v & CSR_IE);
  slu_eval_edge(rxcs, was, &rx_irq_latched);
}

uint32_t rxdb_rd() {
  harvest_keys();
  uint32_t t = rxdb;
  if (rxcs & CSR_DONE) {
    bool was = slu_ready_ie(rxcs);
    rxcs &= ~CSR_DONE;
    slu_eval_edge(rxcs, was, &rx_irq_latched);
    // Prefetch next if available (may 0→1 DONE again).
    harvest_keys();
  }
  return t & 0xFF;
}

uint32_t txcs_rd() { return txcs & (CSR_DONE | CSR_IE); }

void txcs_wr(uint32_t v) {
  bool was = slu_ready_ie(txcs);
  txcs = (txcs & CSR_DONE) | (v & CSR_IE);
  slu_eval_edge(txcs, was, &tx_irq_latched);
}

void txdb_wr(uint32_t v) {
  put_guest((uint8_t)(v & 0xFF));
  // SIMH: CLR_INT on TXDB, then SET_INT when RDY 0→1.
  txcs &= ~CSR_DONE;
  tx_irq_latched = false;
  txcs |= CSR_DONE;
  if (txcs & CSR_IE)
    tx_irq_latched = true;
}

bool irq_rx() { return rx_irq_latched; }
bool irq_tx() { return tx_irq_latched; }
void irq_rx_ack() { rx_irq_latched = false; }
void irq_tx_ack() { tx_irq_latched = false; }

bool selftest() {
  begin();
  reset();
  // TX path
  if (!(txcs_rd() & CSR_DONE)) {
    LOGE("console selftest: TX not ready");
    return false;
  }
  const char* msg = "console OK\r\n";
  for (const char* p = msg; *p; p++)
    txdb_wr((uint8_t)*p);
  // RX path via inject
  inject('Z');
  poll();
  if (!(rxcs_rd() & CSR_DONE)) {
    LOGE("console selftest: RX DONE missing");
    return false;
  }
  if ((rxdb_rd() & 0xFF) != 'Z') {
    LOGE("console selftest: RXDB mismatch");
    return false;
  }
  LOG("console selftest: PASS");
  return true;
}

}  // namespace vax_console
