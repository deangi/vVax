#pragma once

#include <stddef.h>
#include <stdint.h>

void telnet_shell_init();
void telnet_shell_enter();
void telnet_shell_disconnect();
bool telnet_shell_active();

bool telnet_shell_input(uint8_t c);
bool telnet_shell_backspace();

void telnet_shell_poll();

// USB ~>> one-shot: queue one line; output is teed to Serial.
void telnet_shell_queue_usb(const char* line);

size_t telnet_shell_output_peek(const uint8_t** data);
void telnet_shell_output_consume(size_t bytes);
