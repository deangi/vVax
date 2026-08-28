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

#define LOG(fmt, ...)   do { \
    if (!g_serial_silenced) Serial.printf("[" HOST_LOG_TAG "] " fmt "\r\n", ##__VA_ARGS__); \
    if (g_host_log_aux) { \
      char _hl[192]; \
      snprintf(_hl, sizeof(_hl), "[" HOST_LOG_TAG "] " fmt, ##__VA_ARGS__); \
      g_host_log_aux(_hl); \
    } \
  } while (0)
#define LOGE(fmt, ...)  do { \
    if (!g_serial_silenced) Serial.printf("[" HOST_LOG_TAG " ERR] " fmt "\r\n", ##__VA_ARGS__); \
    if (g_host_log_aux) { \
      char _hl[192]; \
      snprintf(_hl, sizeof(_hl), "[" HOST_LOG_TAG " ERR] " fmt, ##__VA_ARGS__); \
      g_host_log_aux(_hl); \
    } \
  } while (0)
