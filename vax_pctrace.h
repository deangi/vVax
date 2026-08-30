#pragma once
#include <stdint.h>
#include "config.h"

// Circular last-N instruction log. Isolated so VVAX_PCTRACE 0 removes the
// ring, the exec_one branch, and this module's data from the binary.
namespace vax_pctrace {

#if VVAX_PCTRACE
static constexpr unsigned DEPTH = 128;

extern bool enabled;

void set_enabled(bool on);
void clear();
void record(uint32_t pc, uint8_t op, const uint32_t r[16], uint32_t psl);
void dump();

#else

static constexpr unsigned DEPTH = 0;

inline void set_enabled(bool) {}
inline void clear() {}
inline void record(uint32_t, uint8_t, const uint32_t*, uint32_t) {}
inline void dump() {}

#endif

}  // namespace vax_pctrace
