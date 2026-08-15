# vVaxSdCard

Copy these files to the **root** of a FAT32 SD card used in the Freenove board.

```text
/wificonfig.ini
/vaxconfig.ini
/firmware/     (optional ROM blobs — see firmware/README.md)
/disks/        (MSCP images — see disks/README.md)
```

Optional variants:

- `wificonfig-HOME.ini` — your private WiFi (do not commit secrets)
- `vaxconfig-netbsd.ini` — sample NetBSD-oriented profile

Split config matches vpdp1170 / v8088: network in `wificonfig.ini`, emulator
in `vaxconfig.ini`.
