#include "vax_mscp.h"
#include "platform.h"
#include "host_lib/sd/storage_guard.h"

#include <SD_MMC.h>
#include <string.h>

namespace vax_mscp {

static constexpr size_t BLOCK = 512;

struct Drive {
  bool     open = false;
  String   path;
  File     file;
  uint64_t bytes = 0;
};

static Drive g_drv[UNIT_COUNT];

void begin() {
  for (int i = 0; i < UNIT_COUNT; i++) {
    if (g_drv[i].open) g_drv[i].file.close();
    g_drv[i] = Drive{};
  }
}

void reset() { /* keep mounts across soft reset */ }

bool mount(Unit u, const char* path) {
  if (u >= UNIT_COUNT || !path || !path[0]) return false;
  unmount(u);
  HostSdGuard guard;
  File f = SD_MMC.open(path, "r+");
  if (!f) {
    LOGE("MSCP[%u] open failed: %s", (unsigned)u, path);
    return false;
  }
  g_drv[u].file = f;
  g_drv[u].path = path;
  g_drv[u].bytes = f.size();
  g_drv[u].open = true;
  LOG("MSCP[%u] mounted %s (%llu bytes)", (unsigned)u, path,
      (unsigned long long)g_drv[u].bytes);
  return true;
}

void unmount(Unit u) {
  if (u >= UNIT_COUNT) return;
  if (g_drv[u].open) {
    HostSdGuard guard;
    g_drv[u].file.close();
  }
  g_drv[u] = Drive{};
}

bool mounted(Unit u) {
  return u < UNIT_COUNT && g_drv[u].open;
}

const char* path(Unit u) {
  if (!mounted(u)) return "";
  return g_drv[u].path.c_str();
}

uint64_t size_bytes(Unit u) {
  return mounted(u) ? g_drv[u].bytes : 0;
}

bool read_blocks(Unit u, uint32_t lba, void* buf, size_t nblocks) {
  if (!mounted(u) || !buf || nblocks == 0) return false;
  HostSdGuard guard;
  uint64_t off = (uint64_t)lba * BLOCK;
  if (!g_drv[u].file.seek(off)) return false;
  size_t want = nblocks * BLOCK;
  return g_drv[u].file.read((uint8_t*)buf, want) == (int)want;
}

bool write_blocks(Unit u, uint32_t lba, const void* buf, size_t nblocks) {
  if (!mounted(u) || !buf || nblocks == 0) return false;
  HostSdGuard guard;
  uint64_t off = (uint64_t)lba * BLOCK;
  if (!g_drv[u].file.seek(off)) return false;
  size_t want = nblocks * BLOCK;
  return g_drv[u].file.write((const uint8_t*)buf, want) == want;
}

}  // namespace vax_mscp
