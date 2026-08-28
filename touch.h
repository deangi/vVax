#pragma once
#include <stdint.h>

// Capacitive touch (FT6336U). Coordinates are mapped to the landscape
// (rotation 1) TFT space: x 0..319, y 0..239.

void touch_init();

// Drain one queued tap (rising edge sampled by the 15 ms touch task).
// Returns true and writes *x,*y; call until false so a double-tap is not lost
// when loop() is busy in the guest.
bool touch_poll(int* x, int* y);
