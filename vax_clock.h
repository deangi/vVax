#pragma once
#include <stdint.h>

// KA630 interval clock + TODR (IPRs ICCS=24, NICR=25, ICR=26, TODR=27).
namespace vax_clock {

enum {
  CSR_IE   = 0x40,
  CSR_DONE = 0x80
};

void begin();
void reset();
void poll();                 // 100 Hz tick from host millis; refresh TODR
void force_tick();           // one extra 10 ms tick (idle-warp during boot)

uint32_t ticks();
void get_toy(uint8_t* y, uint8_t* mon, uint8_t* d,
             uint8_t* h, uint8_t* min, uint8_t* s);

uint32_t iccs_rd();
void     iccs_wr(uint32_t v);
uint32_t nicr_rd();
void     nicr_wr(uint32_t v);
uint32_t icr_rd();
uint32_t todr_rd();
void     todr_wr(uint32_t v);

bool irq_clk();              // latched on 100 Hz tick while IE
void irq_clk_ack();          // taken by CPU — next tick may SET again
bool selftest();

}  // namespace vax_clock
