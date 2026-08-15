#pragma once
#ifndef HOST_LIB_GFX_H
#define HOST_LIB_GFX_H

// vVax is Freenove ESP32-S3 2.8" only (TFT_eSPI / ILI9341).
// Elecrow CrowPanel / LovyanGFX is not supported in this tree.

#include "config.h"

#if !defined(VPDP_DISPLAY_BACKEND) || VPDP_DISPLAY_BACKEND != VPDP_DISPLAY_TFT_ESPI
#error "vVax supports Freenove TFT_eSPI only (no CrowPanel / LovyanGFX)"
#endif

#include <TFT_eSPI.h>
using GfxDisplay = TFT_eSPI;

static inline void gfx_writeback(GfxDisplay&) {}
static inline void gfx_writeback(GfxDisplay&, int, int, int, int) {}

#endif  // HOST_LIB_GFX_H
