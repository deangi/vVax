#pragma once
#include <Arduino.h>
#include <stdio.h>

// Define HOST_LOG_TAG before including this header (e.g. "vpdp1170").
#ifndef HOST_LOG_TAG
#define HOST_LOG_TAG "host"
#endif

// Set true to mute USB-Serial LOG/LOGE (panic dump). TFT/Telnet are not gated.
extern volatile bool g_serial_silenced;
// Optional extra sink (Telnet diag). Must not call LOG/LOGE.
extern void (*g_host_log_aux)(const char* line);

// Round-robin of recent LOG/LOGE lines (no CR/LF). Survives guest flood.
void host_log_ring_push(const char* line);
void host_log_ring_dump(void (*out)(const char* line));
unsigned host_log_ring_count();

#define LOG(fmt, ...)   do { \
    char _hl[192]; \
    snprintf(_hl, sizeof(_hl), "[" HOST_LOG_TAG "] " fmt, ##__VA_ARGS__); \
    host_log_ring_push(_hl); \
    if (!g_serial_silenced) { Serial.print(_hl); Serial.print("\r\n"); } \
    if (g_host_log_aux) g_host_log_aux(_hl); \
  } while (0)
#define LOGE(fmt, ...)  do { \
    char _hl[192]; \
    snprintf(_hl, sizeof(_hl), "[" HOST_LOG_TAG " ERR] " fmt, ##__VA_ARGS__); \
    host_log_ring_push(_hl); \
    if (!g_serial_silenced) { Serial.print(_hl); Serial.print("\r\n"); } \
    if (g_host_log_aux) g_host_log_aux(_hl); \
  } while (0)
