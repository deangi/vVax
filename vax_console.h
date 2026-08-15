#pragma once
#include <stdint.h>

// Console UART (DZ11 / DL-style) → host_lib VT100 + Telnet.
namespace vax_console {

void begin();
void reset();
void poll();                 // TX to TFT/Telnet; RX from key FIFOs
void put_guest(uint8_t c);   // guest OUT
bool get_guest(uint8_t* c);  // guest IN (false if empty)
void inject(uint8_t c);      // host/boot_text inject

}  // namespace vax_console
