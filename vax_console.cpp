#include "vax_console.h"
#include "config.h"
#include "console.h"
#include "telnet.h"
#include "telnet_shell.h"
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

// USB ~>> or `>> one-shot into the host shell (starter only at start of line).
// No timeout: next key either continues the escape or flushes the held prefix.
enum UsbEsc : uint8_t {
  USB_ESC_IDLE = 0,
  USB_ESC_STARTER,
  USB_ESC_STARTER_GT,
  USB_ESC_CMD
};
static uint8_t  g_usb_esc = USB_ESC_IDLE;
static uint8_t  g_usb_starter = '~';
static bool     g_usb_bol = true;
static bool     g_usb_skip_lf = false;
static char     g_usb_cmd[256];
static size_t   g_usb_cmd_len = 0;

static bool usb_is_starter(uint8_t c) { return c == '~' || c == '`'; }

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
  g_usb_esc = USB_ESC_IDLE;
  g_usb_starter = '~';
  g_usb_bol = true;
  g_usb_skip_lf = false;
  g_usb_cmd_len = 0;
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

static void usb_flush_held(uint8_t extra) {
  if (g_usb_esc == USB_ESC_STARTER)
    rxq.push(g_usb_starter);
  else if (g_usb_esc == USB_ESC_STARTER_GT) {
    rxq.push(g_usb_starter);
    rxq.push('>');
  }
  g_usb_esc = USB_ESC_IDLE;
  if (extra) {
    g_usb_bol = (extra == '\r' || extra == '\n');
    rxq.push(extra);
  } else {
    g_usb_bol = false;
  }
}

static void usb_handle(uint8_t c) {
  if (g_usb_skip_lf) {
    g_usb_skip_lf = false;
    if (c == '\n') return;
  }

  if (g_usb_esc == USB_ESC_CMD) {
    if (c == 0x03) {
      g_usb_esc = USB_ESC_IDLE;
      g_usb_cmd_len = 0;
      g_usb_bol = true;
      Serial.print("^C\r\n");
      return;
    }
    if (c == 0x08 || c == 0x7f) {
      if (g_usb_cmd_len) {
        g_usb_cmd_len--;
        Serial.print("\b \b");
      }
      return;
    }
    if (c == '\r' || c == '\n') {
      g_usb_cmd[g_usb_cmd_len] = 0;
      g_usb_esc = USB_ESC_IDLE;
      g_usb_bol = true;
      g_usb_skip_lf = (c == '\r');
      Serial.print("\r\n");
      telnet_shell_queue_usb(g_usb_cmd);
      g_usb_cmd_len = 0;
      return;
    }
    if (c >= 32 && c < 127 && g_usb_cmd_len + 1 < sizeof(g_usb_cmd)) {
      g_usb_cmd[g_usb_cmd_len++] = (char)c;
      Serial.write(c);
    }
    return;
  }

  if (g_usb_esc == USB_ESC_IDLE) {
    if (g_usb_bol && usb_is_starter(c)) {
      g_usb_esc = USB_ESC_STARTER;
      g_usb_starter = c;
      return;
    }
    g_usb_bol = (c == '\r' || c == '\n');
    rxq.push(c);
    return;
  }

  if (g_usb_esc == USB_ESC_STARTER) {
    if (c == '>') {
      g_usb_esc = USB_ESC_STARTER_GT;
      return;
    }
    usb_flush_held(c);
    return;
  }

  if (c == '>') {
    g_usb_esc = USB_ESC_CMD;
    g_usb_cmd_len = 0;
    Serial.print("\r\n[vVax ~>>] ");
    return;
  }
  usb_flush_held(c);
}

static void harvest_keys() {
  if (!inited) begin();
  while (Serial.available() > 0) {
    int v = Serial.read();
    if (v >= 0) usb_handle((uint8_t)v);
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
#if VVAX_DIAG_LEVEL >= 1
      static uint8_t logs = 8;
      if (logs) {
        logs--;
        LOG("cons: rx 0x%02X '%c' ie=%u latch=%u q=%u",
            (unsigned)b,
            (b >= 0x20u && b < 0x7fu) ? (char)b : '.',
            (rxcs & CSR_IE) ? 1u : 0u,
            rx_irq_latched ? 1u : 0u,
            (unsigned)rxq.count());
      }
#endif
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

bool irq_rx() { return rx_irq_latched || slu_ready_ie(rxcs); }
bool irq_tx() { return tx_irq_latched; }
void irq_rx_ack() {
  // SIMH tti_int stays up until RXDB is read (level). Ack-on-delivery plus
  // edge-only made DONE-stuck silent: IPL>=20 skipped the take, latch was
  // cleared, and harvest would not load another char while DONE stayed 1.
  rx_irq_latched = slu_ready_ie(rxcs);
}
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
