#pragma once
#include <stdint.h>

// Host→guest hooks used by Telnet shell / UI.
void host_request_guest_restart();
void host_request_guest_halt();
void host_request_guest_continue();
const char* host_guest_status();
uint8_t host_brightness();           // 10..100 percent
void    host_set_brightness(uint8_t percent);
