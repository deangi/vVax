#pragma once
#include <stdint.h>

// KA630 console terminal via IPRs (MicroVAX II):
//   RXCS=32 RXDB=33 TXCS=34 TXDB=35
// Host path: VT100 TFT + Telnet + USB (vax_console → host_lib).
namespace vax_console {

enum {
  CSR_IE   = 0x40,
  CSR_DONE = 0x80
};

void begin();
void reset();
void poll();                 // harvest host keys; keep TX ready

void put_guest(uint8_t c);   // direct host emit (self-test MMIO / TXDB)
bool get_guest(uint8_t* c);  // pop RX FIFO (also used by RXDB)
void inject(uint8_t c);      // boot_text / host inject

// IPR accessors
uint32_t rxcs_rd();
void     rxcs_wr(uint32_t v);
uint32_t rxdb_rd();
uint32_t txcs_rd();
void     txcs_wr(uint32_t v);
void     txdb_wr(uint32_t v);

bool irq_rx();               // latched 0→1 of DONE && IE (VARM / SIMH)
bool irq_tx();
void irq_rx_ack();           // taken by CPU — do not re-fire until next edge
void irq_tx_ack();

bool selftest();             // TXDB print + RXCS DONE path

}  // namespace vax_console
