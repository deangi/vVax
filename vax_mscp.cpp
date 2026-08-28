#include "vax_mscp.h"
#include "config.h"
#include "vax_cpu.h"
#include "vax_clock.h"
#include "platform.h"
#include "host_lib/sd/storage_guard.h"

#include <SD_MMC.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace vax_mscp {

static constexpr size_t BLOCK = 512;
static constexpr uint16_t PACKET_BYTES = 64;

static constexpr uint16_t SA_ERROR = 0x8000;
static constexpr uint16_t SA_STEP4 = 0x4000;
static constexpr uint16_t SA_STEP3 = 0x2000;
static constexpr uint16_t SA_STEP2 = 0x1000;
static constexpr uint16_t SA_STEP1 = 0x0800;

static constexpr uint16_t S1_CTRL_DIAG = 0x0100;
static constexpr uint16_t S1_CTRL_MAP  = 0x0040;
static constexpr uint16_t S1_HOST_VALID = 0x8000;
static constexpr uint16_t S1_HOST_WRAP  = 0x4000;
static constexpr uint16_t S1_HOST_IE    = 0x0080;
static constexpr uint16_t S1_HOST_VEC   = 0x007f;
static constexpr uint16_t S2_COMM_LOW   = 0xfffe;
static constexpr uint16_t S3_PURGE_POLL = 0x8000;
static constexpr uint16_t S3_COMM_HIGH  = 0x7fff;
static constexpr uint16_t S4_GO         = 0x0001;

static constexpr uint32_t DESC_OWN  = 0x80000000u;
static constexpr uint32_t DESC_FLAG = 0x40000000u;
static constexpr uint32_t DESC_ADDR = 0x003ffffeu;

static constexpr uint16_t PORT_MODEL = 6;   // UDA50A-ish; RQDX3-compatible enough for bring-up
static constexpr uint16_t PORT_SWVER = 3;

static constexpr uint16_t ST_OFL_NV   = 3 | (1 << 5);   // offline / no volume
static constexpr uint16_t ST_OFL_UNK  = 3 | (0 << 5);   // offline / unknown unit
static constexpr uint16_t ST_AVL      = 4;
static constexpr uint16_t ST_WPR      = 6;
static constexpr uint16_t ST_BAD_OP   = 1 | (8 << 8);
static constexpr uint16_t ST_BAD_VER  = 1 | (12 << 8);
static constexpr uint16_t ST_HST_OA   = 9 | (1 << 5);
static constexpr uint16_t ST_HST_OC   = 9 | (2 << 5);
static constexpr uint16_t ST_HST_NXM  = 9 | (3 << 5);
static constexpr uint16_t ST_CMD_BCNT = 1 | (12 << 8);
static constexpr uint16_t ST_CMD_LBN  = 1 | (28 << 8);
static constexpr uint16_t ST_DRV      = 11;

enum class State : uint8_t {
  Step1, Step1Wrap, Step2, Step3, Step3PurgeSa, Step3PurgeIp, Step4, Run, Dead
};

struct Drive {
  bool     open = false;
  bool     readonly = false;
  bool     online = false;
  String   path;
  File     file;
  uint64_t bytes = 0;
};

struct Ring {
  int8_t   irq_off = 0;
  uint32_t base = 0;
  uint16_t byte_len = 0;
  uint16_t index = 0;
};

struct Xfer {
  bool     active = false;
  bool     write = false;
  uint8_t  unit = 0;
  uint32_t lbn = 0;
  uint32_t count = 0;
  uint32_t requested = 0;
  uint32_t address = 0;
  uint32_t offset = 0;
  uint16_t status = 0;
};

static Drive g_drv[UNIT_COUNT];
static PhysRead8Fn  g_rd = nullptr;
static PhysWrite8Fn g_wr = nullptr;

static State   g_state = State::Step1;
static uint16_t g_sa = 0;
static uint16_t g_pending_sa = 0;
static bool     g_sa_pending = false;
static uint16_t g_step1 = 0;
static uint16_t g_vec = 0154;
static uint16_t g_cmd_ring_len = 0;
static uint16_t g_rsp_ring_len = 0;
static uint32_t g_comm = 0;
// Q22 is 22-bit. NetBSD raopen STEP3 only writes 6 high bits
// (((phys)>>16)&077), so /boot above 4 MiB becomes comm=0x003Bxxxx while
// rings live at 0x007Bxxxx. g_q22_high is the 4 MiB alias that restores phys.
#if VAX_MODEL == VAX_MODEL_KA630
static uint32_t g_q22_high = 0;
#endif
static bool     g_logged_s0_dma = false;
static uint8_t  g_dma_log_left = 40;
static uint32_t g_load_sectors = 0;
static uint32_t g_load_log_next = 128;
static bool     g_rsp_own_logged = false;
static bool     g_ie = false;
static bool     g_purge = false;
static bool     g_poll = false;
static bool     g_cmd_valid = false;
static bool     g_rsp_valid = false;
static bool     g_irq_latched = false;
static bool     g_in_doorbell = false;   // IP-read is servicing rings
static uint16_t g_irq_defer = 0;         // guest insns until latch_irq
static bool     g_host_online_wait = false;
static bool     g_first_rsp = true;
static uint8_t  g_rsp_delay = 0;
static uint16_t g_ctl_flags = 0;

static Ring g_cmd_ring;
static Ring g_rsp_ring;
static uint8_t g_cmd_pkt[PACKET_BYTES];
static uint8_t g_rsp_pkt[PACKET_BYTES];
static uint16_t g_rsp_len = 0;
static Xfer g_xfer;
static uint32_t g_access_ms[UNIT_COUNT];
static constexpr uint32_t kAccessHoldMs = 500u;

static void note_access(uint8_t u) {
  if (u < UNIT_COUNT) g_access_ms[u] = millis();
}

static uint32_t g_dump_flags = 0;
static uint32_t g_dump_left  = 0;

static void hard_init_controller();

static void mscp_dump(uint32_t flag, const char* fmt, ...) {
  if (!g_dump_left || !(g_dump_flags & flag)) return;
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  LOG("MSCP %s", buf);
  if (g_dump_left != 0xFFFFFFFFu)  // saturate = unlimited
    g_dump_left--;
}

void set_dump(uint32_t flags, uint32_t count) {
  g_dump_flags = flags;
  g_dump_left = count;
  if (flags && count)
    LOG("MSCP dump armed flags=0x%02X count=%u", (unsigned)flags, (unsigned)count);
}

uint32_t dump_flags() { return g_dump_flags; }
uint32_t dump_remaining() { return g_dump_left; }

uint32_t parse_dump_flags(const char* s) {
  if (!s || !*s) return 0;
  while (*s == ' ' || *s == '\t') s++;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    return (uint32_t)strtoul(s, nullptr, 16);
  // Pure decimal?
  bool all_digit = true;
  for (const char* p = s; *p; p++) {
    if (*p < '0' || *p > '9') { all_digit = false; break; }
  }
  if (all_digit) return (uint32_t)strtoul(s, nullptr, 10);

  uint32_t f = 0;
  char tmp[96];
  strncpy(tmp, s, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = 0;
  for (char* p = tmp; *p; p++)
    if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');

  for (char* tok = strtok(tmp, ",|;+ \t\"'"); tok; tok = strtok(nullptr, ",|;+ \t\"'")) {
    if (!*tok) continue;
    if (!strcmp(tok, "all"))       f |= DUMP_ALL;
    else if (!strcmp(tok, "csr"))  f |= DUMP_CSR;
    else if (!strcmp(tok, "init")) f |= DUMP_INIT;
    else if (!strcmp(tok, "ring")) f |= DUMP_RING;
    else if (!strcmp(tok, "cmd"))  f |= DUMP_CMD;
    else if (!strcmp(tok, "xfer")) f |= DUMP_XFER;
    else if (!strcmp(tok, "irq"))  f |= DUMP_IRQ;
  }
  return f;
}

// ---- media ----

void begin() {
  for (int i = 0; i < UNIT_COUNT; i++) {
    if (g_drv[i].open) g_drv[i].file.close();
    g_drv[i] = Drive{};
    g_access_ms[i] = 0;
  }
  // Keep phys ops if CPU already registered them (remount / restart).
  hard_init_controller();
}

void set_phys_ops(PhysRead8Fn rd, PhysWrite8Fn wr) {
  g_rd = rd;
  g_wr = wr;
}

void reset() {
  // Soft: keep mounts; return controller to STEP1 for guest re-init.
  hard_init_controller();
}

bool mount(Unit u, const char* path) {
  if (u >= UNIT_COUNT || !path || !path[0]) return false;
  unmount(u);
  HostSdGuard guard;
  bool ro = false;
  File f = SD_MMC.open(path, "r+");
  if (!f) {
    f = SD_MMC.open(path, "r");
    ro = true;
  }
  if (!f) {
    LOGE("MSCP[%u] open failed: %s", (unsigned)u, path);
    return false;
  }
  g_drv[u].file = f;
  g_drv[u].path = path;
  g_drv[u].bytes = f.size();
  g_drv[u].open = true;
  g_drv[u].readonly = ro;
  g_drv[u].online = false;
  LOG("MSCP[%u] mounted %s (%llu bytes)%s", (unsigned)u, path,
      (unsigned long long)g_drv[u].bytes, ro ? " [RO]" : "");
  return true;
}

void unmount(Unit u) {
  if (u >= UNIT_COUNT) return;
  if (g_drv[u].open) {
    HostSdGuard guard;
    g_drv[u].file.close();
  }
  g_drv[u] = Drive{};
  g_access_ms[u] = 0;
}

bool mounted(Unit u) { return u < UNIT_COUNT && g_drv[u].open; }
bool readonly(Unit u) { return mounted(u) && g_drv[u].readonly; }
const char* path(Unit u) { return mounted(u) ? g_drv[u].path.c_str() : ""; }
uint64_t size_bytes(Unit u) { return mounted(u) ? g_drv[u].bytes : 0; }

bool read_blocks(Unit u, uint32_t lba, void* buf, size_t nblocks) {
  if (!mounted(u) || !buf || nblocks == 0) return false;
  note_access((uint8_t)u);
  const uint64_t nb = g_drv[u].bytes / BLOCK;
  if ((uint64_t)lba >= nb || (uint64_t)nblocks > nb - lba) return false;
  HostSdGuard guard;
  uint64_t off = (uint64_t)lba * BLOCK;
  // Host File::seek is 32-bit; refuse offsets that would truncate.
  if (off > 0xFFFFFFFFull) return false;
  if (!g_drv[u].file.seek((uint32_t)off)) return false;
  size_t want = nblocks * BLOCK;
  return g_drv[u].file.read((uint8_t*)buf, want) == (int)want;
}

bool write_blocks(Unit u, uint32_t lba, const void* buf, size_t nblocks) {
  if (!mounted(u) || !buf || nblocks == 0) return false;
  note_access((uint8_t)u);
  if (g_drv[u].readonly) return false;
  const uint64_t nb = g_drv[u].bytes / BLOCK;
  if ((uint64_t)lba >= nb || (uint64_t)nblocks > nb - lba) return false;
  HostSdGuard guard;
  uint64_t off = (uint64_t)lba * BLOCK;
  if (off > 0xFFFFFFFFull) return false;
  if (!g_drv[u].file.seek((uint32_t)off)) return false;
  size_t want = nblocks * BLOCK;
  size_t n = g_drv[u].file.write((const uint8_t*)buf, want);
  // Flush once per call site (not per 512B chunk inside controller xfer).
  g_drv[u].file.flush();
  return n == want;
}

// ---- phys helpers ----

static uint16_t phys_r16(uint32_t pa) {
  if (!g_rd) return 0;
  return (uint16_t)g_rd(pa) | ((uint16_t)g_rd(pa + 1) << 8);
}
static void phys_w16(uint32_t pa, uint16_t v) {
  if (!g_wr) return;
  g_wr(pa, (uint8_t)v);
  g_wr(pa + 1, (uint8_t)(v >> 8));
}
static uint32_t phys_r32(uint32_t pa) {
  return (uint32_t)phys_r16(pa) | ((uint32_t)phys_r16(pa + 2) << 16);
}
static void phys_w32(uint32_t pa, uint32_t v) {
  phys_w16(pa, (uint16_t)v);
  phys_w16(pa + 2, (uint16_t)(v >> 16));
}
static bool phys_read_block(uint32_t pa, uint8_t* buf, uint16_t n) {
  if (!g_rd || !buf) return false;
  for (uint16_t i = 0; i < n; i++) buf[i] = g_rd(pa + i);
  return true;
}
static bool phys_write_block(uint32_t pa, const uint8_t* buf, uint16_t n) {
  if (!g_wr || !buf) return false;
  for (uint16_t i = 0; i < n; i++) g_wr(pa + i, buf[i]);
  return true;
}

static uint16_t pkt_w(const uint8_t* p, uint16_t i) {
  uint16_t o = (uint16_t)(i * 2);
  if (o + 1 >= PACKET_BYTES) return 0;
  return (uint16_t)p[o] | ((uint16_t)p[o + 1] << 8);
}
static void set_pkt_w(uint8_t* p, uint16_t i, uint16_t v) {
  uint16_t o = (uint16_t)(i * 2);
  if (o + 1 >= PACKET_BYTES) return;
  p[o] = (uint8_t)v;
  p[o + 1] = (uint8_t)(v >> 8);
}

// ---- controller init ----

static void hard_init_controller() {
  g_state = State::Step1;
  g_sa = SA_STEP1 | S1_CTRL_DIAG | S1_CTRL_MAP;
  g_pending_sa = 0;
  g_sa_pending = false;
  g_step1 = 0;
  g_vec = 0154;
  g_cmd_ring_len = g_rsp_ring_len = 0;
  g_comm = 0;
#if VAX_MODEL == VAX_MODEL_KA630
  g_q22_high = 0;
#endif
  g_logged_s0_dma = false;
  g_dma_log_left =
#if VVAX_DIAG_LEVEL >= 2
      40;
#elif VVAX_DIAG_LEVEL >= 1
      8;
#else
      0;
#endif
  g_load_sectors = 0;
  g_load_log_next =
#if VVAX_DIAG_LEVEL >= 2
      128u;
#elif VVAX_DIAG_LEVEL >= 1
      512u;
#else
      0xFFFFFFFFu;
#endif
  g_rsp_own_logged = false;
  g_ie = false;
  g_purge = false;
  g_poll = false;
  g_cmd_valid = false;
  g_rsp_valid = false;
  g_irq_latched = false;
  g_in_doorbell = false;
  g_irq_defer = 0;
  g_host_online_wait = false;
  g_first_rsp = true;
  g_rsp_delay = 0;
  g_ctl_flags = 0;
  g_cmd_ring = Ring{};
  g_rsp_ring = Ring{};
  memset(g_cmd_pkt, 0, sizeof g_cmd_pkt);
  memset(g_rsp_pkt, 0, sizeof g_rsp_pkt);
  g_rsp_len = 0;
  g_xfer = Xfer{};
  for (int i = 0; i < UNIT_COUNT; i++) g_drv[i].online = false;
  mscp_dump(DUMP_INIT, "hard_init SA=0x%04X", g_sa);
}

static void latch_irq_now() {
  if (!(g_ie && g_vec)) return;
  bool was = g_irq_latched;
  g_irq_latched = true;
  if (!was) {
    mscp_dump(DUMP_IRQ, "latch vec=0%o", g_vec);
    if (g_host_online_wait)
      LOG("MSCP IRQ latch vec=0%o wait=1 ticks=%u PC=%08X",
          g_vec, (unsigned)vax_clock::ticks(),
          (unsigned)vax_cpu::state().r[vax_cpu::R_PC]);
  }
}

// NetBSD rx_putonline: IP-poll then tsleep. Completing ONLINE inside that
// IP-read delivers the IRQ on the next insn, so wakeup runs before tsleep.
static void latch_irq() {
  if (!(g_ie && g_vec)) return;
  if (g_in_doorbell) {
    if (g_irq_defer == 0) {
      g_irq_defer = 256;
      if (g_host_online_wait)
        LOG("MSCP IRQ defer 256 wait=1 ticks=%u PC=%08X",
            (unsigned)vax_clock::ticks(),
            (unsigned)vax_cpu::state().r[vax_cpu::R_PC]);
    }
    return;
  }
  latch_irq_now();
}

static void ring_soft_flag(const Ring& ring) {
  // Always mark soft completion flag; IRQ is separate (review recommendation).
  if (g_comm) phys_w16(g_comm + (uint32_t)(int32_t)ring.irq_off, 1);
  latch_irq();
}

static uint32_t q22_expand(uint32_t ba) {
#if VAX_MODEL == VAX_MODEL_KA630
  return (ba & 0x003fffffu) | g_q22_high;
#else
  return ba & 0x3FFFFFFFu;
#endif
}

// Ring descriptors are 22-bit Q22. MSCP seq_buffer is a 32-bit host address:
// /boot buffers are phys 0x7Axxxx, but loadfile writes the kernel at KERNBASE
// (0x80000000+). Applying the ring alias there lands DMA at 0x400000 and the
// CALLS to e_entry executes leftover RAM (pa-r at a wild PC, /boot SP still live).
static uint32_t mscp_dma_phys(uint32_t ba) {
  uint32_t pam = ba & 0x3FFFFFFFu;
  size_t ram = vax_cpu::ram_bytes();
  if (pam < ram) return pam;
  return q22_expand(ba);
}

static void apply_comm_phys(uint32_t comm) {
  g_comm = comm;
  g_rsp_ring.base = comm;
  g_cmd_ring.base = comm + g_rsp_ring.byte_len;
}

#if VAX_MODEL == VAX_MODEL_KA630
static bool cmd_own_at(uint32_t comm) {
  uint32_t cmd_base = comm + g_rsp_ring.byte_len;
  return (phys_r32(cmd_base + g_cmd_ring.index) & DESC_OWN) != 0;
}

// Prefer the 22-bit window that actually has a posted command descriptor.
static void probe_q22_comm_alias() {
  uint32_t lo = g_comm & 0x003fffffu;
  size_t ram = vax_cpu::ram_bytes();
  uint32_t need = g_rsp_ring.byte_len + g_cmd_ring.byte_len;
  for (uint32_t hi = 0; hi < (uint32_t)ram; hi += 0x400000u) {
    uint32_t comm = lo + hi;
    if (comm < 8u || (uint64_t)comm + need > ram) continue;
    if (!cmd_own_at(comm)) continue;
    if (comm != g_comm) {
      mscp_dump(DUMP_INIT, "Q22 alias comm 0x%08X -> 0x%08X",
                (unsigned)g_comm, (unsigned)comm);
      g_q22_high = hi;
      apply_comm_phys(comm);
    }
    return;
  }
}
#endif

static bool init_comm_area() {
  g_rsp_ring.irq_off = -2;
  g_rsp_ring.base = g_comm;
  g_rsp_ring.byte_len = (uint16_t)(g_rsp_ring_len * 4u);
  g_rsp_ring.index = 0;
  g_cmd_ring.irq_off = -4;
  g_cmd_ring.base = g_comm + g_rsp_ring.byte_len;
  g_cmd_ring.byte_len = (uint16_t)(g_cmd_ring_len * 4u);
  g_cmd_ring.index = 0;

  uint32_t clear_base = g_comm + (g_purge ? (uint32_t)-8 : (uint32_t)-4);
  uint16_t clear_len = (uint16_t)(g_cmd_ring.base + g_cmd_ring.byte_len - clear_base);
  for (uint16_t i = 0; i < clear_len; i++) {
    if (!g_wr) return false;
    g_wr(clear_base + i, 0);
  }
  return true;
}

static void begin_step4() {
  if (!init_comm_area()) {
    g_sa = SA_ERROR | 7;
    g_state = State::Dead;
    mscp_dump(DUMP_INIT, "STEP4 fail (comm clear)");
    return;
  }
  g_sa = SA_STEP4 | (PORT_MODEL << 4) | PORT_SWVER;
  g_state = State::Step4;
  LOG("MSCP STEP4 SA=0x%04X comm=0x%08X cmd_ring=%u rsp_ring=%u",
      g_sa, (unsigned)g_comm, (unsigned)g_cmd_ring_len,
      (unsigned)g_rsp_ring_len);
  latch_irq();
}

static void service_init() {
  if (!g_sa_pending) return;
  uint16_t value = g_pending_sa;
  g_sa_pending = false;
  mscp_dump(DUMP_INIT, "SA host=0x%04X state=%u", value, (unsigned)g_state);

  switch (g_state) {
    case State::Step1:
      if (!(value & S1_HOST_VALID)) return;
      if (value & S1_HOST_WRAP) {
        g_sa = value;
        g_state = State::Step1Wrap;
        mscp_dump(DUMP_INIT, "STEP1 wrap SA=0x%04X", g_sa);
        return;
      }
      g_step1 = value;
      g_cmd_ring_len = (uint16_t)(1u << ((value >> 11) & 7));
      g_rsp_ring_len = (uint16_t)(1u << ((value >> 8) & 7));
      g_ie = (value & S1_HOST_IE) != 0;
      if (value & S1_HOST_VEC)
        g_vec = (uint16_t)((value & S1_HOST_VEC) << 2);
      // STEP1GOOD: MP_STEP2 | IE | (NCMDL2<<3) | NRSPL2  — echo host byte 1.
      g_sa = SA_STEP2 | ((value >> 8) & 0xff);
      g_state = State::Step2;
      LOG("MSCP STEP1->2 rings cmd=%u rsp=%u ie=%d vec=0x%03X SA=0x%04X",
          (unsigned)g_cmd_ring_len, (unsigned)g_rsp_ring_len,
          (int)g_ie, g_vec, g_sa);
      latch_irq();
      break;
    case State::Step1Wrap:
      g_sa = value;
      break;
    case State::Step2:
      g_comm = value & S2_COMM_LOW;
      g_purge = (value & 1) != 0;
      // NetBSD STEP2GOOD(iv) = MP_STEP3 | MP_IE | (ivec>>2). Echo IE and
      // the STEP1 vector, not the ring-size byte — /boot only tests STEP3.
      g_sa = SA_STEP3 | (g_step1 & (S1_HOST_IE | S1_HOST_VEC));
      g_state = State::Step3;
      LOG("MSCP STEP2->3 comm_lo=0x%04X purge=%d SA=0x%04X",
          (unsigned)(g_comm & 0xffff), (int)g_purge, g_sa);
      latch_irq();
      break;
    case State::Step3:
      g_comm |= (uint32_t)(value & S3_COMM_HIGH) << 16;
      LOG("MSCP STEP3 comm=0x%08X host=0x%04X",
          (unsigned)g_comm, value);
      if (value & S3_PURGE_POLL) {
        g_sa = 0;
        g_state = State::Step3PurgeSa;
      } else {
        begin_step4();
      }
      break;
    case State::Step3PurgeSa:
      if (value != 0) {
        g_sa = SA_ERROR | 1;
        g_state = State::Dead;
        mscp_dump(DUMP_INIT, "purge SA fail");
      } else {
        g_state = State::Step3PurgeIp;
        mscp_dump(DUMP_INIT, "purge wait IP");
      }
      break;
    case State::Step4:
      if (value & S4_GO) {
        g_sa = 0;
        g_state = State::Run;
        g_poll = true;
        mscp_dump(DUMP_INIT, "GO -> Run");
#if VAX_MODEL == VAX_MODEL_KA750
        LOG("MSCP: dma=UBA/18-bit (Unibus maps; no Q22 window)");
#else
        LOG("MSCP: V0.6.1 dma=PAMASK/Q22 (expect DMA raw= lines on disk I/O)");
#endif
      }
      break;
    default:
      break;
  }
}

// ---- rings ----

static bool read_desc(const Ring& ring, uint32_t* out) {
  if (!out || ring.byte_len < 4) return false;
  *out = phys_r32(ring.base + ring.index);
  return true;
}

static bool release_desc(Ring* ring, uint32_t desc) {
  if (!ring || ring->byte_len < 4) return false;
  uint32_t addr = ring->base + ring->index;
  uint32_t released = (desc & ~DESC_OWN) | DESC_FLAG;
  phys_w32(addr, released);

  if (desc & DESC_FLAG) {
    bool transition = ring->byte_len == 4;
    if (!transition) {
      uint16_t prev = (uint16_t)((ring->index - 4) & (ring->byte_len - 1));
      uint32_t p = phys_r32(ring->base + prev);
      transition = (p & DESC_OWN) != 0;
    }
    if (transition) ring_soft_flag(*ring);
  }
  ring->index = (uint16_t)((ring->index + 4) & (ring->byte_len - 1));
  return true;
}

static bool fetch_command() {
  if (g_cmd_valid) return true;
  uint32_t desc = 0;
  if (!read_desc(g_cmd_ring, &desc)) {
    g_sa = SA_ERROR | 6;
    g_state = State::Dead;
    return false;
  }
  if (!(desc & DESC_OWN)) {
#if VAX_MODEL == VAX_MODEL_KA630
    probe_q22_comm_alias();
    if (!read_desc(g_cmd_ring, &desc) || !(desc & DESC_OWN)) {
      g_poll = false;
      return true;
    }
#else
    g_poll = false;
    return true;
#endif
  }
  uint32_t pkt = q22_expand(desc & DESC_ADDR);
  mscp_dump(DUMP_RING, "cmd own desc=0x%08X pkt=0x%06X idx=%u",
            (unsigned)desc, (unsigned)pkt, (unsigned)g_cmd_ring.index);
  if (pkt < 4 || !phys_read_block(pkt - 4, g_cmd_pkt, PACKET_BYTES)) {
    g_sa = SA_ERROR | 1;
    g_state = State::Dead;
    return false;
  }
  g_cmd_valid = true;
  if (g_host_online_wait) {
    LOG("MSCP online-wait cleared by op=%u unit=%u ticks=%u PC=%08X",
        (unsigned)pkt_w(g_cmd_pkt, 6), (unsigned)pkt_w(g_cmd_pkt, 4),
        (unsigned)vax_clock::ticks(),
        (unsigned)vax_cpu::state().r[vax_cpu::R_PC]);
  }
  g_host_online_wait = false;
  if (!release_desc(&g_cmd_ring, desc)) {
    g_sa = SA_ERROR | 7;
    g_state = State::Dead;
    return false;
  }
  return true;
}

static bool submit_response(const uint8_t* src, uint16_t len) {
  if (g_rsp_valid || !src || len > PACKET_BYTES) return false;
  memcpy(g_rsp_pkt, src, len);
  if (len < PACKET_BYTES) memset(g_rsp_pkt + len, 0, PACKET_BYTES - len);
  g_rsp_len = len;
  g_rsp_valid = true;
  // NetBSD /boot rings IP once then spins on ca_rspdsc; post in the same
  // doorbell service (delay>0 left responses stuck until a later host poll).
  g_rsp_delay = 0;
  return true;
}

static bool build_response(uint8_t opcode, uint16_t status, uint16_t payload) {
  if (payload + 4u > PACKET_BYTES) return false;
  uint8_t rsp[PACKET_BYTES]{};
  set_pkt_w(rsp, 0, payload);
  set_pkt_w(rsp, 1, g_first_rsp ? 15 : 1);
  set_pkt_w(rsp, 2, pkt_w(g_cmd_pkt, 2));
  set_pkt_w(rsp, 3, pkt_w(g_cmd_pkt, 3));
  set_pkt_w(rsp, 4, pkt_w(g_cmd_pkt, 4));
  set_pkt_w(rsp, 6, (uint16_t)opcode | 0x0080);
  set_pkt_w(rsp, 7, status);
  g_first_rsp = false;
  return submit_response(rsp, (uint16_t)(payload + 4u));
}

static bool post_response() {
  if (!g_rsp_valid) return true;
  if (g_rsp_delay) {
    g_rsp_delay--;
    return true;
  }

  // Always complete into the ring (review: don't gate DMA on IPL).
  uint32_t desc = 0;
  if (!read_desc(g_rsp_ring, &desc)) {
    g_sa = SA_ERROR | 6;
    g_state = State::Dead;
    return false;
  }
  if (!(desc & DESC_OWN)) {
    if (!g_rsp_own_logged) {
      g_rsp_own_logged = true;
      LOG("MSCP rsp wait OWN idx=%u desc=0x%08X",
          (unsigned)g_rsp_ring.index, (unsigned)desc);
    }
    return true;
  }
  g_rsp_own_logged = false;

  uint32_t pkt = q22_expand(desc & DESC_ADDR);
  mscp_dump(DUMP_RING, "rsp post desc=0x%08X pkt=0x%06X len=%u",
            (unsigned)desc, (unsigned)pkt, (unsigned)g_rsp_len);
  if (pkt < 4 || !phys_write_block(pkt - 4, g_rsp_pkt, g_rsp_len)) {
    g_sa = SA_ERROR | 2;
    g_state = State::Dead;
    return false;
  }
  if (!release_desc(&g_rsp_ring, desc)) {
    g_sa = SA_ERROR | 7;
    g_state = State::Dead;
    return false;
  }
  uint16_t op = pkt_w(g_rsp_pkt, 6);
  if ((op & 0x7Fu) == 9u || g_host_online_wait) {
    uint16_t rspint = (g_comm >= 2u) ? phys_r16(g_comm - 2u) : 0;
    LOG("MSCP rsp posted op=0x%02X st=0x%04X rspint=%u irq=%u wait=%u ticks=%u",
        (unsigned)op, (unsigned)pkt_w(g_rsp_pkt, 7), (unsigned)rspint,
        g_irq_latched ? 1u : 0u, g_host_online_wait ? 1u : 0u,
        (unsigned)vax_clock::ticks());
  }
  g_rsp_valid = false;
  g_rsp_len = 0;
  return true;
}

// ---- MSCP commands (dual unit) ----

static bool unit_ok(uint16_t unit, uint16_t* st) {
  if (unit >= UNIT_COUNT) {
    if (st) *st = ST_OFL_UNK;
    return false;
  }
  if (!mounted((Unit)unit)) {
    if (st) *st = ST_OFL_NV;
    return false;
  }
  return true;
}

static void fill_geometry(uint8_t* rsp, uint16_t unit) {
  uint32_t blocks = (uint32_t)(size_bytes((Unit)unit) / BLOCK);
  set_pkt_w(rsp, 9, 0x8000);
  set_pkt_w(rsp, 12, unit);
  set_pkt_w(rsp, 15, (uint16_t)((2u << 8) | 5));
  set_pkt_w(rsp, 16, 0x1051);
  set_pkt_w(rsp, 17, 0x2564);
  set_pkt_w(rsp, 20, (uint16_t)blocks);
  set_pkt_w(rsp, 21, (uint16_t)(blocks >> 16));
}

static bool start_transfer(bool write) {
  uint16_t unit = pkt_w(g_cmd_pkt, 4);
  uint16_t st = 0;
  if (!unit_ok(unit, &st)) return build_response(write ? 34 : 33, st, 32);
  if (!g_drv[unit].online) return build_response(write ? 34 : 33, ST_AVL, 32);
  if (write && g_drv[unit].readonly)
    return build_response(34, ST_WPR, 32);

  uint32_t count = (uint32_t)pkt_w(g_cmd_pkt, 8) |
                   ((uint32_t)pkt_w(g_cmd_pkt, 9) << 16);
  uint32_t raw_addr = (uint32_t)pkt_w(g_cmd_pkt, 10) |
                      ((uint32_t)pkt_w(g_cmd_pkt, 11) << 16);
  uint32_t address = mscp_dma_phys(raw_addr);
  uint32_t lbn = (uint32_t)pkt_w(g_cmd_pkt, 16) |
                 ((uint32_t)pkt_w(g_cmd_pkt, 17) << 16);

  if (address & 1) return build_response(write ? 34 : 33, ST_HST_OA, 32);
  if (count & 1) return build_response(write ? 34 : 33, ST_HST_OC, 32);
  if (count > 0x0fffffffu) return build_response(write ? 34 : 33, ST_CMD_BCNT, 32);
  if ((uint64_t)lbn * BLOCK + count > size_bytes((Unit)unit))
    return build_response(write ? 34 : 33, ST_CMD_LBN, 32);

  g_xfer = Xfer{};
  g_xfer.active = true;
  g_xfer.write = write;
  g_xfer.unit = (uint8_t)unit;
  g_xfer.lbn = lbn;
  g_xfer.count = count;
  g_xfer.requested = count;
  g_xfer.address = address;
  g_xfer.offset = lbn * (uint32_t)BLOCK;
  g_xfer.status = 0;
  note_access((uint8_t)unit);
  // Always log early DMA so we can see whether loadfile uses KERNBASE
  // (0x80000000) or a bounce/Q22-mapped low address — Arduino "already in
  // flash" skips made the one-shot S0 line alone unreliable.
  if (g_dma_log_left) {
    g_dma_log_left--;
    LOG("MSCP DMA raw=0x%08X pa=0x%08X count=%u lbn=%u",
        (unsigned)raw_addr, (unsigned)address, (unsigned)count,
        (unsigned)lbn);
  }
  if (!g_logged_s0_dma && (raw_addr & 0x80000000u)) {
    g_logged_s0_dma = true;
    LOG("MSCP DMA S0 0x%08X -> pa 0x%08X", (unsigned)raw_addr,
        (unsigned)address);
  }
  mscp_dump(DUMP_XFER, "%s start unit=%u lbn=%u count=%u raw=0x%08X pa=0x%08X",
            write ? "WR" : "RD", (unsigned)unit, (unsigned)lbn,
            (unsigned)count, (unsigned)raw_addr, (unsigned)address);
  return true;  // response when xfer completes
}

static void continue_transfer() {
  if (!g_xfer.active) return;
  note_access(g_xfer.unit);
  // Up to 8 KiB per service_transport call — still bounded, much faster than
  // one 512 B chunk per outer poll when /boot loadfile reads large counts.
  static constexpr uint32_t kXferBurst = 8192u;
  uint8_t buf[BLOCK];
  uint32_t burst = 0;
  while (g_xfer.active && !g_xfer.status && g_xfer.count && burst < kXferBurst) {
    uint32_t chunk = g_xfer.count > BLOCK ? BLOCK : g_xfer.count;
    mscp_dump(DUMP_XFER, "%s chunk=%u rem=%u lba=%u",
              g_xfer.write ? "WR" : "RD", (unsigned)chunk,
              (unsigned)g_xfer.count, (unsigned)(g_xfer.offset / BLOCK));
    if (g_xfer.write) {
      if (!phys_read_block(g_xfer.address, buf, (uint16_t)chunk))
        g_xfer.status = ST_HST_NXM;
      else if (!write_blocks((Unit)g_xfer.unit, g_xfer.offset / BLOCK, buf,
                             (chunk + BLOCK - 1) / BLOCK) &&
               chunk == BLOCK)
        g_xfer.status = ST_DRV;
      else if (chunk < BLOCK) {
        HostSdGuard guard;
        if (g_drv[g_xfer.unit].readonly) g_xfer.status = ST_WPR;
        else if (!g_drv[g_xfer.unit].file.seek(g_xfer.offset) ||
                 g_drv[g_xfer.unit].file.write(buf, chunk) != chunk)
          g_xfer.status = ST_DRV;
        else
          g_drv[g_xfer.unit].file.flush();
      }
    } else {
      if (chunk == BLOCK) {
        if (!read_blocks((Unit)g_xfer.unit, g_xfer.offset / BLOCK, buf, 1))
          g_xfer.status = ST_DRV;
      } else {
        HostSdGuard guard;
        if (!g_drv[g_xfer.unit].file.seek(g_xfer.offset) ||
            g_drv[g_xfer.unit].file.read(buf, chunk) != (int)chunk)
          g_xfer.status = ST_DRV;
      }
      if (!g_xfer.status && !phys_write_block(g_xfer.address, buf, (uint16_t)chunk))
        g_xfer.status = ST_HST_NXM;
    }

    if (!g_xfer.status && chunk) {
      g_xfer.address += chunk;
      g_xfer.offset += chunk;
      g_xfer.count -= chunk;
      burst += chunk;
      if (!g_xfer.write && chunk == BLOCK) {
        g_load_sectors++;
        if (g_load_sectors >= g_load_log_next) {
          LOG("MSCP load: %u sectors", (unsigned)g_load_sectors);
          g_load_log_next += 128;
        }
      }
    }
  }

  if (g_xfer.status || g_xfer.count == 0) {
    uint32_t processed = g_xfer.requested - g_xfer.count;
    mscp_dump(DUMP_XFER, "%s done processed=%u st=0x%04X",
              g_xfer.write ? "WR" : "RD", (unsigned)processed, g_xfer.status);
    if (g_dma_log_left) {
      g_dma_log_left--;
      LOG("MSCP xfer done %s n=%u st=0x%04X",
          g_xfer.write ? "WR" : "RD", (unsigned)processed, g_xfer.status);
    }
    bool ok = build_response(g_xfer.write ? 34 : 33, g_xfer.status, 32);
    if (ok) {
      set_pkt_w(g_rsp_pkt, 8, (uint16_t)processed);
      set_pkt_w(g_rsp_pkt, 9, (uint16_t)(processed >> 16));
    }
    g_xfer = Xfer{};
    g_cmd_valid = false;
    g_poll = true;
  }
}

static bool process_command() {
  if (!g_cmd_valid || g_rsp_valid || g_xfer.active) return false;
  uint8_t op = (uint8_t)pkt_w(g_cmd_pkt, 6);
  uint16_t unit = pkt_w(g_cmd_pkt, 4);
  bool accepted = false;
  mscp_dump(DUMP_CMD, "op=%u unit=%u", (unsigned)op, (unsigned)unit);

  switch (op) {
    case 4:  // Set Controller Characteristics
      if (pkt_w(g_cmd_pkt, 8) != 0) {
        accepted = build_response(op, ST_BAD_VER, 12);
        break;
      }
      g_ctl_flags = pkt_w(g_cmd_pkt, 9) & 0x80f0;
      accepted = build_response(4, 0, 32);
      if (accepted) {
        set_pkt_w(g_rsp_pkt, 9, g_ctl_flags);
        set_pkt_w(g_rsp_pkt, 10, 120);
        set_pkt_w(g_rsp_pkt, 11, PORT_SWVER);
        set_pkt_w(g_rsp_pkt, 15, (uint16_t)((2u << 8) | PORT_MODEL));
      }
      break;

    case 3: {  // Get Unit Status
      uint16_t want = unit;
      uint16_t st = 0;
      // M_GUM_NEXTUNIT: next present unit >= requested. None → wrap with
      // unit 0 + OFFLINE/UNKNOWN so NetBSD mscp_attach sees unit < want
      // and stops (otherwise it walks 0..4095 and DELAY-timeouts).
      if (pkt_w(g_cmd_pkt, 7) & 0x0001) {
        uint16_t next = 0xFFFF;
        for (uint16_t u = want; u < UNIT_COUNT; u++) {
          if (mounted((Unit)u)) {
            next = u;
            break;
          }
        }
        if (next == 0xFFFF) {
          accepted = build_response(3, ST_OFL_UNK, 48);
          if (accepted) set_pkt_w(g_rsp_pkt, 4, 0);
          break;
        }
        unit = next;
      }
      if (!unit_ok(unit, &st)) {
        accepted = build_response(3, st, 48);
        break;
      }
      st = g_drv[unit].online ? 0 : ST_AVL;
      accepted = build_response(3, st, 48);
      if (accepted) {
        if (unit != want) set_pkt_w(g_rsp_pkt, 4, unit);
        fill_geometry(g_rsp_pkt, unit);
        set_pkt_w(g_rsp_pkt, 20, 51);
        set_pkt_w(g_rsp_pkt, 21, 14);
        set_pkt_w(g_rsp_pkt, 22, 1);
        set_pkt_w(g_rsp_pkt, 24, 2856);
        set_pkt_w(g_rsp_pkt, 25, 0401);
      }
      break;
    }

    case 9: {  // Online
      uint16_t st = 0;
      if (!unit_ok(unit, &st)) {
        accepted = build_response(9, st, 44);
        break;
      }
      g_drv[unit].online = true;
      accepted = build_response(9, 0, 44);
      if (accepted) {
        fill_geometry(g_rsp_pkt, unit);
        g_host_online_wait = true;
        uint32_t blocks = (uint32_t)(size_bytes((Unit)unit) / BLOCK);
        uint32_t psl = vax_cpu::state().psl;
        LOG("MSCP ONLINE unit=%u st=0 size=%u ticks=%u PC=%08X PSL=%08X IPL=%u",
            (unsigned)unit, (unsigned)blocks,
            (unsigned)vax_clock::ticks(),
            (unsigned)vax_cpu::state().r[vax_cpu::R_PC],
            (unsigned)psl, (unsigned)((psl >> 16) & 0x1Fu));
      }
      break;
    }

    case 10: {  // Set Unit Characteristics
      uint16_t st = 0;
      if (!unit_ok(unit, &st)) {
        accepted = build_response(10, st, 44);
        break;
      }
      accepted = build_response(10, 0, 44);
      break;
    }

    case 8: {  // Available — only the addressed unit
      uint16_t st = 0;
      if (!unit_ok(unit, &st)) {
        accepted = build_response(8, st, 12);
        break;
      }
      g_drv[unit].online = false;
      accepted = build_response(8, 0, 12);
      break;
    }

    case 33:
      accepted = start_transfer(false);
      if (accepted && g_xfer.active) return true;  // keep command until xfer done
      break;
    case 34:
      accepted = start_transfer(true);
      if (accepted && g_xfer.active) return true;
      break;

    case 11:
    case 17:
      accepted = build_response(op, 0, 12);
      break;
    case 19: {  // Flush — real barrier for addressed unit
      uint16_t st = 0;
      if (!unit_ok(unit, &st)) {
        accepted = build_response(19, st, 12);
        break;
      }
      {
        HostSdGuard guard;
        g_drv[unit].file.flush();
      }
      accepted = build_response(19, 0, 12);
      break;
    }

    default:
      accepted = build_response(op, ST_BAD_OP, 12);  // echo opcode
      break;
  }

  if (accepted) {
    g_cmd_valid = false;
    g_poll = true;
  }
  return accepted;
}

static void service_transport() {
  // At most one 8 KiB burst per call. Draining the whole MSCP byte count
  // here ran inside the guest's IP-read instruction, so loop()/heartbeats
  // never ran and a multi-block loadfile looked dead after the first `|`.
  if (g_xfer.active) continue_transfer();
  if (g_rsp_valid) post_response();
  if (g_poll && !g_cmd_valid) fetch_command();
  if (g_cmd_valid && !g_rsp_valid && !g_xfer.active) process_command();
  if (g_xfer.active) continue_transfer();
  if (g_rsp_valid) post_response();
}

// ---- CSR / poll / IRQ ----

// Must NOT match those offsets in guest RAM — /boot BSS and ca_rspdsc sit
// around 0x007Bxxxx; 0x007B1C68 was decoding as IP (V0.6.19).
static uint32_t csr_io_off(uint32_t pa) { return pa & 0x1FFEu; }

static bool csr_in_io_page(uint32_t pa) {
#if VAX_MODEL == VAX_MODEL_KA750
  return (pa & 0xFFFFE000u) == 0x00FFE000u;  // Unibus I/O page
#else
  return (pa & 0x3FFFE000u) == 0x20000000u;  // Q22 8K I/O page
#endif
}

static bool csr_is_ip(uint32_t pa) {
  if (!csr_in_io_page(pa)) return false;
  uint32_t o = csr_io_off(pa);
#if VAX_MODEL == VAX_MODEL_KA750
  return o == 0x1468u;  // 0172150 only — no Q22 0x1C68 alias
#else
  return o == 0x1C68u || o == 0x1468u;
#endif
}

static bool csr_is_sa(uint32_t pa) {
  if (!csr_in_io_page(pa)) return false;
  uint32_t o = csr_io_off(pa);
#if VAX_MODEL == VAX_MODEL_KA750
  return o == 0x146Au;
#else
  return o == 0x1C6Au || o == 0x146Au;
#endif
}

bool csr_hit(uint32_t pa) { return csr_is_ip(pa) || csr_is_sa(pa); }

uint16_t csr_read(uint32_t pa) {
  uint32_t base = pa & ~1u;
  if (csr_is_ip(base)) {
    mscp_dump(DUMP_CSR, "IP read (poll)");
    // IP read is the UQSSP doorbell. Always wake transport: after GO, host
    // poll() can clear g_poll before the guest ORs OWN onto the descriptors.
    if (g_sa_pending) service_init();
    if (g_state == State::Step3PurgeIp) begin_step4();
    if (g_state == State::Run || g_xfer.active || g_rsp_valid || g_cmd_valid) {
      g_poll = true;
      g_in_doorbell = true;
      service_transport();
      g_in_doorbell = false;
    }
    return 0;
  }
  if (csr_is_sa(base)) {
    if (g_sa_pending) service_init();
    mscp_dump(DUMP_CSR, "SA read -> 0x%04X", g_sa);
    return g_sa;
  }
  return 0;
}

void csr_write(uint32_t pa, uint16_t v) {
  uint32_t base = pa & ~1u;
  if (csr_is_ip(base)) {
    if (g_state == State::Run) {
      LOG("MSCP IP write 0x%04X RESET while Run PC=%08X",
          v, (unsigned)vax_cpu::state().r[vax_cpu::R_PC]);
    } else {
      mscp_dump(DUMP_CSR, "IP write 0x%04X (hard init)", v);
    }
    hard_init_controller();
    return;
  }
  if (csr_is_sa(base)) {
    mscp_dump(DUMP_CSR, "SA write 0x%04X", v);
    g_pending_sa = v;
    g_sa_pending = true;
    // Apply init step immediately — NetBSD spins on SA between host poll() ticks.
    service_init();
  }
}

void poll() {
#if VVAX_DIAG_LEVEL >= 1
  static uint32_t live_ms = 0;
  uint32_t now = millis();
  uint32_t period = g_host_online_wait ? 5000u :
#if VVAX_DIAG_LEVEL >= 2
                    10000u;
#else
                    30000u;
#endif
  if (now - live_ms >= period) {
    live_ms = now;
    LOG("MSCP live state=%u xfer=%u rem=%u wait=%u irq=%u ticks=%u",
        (unsigned)g_state, g_xfer.active ? 1u : 0u,
        (unsigned)g_xfer.count,
        g_host_online_wait ? 1u : 0u,
        g_irq_latched ? 1u : 0u,
        (unsigned)vax_clock::ticks());
  }
#endif
  if (g_sa_pending) service_init();
  // Keep looking at rings in Run: NetBSD may post OWN after an early IP poll
  // has already cleared g_poll, and /boot above 4 MiB needs a Q22 alias probe.
  if (g_state == State::Run)
    g_poll = true;
  if (g_state == State::Run || g_xfer.active || g_rsp_valid || g_cmd_valid)
    service_transport();
}

bool irq_pending() { return g_irq_latched; }
bool busy() {
  return g_irq_latched || g_irq_defer != 0 || g_cmd_valid || g_rsp_valid ||
         g_xfer.active;
}
bool unit_busy(Unit u) {
  if (u >= UNIT_COUNT || !g_drv[u].open) return false;
  if (g_xfer.active && g_xfer.unit == (uint8_t)u) return true;
  uint32_t t = g_access_ms[u];
  return t != 0 && (millis() - t) < kAccessHoldMs;
}
bool host_online_wait() { return g_host_online_wait; }
uint16_t irq_vector() { return g_vec; }

void irq_clear() {
  if (g_irq_latched) mscp_dump(DUMP_IRQ, "clear");
  g_irq_latched = false;
}

void instr_tick() {
  if (g_irq_defer == 0) return;
  if (--g_irq_defer) return;
  latch_irq_now();
}

bool selftest() {
  if (!g_rd || !g_wr) {
    LOGE("MSCP selftest: no phys ops");
    return false;
  }
  // Use a scratch region high in guest RAM for rings/packets.
  // Caller must have allocated RAM first.
  hard_init_controller();
  // Simulate host init: rings of length 1 (<<0), IE off, vector 154
  // Step1 host word: VALID | (0<<11 cmd) | (0<<8 rsp) = 0x8000
  csr_write(CSR_IP_PA, 0);
  if ((csr_read(CSR_SA_PA) & SA_STEP1) == 0) {
    LOGE("MSCP selftest: no STEP1");
    return false;
  }
  csr_write(CSR_SA_PA, S1_HOST_VALID);
  poll();
  if ((csr_read(CSR_SA_PA) & SA_STEP2) == 0) {
    LOGE("MSCP selftest: no STEP2");
    return false;
  }
  // Put communication area at PA 0x4000
  uint32_t comm = 0x4000;
  csr_write(CSR_SA_PA, (uint16_t)(comm & S2_COMM_LOW));
  poll();
  if ((csr_read(CSR_SA_PA) & SA_STEP3) == 0) {
    LOGE("MSCP selftest: no STEP3");
    return false;
  }
  csr_write(CSR_SA_PA, (uint16_t)((comm >> 16) & S3_COMM_HIGH));
  poll();
  if ((csr_read(CSR_SA_PA) & SA_STEP4) == 0) {
    LOGE("MSCP selftest: no STEP4");
    return false;
  }
  csr_write(CSR_SA_PA, S4_GO);
  poll();
  if (g_state != State::Run) {
    LOGE("MSCP selftest: not RUN");
    return false;
  }
  LOG("MSCP selftest: PASS (UQSSP init, units A=%s B=%s)",
      mounted(UNIT_A) ? "mounted" : "empty",
      mounted(UNIT_B) ? "mounted" : "empty");
  hard_init_controller();  // leave idle for guest
  return true;
}

}  // namespace vax_mscp
