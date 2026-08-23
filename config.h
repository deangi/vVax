#pragma once
#ifndef VVAX_CONFIG_H
#define VVAX_CONFIG_H

// ---- App metadata ----
#define APP_TITLE       "vVax"
#define APP_VERSION     "V0.6.76"
// Stamp is expanded in version.cpp (not the .ino). Arduino does not reliably
// rebuild vVax.ino.cpp when only this header changes.
#define APP_BUILD_DATE  (__DATE__ " " __TIME__)

// Freenove 2.8" only. LovyanGFX / CrowPanel is not built in this tree.
#define VPDP_DISPLAY_TFT_ESPI    1
#define VPDP_DISPLAY_LOVYANGFX   2
#define VPDP_DISPLAY_BACKEND     VPDP_DISPLAY_TFT_ESPI

// ---- RGB LED (WS2812) ----
#define LED_PIN         42
#define LED_CHANNEL     0
#define LED_COUNT       1

// ---- Onboard button ----
#define BUTTON_PIN      0

// ---- TFT (ILI9341 via TFT_eSPI FNK0104B) ----
#define TFT_W           320
#define TFT_H           240
#define TEXT_COLS       80
#define TEXT_ROWS       25
#define CELL_W          4
#define CELL_H          8

// ---- Capacitive touch FT6336U (I2C) ----
#define TOUCH_SDA       16
#define TOUCH_SCL       15
#define TOUCH_RST       18
#define TOUCH_INT       17
#define TOUCH_I2C_ADDR  0x38

// ---- SD_MMC 4-bit ----
#define SD_MMC_CMD      40
#define SD_MMC_CLK      38
#define SD_MMC_D0       39
#define SD_MMC_D1       41
#define SD_MMC_D2       48
#define SD_MMC_D3       47

// ---- File paths on SD ----
#define WIFI_CFG_PATH   "/wificonfig.ini"
#define VAX_CFG_PATH    "/vaxconfig.ini"
#define DEFAULT_DUA0    "/disks/dua0.dsk"
#define DEFAULT_DUB0    "/disks/NetBSD-10.1-vax.iso"

// ---- Guest RAM: /vaxconfig.ini [system] ram_mb=  must be 2, 4, 6, or 8 ----
// 8 MB is enough for stock NetBSD /boot @ 0x7d0000. alloc_guest_ram() tries
// full 8 MiB then backs off 64 KiB at a time (before WiFi) down to 6 MiB.
#define VAX_RAM_MB_MIN      2
#define VAX_RAM_MB_DEFAULT  8
#define VAX_RAM_MB_MAX      8
#define vax_ram_mb_ok(mb)   ((mb) == 2 || (mb) == 4 || (mb) == 6 || (mb) == 8)

// ---- Guest diagnostics (USB serial [vVax] lines) ----
// 0 = quiet (default): milestones, errors, suspicious ACV (kernel PSL at user PC)
// 1 = boot debug: kprobe, limited MSCP DMA, repair/REI logs
// 2 = verbose: all ACV, DMA/xfer/load, frequent heartbeats
#define VVAX_DIAG_LEVEL 0

// Idle clock warp during kernel boot waits (config_finalize / ra_putonline).
// Compile-time only: 0 = omitted from exec_one (default); 1 = enable boot warp.
#define VAX_CLOCK_WARP_DEFAULT  0

// Guest instructions per loop() when running. Was 1000 + delay(1).
#define VVAX_STEP_BATCH         10000

// Compile-time guest CPU. One machine per firmware image — not a runtime switch.
// main / KA630 firmware stays MicroVAX II. This branch builds KA750 (11/750).
// Experimental: C0/C1 only; not a flashable guest-OS machine yet. Do not bump
// APP_VERSION until a real 750 drop exists (see docs/VAX11750.md).
#define VAX_MODEL_KA630  630
#define VAX_MODEL_KA750  750
#define VAX_MODEL        VAX_MODEL_KA750
#if VAX_MODEL != VAX_MODEL_KA630 && VAX_MODEL != VAX_MODEL_KA750
#error "VAX_MODEL must be VAX_MODEL_KA630 or VAX_MODEL_KA750"
#endif

// ---- Network ----
#define TELNET_PORT     23
#define FTP_PORT        21
#define FTP_DEFAULT_USER "esp32"
#define FTP_DEFAULT_PASS "esp32"

// ---- Boot tuning ----
#define WIFI_CONNECT_TIMEOUT_MS  20000
#define NTP_BOOT_WAIT_MS         12000

#endif  // VVAX_CONFIG_H
