#pragma once
#include <stdint.h>

// KA750 console TU58 via IPRs CSRS/CSRD/CSTS/CSTD (28–31).
// Device is always present; no cartridge is ever mounted (RSP END + NOC).
// This does not replace the ctuattach RET plant: NetBSD 10 ka750_conf()
// calls ctuattach() before bufq_init regardless of these CSRs.
namespace vax_tu58 {

enum {
  CSR_IE   = 0x40,
  CSR_DONE = 0x80,  // CSRS: RX data ready; CSTS: TX ready
  CSR_BRK  = 0x01   // CSTS: break
};

void begin();
void reset();
void poll();

uint32_t csrs_rd();
void     csrs_wr(uint32_t v);
uint32_t csrd_rd();
uint32_t csts_rd();
void     csts_wr(uint32_t v);
void     cstd_wr(uint32_t v);

bool irq_rx();
bool irq_tx();
void irq_rx_ack();
void irq_tx_ack();

}  // namespace vax_tu58
