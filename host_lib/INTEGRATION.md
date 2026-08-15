# host_lib (v8088 Freenove)

Snapshot of vpdp1170 / vZ80 host components used by v8088. **Freenove ESP32-S3 2.8" only** — no Elecrow CrowPanel, LovyanGFX, GT911, or SDSPI.

User-facing behavior (Telnet shell, NTP DATE/TIME, PRN capture, disk sizes) is
documented in the sketch [`README.md`](../README.md). This file is for how the
snapshot is wired into the Arduino sketch.

## Arduino compile rule

Sketch-root shims pull in `host_lib/` sources (Arduino compiles sketch-root `.cpp` only):

| Shim | Pulls in |
|------|----------|
| `fifo.h` | `host_lib/util/fifo.h` |
| `gfx.h` | `host_lib/gfx/gfx.h` (TFT_eSPI only) |
| `sd_fs.h` | `#define SD_FS SD_MMC` |
| `console.cpp` | `host_lib/console/console.cpp` |
| `host_time.cpp` | `host_lib/time/host_time.cpp` |
| `lp_capture.cpp` | `host_lib/capture/lp_capture.cpp` |
| `host_boot_input_build.cpp` | `boot/boot_input.cpp` |
| `host_lib_build.cpp` | storage guard, log, shell_*, TelnetPipe, wifi/net_task/net_ini, ADM-3A TU |

Not included: `board/*`, `touch/*`, `sd/sd_fs.*`, `boot_script`.

`platform.h` sets `HOST_LOG_TAG "v8088"` then includes `host_lib/log/host_log.h`.
`config.h` sets `VPDP_DISPLAY_BACKEND` to TFT_eSPI.

## Console

Default personality is **VT100** (DOS/ANSI). Do **not** switch to ADM-3A for this tree.

- `console_set_personality(HOST_TERM_VT100)` at boot
- `console_start_output_task()` — ANSI parse/render off the 8088 core
- Guest PUTCHAR → `console_feed` + `telnet_write`; USB CDC only if `Serial.availableForWrite()`

## Telnet

Sketch `telnet.cpp` wraps `TelnetPipe` and routes `ESC` `>` into `telnet_shell.*`.

Shell MediaOps (PC geometry, not CP/M):

| create type | Size | How |
|-------------|------|-----|
| `floppy` | 1,474,560 | `disk_create_floppy` (FAT12) |
| `hdd` | 33,554,432 | `ensure_disk_image` zero-fill |

`reset` / GUI **Reboot 8088** → remount mounted images + `cpu_cold_boot()` + new LP session.

## Printer (LPT1 → /LPn.TXT)

Sketch owns the guest adapter; `lp_capture` owns the FIFO + SD consumer:

- INT 17h AH=00h prints AL; equipment / BDA advertise one parallel port at 0378h
- OUT 0378h latches data; falling strobe on 037Ah bit0 pushes into the FIFO
- `lp_capture::init()` once in `setup`; `begin_session()` on each guest cold restart

## Network / time

- `host_wifi_connect` + `host_net_task_*` (Telnet, FTP, `host_time_poll`)
- `[ntp]` in `/wificonfig.ini` → SNTP UTC
- Before DOS boot: wait up to `NTP_BOOT_WAIT_MS`, then `cpu_apply_host_utc()`
- Quiet DATE prompt + empty `boot_text`: apply RTC again, inject Enter/Enter
  (DOS 3.31 rejects two-digit years outside 80–99)

## SD mutex

`host_lib/sd/storage_guard.h` owns the recursive SD mutex. FTP and sketch
disk/config/shell paths use `SD_FTP_StorageGuard` (`HostSdGuard`).
