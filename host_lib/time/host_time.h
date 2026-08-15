#pragma once
#include <stddef.h>

// Host wall-clock via SNTP (UTC). FatFs get_fattime() uses time()/localtime;
// with configTime(0,0,server) that is UTC. Non-blocking: begin after WiFi
// is up and poll until the first sync.

void host_time_begin(bool enabled, const char* server);
bool host_time_synced();
void host_time_poll();

// UTC "YYYY-MM-DD HH:MM:SS" after SNTP sync. Returns false if not synced.
bool host_time_format_utc(char* buf, size_t buflen);
