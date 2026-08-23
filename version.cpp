#include "version.h"

#include "config.h"
#include "platform.h"

const char* vvax_app_title() { return APP_TITLE; }
const char* vvax_app_version() { return APP_VERSION; }
const char* vvax_build_date() { return APP_BUILD_DATE; }

void vvax_log_banner() {
  LOG("%s %s build %s", APP_TITLE, APP_VERSION, APP_BUILD_DATE);
}
