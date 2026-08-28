#include "telnet_shell.h"

#include "appconfig.h"
#include "config.h"
#include "fifo.h"
#include "platform.h"
#include "ui.h"
#include "vVax_host.h"
#include "vax_cpu.h"
#include "vax_mmu.h"
#include "vax_mscp.h"
#include "host_lib/sd/storage_guard.h"
#include "host_lib/shell/shell_core.h"
#include "host_lib/shell/shell_media.h"
#include "host_lib/shell/shell_settings.h"
#include "host_lib/time/host_time.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include "esp_attr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

static constexpr size_t SHELL_LINE_MAX = 255;
static constexpr size_t SHELL_QUEUE_DEPTH = 4;
static constexpr size_t SHELL_OUTPUT_BYTES = 4096;

static volatile bool g_active = false;
static char g_input_line[SHELL_LINE_MAX + 1];
static size_t g_input_len = 0;
static char g_commands[SHELL_QUEUE_DEPTH][SHELL_LINE_MAX + 1];
static volatile uint8_t g_command_head = 0;
static volatile uint8_t g_command_tail = 0;
EXT_RAM_BSS_ATTR static uint8_t g_output_storage[SHELL_OUTPUT_BYTES];
static Fifo g_output;
static bool g_initialized = false;
static bool g_cmds_registered = false;
static char g_cwd[128] = "/";

static void output_char(uint8_t value) {
  g_output.push(value);
  if (value == 255) g_output.push(value);
}

static void output_text(const char* text) {
  if (!text) return;
  while (*text) output_char((uint8_t)*text++);
}

static void prompt() { output_text("vVax:/> "); }

static bool queue_command(const char* command) {
  uint8_t next = (uint8_t)((g_command_head + 1) % SHELL_QUEUE_DEPTH);
  if (next == g_command_tail) return false;
  strncpy(g_commands[g_command_head], command, SHELL_LINE_MAX);
  g_commands[g_command_head][SHELL_LINE_MAX] = 0;
  g_command_head = next;
  return true;
}

static bool pop_command(char* command, size_t size) {
  if (g_command_head == g_command_tail) return false;
  strncpy(command, g_commands[g_command_tail], size - 1);
  command[size - 1] = 0;
  g_command_tail = (uint8_t)((g_command_tail + 1) % SHELL_QUEUE_DEPTH);
  return true;
}

static int split_args(char* line, char** argv, int max) {
  int argc = 0;
  char* p = line;
  while (*p && argc < max) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    if (*p == '"' || *p == '\'') {
      char q = *p++;
      argv[argc++] = p;
      while (*p && *p != q) p++;
      if (*p) *p++ = 0;
    } else {
      argv[argc++] = p;
      while (*p && *p != ' ' && *p != '\t') p++;
      if (*p) *p++ = 0;
    }
  }
  return argc;
}

static void join_path(char* out, size_t outlen, const char* base, const char* rel) {
  if (!rel || !*rel) {
    strncpy(out, base, outlen - 1);
    out[outlen - 1] = 0;
    return;
  }
  if (rel[0] == '/') {
    strncpy(out, rel, outlen - 1);
    out[outlen - 1] = 0;
    return;
  }
  snprintf(out, outlen, "%s%s%s", base,
           (base[0] && base[strlen(base) - 1] == '/') ? "" : "/", rel);
}

static void normalize_path(char* path) {
  // Collapse "/foo/../bar" and trailing slash (except root).
  char tmp[128];
  char* stack[16];
  int n = 0;
  strncpy(tmp, path, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = 0;
  for (char* tok = strtok(tmp, "/"); tok; tok = strtok(nullptr, "/")) {
    if (!strcmp(tok, ".") || !*tok) continue;
    if (!strcmp(tok, "..")) {
      if (n > 0) n--;
      continue;
    }
    if (n < 16) stack[n++] = tok;
  }
  char* o = path;
  *o++ = '/';
  for (int i = 0; i < n; i++) {
    size_t L = strlen(stack[i]);
    if ((size_t)(o - path) + L + 2 >= 128) break;
    if (i) *o++ = '/';
    memcpy(o, stack[i], L);
    o += L;
  }
  *o = 0;
}

static int parse_unit(const char* s) {
  if (!s) return -1;
  if (!strcasecmp(s, "a") || !strcasecmp(s, "dua") || !strcasecmp(s, "dua0") ||
      !strcasecmp(s, "ra0") || !strcasecmp(s, "0"))
    return 0;
  if (!strcasecmp(s, "b") || !strcasecmp(s, "dub") || !strcasecmp(s, "dub0") ||
      !strcasecmp(s, "ra1") || !strcasecmp(s, "1"))
    return 1;
  return -1;
}

static bool media_list(int index, MediaUnitInfo* out) {
  if (!out || index < 0 || index >= vax_mscp::UNIT_COUNT) return false;
  auto u = (vax_mscp::Unit)index;
  memset(out, 0, sizeof(*out));
  snprintf(out->name, sizeof(out->name), "%c", 'A' + index);
  out->mounted = vax_mscp::mounted(u);
  if (out->mounted) {
    strncpy(out->path, vax_mscp::path(u), sizeof(out->path) - 1);
    uint64_t b = vax_mscp::size_bytes(u);
    out->size_bytes = b > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)b;
    out->readonly = vax_mscp::readonly(u);
    strncpy(out->extra, "MSCP", sizeof(out->extra) - 1);
  }
  return true;
}

static bool media_mount(const char* unit, const char* path, bool readonly,
                        char* err, size_t errlen) {
  int u = parse_unit(unit);
  if (u < 0) {
    snprintf(err, errlen, "usage: mount a|b <path> [ro]");
    return false;
  }
  char full[160];
  join_path(full, sizeof(full), g_cwd, path);
  normalize_path(full);
  if (!vax_mscp::mount((vax_mscp::Unit)u, full)) {
    snprintf(err, errlen, "open failed: %s", full);
    return false;
  }
  (void)readonly;
  return true;
}

static bool media_dismount(const char* unit, char* err, size_t errlen) {
  int u = parse_unit(unit);
  if (u < 0) {
    snprintf(err, errlen, "usage: dismount a|b");
    return false;
  }
  vax_mscp::unmount((vax_mscp::Unit)u);
  return true;
}

static bool media_create(const char*, const char*, char* err, size_t errlen) {
  snprintf(err, errlen, "creating MSCP images on-device is not supported");
  return false;
}

static bool media_protected(const char* path) {
  if (!path) return false;
  for (int i = 0; i < vax_mscp::UNIT_COUNT; i++) {
    if (vax_mscp::mounted((vax_mscp::Unit)i) &&
        !strcmp(vax_mscp::path((vax_mscp::Unit)i), path))
      return true;
  }
  return false;
}

static const char* media_mount_usage() {
  return "usage: mount a|b <path> [ro]\r\n";
}

static bool guest_restart(char* err, size_t errlen) {
  (void)err;
  (void)errlen;
  host_request_guest_restart();
  return true;
}

static const char* guest_restart_help() {
  return "cold boot / remount";
}

static void cmd_help(int, char**) {
  shell_print_help();
  shell_out_text("  exit                       return to VAX console\r\n");
}

static void cmd_status(int, char**) {
  vax_cpu::State& st = vax_cpu::state();
  shell_out_printf("guest: %s\r\n", host_guest_status());
  shell_out_printf("PC=%08X PSL=%08X SP=%08X MAPEN=%u  %u ips\r\n",
                   (unsigned)st.r[vax_cpu::R_PC], (unsigned)st.psl,
                   (unsigned)st.r[vax_cpu::R_SP],
                   vax_mmu::mapen() ? 1u : 0u, (unsigned)ui_ips());
  shell_out_printf("RAM:   %u bytes\r\n", (unsigned)vax_cpu::ram_bytes());
  for (int i = 0; i < vax_mscp::UNIT_COUNT; i++) {
    if (vax_mscp::mounted((vax_mscp::Unit)i)) {
      shell_out_printf("MSCP %c: %s (%llu bytes)%s\r\n",
                       'A' + i, vax_mscp::path((vax_mscp::Unit)i),
                       (unsigned long long)vax_mscp::size_bytes((vax_mscp::Unit)i),
                       vax_mscp::readonly((vax_mscp::Unit)i) ? " ro" : "");
    } else {
      shell_out_printf("MSCP %c: (not mounted)\r\n", 'A' + i);
    }
  }
  if (WiFi.status() == WL_CONNECTED)
    shell_out_printf("IP:    %s\r\n", WiFi.localIP().toString().c_str());
  char tbuf[32];
  if (host_time_format_utc(tbuf, sizeof(tbuf)))
    shell_out_printf("NTP:   %s UTC\r\n", tbuf);
  else
    shell_out_text("NTP:   (not synced)\r\n");
}

static void cmd_halt(int, char**) {
  host_request_guest_halt();
  shell_out_text("guest halt requested\r\n");
}

static void cmd_continue(int, char**) {
  host_request_guest_continue();
  shell_out_text("guest continue\r\n");
}

static void cmd_regs(int, char**) {
  vax_cpu::State& st = vax_cpu::state();
  for (int i = 0; i < 12; i += 4) {
    shell_out_printf("R%-2d=%08X  R%-2d=%08X  R%-2d=%08X  R%-2d=%08X\r\n",
                     i, (unsigned)st.r[i], i + 1, (unsigned)st.r[i + 1],
                     i + 2, (unsigned)st.r[i + 2], i + 3, (unsigned)st.r[i + 3]);
  }
  shell_out_printf("AP=%08X FP=%08X SP=%08X PC=%08X\r\n",
                   (unsigned)st.r[vax_cpu::R_AP], (unsigned)st.r[vax_cpu::R_FP],
                   (unsigned)st.r[vax_cpu::R_SP], (unsigned)st.r[vax_cpu::R_PC]);
  shell_out_printf("PSL=%08X SCBB=%08X PCBB=%08X halt=%u fault=%u\r\n",
                   (unsigned)st.psl, (unsigned)st.scbb, (unsigned)st.pcbb,
                   st.halt ? 1u : 0u, (unsigned)st.fault);
}

static void cmd_pwd(int, char**) {
  shell_out_printf("%s\r\n", g_cwd);
}

static void cmd_cd(int argc, char** argv) {
  if (argc < 2) {
    shell_out_text("usage: cd <path>\r\n");
    return;
  }
  char next[128];
  join_path(next, sizeof(next), g_cwd, argv[1]);
  normalize_path(next);
  HostSdGuard guard;
  fs::File d = SD_MMC.open(next);
  if (!d || !d.isDirectory()) {
    shell_out_printf("error: not a directory: %s\r\n", next);
    if (d) d.close();
    return;
  }
  d.close();
  strncpy(g_cwd, next, sizeof(g_cwd) - 1);
  g_cwd[sizeof(g_cwd) - 1] = 0;
  shell_out_printf("%s\r\n", g_cwd);
}

static void cmd_ls(int argc, char** argv) {
  char path[128];
  join_path(path, sizeof(path), g_cwd, argc > 1 ? argv[1] : "");
  normalize_path(path);
  HostSdGuard guard;
  fs::File d = SD_MMC.open(path);
  if (!d) {
    shell_out_printf("error: cannot open %s\r\n", path);
    return;
  }
  if (!d.isDirectory()) {
    shell_out_printf("%s  %u\r\n", path, (unsigned)d.size());
    d.close();
    return;
  }
  int n = 0;
  fs::File f;
  while ((f = d.openNextFile()) && n < 48) {
    const char* nm = f.name();
    const char* base = strrchr(nm, '/');
    base = base ? base + 1 : nm;
    if (f.isDirectory())
      shell_out_printf("  %s/\r\n", base);
    else
      shell_out_printf("  %-24s %lu\r\n", base, (unsigned long)f.size());
    f.close();
    n++;
  }
  d.close();
}

static bool get_bright(uint32_t* out) {
  if (out) *out = host_brightness();
  return true;
}

static bool set_bright(uint32_t v, char* err, size_t errlen) {
  (void)err;
  (void)errlen;
  host_set_brightness((uint8_t)v);
  return true;
}

static bool get_ram(int32_t* out) {
  if (out) *out = cfg.ram_mb;
  return true;
}

static bool set_ram(int32_t v, char* err, size_t errlen) {
  if (!vax_ram_mb_ok(v)) {
    snprintf(err, errlen, "ram_mb must be 2, 4, 6, or 8");
    return false;
  }
  cfg.ram_mb = v;
  return true;
}

static void register_commands() {
  if (g_cmds_registered) return;
  g_cmds_registered = true;
  shell_set_out(output_text);

  static MediaOps media = {
      media_list, media_mount, media_dismount, media_create,
      media_protected, media_mount_usage, nullptr};
  shell_set_media_ops(&media);
  shell_register_media_commands();

  static GuestControlOps guest = { guest_restart, guest_restart_help };
  shell_set_guest_control_ops(&guest);
  shell_register_guest_control_commands();

  shell_register_set_command();
  ShellSettingDesc bright{};
  bright.name = "brightness";
  bright.type = ShellValueType::UInt;
  bright.help = "TFT backlight 10..100 percent";
  bright.min_u = 10;
  bright.max_u = 100;
  bright.get_u32 = get_bright;
  bright.set_u32 = set_bright;
  shell_register_setting(bright);

  ShellSettingDesc ram{};
  ram.name = "ram_mb";
  ram.type = ShellValueType::Int;
  ram.help = "guest RAM 2/4/6/8 (next host reboot)";
  ram.flags = ShellSetting_NextReboot;
  ram.min_i = 2;
  ram.max_i = 8;
  ram.get_i32 = get_ram;
  ram.set_i32 = set_ram;
  shell_register_setting(ram);

  static const char* help_alias[] = { "?", nullptr };
  shell_register("help", cmd_help, "help                       this text",
                 help_alias, "Host commands");
  shell_register("status", cmd_status,
                 "status                     guest / MSCP / IP / KIPS",
                 nullptr, "Host commands");
  shell_register("pwd", cmd_pwd, "pwd                        SD working directory",
                 nullptr, "File commands");
  shell_register("cd", cmd_cd, "cd <path>                  change SD directory",
                 nullptr, "File commands");
  shell_register("ls", cmd_ls, "ls [path]                  list SD files",
                 nullptr, "File commands");
  shell_register("halt", cmd_halt, "halt                       stop the VAX CPU",
                 nullptr, "VAX commands");
  static const char* cont_alias[] = { "continue", "go", nullptr };
  shell_register("cont", cmd_continue,
                 "cont                       resume the VAX CPU",
                 cont_alias, "VAX commands");
  static const char* reg_alias[] = { "registers", nullptr };
  shell_register("regs", cmd_regs, "regs                       dump GPRs / PSL",
                 reg_alias, "VAX commands");
}

static void run_command(const char* line) {
  if (!line || !*line) return;
  if (!strcasecmp(line, "exit") || !strcasecmp(line, "quit")) {
    telnet_shell_disconnect();
    output_text("returning to console\r\n");
    return;
  }
  char buf[SHELL_LINE_MAX + 1];
  strncpy(buf, line, SHELL_LINE_MAX);
  buf[SHELL_LINE_MAX] = 0;
  char* argv[8];
  int argc = split_args(buf, argv, 8);
  if (argc <= 0) return;
  if (!shell_dispatch(argc, argv)) {
    output_text("unknown: ");
    output_text(argv[0]);
    output_text("\r\n");
  }
  if (g_active) prompt();
}

void telnet_shell_init() {
  if (g_initialized) return;
  g_output.init(g_output_storage, SHELL_OUTPUT_BYTES);
  g_output.clear();
  register_commands();
  g_initialized = true;
}

void telnet_shell_enter() {
  telnet_shell_init();
  g_active = true;
  g_input_len = 0;
  g_command_head = g_command_tail = 0;
  g_output.clear();
}

void telnet_shell_disconnect() {
  g_active = false;
  g_input_len = 0;
}

bool telnet_shell_active() { return g_active; }

bool telnet_shell_input(uint8_t c) {
  if (!g_active) return false;
  if (c == '\r') {
    g_input_line[g_input_len] = 0;
    queue_command(g_input_line);
    g_input_len = 0;
    return false;
  }
  if (g_input_len < SHELL_LINE_MAX) {
    g_input_line[g_input_len++] = (char)c;
    return true;
  }
  return false;
}

bool telnet_shell_backspace() {
  if (!g_active || g_input_len == 0) return false;
  g_input_len--;
  return true;
}

void telnet_shell_poll() {
  char cmd[SHELL_LINE_MAX + 1];
  while (pop_command(cmd, sizeof(cmd)))
    run_command(cmd);
}

size_t telnet_shell_output_peek(const uint8_t** data) {
  return g_output.peek(data);
}

void telnet_shell_output_consume(size_t bytes) {
  g_output.consume(bytes);
}
