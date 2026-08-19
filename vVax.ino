// vVax — MicroVAX II–class emulator on Freenove ESP32-S3 2.8" Display
//
// Requires TFT_eSPI with FNK0104B selected in User_Setup_Select.h.
// Board: ESP32S3 Dev Module, USB CDC on boot enabled, OPI PSRAM, 16 MB flash,
// Huge App partition (3 MB APP).
// V0.1 scaffold — Dean Gienger, Cursor
// First test - boot NetBSD 10.1 - from image that boots on open simh
// Second test - boot VAX/VMS
//

#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <SD_MMC.h>
#include "Freenove_WS2812_Lib_for_ESP32.h"

#include "config.h"
#include "platform.h"
#include "secrets.h"
#include "appconfig.h"
#include "console.h"
#include "telnet.h"
#include "telnet_shell.h"
#include "ftp.h"
#include "touch.h"
#include "ui.h"
#include "vVax_host.h"
#include "vax_cpu.h"
#include "vax_mmu.h"
#include "vax_console.h"
#include "vax_clock.h"
#include "vax_mscp.h"
#include "vax_boot.h"
#include "eth_nat.h"
#include "host_lib/console/term_personality.h"
#include "host_lib/net/net_task.h"
#include "host_lib/net/wifi_sta.h"
#include "host_lib/sd/storage_guard.h"
#include "host_lib/time/host_time.h"

static TFT_eSPI tft;
static Freenove_ESP32_WS2812 strip(LED_COUNT, LED_PIN, LED_CHANNEL, TYPE_GRB);
AppConfig cfg;
static bool sd_ok = false;
static bool guest_ready = false;
static volatile bool g_guest_restart_req = false;
static SemaphoreHandle_t g_ui_mutex = nullptr;

enum BootState { BOOT_RUNNING, BOOT_OK, BOOT_FAIL };
static BootState boot_state = BOOT_RUNNING;

static void led(uint8_t r, uint8_t g, uint8_t b) {
  strip.setLedColorData(0, r, g, b);
  strip.show();
}

static void tft_banner() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(4, 4);
  tft.printf("%s  %s", APP_TITLE, APP_VERSION);
  tft.setCursor(4, 22);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.printf("build %s  MicroVAX II", APP_BUILD_DATE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

static void tft_status(int row, const char* label, const char* value, uint16_t color) {
  int y = 50 + row * 18;
  tft.fillRect(0, y, TFT_W, 18, TFT_BLACK);
  tft.setCursor(4, y);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.print(label);
  tft.setTextColor(color, TFT_BLACK);
  tft.print(value);
}

enum {
  ROW_PSRAM = 0, ROW_SD, ROW_CFG, ROW_MSCP, ROW_WIFI, ROW_IP, ROW_CPU
};

void host_request_guest_restart() { g_guest_restart_req = true; }

const char* host_guest_status() {
  if (!guest_ready) return "not ready";
  return vax_cpu::running() ? "running" : "halted (stub)";
}

static void on_wifi_up() {
  if (!host_time_synced())
    host_time_begin(cfg.ntp_enabled, cfg.ntp_server.c_str());
}

static void wifi_connect() {
  const char* ssid = cfg.wifi_ssid.c_str();
  const char* pass = cfg.wifi_password.c_str();
  const char* host = cfg.wifi_hostname.length() ? cfg.wifi_hostname.c_str() : WIFI_HOSTNAME;

  if (cfg.wifi_ssid.length() == 0) {
    LOGE("WiFi SSID is empty - set [wifi] ssid= in %s", WIFI_CFG_PATH);
    tft_status(ROW_WIFI, "WiFi:  ", "no SSID in wificonfig", TFT_RED);
    tft_status(ROW_IP,   "IP:    ", "(none)", TFT_RED);
    boot_state = BOOT_FAIL;
    return;
  }

  tft_status(ROW_WIFI, "WiFi:  ", "connecting...", TFT_YELLOW);
  host_wifi_set_up_hook(on_wifi_up);
  HostWifiConnectResult r =
      host_wifi_connect(ssid, pass, host, WIFI_CONNECT_TIMEOUT_MS);
  if (r.ok) {
    tft_status(ROW_WIFI, "WiFi:  ", ssid, TFT_GREEN);
    tft_status(ROW_IP,   "IP:    ", WiFi.localIP().toString().c_str(), TFT_GREEN);
    boot_state = BOOT_OK;
  } else {
    tft_status(ROW_WIFI, "WiFi:  ", "FAILED", TFT_RED);
    tft_status(ROW_IP,   "IP:    ", "(none)", TFT_RED);
    boot_state = BOOT_FAIL;
  }
}

static void sd_and_config_init() {
  tft_status(ROW_SD, "SD:    ", "mounting...", TFT_YELLOW);
  if (sd_mount()) {
    char info[32];
    uint64_t mb = SD_MMC.cardSize() / (1024ULL * 1024ULL);
    snprintf(info, sizeof(info), "OK  %llu MB", (unsigned long long)mb);
    tft_status(ROW_SD, "SD:    ", info, TFT_GREEN);
    sd_ok = true;
  } else {
    tft_status(ROW_SD, "SD:    ", "FAILED", TFT_RED);
    sd_ok = false;
  }

  tft_status(ROW_CFG, "Cfg:   ", "(reading)", TFT_YELLOW);
  config_apply_compiled_defaults(cfg);
  if (!sd_ok) {
    tft_status(ROW_CFG, "Cfg:   ", "defaults (no SD)", TFT_YELLOW);
  } else {
    bool wifi_ok = config_load_wifi(cfg);
    bool vax_ok  = config_load_vax(cfg);
    tft_status(ROW_CFG, "Cfg:   ",
               (wifi_ok && vax_ok) ? "loaded split cfg" : "wrote default cfg",
               (wifi_ok && vax_ok) ? TFT_GREEN : TFT_YELLOW);
  }
  config_print(cfg);
}

static void mount_mscp_drives() {
  vax_mscp::begin();
  vax_mscp::set_dump(vax_mscp::parse_dump_flags(cfg.mscp_dump_flags.c_str()),
                     cfg.mscp_dump_count);
  char line[48];
  int mounted = 0;
  if (cfg.disk_a.length()) {
    if (vax_mscp::mount(vax_mscp::UNIT_A, cfg.disk_a.c_str())) mounted++;
  }
  if (cfg.disk_b.length()) {
    if (vax_mscp::mount(vax_mscp::UNIT_B, cfg.disk_b.c_str())) mounted++;
  }
  snprintf(line, sizeof(line), "%d/2 mounted", mounted);
  tft_status(ROW_MSCP, "MSCP:  ", line, mounted ? TFT_GREEN : TFT_YELLOW);
}

static bool alloc_guest_ram() {
  // Call this *before* WiFi/lwIP — they consume enough PSRAM that a late
  // 8 MiB alloc always fails and we fall back to 6 MiB (which cannot hold
  // stock /boot @ 0x7D0000).
  const size_t free0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  LOG("PSRAM free before guest alloc: %u KB (largest block %u KB)",
      (unsigned)(free0 / 1024u), (unsigned)(largest / 1024u));

  int want = cfg.ram_mb;
  if (!vax_ram_mb_ok(want)) want = VAX_RAM_MB_DEFAULT;

  const size_t step = 64UL * 1024UL;
  const size_t size8 = 8UL * 1024UL * 1024UL;
  const size_t floor6 = 6UL * 1024UL * 1024UL;

  // For 8 MB: start at min(8 MiB, largest free block), aligned down to 64 KiB,
  // then back off 64 KiB at a time until alloc succeeds (or we hit 6 MiB).
  if (want >= 8) {
    size_t start = size8;
    if (largest < start) start = largest & ~(step - 1u);
    if (start > size8) start = size8;
    for (size_t bytes = start; bytes >= floor6; bytes -= step) {
      if (!vax_cpu::init(bytes)) {
        if (bytes == floor6) break;
        continue;
      }
      cfg.ram_mb = 8;
      if (bytes < size8) {
        LOG("Guest RAM: %u bytes (8 MB − %u KB)",
            (unsigned)bytes,
            (unsigned)((size8 - bytes) / 1024UL));
      }
      // /boot @ 0x7A0000 needs RAM through ~0x7B5000; stock @ 0x7D0000 needs ~0x7E5000.
      if (bytes < 0x7B5000u) {
        LOGE("Guest RAM < 0x7B5000 — /boot @ 0x7A0000 will not fit; "
             "use --new-base 0x5D0000 or free PSRAM");
      }
      return true;
    }
  }

  static const int kFall[] = { 6, 4, 2 };
  for (int i = 0; i < 3; i++) {
    int mb = kFall[i];
    if (mb > want) continue;
    size_t bytes = (size_t)mb * 1024UL * 1024UL;
    if (!vax_cpu::init(bytes)) continue;
    cfg.ram_mb = mb;
    if (mb != want)
      LOGE("Guest RAM fell back from %d MB to %d MB (PSRAM pressure)", want, mb);
    if (bytes < 0x7B5000u) {
      LOGE("Guest RAM < 0x7B5000 — need /boot @ 0x5D0000 (6 MB) or 0x7A0000 (≈8 MB−192 KB)");
    }
    return true;
  }
  LOGE("Guest RAM alloc failed (want %d MB, largest PSRAM block %u KB)",
       want, (unsigned)(largest / 1024u));
  return false;
}

static void apply_ethernet_nat() {
  eth_nat::reset();
  if (!cfg.eth_enabled) {
    LOG("ethernet NAT: disabled");
    return;
  }
  eth_nat::set_guest_mac(cfg.eth_mac);
  eth_nat::set_addresses(cfg.eth_guest_ip, cfg.eth_guest_mask, cfg.eth_gateway_ip);
  if (WiFi.status() == WL_CONNECTED)
    eth_nat::set_sta_ip((uint32_t)WiFi.localIP());
  LOG("ethernet NAT: enabled (DELQA stub hooks later)");
}

static void guest_cold_start() {
  vax_mmu::reset();
  vax_console::reset();
  if (cfg.clock_enabled) vax_clock::reset();
  vax_mscp::reset();
  telnet_reset_guest_io();
  for (size_t i = 0; i < cfg.boot_input_len; i++)
    vax_console::inject(cfg.boot_input[i]);
  vax_cpu::cold_boot();

  // Phase 6: NetBSD xxboot FROM750 handoff from configured boot unit.
  uint8_t unit = (cfg.boot_unit == 'b' || cfg.boot_unit == 'B') ? 1 : 0;
  if (vax_boot::start_mscp(unit)) {
    vax_cpu::run();
    LOG("guest running (NetBSD xxboot)");
  } else {
    LOG("guest halted (no MSCP boot)");
  }
}

static void render_task(void*) {
  for (;;) {
    console_render(tft);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void start_net_task() {
  host_net_task_add(telnet_poll);
  host_net_task_add(ftp_poll);
  host_net_task_add(host_time_poll);
  host_net_task_add(eth_nat::host_poll);
  host_net_task_start();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  LOG("%s %s build %s", APP_TITLE, APP_VERSION, APP_BUILD_DATE);

  strip.begin();
  led(0, 0, 32);

  tft.init();
  tft.setRotation(1);
  tft_banner();

  size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  char ps[40];
  snprintf(ps, sizeof(ps), "%u KB free", (unsigned)(free_psram / 1024));
  tft_status(ROW_PSRAM, "PSRAM: ", ps, free_psram > (5 * 1024 * 1024) ? TFT_GREEN : TFT_YELLOW);

  g_ui_mutex = xSemaphoreCreateMutex();
  sd_and_config_init();

  // Guest arena before WiFi/lwIP so 8 MB-class alloc can succeed.
  tft_status(ROW_CPU, "CPU:   ", "alloc RAM...", TFT_YELLOW);
  if (alloc_guest_ram()) {
    char msg[40];
    size_t rb = vax_cpu::ram_bytes();
    if (cfg.ram_mb == 8 && rb < 8UL * 1024UL * 1024UL)
      snprintf(msg, sizeof(msg), "OK %uK", (unsigned)(rb / 1024u));
    else
      snprintf(msg, sizeof(msg), "OK %d MB", cfg.ram_mb);
    tft_status(ROW_CPU, "CPU:   ", msg, TFT_GREEN);
    guest_ready = true;
  } else {
    tft_status(ROW_CPU, "CPU:   ", "RAM FAIL", TFT_RED);
    guest_ready = false;
  }

  mount_mscp_drives();

  console_init();
  console_set_personality(HOST_TERM_VT100);
  console_force_redraw();
  console_start_output_task();
  vax_console::begin();
  if (cfg.clock_enabled) vax_clock::begin();

  wifi_connect();
  if (NTP_BOOT_WAIT_MS > 0 && cfg.ntp_enabled) {
    uint32_t t0 = millis();
    while (!host_time_synced() && millis() - t0 < NTP_BOOT_WAIT_MS)
      delay(100);
  }

  telnet_begin((uint16_t)cfg.telnet_port, cfg.telnet_enabled);
  ftp_begin((uint16_t)cfg.ftp_port, cfg.ftp_enabled,
            cfg.ftp_user.c_str(), cfg.ftp_password.c_str());
  start_net_task();

  apply_ethernet_nat();

  if (guest_ready) {
    guest_cold_start();
    tft_status(ROW_CPU, "CPU:   ",
               vax_cpu::running() ? "booting MSCP" :
               (vax_cpu::state().r[0] == 0x4F4B0000u) ? "selftest PASS" : "halted",
               vax_cpu::running() ? TFT_GREEN :
               (vax_cpu::state().r[0] == 0x4F4B0000u) ? TFT_GREEN : TFT_YELLOW);
  }

  ui_begin(&tft, g_ui_mutex);
  xTaskCreatePinnedToCore(render_task, "render", 4096, nullptr, 1, nullptr, 0);

  led(0, 32, 0);
  LOG("setup complete — VT100 console active");
}

void loop() {
  telnet_shell_poll();
  ui_poll();
  eth_nat::tick();

  if (g_guest_restart_req) {
    g_guest_restart_req = false;
    if (guest_ready) guest_cold_start();
  }

  if (guest_ready) {
    static uint32_t live_ms = 0;
    uint32_t now = millis();
    if (now - live_ms >= 10000u) {
      live_ms = now;
      vax_cpu::State& st = vax_cpu::state();
      LOG("live: PC=%08X SP=%08X run=%u halt=%u MAPEN=%u",
          (unsigned)st.r[vax_cpu::R_PC], (unsigned)st.r[vax_cpu::R_SP],
          vax_cpu::running() ? 1u : 0u, st.halt ? 1u : 0u,
          vax_mmu::mapen() ? 1u : 0u);
    }
    vax_console::poll();
    if (cfg.clock_enabled) vax_clock::poll();
    vax_mscp::poll();
    if (vax_cpu::running())
      vax_cpu::step(5000);
  }

  delay(1);
}
