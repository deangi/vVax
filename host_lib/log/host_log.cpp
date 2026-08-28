#include "host_log.h"

volatile bool g_serial_silenced = false;
void (*g_host_log_aux)(const char* line) = nullptr;
