#pragma once
#include <stdint.h>

// Interval timer + TOY (time-of-year) as required for OS boot.
namespace vax_clock {

void begin();
void reset();
void poll();                 // advance ticks from host millis / NTP
uint32_t ticks();
void get_toy(uint8_t* y, uint8_t* mon, uint8_t* d,
             uint8_t* h, uint8_t* min, uint8_t* s);

}  // namespace vax_clock
