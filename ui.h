#pragma once
#include <TFT_eSPI.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Phase 7: status band + double-tap settings overlay. Guest is paused while
// the menu is open. Take the TFT mutex around console_render().
void ui_begin(TFT_eSPI* tft, SemaphoreHandle_t ui_mutex);
void ui_poll();
bool ui_menu_open();
bool ui_take_tft();
void ui_give_tft();
void ui_clear_screen();              // blank TFT + redraw status (guest restart)
uint32_t ui_ips();
