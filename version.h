#pragma once

// Version strings live in version.cpp so a config.h bump rebuilds that
// translation unit. Arduino often leaves vVax.ino.cpp.o cached when only
// a header changes, which froze APP_VERSION in the USB/TFT banner.

const char* vvax_app_title();
const char* vvax_app_version();
const char* vvax_build_date();
void vvax_log_banner();
