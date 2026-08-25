#include "vax_tu58.h"
#include "config.h"
#include "platform.h"

#include <string.h>

namespace vax_tu58 {

#if VAX_MODEL != VAX_MODEL_KA750

void begin() {}
void reset() {}
void poll() {}
uint32_t csrs_rd() { return 0; }
void     csrs_wr(uint32_t) {}
uint32_t csrd_rd() { return 0; }
uint32_t csts_rd() { return 0; }
void     csts_wr(uint32_t) {}
void     cstd_wr(uint32_t) {}
bool irq_rx() { return false; }
bool irq_tx() { return false; }
void irq_rx_ack() {}
void irq_tx_ack() {}

#else

// RSP (NetBSD sys/arch/vax/include/rsp.h). Single-byte INIT/CONTINUE;
// 14-byte command/end packets. No media → END + RSP_MOD_NOC.
static constexpr uint8_t RSP_TYP_COMMAND  = 002;
static constexpr uint8_t RSP_TYP_INIT     = 004;
static constexpr uint8_t RSP_TYP_CONTINUE = 020;
static constexpr uint8_t RSP_OP_END       = 0100;
static constexpr uint8_t RSP_MOD_NOC      = (uint8_t)(int8_t)-9;  // no cartridge

static constexpr unsigned RX_CAP = 32;

static uint32_t csrs = 0;
static uint32_t csts = CSR_DONE;
static uint8_t  csrd = 0;
static uint8_t  rxq[RX_CAP];
static uint8_t  rx_n = 0;
static uint8_t  rx_rd = 0;
static uint8_t  cmd[14];
static uint8_t  cmd_n = 0;
static bool     rx_irq_latched = false;
static bool     tx_irq_latched = false;
static bool     logged_present = false;

static bool slu_ready_ie(uint32_t csr) {
  return (csr & (CSR_DONE | CSR_IE)) == (CSR_DONE | CSR_IE);
}

static void slu_eval_edge(uint32_t csr, bool was, bool* latched) {
  bool now = slu_ready_ie(csr);
  if (now && !was)
    *latched = true;
  if (!now)
    *latched = false;
}

static void rx_push(uint8_t b) {
  if (rx_n >= RX_CAP) return;
  unsigned i = (unsigned)rx_rd + (unsigned)rx_n;
  if (i >= RX_CAP) i -= RX_CAP;
  rxq[i] = b;
  rx_n++;
}

static void rx_present() {
  if (csrs & CSR_DONE) return;
  if (rx_n == 0) return;
  csrd = rxq[rx_rd];
  rx_rd++;
  if (rx_rd >= RX_CAP) rx_rd = 0;
  rx_n--;
  bool was = slu_ready_ie(csrs);
  csrs |= CSR_DONE;
  slu_eval_edge(csrs, was, &rx_irq_latched);
}

static uint16_t rsp_cksum(const uint8_t* p, unsigned nbytes) {
  unsigned sum = 0;
  for (unsigned i = 0; i + 1u < nbytes; i += 2u)
    sum += (unsigned)p[i] | ((unsigned)p[i + 1u] << 8);
  while (sum > 65535u)
    sum = (sum & 65535u) + (sum >> 16);
  return (uint16_t)sum;
}

static void queue_continue() {
  rx_push(RSP_TYP_CONTINUE);
  rx_present();
  if (!logged_present) {
    logged_present = true;
    LOG("TU58: present, no cartridge");
  }
}

static void queue_end_noc() {
  uint8_t pkt[14];
  memset(pkt, 0, sizeof pkt);
  pkt[0] = RSP_TYP_COMMAND;
  pkt[1] = 012;           // 10 data bytes after typ/sz
  pkt[2] = RSP_OP_END;
  pkt[3] = RSP_MOD_NOC;
  uint16_t sum = rsp_cksum(pkt, 12);
  pkt[12] = (uint8_t)(sum & 0xFF);
  pkt[13] = (uint8_t)(sum >> 8);
  for (unsigned i = 0; i < 14; i++)
    rx_push(pkt[i]);
  rx_present();
}

void begin() { reset(); }

void reset() {
  csrs = 0;
  csts = CSR_DONE;
  csrd = 0;
  rx_n = 0;
  rx_rd = 0;
  cmd_n = 0;
  rx_irq_latched = false;
  tx_irq_latched = false;
  logged_present = false;
}

void poll() {
  rx_present();
  if (!(csts & CSR_DONE)) {
    bool was = slu_ready_ie(csts);
    csts |= CSR_DONE;
    slu_eval_edge(csts, was, &tx_irq_latched);
  }
}

uint32_t csrs_rd() {
  rx_present();
  return csrs & (CSR_DONE | CSR_IE);
}

void csrs_wr(uint32_t v) {
  bool was = slu_ready_ie(csrs);
  csrs = (csrs & CSR_DONE) | (v & CSR_IE);
  slu_eval_edge(csrs, was, &rx_irq_latched);
}

uint32_t csrd_rd() {
  rx_present();
  uint32_t t = csrd;
  if (csrs & CSR_DONE) {
    bool was = slu_ready_ie(csrs);
    csrs &= ~CSR_DONE;
    slu_eval_edge(csrs, was, &rx_irq_latched);
    rx_present();
  }
  return t & 0xFFu;
}

uint32_t csts_rd() { return csts & (CSR_DONE | CSR_IE | CSR_BRK); }

void csts_wr(uint32_t v) {
  bool was = slu_ready_ie(csts);
  uint32_t keep = csts & CSR_DONE;
  csts = keep | (v & (CSR_IE | CSR_BRK));
  if (v & CSR_BRK) {
    cmd_n = 0;
    rx_n = 0;
    rx_rd = 0;
    csrs &= ~CSR_DONE;
    rx_irq_latched = false;
  }
  slu_eval_edge(csts, was, &tx_irq_latched);
}

void cstd_wr(uint32_t v) {
  uint8_t c = (uint8_t)(v & 0xFFu);
  csts &= ~CSR_DONE;
  tx_irq_latched = false;
  if (c == RSP_TYP_INIT) {
    cmd_n = 0;
    queue_continue();
  } else if (c == RSP_TYP_COMMAND || cmd_n != 0) {
    if (cmd_n < sizeof cmd)
      cmd[cmd_n++] = c;
    if (cmd_n >= sizeof cmd) {
      queue_end_noc();
      cmd_n = 0;
    }
  }
  csts |= CSR_DONE;
  if (csts & CSR_IE)
    tx_irq_latched = true;
}

bool irq_rx() { return rx_irq_latched; }
bool irq_tx() { return tx_irq_latched; }
void irq_rx_ack() { rx_irq_latched = false; }
void irq_tx_ack() { tx_irq_latched = false; }

#endif  // VAX_MODEL_KA750

}  // namespace vax_tu58
