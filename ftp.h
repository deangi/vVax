#pragma once
#include <stdint.h>

void        ftp_begin(uint16_t port, bool enabled, const char* user, const char* pass);
void        ftp_poll();
bool        ftp_enabled();
bool        ftp_listening();
bool        ftp_connected();
uint16_t    ftp_port();
