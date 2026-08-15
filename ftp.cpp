#include "ftp.h"
#include "platform.h"
#include "vax_mscp.h"
#include "SD_FTP_Server/src/SD_FTP_Server.h"

static bool s_enabled = false;

static void ftp_log_info(const char* m) { LOG("%s", m); }
static void ftp_log_err(const char* m) { LOGE("%s", m); }

static bool ftp_path_protected(const char* path) {
  if (!path || !*path) return false;
  SD_FTP_StorageGuard guard;
  String candidate(path);
  candidate.toLowerCase();
  for (int i = 0; i < vax_mscp::UNIT_COUNT; i++) {
    if (!vax_mscp::mounted((vax_mscp::Unit)i)) continue;
    String image(vax_mscp::path((vax_mscp::Unit)i));
    if (!image.startsWith("/")) image = "/" + image;
    image.toLowerCase();
    if (image == candidate) return true;
    if (candidate != "/" && image.startsWith(candidate + "/")) return true;
  }
  return false;
}

void ftp_begin(uint16_t port, bool enabled, const char* user, const char* pass) {
  s_enabled = enabled;
  if (!enabled) { LOG("ftp: disabled in config"); return; }
  SD_FTP_Server::Config cfg;
  cfg.port = port;
  cfg.user = user ? user : "";
  cfg.pass = pass ? pass : "";
  cfg.vfs_root = "/sdcard";
  cfg.log_fn = ftp_log_info;
  cfg.log_err_fn = ftp_log_err;
  cfg.path_protected_fn = ftp_path_protected;
  SDFTPServer.begin(cfg);
}

void     ftp_poll()      { if (s_enabled) SDFTPServer.poll(); }
bool     ftp_enabled()   { return s_enabled; }
bool     ftp_listening() { return s_enabled && SDFTPServer.listening(); }
bool     ftp_connected() { return s_enabled && SDFTPServer.connected(); }
uint16_t ftp_port()      { return SDFTPServer.port(); }
