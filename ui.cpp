#include "ui.h"

#include "appconfig.h"
#include "config.h"
#include "console.h"
#include "ftp.h"
#include "platform.h"
#include "telnet.h"
#include "touch.h"
#include "version.h"
#include "vVax_host.h"
#include "vax_cpu.h"
#include "vax_mscp.h"
#include "host_lib/sd/storage_guard.h"

#include <SD_MMC.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

static TFT_eSPI* g_tft = nullptr;
static SemaphoreHandle_t g_ui_mutex = nullptr;
static bool g_open = false;
static uint32_t g_ips = 0;
static uint32_t g_ips_ms = 0;
static uint32_t g_ips_instr = 0;
static uint32_t g_status_ms = 0;
static uint32_t g_tap1_ms = 0;
static int g_tap1_x = 0, g_tap1_y = 0;

enum Screen : uint8_t { SCR_MAIN, SCR_FILES, SCR_WIFI, SCR_VAX };
static Screen g_scr = SCR_MAIN;
static int g_file_unit = 0;

static constexpr uint16_t COL_BG = 0x1082;
static constexpr uint16_t COL_BTN = 0x3186;
static constexpr uint16_t COL_HI = TFT_YELLOW;
// Status band: medium blue so grey 3D pills read as buttons, not the bar.
static constexpr uint16_t COL_BAND     = 0x3315;  // RGB ~48,96,176
static constexpr uint16_t COL_PILL_OFF    = 0x4A69;  // slate / grey
static constexpr uint16_t COL_PILL_YELLOW = 0xC4A0;  // amber (white text readable)
static constexpr uint16_t COL_PILL_GREEN  = 0x2444;  // forest green (white text readable)
static constexpr uint16_t COL_PILL_ON     = COL_PILL_YELLOW;
static constexpr uint16_t COL_PILL_HI     = 0xDEFB;  // bevel highlight
static constexpr uint16_t COL_PILL_LO     = 0x2104;  // bevel shadow
static constexpr int kPillH = 14;
static constexpr int kPillW = 32;

static constexpr int kCols = 2;
static constexpr int kRows = 4;
static constexpr int kGridX = 6;
static constexpr int kGridY = 28;
static constexpr int kGap = 6;
static constexpr int kCellW = (TFT_W - 2 * kGridX - kGap) / kCols;
static constexpr int kCellH = (TFT_H - kGridY - 6 - (kRows - 1) * kGap) / kRows;

bool ui_take_tft() {
  if (!g_ui_mutex) return true;
  return xSemaphoreTake(g_ui_mutex, pdMS_TO_TICKS(40)) == pdTRUE;
}

void ui_give_tft() {
  if (g_ui_mutex) xSemaphoreGive(g_ui_mutex);
}

bool ui_menu_open() { return g_open; }
uint32_t ui_ips() { return g_ips; }

static void sample_ips() {
  uint32_t now = millis();
  uint32_t n = vax_cpu::instr_count();
  if (g_ips_ms == 0) {
    g_ips_ms = now;
    g_ips_instr = n;
    return;
  }
  uint32_t dt = now - g_ips_ms;
  if (dt < 1000) return;
  g_ips = (uint32_t)((uint64_t)(n - g_ips_instr) * 1000ull / dt);
  g_ips_ms = now;
  g_ips_instr = n;
}

// Raised 3D chip. Face color is the status: grey / yellow / green.
static void pill(int x, int y, int w, const char* txt, uint16_t face) {
  g_tft->fillRoundRect(x, y, w, kPillH, 3, face);
  g_tft->drawFastHLine(x + 2, y, w - 4, COL_PILL_HI);
  g_tft->drawFastVLine(x, y + 2, kPillH - 4, COL_PILL_HI);
  g_tft->drawFastHLine(x + 2, y + kPillH - 1, w - 4, COL_PILL_LO);
  g_tft->drawFastVLine(x + w - 1, y + 2, kPillH - 4, COL_PILL_LO);
  g_tft->drawFastHLine(x + 3, y + 1, w - 6, COL_PILL_HI);
  g_tft->setTextFont(1);
  g_tft->setTextColor(TFT_WHITE, face);
  const int tw = (int)strlen(txt) * 6;
  int tx = x + (w - tw) / 2;
  if (tx < x + 2) tx = x + 2;
  g_tft->setCursor(tx, y + (kPillH - 8) / 2);
  g_tft->print(txt);
}

static void pill(int x, int y, int w, const char* txt, bool on) {
  pill(x, y, w, txt, on ? COL_PILL_ON : COL_PILL_OFF);
}

static uint16_t du_face(vax_mscp::Unit u) {
  if (!vax_mscp::mounted(u)) return COL_PILL_OFF;
  if (vax_mscp::unit_busy(u)) return COL_PILL_YELLOW;
  return COL_PILL_GREEN;
}

static void draw_status_band() {
  if (!g_tft) return;
  const int y = VPDP_STATUS_BAND_Y;
  g_tft->fillRect(0, y, TFT_W, VPDP_STATUS_BAND_H, COL_BAND);
  g_tft->drawFastHLine(0, y, TFT_W, TFT_DARKGREY);

  g_tft->setTextFont(1);
  g_tft->setTextColor(TFT_WHITE, COL_BAND);
  g_tft->setCursor(4, y + 3);
  const char* title = cfg.title.length() ? cfg.title.c_str() : vvax_app_title();
  g_tft->print(title);
  g_tft->print(' ');
  g_tft->print(vvax_app_version());

  char kbuf[20];
  uint32_t k = g_ips;
  if (k >= 1000)
    snprintf(kbuf, sizeof(kbuf), "%u.%01uk", (unsigned)(k / 1000),
             (unsigned)((k / 100) % 10));
  else
    snprintf(kbuf, sizeof(kbuf), "%u", (unsigned)k);
  const int kips_w = (int)(strlen(kbuf) + 4) * 6;
  g_tft->setTextColor(TFT_CYAN, COL_BAND);
  g_tft->setCursor(TFT_W - 4 - kips_w, y + 4);
  g_tft->print(kbuf);
  g_tft->print(" ips");

  // Guest CPU lamp: RUN = interpreter executing. Parked right of title,
  // just left of KIPS, so it cannot collide with the version string.
  const char* run_txt = "STOP";
  bool run_on = false;
  const char* gs = host_guest_status();
  if (!strcmp(gs, "running")) {
    run_txt = "RUN";
    run_on = true;
  } else if (!strcmp(gs, "paused")) {
    run_txt = "PAUSE";
  } else if (!strcmp(gs, "fault")) {
    run_txt = "FAULT";
  } else if (!strcmp(gs, "halted")) {
    run_txt = "HALT";
  } else if (!strcmp(gs, "not ready")) {
    run_txt = "----";
  }
  const int run_w = 40;
  const int run_x = TFT_W - 4 - kips_w - 8 - run_w;
  pill(run_x, y + 2, run_w, run_txt, run_on);

  char ip[20] = "(no ip)";
  const bool wifi_up = (WiFi.status() == WL_CONNECTED);
  if (wifi_up)
    strncpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip) - 1);

  const int pill_gap = 4;
  const int py = y + 20;
  pill(4, py, kPillW, "DU0", du_face(vax_mscp::UNIT_A));
  pill(4 + kPillW + pill_gap, py, kPillW, "DU1", du_face(vax_mscp::UNIT_B));

  const int ftp_x = TFT_W - 4 - kPillW;
  const int tel_x = ftp_x - pill_gap - kPillW;
  uint16_t tel_face = COL_PILL_OFF;
  if (wifi_up)
    tel_face = telnet_connected() ? COL_PILL_GREEN : COL_PILL_YELLOW;
  pill(tel_x, py, kPillW, "TEL", tel_face);
  pill(ftp_x, py, kPillW, "FTP", ftp_connected());

  g_tft->setTextFont(1);
  g_tft->setTextColor(TFT_WHITE, COL_BAND);
  g_tft->setCursor(4 + 2 * (kPillW + pill_gap), py + (kPillH - 8) / 2);
  g_tft->print(ip);
}

static void cell_geom(int col, int row, int* x, int* y, int* w, int* h) {
  *x = kGridX + col * (kCellW + kGap);
  *y = kGridY + row * (kCellH + kGap);
  *w = kCellW;
  *h = kCellH;
}

static void draw_cell(int col, int row, const char* l1, const char* l2) {
  int x, y, w, h;
  cell_geom(col, row, &x, &y, &w, &h);
  g_tft->fillRoundRect(x, y, w, h, 4, COL_BTN);
  g_tft->setTextFont(2);
  g_tft->setTextColor(TFT_WHITE, COL_BTN);
  g_tft->setCursor(x + 8, y + (l2 && l2[0] ? 8 : (h / 2 - 8)));
  g_tft->print(l1);
  if (l2 && l2[0]) {
    g_tft->setTextFont(1);
    g_tft->setTextColor(TFT_LIGHTGREY, COL_BTN);
    g_tft->setCursor(x + 8, y + 28);
    g_tft->print(l2);
  }
}

static bool hit_cell(int tx, int ty, int* col, int* row) {
  for (int r = 0; r < kRows; r++) {
    for (int c = 0; c < kCols; c++) {
      int x, y, w, h;
      cell_geom(c, r, &x, &y, &w, &h);
      if (tx >= x && tx < x + w && ty >= y && ty < y + h) {
        *col = c;
        *row = r;
        return true;
      }
    }
  }
  return false;
}

static void file_base(const char* path, char* out, size_t n) {
  const char* base = strrchr(path, '/');
  base = base ? base + 1 : path;
  strncpy(out, base, n - 1);
  out[n - 1] = 0;
}

static void du_subtitle(vax_mscp::Unit u, char* out, size_t n) {
  if (!vax_mscp::mounted(u)) {
    strncpy(out, "(empty)", n - 1);
    out[n - 1] = 0;
    return;
  }
  file_base(vax_mscp::path(u), out, n);
}

static void draw_title(const char* t) {
  g_tft->fillScreen(COL_BG);
  g_tft->setTextFont(2);
  g_tft->setTextColor(COL_HI, COL_BG);
  g_tft->setCursor(8, 6);
  g_tft->print(t);
}

static void draw_main() {
  draw_title("vVax settings  (paused)");
  char sub0[20], sub1[20], br[12];
  du_subtitle(vax_mscp::UNIT_A, sub0, sizeof(sub0));
  du_subtitle(vax_mscp::UNIT_B, sub1, sizeof(sub1));
  snprintf(br, sizeof(br), "%u%%", (unsigned)host_brightness());
  draw_cell(0, 0, "DU0", sub0);
  draw_cell(1, 0, "DU1", sub1);
  draw_cell(0, 1, "WiFi config", "wificonfig*.ini");
  draw_cell(1, 1, "VAX config", "vaxconfig*.ini");
  draw_cell(0, 2, "Brightness -", br);
  draw_cell(1, 2, "Brightness +", br);
  draw_cell(0, 3, "Restart guest", nullptr);
  draw_cell(1, 3, "Close", nullptr);
}

static char g_files[7][44];
static int g_nfiles = 0;
static char g_cfgs[7][44];
static int g_ncfgs = 0;

static int list_dir_files(const char* dir, const char* suffix,
                          char names[][44], int max) {
  HostSdGuard guard;
  fs::File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) return 0;
  int n = 0;
  fs::File f;
  while (n < max && (f = d.openNextFile())) {
    if (!f.isDirectory()) {
      const char* nm = f.name();
      const char* base = strrchr(nm, '/');
      base = base ? base + 1 : nm;
      if (!suffix || strstr(base, suffix)) {
        strncpy(names[n], base, 43);
        names[n][43] = 0;
        n++;
      }
    }
    f.close();
  }
  d.close();
  return n;
}

static void draw_grid_list(const char* title, char names[][44], int n,
                           const char* empty_msg) {
  draw_title(title);
  for (int i = 0; i < n; i++)
    draw_cell(i % kCols, i / kCols, names[i], nullptr);
  if (n == 0) {
    g_tft->setTextFont(2);
    g_tft->setTextColor(TFT_WHITE, COL_BG);
    g_tft->setCursor(12, 48);
    g_tft->print(empty_msg);
  }
  draw_cell(1, 3, "Back", nullptr);
}

static void draw_files() {
  char title[28];
  snprintf(title, sizeof(title), "Mount DU%d", g_file_unit);
  g_nfiles = list_dir_files("/disks", nullptr, g_files, 7);
  draw_grid_list(title, g_files, g_nfiles, "(no images in /disks)");
}

static void draw_wifi_cfg() {
  g_ncfgs = config_list_variants("wificonfig", g_cfgs, 7);
  draw_grid_list("WiFi config (reboot host)", g_cfgs, g_ncfgs,
                 "(no wificonfig*.ini)");
}

static void draw_vax_cfg() {
  g_ncfgs = config_list_variants("vaxconfig", g_cfgs, 7);
  draw_grid_list("VAX config (reboot host)", g_cfgs, g_ncfgs,
                 "(no vaxconfig*.ini)");
}

static void draw_menu() {
  switch (g_scr) {
    case SCR_MAIN: draw_main(); break;
    case SCR_FILES: draw_files(); break;
    case SCR_WIFI: draw_wifi_cfg(); break;
    case SCR_VAX: draw_vax_cfg(); break;
  }
}

static void open_menu() {
  g_open = true;
  g_scr = SCR_MAIN;
  if (ui_take_tft()) {
    draw_menu();
    ui_give_tft();
  }
}

static void close_menu() {
  g_open = false;
  console_force_redraw();
  if (ui_take_tft()) {
    draw_status_band();
    ui_give_tft();
  }
}

static bool hit_back(int col, int row) { return col == 1 && row == 3; }

static void handle_main(int x, int y) {
  int col, row;
  if (!hit_cell(x, y, &col, &row)) return;
  const int i = row * kCols + col;
  switch (i) {
    case 0:
      g_file_unit = 0;
      g_scr = SCR_FILES;
      draw_menu();
      break;
    case 1:
      g_file_unit = 1;
      g_scr = SCR_FILES;
      draw_menu();
      break;
    case 2:
      g_scr = SCR_WIFI;
      draw_menu();
      break;
    case 3:
      g_scr = SCR_VAX;
      draw_menu();
      break;
    case 4: {
      uint8_t b = host_brightness();
      host_set_brightness((uint8_t)(b >= 20 ? b - 10 : 10));
      draw_menu();
      break;
    }
    case 5: {
      uint8_t b = host_brightness();
      host_set_brightness((uint8_t)(b <= 90 ? b + 10 : 100));
      draw_menu();
      break;
    }
    case 6:
      host_request_guest_restart();
      close_menu();
      break;
    case 7:
      close_menu();
      break;
  }
}

static void handle_files(int x, int y) {
  int col, row;
  if (!hit_cell(x, y, &col, &row)) return;
  if (hit_back(col, row)) {
    g_scr = SCR_MAIN;
    draw_menu();
    return;
  }
  int i = row * kCols + col;
  if (i < 0 || i >= g_nfiles) return;
  char path[64];
  snprintf(path, sizeof(path), "/disks/%s", g_files[i]);
  vax_mscp::mount((vax_mscp::Unit)g_file_unit, path);
  g_scr = SCR_MAIN;
  draw_menu();
}

static void handle_config(int x, int y, Screen back, const char* dst) {
  int col, row;
  if (!hit_cell(x, y, &col, &row)) return;
  if (hit_back(col, row)) {
    g_scr = back;
    draw_menu();
    return;
  }
  int i = row * kCols + col;
  if (i < 0 || i >= g_ncfgs) return;
  char src[64];
  snprintf(src, sizeof(src), "/%s", g_cfgs[i]);
  if (config_copy_file(src, dst)) {
    LOG("config: copied %s -> %s", src, dst);
    if (!strcmp(dst, VAX_CFG_PATH)) {
      host_request_guest_restart();
      close_menu();
      return;
    }
  } else {
    LOGE("config: copy %s failed", src);
  }
  g_scr = SCR_MAIN;
  draw_menu();
}

static void on_tap(int x, int y) {
  if (!g_open) {
    uint32_t now = millis();
    int dx = x - g_tap1_x, dy = y - g_tap1_y;
    uint32_t dt = now - g_tap1_ms;
    bool dbl = g_tap1_ms != 0 && dt < 700u &&
               (dx * dx + dy * dy) < 80 * 80;
    g_tap1_ms = now;
    g_tap1_x = x;
    g_tap1_y = y;
    if (dbl || y >= VPDP_STATUS_BAND_Y)
      open_menu();
    return;
  }
  if (ui_take_tft()) {
    switch (g_scr) {
      case SCR_MAIN: handle_main(x, y); break;
      case SCR_FILES: handle_files(x, y); break;
      case SCR_WIFI: handle_config(x, y, SCR_MAIN, WIFI_CFG_PATH); break;
      case SCR_VAX: handle_config(x, y, SCR_MAIN, VAX_CFG_PATH); break;
    }
    ui_give_tft();
  }
}

void ui_clear_screen() {
  if (!g_tft || g_open) return;
  if (!ui_take_tft()) return;
  g_tft->fillScreen(TFT_BLACK);
  draw_status_band();
  ui_give_tft();
}

void ui_begin(TFT_eSPI* tft, SemaphoreHandle_t ui_mutex) {
  g_tft = tft;
  g_ui_mutex = ui_mutex;
  touch_init();
  if (ui_take_tft()) {
    draw_status_band();
    ui_give_tft();
  }
}

void ui_poll() {
  sample_ips();
  int x = 0, y = 0;
  while (touch_poll(&x, &y))
    on_tap(x, y);

  uint32_t now = millis();
  uint32_t interval =
      (vax_mscp::unit_busy(vax_mscp::UNIT_A) ||
       vax_mscp::unit_busy(vax_mscp::UNIT_B))
          ? 150u
          : 400u;
  if (!g_open && (now - g_status_ms) >= interval) {
    g_status_ms = now;
    if (ui_take_tft()) {
      draw_status_band();
      ui_give_tft();
    }
  }
}
