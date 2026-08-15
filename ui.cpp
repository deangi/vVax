#include "ui.h"
#include "touch.h"

static TFT_eSPI* g_tft = nullptr;
static SemaphoreHandle_t g_ui_mutex = nullptr;
static bool g_open = false;

void ui_begin(TFT_eSPI* tft, SemaphoreHandle_t ui_mutex) {
  g_tft = tft;
  g_ui_mutex = ui_mutex;
  touch_init();
}

void ui_poll() {
  // Touch settings menu lands with later host polish.
  (void)g_tft;
  (void)g_ui_mutex;
  g_open = false;
}

bool ui_menu_open() { return g_open; }
