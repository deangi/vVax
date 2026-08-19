#include "vax_boot.h"
#include "vax_cpu.h"
#include "vax_mscp.h"
#include "vax_console.h"
#include "platform.h"

#include <string.h>

namespace vax_boot {

static uint8_t g_unit = 0;
static unsigned g_rom_reads = 0;
static unsigned g_fail_streak = 0;
static uint32_t g_last_fail_lbn = 0xFFFFFFFFu;
static uint32_t g_conspage_pa = 0;

uint8_t boot_unit() { return g_unit; }

uint32_t ka630_conspage_pa() { return g_conspage_pa; }

void plant_ka630_console() {
  uint8_t* ram = vax_cpu::ram();
  size_t ram_bytes = vax_cpu::ram_bytes();
  // Last 512-byte VAX page. Kernel loadfile occupies PA 0..~4MB; /boot sits
  // ~192 KB below ram_end and is only ~80 KB, so the last page is free.
  uint32_t cons = 0;
  if (ram_bytes >= 1024u)
    cons = ((uint32_t)ram_bytes - 512u) & ~511u;
  if (!ram || cons < 0x400000u || (uint64_t)cons + 0x80u > ram_bytes) {
    LOGE("boot: no room for KA630 conspage (ram=%u)", (unsigned)ram_bytes);
    return;
  }
  g_conspage_pa = cons;

  uint8_t* page = ram + cons;
  memset(page, 0, 0x80);

  // Console-page vectors (see sys/arch/vax/include/ka630.h + boot/consio2.S).
  auto put32 = [](uint8_t* p, uint32_t off, uint32_t v) {
    p[off]     = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
    p[off + 2] = (uint8_t)(v >> 16);
    p[off + 3] = (uint8_t)(v >> 24);
  };
  put32(page, 0x1C, KA630_GETC_PA);       // KA630_GETC
  put32(page, 0x20, KA630_PUTC_POLL_PA);  // KA630_PUTC_POLL
  put32(page, 0x24, KA630_PUTC_PA);       // KA630_PUTC

  // Cursor bounds so ka630_consinit can copy MAXROW→ROW / MINCOL→COL.
  page[0x4D] = 0;   // MINROW
  page[0x4E] = 24;  // MAXROW
  page[0x51] = 0;   // MINCOL
  page[0x52] = 79;  // MAXCOL
  page[0x4C] = 24;  // ROW
  page[0x50] = 0;   // COL

  LOG("boot: KA630 conspage @0x%08X (last page, NVR→host console stubs)",
      (unsigned)cons);
}

static uint32_t g_niclose_pa = 0;

void plant_boot_stubs(uint32_t boot_base) {
  uint8_t* ram = vax_cpu::ram();
  size_t ram_bytes = vax_cpu::ram_bytes();
  // machdep_start `calls $0, niclose` target: boot+0x51FE (not 0xC9CA —
  // that is a different /boot helper; patching it stalled loadfile).
  static constexpr uint32_t NICLOSE_OFF = 0x51FEu;
  uint32_t niclose = boot_base + NICLOSE_OFF;
  g_niclose_pa = niclose;
  if (!ram || niclose + 4 > ram_bytes) {
    LOGE("boot: no room for niclose stub @%08X", (unsigned)niclose);
    return;
  }
  // CALLS skips the entry-mask word; body must RET immediately.
  ram[niclose] = 0;
  ram[niclose + 1] = 0;
  ram[niclose + 2] = 0x04;  // RET
  ram[niclose + 3] = 0;
  LOG("boot: niclose stub @%08X (machdep_start, no DEUNA)", (unsigned)niclose);
}

bool is_niclose(uint32_t pa) {
  if (g_niclose_pa && pa == g_niclose_pa) return true;
  // Fallback if plant_boot_stubs did not run (still match relocated /boot).
  static constexpr uint32_t kOff = 0x51FEu;
  return pa == (0x7A0000u + kOff) || pa == (0x7D0000u + kOff) ||
         pa == (0x5D0000u + kOff);
}

bool nvr_hit(uint32_t pa) {
  return pa >= KA630_NVR_PA && pa < KA630_NVR_PA + 8u;
}

uint8_t nvr_read8(uint32_t pa) {
  // ka630_consinit reads four shorts and takes the low byte of each to form
  // the console-page physical address.
  uint32_t off = pa - KA630_NVR_PA;
  uint32_t byte_i = off / 2u;  // which address byte (0..3)
  if (off & 1u) return 0;      // high byte of each short is ignored
  return (uint8_t)(g_conspage_pa >> (byte_i * 8u));
}

static void jsb_return() {
  auto& st = vax_cpu::state();
  uint8_t* ram = vax_cpu::ram();
  size_t ram_bytes = vax_cpu::ram_bytes();
  uint32_t sp = st.r[vax_cpu::R_SP];
  uint32_t ret = 0;
  if (ram && (size_t)sp + 4 <= ram_bytes) {
    ret = (uint32_t)ram[sp] |
          ((uint32_t)ram[sp + 1] << 8) |
          ((uint32_t)ram[sp + 2] << 16) |
          ((uint32_t)ram[sp + 3] << 24);
  }
  st.r[vax_cpu::R_SP] = sp + 4;
  st.r[vax_cpu::R_PC] = ret;
}

bool ka630_console_jsb(uint32_t target) {
  auto& st = vax_cpu::state();
  if (target == KA630_PUTC_POLL_PA) {
    st.r[0] = 1;  // ready
    jsb_return();
    return true;
  }
  if (target == KA630_PUTC_PA) {
    vax_console::put_guest((uint8_t)(st.r[1] & 0xFF));
    jsb_return();
    return true;
  }
  if (target == KA630_GETC_PA) {
    vax_console::poll();
    uint8_t c = 0;
    if (vax_console::get_guest(&c)) {
      st.r[0] = 1;
      st.r[1] = c;
    } else if (vax_console::rxcs_rd() & vax_console::CSR_DONE) {
      st.r[0] = 1;
      st.r[1] = (uint8_t)vax_console::rxdb_rd();
    } else {
      st.r[0] = 0;
    }
    jsb_return();
    return true;
  }
  return false;
}

void rom_disk_read() {
  // 11/750 ROM ABI used by NetBSD cont_750 / read750:
  //   R8 = logical block number
  //   4(SP) = destination physical address  (JSB has already pushed PC at (SP))
  //   R0<0> = 1 on success
  auto& st = vax_cpu::state();
  uint8_t* ram = vax_cpu::ram();
  size_t ram_bytes = vax_cpu::ram_bytes();
  uint32_t lbn = st.r[8];
  uint32_t sp = st.r[vax_cpu::R_SP];
  uint32_t ret = 0;
  uint32_t dest = 0;
  if (ram && (size_t)sp + 8 <= ram_bytes) {
    ret = (uint32_t)ram[sp] |
          ((uint32_t)ram[sp + 1] << 8) |
          ((uint32_t)ram[sp + 2] << 16) |
          ((uint32_t)ram[sp + 3] << 24);
    uint32_t off = sp + 4;
    dest = (uint32_t)ram[off] |
           ((uint32_t)ram[off + 1] << 8) |
           ((uint32_t)ram[off + 2] << 16) |
           ((uint32_t)ram[off + 3] << 24);
  }
  bool ok = false;
  uint64_t disk_blocks = vax_mscp::size_bytes((vax_mscp::Unit)g_unit) / 512u;
  if (dest + 512u <= ram_bytes && ram && (uint64_t)lbn < disk_blocks)
    ok = vax_mscp::read_blocks((vax_mscp::Unit)g_unit, lbn, ram + dest, 1);
  st.r[0] = ok ? 1u : 0u;

  bool suspicious = (lbn & 0xF0000000u) != 0 || (uint64_t)lbn >= disk_blocks;
  if (ok) {
    g_fail_streak = 0;
  } else {
    if (lbn == g_last_fail_lbn)
      g_fail_streak++;
    else {
      g_last_fail_lbn = lbn;
      g_fail_streak = 1;
    }
  }

  // Always log first failures / I/O-space LBNs; then every 32nd; skip flood.
  bool log_line = (!ok && g_fail_streak <= 3) || suspicious || g_rom_reads < 16 ||
                  (g_rom_reads & 31u) == 0;
  if (log_line) {
    LOG("boot ROM-read #%u LBN=%u (0x%X) dest=0x%08X ret=0x%08X %s",
        g_rom_reads, (unsigned)lbn, (unsigned)lbn, (unsigned)dest,
        (unsigned)ret, ok ? "OK" : "FAIL");
  }

  // NetBSD romstrategy ignores R0 and will spin forever on a bad disk.
  if (!ok && g_fail_streak == 8) {
    LOGE("boot: %u consecutive ROM-read fails (LBN=0x%X). "
         "Unit A has xxboot but no usable FFS (/boot missing)? Halting.",
         g_fail_streak, (unsigned)lbn);
    st.halt = true;
  }

  g_rom_reads++;
}

bool start_mscp(uint8_t unit) {
  if (unit >= vax_mscp::UNIT_COUNT) {
    LOGE("boot: bad unit %u", (unsigned)unit);
    return false;
  }
  if (!vax_mscp::mounted((vax_mscp::Unit)unit)) {
    LOGE("boot: MSCP unit %c not mounted", 'A' + unit);
    return false;
  }
  if (vax_cpu::ram_bytes() < 0x200000u) {
    LOGE("boot: need >= 2 MB RAM for xxboot reloc @ 0x100000");
    return false;
  }

  g_unit = unit;
  g_rom_reads = 0;
  g_fail_streak = 0;
  g_last_fail_lbn = 0xFFFFFFFFu;

  auto& st = vax_cpu::state();
  memset(st.r, 0, sizeof(st.r));
  st.psl = 0;
  st.scbb = 0;
  st.pcbb = 0;
  st.halt = false;
  st.fault = 0;
  st.irq_count = 0;

  uint8_t* ram = vax_cpu::ram();
  memset(ram, 0, BOOT_BYTES);

  if (!vax_mscp::read_blocks((vax_mscp::Unit)unit, 0, ram, BOOT_BLOCKS)) {
    LOGE("boot: failed reading xxboot (LBA 0-%u)", (unsigned)(BOOT_BLOCKS - 1));
    return false;
  }

  if (ram[0x0C] == 0 && ram[0x200] == 0) {
    LOGE("boot: disk does not look like NetBSD xxboot");
    return false;
  }

  plant_ka630_console();

  st.r[0] = BDEV_UDA;
  st.r[1] = 0x20000000u;
  st.r[2] = vax_mscp::CSR_IP_PA;
  st.r[3] = unit;
  st.r[5] = 0;
  st.r[6] = ROM_READ_PA;
  st.r[vax_cpu::R_SP] = 0x100000u;
  st.r[vax_cpu::R_PC] = 0x0Cu;

  LOG("boot: MSCP %c xxboot FROM750 PC=0x0C R0=%u R2=0x%08X R6=0x%08X",
      'A' + unit, (unsigned)st.r[0], (unsigned)st.r[2], (unsigned)st.r[6]);
  return true;
}

}  // namespace vax_boot
