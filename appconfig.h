#pragma once
#include <Arduino.h>

enum GuestOs : uint8_t { GUEST_OS_NETBSD = 0, GUEST_OS_VMS = 1 };

struct AppConfig {
  // [system] in /vaxconfig.ini
  String title;
  int    ram_mb = 8;   // [system] ram_mb= 2, 4, 6, or 8
  GuestOs os = GUEST_OS_NETBSD;  // [system] os= netbsd|bsd|vms|openvms

  // [wifi] in /wificonfig.ini
  String wifi_ssid;
  String wifi_password;
  String wifi_hostname;

  // [ntp] in /wificonfig.ini
  bool   ntp_enabled = true;
  String ntp_server;

  // [telnet] in /wificonfig.ini
  bool   telnet_enabled = true;
  int    telnet_port    = 23;

  // [ftp] in /wificonfig.ini
  bool   ftp_enabled  = true;
  int    ftp_port     = 21;
  String ftp_user;
  String ftp_password;

  // [console] in /vaxconfig.ini
  static const size_t BOOT_INPUT_MAX = 256;
  uint8_t boot_input[BOOT_INPUT_MAX];
  size_t  boot_input_len = 0;

  // [disks] — two MSCP slots (dua / dub)
  String disk_a;   // MSCP unit A (dua0)
  String disk_b;   // MSCP unit B (dub0)
  char   boot_unit = 'a';

  // [ethernet] — DELQA-class + NAT (vpdp1170 semantics); secondary feature
  bool     eth_enabled = false;
  uint8_t  eth_mac[6] = { 0x08, 0x00, 0x2B, 0xAA, 0x00, 0x01 };
  uint32_t eth_guest_ip   = 0x0A0B0002u;  // 10.11.0.2
  uint32_t eth_guest_mask = 0xFFFFFF00u;  // 255.255.255.0
  uint32_t eth_gateway_ip = 0x0A0B0001u;  // 10.11.0.1

  // [clock]
  bool clock_enabled = true;

  // [diag] — MSCP serial dump (flags: csr,init,ring,cmd,xfer,irq,all | 0xNN)
  String   mscp_dump_flags;     // raw token list / hex from INI
  uint32_t mscp_dump_count = 0; // max log lines; 0 = off
  bool     pctrace = false;     // last-N insn ring; dump on HALT (needs VVAX_PCTRACE)
};

extern AppConfig cfg;

bool sd_mount();

bool config_load_wifi(AppConfig& cfg);
bool config_load_vax(AppConfig& cfg);
bool config_write_default_wifi(const AppConfig& cfg);
bool config_write_default_vax(const AppConfig& cfg);
void config_apply_compiled_defaults(AppConfig& cfg);
void config_apply_ethernet_defaults(AppConfig& cfg);

bool config_parse_ipv4(const char* s, uint32_t* out_host_order);
void config_format_ipv4(uint32_t host_order, char* buf, size_t buflen);
void config_format_mac(const uint8_t mac[6], char* buf, size_t buflen);

bool config_copy_file(const char* src, const char* dst);
int  config_list_variants(const char* prefix, char names[][44], int max);

void config_print(const AppConfig& cfg);

const char* config_guest_os_name(GuestOs os);
