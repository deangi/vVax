# host_lib (vVax Freenove)

Snapshot of vpdp1170 / v8088 host components used by vVax. **Freenove ESP32-S3 2.8" only** — no Elecrow CrowPanel, LovyanGFX, GT911, or SDSPI.

User-facing behavior is documented in the sketch [`README.md`](../README.md).
This file is for how the snapshot is wired into the Arduino sketch.

## Arduino compile rule

Sketch-root shims pull in `host_lib/` sources (Arduino compiles sketch-root `.cpp` only):

| Shim | Pulls in |
|------|----------|
| `fifo.h` | `host_lib/util/fifo.h` |
| `gfx.h` | `host_lib/gfx/gfx.h` (TFT_eSPI only) |
| `sd_fs.h` | `#define SD_FS SD_MMC` |
| `console.cpp` | `host_lib/console/console.cpp` |
| `host_time.cpp` | `host_lib/time/host_time.cpp` |
| `host_boot_input_build.cpp` | `boot/boot_input.cpp` |
| `host_lib_build.cpp` | storage guard, log, shell_*, TelnetPipe, wifi/net_task/net_ini, ADM-3A TU |

Not included: `board/*`, `touch/*` (sketch `touch.cpp`), `sd/sd_fs.*`, `boot_script`.

`platform.h` sets `HOST_LOG_TAG "vVax"` then includes `host_lib/log/host_log.h`.
`config.h` sets `VPDP_DISPLAY_BACKEND` to TFT_eSPI.

## Console

Default personality is **VT100** (same as vpdp1170). Do **not** switch to ADM-3A.

- `console_set_personality(HOST_TERM_VT100)` at boot
- `console_start_output_task()` — ANSI parse/render off the guest core
- Guest console → `vax_console` → `console_feed` + `telnet_write`

## Telnet

Sketch `telnet.cpp` wraps `TelnetPipe` and routes `ESC` `>` into `telnet_shell.*`
(status / reset / help for now).

## Network / time / NAT

- `host_wifi_connect` + `host_net_task_*` (Telnet, FTP, `host_time_poll`, `eth_nat::host_poll`)
- `[ntp]` in `/wificonfig.ini` → SNTP UTC for TOY
- Guest Ethernet NAT: sketch-root `eth_nat.*` (from vpdp1170); INI under `[ethernet]` in `/vaxconfig.ini`

## SD mutex

`host_lib/sd/storage_guard.h` owns the recursive SD mutex. FTP and MSCP paths
use `SD_FTP_StorageGuard` / `HostSdGuard`.
