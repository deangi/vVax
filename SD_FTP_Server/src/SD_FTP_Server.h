#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "host_lib/sd/storage_guard.h"

// Minimal passive/active FTP server backed by an SD_MMC card mounted under
// a POSIX VFS root (default "/sdcard"). Single client at a time.

class SD_FTP_Server {
public:
  struct Config {
    uint16_t    port        = 21;
    const char* user        = "";
    const char* pass        = "";
    const char* vfs_root    = "/sdcard";
    void (*log_fn)(const char* msg)     = nullptr;
    void (*log_err_fn)(const char* msg) = nullptr;
    bool (*path_protected_fn)(const char* ftp_path) = nullptr;
  };

  void     begin(const Config& cfg);
  void     poll();
  bool     listening() const;
  bool     connected() const;
  uint16_t port()      const;
};

extern SD_FTP_Server SDFTPServer;
