#pragma once
#include <stdint.h>

// Single-client Telnet server. Guest console output is sent raw; client
// keystrokes are queued to the guest keyboard. ESC > enters the host shell.

void        telnet_begin(uint16_t port, bool enabled);
void        telnet_poll();
void        telnet_reset_guest_io();
void        telnet_write(uint8_t c);
void        telnet_diag_write(uint8_t c);

bool        telnet_in_pop(uint8_t* out);

bool        telnet_connected();
bool        telnet_listening();
const char* telnet_client_ip();
uint16_t    telnet_port();
bool        telnet_enabled();
bool        telnet_shell_connected();
