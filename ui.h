#pragma once
#include <TFT_eSPI.h>
#include <stdint.h>

// Minimal settings overlay (touch). Full menus land with later host polish.
void ui_begin(TFT_eSPI* tft, SemaphoreHandle_t ui_mutex);
void ui_poll();
bool ui_menu_open();
