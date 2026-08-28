#include "touch.h"
#include "config.h"
#include "platform.h"
#include "FT6336U.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static FT6336U ft(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);
static bool    was_down = false;
static QueueHandle_t g_taps = nullptr;

struct Tap {
  int16_t x;
  int16_t y;
};

static void map_point(const FT6336U_TouchPointType& tp, int* sx, int* sy) {
  // Landscape (rotation 1) mapping - same as the Freenove touch tutorial.
  // This FT6336U reports Y about 22 px below the visible pixel position on
  // this display, so compensate here before UI hit testing.
  int x = tp.tp[0].y;
  int y = 240 - tp.tp[0].x - 22;
  if (x < 0) x = 0; else if (x > 319) x = 319;
  if (y < 0) y = 0; else if (y > 239) y = 239;
  *sx = x;
  *sy = y;
}

static void touch_scan() {
  FT6336U_TouchPointType tp = ft.scan();
  bool down = (tp.touch_count != 0);
  if (down && !was_down && g_taps) {
    int x = 0, y = 0;
    map_point(tp, &x, &y);
    Tap t = {(int16_t)x, (int16_t)y};
    xQueueSend(g_taps, &t, 0);
  }
  was_down = down;
}

static void touch_task(void*) {
  for (;;) {
    touch_scan();
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

void touch_init() {
  ft.begin();
  g_taps = xQueueCreate(8, sizeof(Tap));
  xTaskCreatePinnedToCore(touch_task, "touch", 3072, nullptr, 2, nullptr, 0);
  LOG("touch: FT6336U firmware id 0x%02X", ft.read_firmware_id());
}

bool touch_poll(int* x, int* y) {
  Tap t;
  if (!g_taps || xQueueReceive(g_taps, &t, 0) != pdTRUE)
    return false;
  if (x) *x = t.x;
  if (y) *y = t.y;
  return true;
}
