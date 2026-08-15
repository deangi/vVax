#include "telnet_shell.h"

#include "appconfig.h"
#include "config.h"
#include "fifo.h"
#include "platform.h"
#include "vVax_host.h"
#include "vax_cpu.h"
#include "vax_mscp.h"

#include <Arduino.h>
#include "esp_attr.h"
#include <stdarg.h>
#include <stdio.h>
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

static void output_char(uint8_t value) {
  g_output.push(value);
  if (value == 255) g_output.push(value);
}

static void output_text(const char* text) {
  if (!text) return;
  while (*text) output_char((uint8_t)*text++);
}

static void output_printf(const char* format, ...) {
  char buffer[384];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  output_text(buffer);
}

static void prompt() {
  output_text("vVax:/> ");
}

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

static void cmd_help() {
  output_text(
      "Commands:\r\n"
      "  help              this text\r\n"
      "  status            guest / MSCP / RAM summary\r\n"
      "  reset             cold-restart guest\r\n"
      "  exit              return to VAX console\r\n");
}

static void cmd_status() {
  output_printf("guest: %s\r\n", host_guest_status());
  output_printf("RAM:   %u bytes\r\n", (unsigned)vax_cpu::ram_bytes());
  for (int i = 0; i < vax_mscp::UNIT_COUNT; i++) {
    if (vax_mscp::mounted((vax_mscp::Unit)i)) {
      output_printf("MSCP %c: %s (%llu bytes)\r\n",
                    'A' + i, vax_mscp::path((vax_mscp::Unit)i),
                    (unsigned long long)vax_mscp::size_bytes((vax_mscp::Unit)i));
    } else {
      output_printf("MSCP %c: (not mounted)\r\n", 'A' + i);
    }
  }
  output_printf("ethernet config: %s\r\n",
                cfg.eth_enabled ? "enabled (NAT secondary)" : "disabled");
}

static void run_command(const char* line) {
  if (!line || !*line) return;
  if (!strcasecmp(line, "help") || !strcasecmp(line, "?")) {
    cmd_help();
  } else if (!strcasecmp(line, "status")) {
    cmd_status();
  } else if (!strcasecmp(line, "reset")) {
    host_request_guest_restart();
    output_text("guest restart requested\r\n");
  } else if (!strcasecmp(line, "exit") || !strcasecmp(line, "quit")) {
    telnet_shell_disconnect();
    output_text("returning to console\r\n");
    return;
  } else {
    output_printf("unknown: %s\r\n", line);
  }
  if (g_active) prompt();
}

void telnet_shell_init() {
  if (g_initialized) return;
  g_output.init(g_output_storage, SHELL_OUTPUT_BYTES);
  g_output.clear();
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
