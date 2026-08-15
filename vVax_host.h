#pragma once

// Host→guest hooks used by Telnet shell / UI.
void host_request_guest_restart();
const char* host_guest_status();
