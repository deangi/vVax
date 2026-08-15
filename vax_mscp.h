#pragma once
#include <stdint.h>
#include <stddef.h>

// RQDX3-class MSCP: at least two drives (dua / dub).
namespace vax_mscp {

enum Unit : uint8_t { UNIT_A = 0, UNIT_B = 1, UNIT_COUNT = 2 };

void begin();
void reset();
bool mount(Unit u, const char* path);
void unmount(Unit u);
bool mounted(Unit u);
const char* path(Unit u);
uint64_t size_bytes(Unit u);

// Stub I/O — real MSCP packets later.
bool read_blocks(Unit u, uint32_t lba, void* buf, size_t nblocks);
bool write_blocks(Unit u, uint32_t lba, const void* buf, size_t nblocks);

}  // namespace vax_mscp
