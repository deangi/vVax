#include "appconfig.h"
#include "config.h"
#include "platform.h"
#include "secrets.h"
#include "SD_FTP_Server/src/SD_FTP_Server.h"

#include <SD_MMC.h>
#include <string.h>

static String trim(const String& s) {
  int a = 0, b = (int)s.length();
  while (a < b && isspace((uint8_t)s[a])) a++;
  while (b > a && isspace((uint8_t)s[b - 1])) b--;
  return s.substring(a, b);
}

static String to_lower(const String& s) {
  String t = s;
  t.toLowerCase();
  return t;
}

static bool truthy(const String& v) {
  return v.equalsIgnoreCase("true") || v == "1"
      || v.equalsIgnoreCase("yes")  || v.equalsIgnoreCase("on");
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static String strip_inline_comment(const String& val) {
  bool in_quote = false;
  char quote = 0;
  bool escaped = false;
  for (int i = 0; i < (int)val.length(); i++) {
    char c = val[i];
    if (escaped) { escaped = false; continue; }
    if (c == '\\') { escaped = true; continue; }
    if (in_quote) {
      if (c == quote) in_quote = false;
      continue;
    }
    if (c == '"' || c == '\'') {
      in_quote = true;
      quote = c;
      continue;
    }
    if (c == ';' || c == '#') return trim(val.substring(0, i));
  }
  return trim(val);
}

static String unquote_config_value(const String& val) {
  if (val.length() >= 2) {
    char q = val[0];
    if ((q == '"' || q == '\'') && val[val.length() - 1] == q)
      return val.substring(1, val.length() - 1);
  }
  return val;
}

static void config_set_boot_input(AppConfig& cfg, const String& encoded) {
  cfg.boot_input_len = 0;
  String s = unquote_config_value(encoded);

  for (int i = 0; i < (int)s.length() &&
                  cfg.boot_input_len < AppConfig::BOOT_INPUT_MAX; i++) {
    uint8_t out = (uint8_t)s[i];

    if (s[i] == '^' && i + 1 < (int)s.length()) {
      char c = s[++i];
      if (c == '?') out = 0x7f;
      else          out = ((uint8_t)c) & 0x1f;
    } else if (s[i] == '\\' && i + 1 < (int)s.length()) {
      char c = s[++i];
      switch (c) {
        case 'r': out = '\r'; break;
        case 'n': out = '\n'; break;
        case 't': out = '\t'; break;
        case 'b': out = '\b'; break;
        case 'f': out = '\f'; break;
        case 'e': out = 0x1b; break;
        case 's': out = ' ';  break;
        case '\\': out = '\\'; break;
        case '"': out = '"'; break;
        case '\'': out = '\''; break;
        case 'x': {
          int v = 0;
          int digits = 0;
          while (i + 1 < (int)s.length() && digits < 2) {
            int h = hex_value(s[i + 1]);
            if (h < 0) break;
            v = (v << 4) | h;
            i++;
            digits++;
          }
          out = (uint8_t)v;
          break;
        }
        default:
          out = (uint8_t)c;
          break;
      }
    }

    cfg.boot_input[cfg.boot_input_len++] = out;
  }
}

static String escaped_bytes(const uint8_t* bytes, size_t len) {
  String out;
  char tmp[6];
  for (size_t i = 0; i < len; i++) {
    uint8_t c = bytes[i];
    switch (c) {
      case '\r': out += "\\r"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case 0x1b: out += "\\e"; break;
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      default:
        if (c >= 32 && c < 127) out += (char)c;
        else {
          snprintf(tmp, sizeof(tmp), "\\x%02X", c);
          out += tmp;
        }
        break;
    }
  }
  return out;
}

bool config_parse_ipv4(const char* s, uint32_t* out_host_order) {
  if (!s || !out_host_order) return false;
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  *out_host_order = (a << 24) | (b << 16) | (c << 8) | d;
  return true;
}

void config_format_ipv4(uint32_t host_order, char* buf, size_t buflen) {
  if (!buf || buflen < 8) return;
  snprintf(buf, buflen, "%u.%u.%u.%u",
           (unsigned)((host_order >> 24) & 0xff),
           (unsigned)((host_order >> 16) & 0xff),
           (unsigned)((host_order >> 8) & 0xff),
           (unsigned)(host_order & 0xff));
}

void config_format_mac(const uint8_t mac[6], char* buf, size_t buflen) {
  if (!buf || buflen < 18 || !mac) return;
  snprintf(buf, buflen, "%02X-%02X-%02X-%02X-%02X-%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool parse_mac(const String& val, uint8_t out[6]) {
  unsigned v[6] = {};
  char sep = 0;
  if (sscanf(val.c_str(), "%02x%c%02x%c%02x%c%02x%c%02x%c%02x",
             &v[0], &sep, &v[1], &sep, &v[2], &sep,
             &v[3], &sep, &v[4], &sep, &v[5]) != 11)
    return false;
  for (int i = 0; i < 6; i++) {
    if (v[i] > 255) return false;
    out[i] = (uint8_t)v[i];
  }
  return true;
}

bool sd_mount() {
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0, SD_MMC_D1, SD_MMC_D2, SD_MMC_D3);
  if (!SD_MMC.begin("/sdcard", false, false, 20000)) {
    LOGE("SD_MMC.begin() failed");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    LOGE("No SD card detected");
    return false;
  }
  uint64_t mb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
  LOG("SD mounted: size=%llu MB", (unsigned long long)mb);
  return true;
}

void config_apply_ethernet_defaults(AppConfig& cfg) {
  cfg.eth_enabled = false;
  const uint8_t def_mac[6] = { 0x08, 0x00, 0x2B, 0xAA, 0x00, 0x01 };
  memcpy(cfg.eth_mac, def_mac, 6);
  cfg.eth_guest_ip   = 0x0A0B0002u;
  cfg.eth_guest_mask = 0xFFFFFF00u;
  cfg.eth_gateway_ip = 0x0A0B0001u;
}

void config_apply_compiled_defaults(AppConfig& cfg) {
  cfg.title         = APP_TITLE;
  cfg.ram_mb        = VAX_RAM_MB_DEFAULT;

  cfg.wifi_ssid     = WIFI_SSID;
  cfg.wifi_password = WIFI_PASS;
  cfg.wifi_hostname = WIFI_HOSTNAME;

  cfg.ntp_enabled = true;
  cfg.ntp_server  = "pool.ntp.org";

  cfg.telnet_enabled = true;
  cfg.telnet_port    = TELNET_PORT;

  cfg.ftp_enabled  = true;
  cfg.ftp_port     = FTP_PORT;
  cfg.ftp_user     = FTP_DEFAULT_USER;
  cfg.ftp_password = FTP_DEFAULT_PASS;

  cfg.boot_input_len = 0;
  cfg.disk_a = "";
  cfg.disk_b = "/disks/NetBSD-10.1-vax.iso";
  cfg.boot_unit = 'a';

  config_apply_ethernet_defaults(cfg);
  cfg.clock_enabled = true;
  cfg.mscp_dump_flags = "";
  cfg.mscp_dump_count = 0;
}

enum ConfigDomain : uint8_t { CONFIG_NETWORK, CONFIG_EMULATOR };

static void parse_line(AppConfig& cfg, String& section, const String& raw,
                       ConfigDomain domain) {
  String t = trim(raw);
  if (t.length() == 0) return;
  if (t.startsWith(";") || t.startsWith("#")) return;
  if (t.startsWith("[") && t.endsWith("]")) {
    section = to_lower(t.substring(1, t.length() - 1));
    return;
  }
  int eq = t.indexOf('=');
  if (eq < 0) return;

  String key = to_lower(trim(t.substring(0, eq)));
  String val = strip_inline_comment(t.substring(eq + 1));

  bool network_section = section == "wifi" || section == "ntp" ||
                         section == "telnet" || section == "ftp";
  if ((domain == CONFIG_NETWORK) != network_section) return;

  if (section == "system") {
    if (key == "title") cfg.title = val;
    else if (key == "ram_mb" || key == "memory_mb") cfg.ram_mb = val.toInt();
  } else if (section == "wifi") {
    if      (key == "ssid")     cfg.wifi_ssid     = val;
    else if (key == "password") cfg.wifi_password = val;
    else if (key == "hostname") cfg.wifi_hostname = val;
  } else if (section == "ntp") {
    if      (key == "enabled")  cfg.ntp_enabled = truthy(val);
    else if (key == "server")   cfg.ntp_server  = val;
  } else if (section == "telnet") {
    if      (key == "enabled")  cfg.telnet_enabled = truthy(val);
    else if (key == "port")     cfg.telnet_port    = val.toInt();
  } else if (section == "ftp") {
    if      (key == "enabled")  cfg.ftp_enabled  = truthy(val);
    else if (key == "port")     cfg.ftp_port     = val.toInt();
    else if (key == "user")     cfg.ftp_user     = val;
    else if (key == "password") cfg.ftp_password = val;
  } else if (section == "console") {
    if (key == "boot_text" || key == "boot_input" ||
        key == "typeahead" || key == "boot_keys")
      config_set_boot_input(cfg, val);
  } else if (section == "disks") {
    if      (key == "a" || key == "dua" || key == "dua0") cfg.disk_a = val;
    else if (key == "b" || key == "dub" || key == "dub0") cfg.disk_b = val;
    else if (key == "boot")
      cfg.boot_unit = val.length() ? (char)tolower((uint8_t)val[0]) : 'a';
  } else if (section == "ethernet") {
    if (key == "enabled" || key == "ethernet") {
      cfg.eth_enabled = truthy(val);
    } else if (key == "mac" || key == "ethernet_mac") {
      uint8_t mac[6];
      if (parse_mac(val, mac)) memcpy(cfg.eth_mac, mac, 6);
    } else if (key == "guest_ip" || key == "ethernet_guest_ip") {
      uint32_t ip;
      if (config_parse_ipv4(val.c_str(), &ip)) cfg.eth_guest_ip = ip;
    } else if (key == "guest_mask" || key == "ethernet_guest_mask") {
      uint32_t ip;
      if (config_parse_ipv4(val.c_str(), &ip)) cfg.eth_guest_mask = ip;
    } else if (key == "gateway_ip" || key == "ethernet_gateway_ip") {
      uint32_t ip;
      if (config_parse_ipv4(val.c_str(), &ip)) cfg.eth_gateway_ip = ip;
    }
  } else if (section == "clock") {
    if (key == "enabled") cfg.clock_enabled = truthy(val);
  } else if (section == "diag") {
    if (key == "mscp_dump_flags" || key == "mscp_dump")
      cfg.mscp_dump_flags = val;
    else if (key == "mscp_dump_count") {
      long n = val.toInt();
      if (n < 0) cfg.mscp_dump_count = 0xFFFFFFFFu;  // unlimited
      else cfg.mscp_dump_count = (uint32_t)n;
    }
  }
}

static void recover_config_backup(const char* path) {
  if (SD_MMC.exists(path)) return;
  char backup[192];
  if (snprintf(backup, sizeof(backup), "%s.bak", path) >= (int)sizeof(backup)) return;
  if (SD_MMC.exists(backup)) {
    if (SD_MMC.rename(backup, path))
      LOG("Restored interrupted config update: %s", path);
  }
}

static bool parse_config_file(AppConfig& cfg, const char* path, ConfigDomain domain) {
  SD_FTP_StorageGuard guard;
  recover_config_backup(path);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  String section;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    parse_line(cfg, section, line, domain);
  }
  f.close();
  return true;
}

bool config_load_wifi(AppConfig& cfg) {
  cfg.wifi_ssid     = "";
  cfg.wifi_password = "";
  cfg.wifi_hostname = "";

  bool existed = parse_config_file(cfg, WIFI_CFG_PATH, CONFIG_NETWORK);
  if (!existed) {
    LOG("%s not found, writing defaults", WIFI_CFG_PATH);
    cfg.wifi_ssid     = WIFI_SSID;
    cfg.wifi_password = WIFI_PASS;
    cfg.wifi_hostname = WIFI_HOSTNAME;
    config_write_default_wifi(cfg);
    return false;
  }

  if (cfg.wifi_ssid.length() == 0)     cfg.wifi_ssid     = WIFI_SSID;
  if (cfg.wifi_password.length() == 0) cfg.wifi_password = WIFI_PASS;
  if (cfg.wifi_hostname.length() == 0) cfg.wifi_hostname = WIFI_HOSTNAME;
  if (cfg.ntp_server.length() == 0)    cfg.ntp_server    = "pool.ntp.org";
  if (cfg.telnet_port <= 0)            cfg.telnet_port    = TELNET_PORT;
  if (cfg.ftp_port <= 0)               cfg.ftp_port       = FTP_PORT;
  if (cfg.ftp_user.length() == 0)      cfg.ftp_user       = FTP_DEFAULT_USER;
  if (cfg.ftp_password.length() == 0)  cfg.ftp_password   = FTP_DEFAULT_PASS;
  return true;
}

bool config_load_vax(AppConfig& cfg) {
  cfg.title = APP_TITLE;
  cfg.disk_a = "";
  cfg.disk_b = "";
  cfg.boot_input_len = 0;
  config_apply_ethernet_defaults(cfg);

  bool existed = parse_config_file(cfg, VAX_CFG_PATH, CONFIG_EMULATOR);
  if (!existed) {
    LOG("%s not found, writing defaults", VAX_CFG_PATH);
    cfg.title = APP_TITLE;
    cfg.ram_mb = VAX_RAM_MB_DEFAULT;
    cfg.disk_a = "";
    cfg.disk_b = "/disks/NetBSD-10.1-vax.iso";
    cfg.boot_unit = 'a';
    config_write_default_vax(cfg);
    return false;
  }
  if (cfg.title.length() == 0) cfg.title = APP_TITLE;
  if (!vax_ram_mb_ok(cfg.ram_mb)) {
    LOGE("[system] ram_mb=%d invalid (use 2, 4, 6, or 8); defaulting to %d",
         cfg.ram_mb, VAX_RAM_MB_DEFAULT);
    cfg.ram_mb = VAX_RAM_MB_DEFAULT;
  }
  return true;
}

bool config_write_default_wifi(const AppConfig& cfg) {
  SD_FTP_StorageGuard guard;
  File f = SD_MMC.open(WIFI_CFG_PATH, FILE_WRITE);
  if (!f) { LOGE("Could not open %s for write", WIFI_CFG_PATH); return false; }
  f.println("; vVax network configuration");
  f.println("; Copy this to wificonfig-NAME.ini to create a named variant.");
  f.println();
  f.println("[wifi]");
  f.println("; Leave ssid/password blank to use the values compiled into secrets.h.");
  f.println("ssid     = ");
  f.println("password = ");
  f.printf ("hostname = %s\r\n", cfg.wifi_hostname.c_str());
  f.println();
  f.println("[ntp]");
  f.printf ("enabled = %s\r\n", cfg.ntp_enabled ? "true" : "false");
  f.printf ("server  = %s\r\n", cfg.ntp_server.c_str());
  f.println();
  f.println("[telnet]");
  f.printf ("enabled = %s\r\n", cfg.telnet_enabled ? "true" : "false");
  f.printf ("port    = %d\r\n", cfg.telnet_port);
  f.println();
  f.println("[ftp]");
  f.printf ("enabled  = %s\r\n", cfg.ftp_enabled ? "true" : "false");
  f.printf ("port     = %d\r\n", cfg.ftp_port);
  f.printf ("user     = %s\r\n", cfg.ftp_user.c_str());
  f.printf ("password = %s\r\n", cfg.ftp_password.c_str());
  f.close();
  return true;
}

bool config_write_default_vax(const AppConfig& cfg) {
  SD_FTP_StorageGuard guard;
  File f = SD_MMC.open(VAX_CFG_PATH, FILE_WRITE);
  if (!f) { LOGE("Could not open %s for write", VAX_CFG_PATH); return false; }
  char ip[16], mask[16], gw[16], mac[24];
  config_format_ipv4(cfg.eth_guest_ip, ip, sizeof(ip));
  config_format_ipv4(cfg.eth_guest_mask, mask, sizeof(mask));
  config_format_ipv4(cfg.eth_gateway_ip, gw, sizeof(gw));
  config_format_mac(cfg.eth_mac, mac, sizeof(mac));

  f.println("; vVax MicroVAX II configuration");
  f.println("; Copy this to vaxconfig-NAME.ini to create a named variant.");
  f.println();
  f.println("[system]");
  f.printf ("title  = %s\r\n", cfg.title.c_str());
  f.println("; Guest RAM in MB. Allowed: 2, 4, 6, or 8.");
  f.printf ("ram_mb = %d\r\n", cfg.ram_mb);
  f.println();
  f.println("[console]");
  f.println("; VT100 personality (TFT + Telnet + USB). Escaped boot_text:");
  f.printf ("boot_text = \"%s\"\r\n",
            escaped_bytes(cfg.boot_input, cfg.boot_input_len).c_str());
  f.println();
  f.println("[disks]");
  f.println("; a = installed RA disk; b = NetBSD ISO (or second pack).");
  f.printf ("a = %s\r\n", cfg.disk_a.c_str());
  f.printf ("b = %s\r\n", cfg.disk_b.c_str());
  f.printf ("boot = %c\r\n", cfg.boot_unit);
  f.println();
  f.println("[clock]");
  f.printf ("enabled = %s\r\n", cfg.clock_enabled ? "true" : "false");
  f.println();
  f.println("[ethernet]");
  f.println("; Secondary: DELQA-class guest Ethernet + NAT (vpdp1170 semantics).");
  f.printf ("enabled    = %s\r\n", cfg.eth_enabled ? "true" : "false");
  f.printf ("mac        = %s\r\n", mac);
  f.printf ("guest_ip   = %s\r\n", ip);
  f.printf ("guest_mask = %s\r\n", mask);
  f.printf ("gateway_ip = %s\r\n", gw);
  f.println();
  f.println("[diag]");
  f.println("; MSCP dump: flags=csr,init,ring,cmd,xfer,irq,all (or 0x3F); count=max lines (0=off, -1=unlimited).");
  f.printf ("mscp_dump_flags = %s\r\n", cfg.mscp_dump_flags.c_str());
  if (cfg.mscp_dump_count == 0xFFFFFFFFu)
    f.println("mscp_dump_count = -1");
  else
    f.printf ("mscp_dump_count = %lu\r\n", (unsigned long)cfg.mscp_dump_count);
  f.close();
  return true;
}

bool config_copy_file(const char* src, const char* dst) {
  SD_FTP_StorageGuard guard;
  File in = SD_MMC.open(src, FILE_READ);
  if (!in) return false;
  File out = SD_MMC.open(dst, FILE_WRITE);
  if (!out) { in.close(); return false; }
  uint8_t buf[512];
  while (in.available()) {
    int n = in.read(buf, sizeof(buf));
    if (n <= 0) break;
    out.write(buf, n);
  }
  in.close();
  out.close();
  return true;
}

int config_list_variants(const char* prefix, char names[][44], int max) {
  (void)prefix; (void)names; (void)max;
  return 0;  // variant picker UI later
}

void config_print(const AppConfig& cfg) {
  char ip[16], mask[16], gw[16], mac[24];
  config_format_ipv4(cfg.eth_guest_ip, ip, sizeof(ip));
  config_format_ipv4(cfg.eth_guest_mask, mask, sizeof(mask));
  config_format_ipv4(cfg.eth_gateway_ip, gw, sizeof(gw));
  config_format_mac(cfg.eth_mac, mac, sizeof(mac));
  LOG("---- %s + %s ----", WIFI_CFG_PATH, VAX_CFG_PATH);
  LOG("[system]  title=\"%s\"  ram_mb=%d", cfg.title.c_str(), cfg.ram_mb);
  LOG("[wifi]    ssid=\"%s\"  hostname=\"%s\"", cfg.wifi_ssid.c_str(),
      cfg.wifi_hostname.c_str());
  LOG("[telnet]  enabled=%s  port=%d", cfg.telnet_enabled ? "true" : "false",
      cfg.telnet_port);
  LOG("[disks]   a=\"%s\"  b=\"%s\"  boot=%c",
      cfg.disk_a.c_str(), cfg.disk_b.c_str(), cfg.boot_unit);
  LOG("[ethernet] enabled=%s  mac=%s  guest=%s/%s  gateway=%s",
      cfg.eth_enabled ? "true" : "false", mac, ip, mask, gw);
  LOG("[clock]   enabled=%s", cfg.clock_enabled ? "true" : "false");
  LOG("[diag]    mscp_dump_flags=\"%s\"  mscp_dump_count=%lu",
      cfg.mscp_dump_flags.c_str(), (unsigned long)cfg.mscp_dump_count);
}
