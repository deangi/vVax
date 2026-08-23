#include "vax_boot.h"
#include "config.h"
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

static bool g_saw_elf = false;
static uint32_t g_elf_hdr_dest = 0;
static uint32_t g_elf_entry = 0;
static uint32_t g_elf_vaddr = 0;
static uint32_t g_elf_filesz = 0;
static uint32_t g_elf_memsz = 0;
static uint32_t g_elf_poff = 0;  // PT_LOAD p_offset — text starts here, not 0
static uint32_t g_elf_shoff = 0;
static uint16_t g_elf_shentsize = 0;
static uint16_t g_elf_shnum = 0;
static uint16_t g_elf_shstrndx = 0;
static uint32_t g_load_dest = 0xFFFFFFFFu;

// Stock CD BOOT.;1: e_entry=0, p_vaddr in the usual /boot windows. Host
// places PT_LOAD (skip p_offset) at 0x7A0000 (HDD /boot reloc base). dest=0
// after xxboot self-reloc is a bounce — same USB dest=0 as HDD — not load-at-0.
static constexpr uint32_t CD_RELOC_BASE = 0x7A0000u;
static constexpr uint32_t CD_BOOT_CAP = 0x20000u;  // 128 KiB; /boot is ~80 KiB
static bool g_cd_reloc = false;
static uint32_t g_cd_hdr_lbn = 0xFFFFFFFFu;
static uint32_t g_cd_nsec = 0;
static uint32_t g_cd_copied = 0;
static bool g_cd_patched = false;

static uint32_t rd_le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static bool stock_cd_elf(uint32_t entry, uint32_t vaddr) {
  return entry == 0u &&
         (vaddr == 0x7D0000u || vaddr == 0x7A0000u || vaddr == 0x5D0000u);
}

static bool iso_walk_lbn(uint32_t lbn) {
  return lbn == 16u || lbn == 129u || lbn == 65u || lbn == 169u || lbn == 201u;
}

static bool looks_elf(const uint8_t* p) {
  return p && p[0] == 0x7Fu && p[1] == 'E' && p[2] == 'L' && p[3] == 'F';
}

static bool sh_name_is(const uint8_t* str, uint32_t strsz, uint32_t off,
                       const char* want) {
  if (!str || !want || off >= strsz) return false;
  for (uint32_t i = 0;; i++) {
    if (off + i >= strsz) return false;
    char c = (char)str[off + i];
    if (c != want[i]) return false;
    if (c == 0) return true;
  }
}

static bool parse_pt_load(const uint8_t* hdr, uint32_t avail, uint32_t* poff,
                          uint32_t* vaddr, uint32_t* filesz, uint32_t* memsz) {
  if (!hdr || avail < 52u || !poff || !vaddr || !filesz || !memsz) return false;
  uint32_t phoff = rd_le32(hdr + 28);
  uint16_t phentsize = (uint16_t)(hdr[42] | ((uint16_t)hdr[43] << 8));
  uint16_t phnum = (uint16_t)(hdr[44] | ((uint16_t)hdr[45] << 8));
  if (phentsize < 32u || phnum == 0u) return false;
  for (uint16_t i = 0; i < phnum; i++) {
    uint32_t o = phoff + (uint32_t)i * phentsize;
    if (o + 24u > avail) break;
    if (rd_le32(hdr + o) != 1u) continue;  // PT_LOAD
    *poff = rd_le32(hdr + o + 4);
    *vaddr = rd_le32(hdr + o + 8);
    *filesz = rd_le32(hdr + o + 16);
    *memsz = rd_le32(hdr + o + 20);
    return true;
  }
  return false;
}

// Same rules as tools/opcodes/relocate_boot_elf.py (HDD /boot). Not a
// general ELF loader — only this stock /boot window (0x7D/7A/5D → 0x7A).
static void patch_abs_span(uint8_t* img, uint32_t start, uint32_t size,
                           uint32_t lo, uint32_t hi, uint32_t delta) {
  if (size < 4u) return;
  uint32_t last = start - 4u;
  uint32_t end = start + size;
  for (uint32_t off = start; off + 3u < end; off++) {
    if (off < last + 4u) continue;
    uint32_t v = rd_le32(img + off);
    if (v >= lo && v < hi) {
      wr_le32(img + off, v + delta);
      last = off;
    }
  }
}

static void patch_text_safe(uint8_t* img, uint32_t start, uint32_t size,
                            uint32_t lo, uint32_t hi, uint32_t delta) {
  uint32_t end = start + size;
  uint32_t off = start;
  while (off + 4u < end) {
    uint8_t b = img[off];
    if (b == 0x8Fu || b == 0x9Fu || (b >= 0xE0u && b <= 0xEEu)) {
      uint32_t v = rd_le32(img + off + 1);
      if (v >= lo && v < hi) {
        wr_le32(img + off + 1, v + delta);
        off += 5u;
        continue;
      }
    }
    off++;
  }
}

// bias = 0 when img is the full ELF file; bias = p_offset when img is PT_LOAD only.
static void relocate_cd_boot_image(uint8_t* img, uint32_t n, uint32_t old_base,
                                   uint32_t bias) {
  if (!img || n < 4u || old_base == CD_RELOC_BASE || g_cd_patched) return;
  const uint32_t delta = CD_RELOC_BASE - old_base;
  const bool have_elf = (bias == 0u && n >= 52u && looks_elf(img));
  if (have_elf) {
    uint32_t phoff = rd_le32(img + 28);
    wr_le32(img + 24, CD_RELOC_BASE);
    if (phoff + 16u <= n) {
      wr_le32(img + phoff + 8, CD_RELOC_BASE);
      wr_le32(img + phoff + 12, CD_RELOC_BASE);
    }
  }
  uint32_t span = g_elf_memsz > g_elf_filesz ? g_elf_memsz : g_elf_filesz;
  if (span == 0u) span = n + bias;
  uint32_t lo = old_base;
  uint32_t hi = old_base + span + 1u;

  uint32_t shoff = have_elf ? rd_le32(img + 32) : g_elf_shoff;
  uint16_t shentsize = have_elf
                           ? (uint16_t)(img[46] | ((uint16_t)img[47] << 8))
                           : g_elf_shentsize;
  uint16_t shnum = have_elf
                       ? (uint16_t)(img[48] | ((uint16_t)img[49] << 8))
                       : g_elf_shnum;
  uint16_t shstrndx = have_elf
                          ? (uint16_t)(img[50] | ((uint16_t)img[51] << 8))
                          : g_elf_shstrndx;
  bool did_named = false;
  if (shoff >= bias && shentsize >= 40u && shnum != 0u && shstrndx < shnum) {
    uint32_t sh_img = shoff - bias;
    if ((uint64_t)sh_img + (uint64_t)shnum * shentsize <= n) {
      uint32_t stroff_ent = sh_img + (uint32_t)shstrndx * shentsize;
      if (stroff_ent + 24u <= n) {
        uint32_t stroff = rd_le32(img + stroff_ent + 16);
        uint32_t strsz = rd_le32(img + stroff_ent + 20);
        if (stroff >= bias && (stroff - bias) + strsz <= n) {
          const uint8_t* shstr = img + (stroff - bias);
          for (uint16_t i = 0; i < shnum; i++) {
            uint32_t o = sh_img + (uint32_t)i * shentsize;
            uint32_t name_off = rd_le32(img + o);
            uint32_t addr = rd_le32(img + o + 12);
            uint32_t offset = rd_le32(img + o + 16);
            uint32_t size = rd_le32(img + o + 20);
            if (addr >= old_base && addr < old_base + span + 1u)
              wr_le32(img + o + 12, addr + delta);
            if (offset < bias) continue;
            uint32_t io = offset - bias;
            if (io + size > n) continue;
            if (sh_name_is(shstr, strsz, name_off, ".text"))
              patch_text_safe(img, io, size, lo, hi, delta);
            else if (sh_name_is(shstr, strsz, name_off, ".rodata") ||
                     sh_name_is(shstr, strsz, name_off, ".data") ||
                     sh_name_is(shstr, strsz, name_off, ".eh_frame"))
              patch_abs_span(img, io, size, lo, hi, delta);
          }
          did_named = true;
        }
      }
    }
  }
  if (!did_named) {
    uint32_t poff_img = have_elf ? g_elf_poff : 0u;
    uint32_t psz = g_elf_filesz ? g_elf_filesz : n;
    if (poff_img + psz > n) psz = n - poff_img;
    patch_text_safe(img, poff_img, psz, lo, hi, delta);
  }
  g_cd_patched = true;
}

// Pull BOOT.;1 PT_LOAD (+ trailing SHDRs) to 0x7A0000. xxboot may never
// read past p_filesz, and dest=0 bounce overwrites the header sector.
static void prefetch_cd_pt_load() {
  uint8_t* ram = vax_cpu::ram();
  size_t ram_bytes = vax_cpu::ram_bytes();
  if (!ram || g_cd_hdr_lbn == 0xFFFFFFFFu || g_cd_nsec == 0u) return;
  uint8_t tmp[512];
  uint32_t cons = g_conspage_pa;
  for (uint32_t i = 0; i < g_cd_nsec; i++) {
    uint32_t file_off = i * 512u;
    if (file_off + 512u <= g_elf_poff) continue;
    uint32_t src_skip = 0;
    uint32_t nbytes = 512u;
    uint32_t off;
    if (file_off < g_elf_poff) {
      src_skip = g_elf_poff - file_off;
      nbytes = 512u - src_skip;
      off = 0;
    } else {
      off = file_off - g_elf_poff;
    }
    uint32_t nd = CD_RELOC_BASE + off;
    if (nd + nbytes > ram_bytes) break;
    if (cons && nd + nbytes > cons) break;
    if (!vax_mscp::read_blocks((vax_mscp::Unit)g_unit, g_cd_hdr_lbn + i, tmp,
                               1))
      break;
    memcpy(ram + nd, tmp + src_skip, nbytes);
    if (off + nbytes > g_cd_copied) g_cd_copied = off + nbytes;
  }
}

uint8_t boot_unit() { return g_unit; }

uint32_t ka630_conspage_pa() { return g_conspage_pa; }

void plant_ka630_console() {
#if VAX_MODEL != VAX_MODEL_KA630
  return;
#else
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
#endif
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
#if VAX_MODEL != VAX_MODEL_KA630
  (void)pa;
  return false;
#else
  return pa >= KA630_NVR_PA && pa < KA630_NVR_PA + 8u;
#endif
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
#if VAX_MODEL != VAX_MODEL_KA630
  (void)target;
  return false;
#else
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
#endif
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
  // Stock CD PT_LOAD dest is p_vaddr (0x7D0000) — that is past RAM+conspage
  // and currently drops. Redirect only that window. dest=0 is xxboot's bounce
  // (HDD also logs dest=0); do not steal ISO9660 walks at dest=0.
  uint32_t read_dest = dest;
  uint32_t span = g_elf_filesz;
  if (g_elf_memsz > span) span = g_elf_memsz;
  if (span == 0u || span > CD_BOOT_CAP) span = CD_BOOT_CAP;
  if (g_cd_reloc && dest >= g_elf_vaddr && dest < g_elf_vaddr + span &&
      !iso_walk_lbn(lbn)) {
    uint32_t off = dest - g_elf_vaddr;
    uint32_t nd = CD_RELOC_BASE + off;
    if (nd + 512u <= ram_bytes) read_dest = nd;
  }

  bool ok = false;
  uint64_t disk_blocks = vax_mscp::size_bytes((vax_mscp::Unit)g_unit) / 512u;
  if (read_dest + 512u <= ram_bytes && ram && (uint64_t)lbn < disk_blocks)
    ok = vax_mscp::read_blocks((vax_mscp::Unit)g_unit, lbn, ram + read_dest, 1);
  st.r[0] = ok ? 1u : 0u;

  // xxboot reads ELF hdr into a bounce/stack dest, then PT_LOAD at e_entry
  // or p_vaddr. dest=0 after xxboot reloc is a bounce, not load-at-0.
  if (ok && ram && read_dest + 52u <= ram_bytes &&
      looks_elf(ram + read_dest) && !g_saw_elf) {
    g_saw_elf = true;
    g_elf_hdr_dest = dest;
    g_elf_entry = rd_le32(ram + read_dest + 24);
    g_elf_shoff = rd_le32(ram + read_dest + 32);
    g_elf_shentsize = (uint16_t)(ram[read_dest + 46] | ((uint16_t)ram[read_dest + 47] << 8));
    g_elf_shnum = (uint16_t)(ram[read_dest + 48] | ((uint16_t)ram[read_dest + 49] << 8));
    g_elf_shstrndx = (uint16_t)(ram[read_dest + 50] | ((uint16_t)ram[read_dest + 51] << 8));
    uint32_t poff = 0, vaddr = 0, filesz = 0, memsz = 0;
    if (parse_pt_load(ram + read_dest, 512u, &poff, &vaddr, &filesz, &memsz)) {
      g_elf_poff = poff;
      g_elf_vaddr = vaddr;
      g_elf_filesz = filesz;
      g_elf_memsz = memsz;
    }
    LOG("boot: ELF hdr dest=%08X e_entry=%08X p_vaddr=%08X p_offset=%u p_filesz=%u hopp=%08X",
        (unsigned)dest, (unsigned)g_elf_entry, (unsigned)g_elf_vaddr,
        (unsigned)g_elf_poff, (unsigned)g_elf_filesz,
        (unsigned)(g_elf_entry + 2u));
    if (stock_cd_elf(g_elf_entry, g_elf_vaddr)) {
      g_cd_reloc = true;
      g_cd_hdr_lbn = lbn;
      uint32_t total = g_elf_poff + g_elf_filesz;
      if (g_elf_shoff != 0u && g_elf_shentsize >= 40u && g_elf_shnum != 0u) {
        uint32_t sh_end =
            g_elf_shoff + (uint32_t)g_elf_shnum * g_elf_shentsize;
        if (sh_end > total) total = sh_end;
      }
      g_cd_nsec = (total + 511u) / 512u;
      if (g_cd_nsec == 0u || g_cd_nsec > CD_BOOT_CAP / 512u)
        g_cd_nsec = CD_BOOT_CAP / 512u;
      LOG("boot: CD /boot reloc p_vaddr=%08X -> %08X p_offset=%u p_filesz=%u",
          (unsigned)g_elf_vaddr, (unsigned)CD_RELOC_BASE,
          (unsigned)g_elf_poff, (unsigned)g_elf_filesz);
      prefetch_cd_pt_load();
      if (g_cd_copied != 0u && g_load_dest == 0xFFFFFFFFu) {
        g_load_dest = CD_RELOC_BASE;
        LOG("boot: ELF load dest=%08X LBN=%u", (unsigned)CD_RELOC_BASE,
            (unsigned)(g_cd_hdr_lbn + (g_elf_poff / 512u)));
      }
    }
  } else if (ok && g_saw_elf && g_load_dest == 0xFFFFFFFFu && dest != g_elf_hdr_dest) {
    // Payload dest is e_entry (stock 0) or a linked window — not xxboot stack.
    if (dest < 0x10000u || dest >= 0x5D0000u) {
      g_load_dest = g_cd_reloc ? CD_RELOC_BASE : dest;
      LOG("boot: ELF load dest=%08X LBN=%u", (unsigned)g_load_dest, (unsigned)lbn);
    }
  }

  // Assemble PT_LOAD at 0x7A0000 (skip ELF p_offset). p_vaddr dests already
  // redirected. dest=0 bounce uses LBN-from-hdr minus p_offset. ISO dir
  // walks stay at dest=0.
  if (ok && g_cd_reloc && ram && !iso_walk_lbn(lbn) &&
      g_cd_hdr_lbn != 0xFFFFFFFFu) {
    uint32_t off = 0xFFFFFFFFu;
    uint32_t src_skip = 0;
    uint32_t nbytes = 512u;
    if (dest >= g_elf_vaddr && dest < g_elf_vaddr + span)
      off = dest - g_elf_vaddr;
    else if (lbn >= g_cd_hdr_lbn && lbn < g_cd_hdr_lbn + g_cd_nsec) {
      uint32_t file_off = (lbn - g_cd_hdr_lbn) * 512u;
      if (file_off + 512u <= g_elf_poff) {
        // ELF header / phdrs — not text
      } else if (file_off < g_elf_poff) {
        src_skip = g_elf_poff - file_off;
        nbytes = 512u - src_skip;
        off = 0;
      } else {
        off = file_off - g_elf_poff;
      }
    }
    if (off != 0xFFFFFFFFu) {
      uint32_t nd = CD_RELOC_BASE + off;
      if (nd + nbytes <= ram_bytes) {
        if (read_dest + src_skip != nd)
          memcpy(ram + nd, ram + read_dest + src_skip, nbytes);
        if (off + nbytes > g_cd_copied) g_cd_copied = off + nbytes;
        if (g_load_dest == 0xFFFFFFFFu) {
          g_load_dest = CD_RELOC_BASE;
          LOG("boot: ELF load dest=%08X LBN=%u", (unsigned)CD_RELOC_BASE,
              (unsigned)lbn);
        }
      }
    }
  }

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

bool apply_cd_boot_hopp(uint32_t* npc) {
  if (!g_cd_reloc || !npc) return false;
  if (*npc != g_elf_entry + 2u) return false;

  uint8_t* ram = vax_cpu::ram();
  size_t ram_bytes = vax_cpu::ram_bytes();
  uint32_t copy_n = g_elf_filesz ? g_elf_filesz : CD_BOOT_CAP;
  if (g_cd_copied > copy_n) copy_n = g_cd_copied;
  if (copy_n > CD_BOOT_CAP) copy_n = CD_BOOT_CAP;
  if (CD_RELOC_BASE + copy_n > ram_bytes)
    copy_n = (ram_bytes > CD_RELOC_BASE) ? (uint32_t)(ram_bytes - CD_RELOC_BASE)
                                         : 0u;
  uint32_t cons = g_conspage_pa;
  if (cons && CD_RELOC_BASE + copy_n > cons)
    copy_n = cons - CD_RELOC_BASE;

  if (ram && copy_n >= 4u) {
    const bool have_elf = looks_elf(ram + CD_RELOC_BASE);
    if (g_elf_vaddr != CD_RELOC_BASE) {
      uint32_t bias = have_elf ? 0u : g_elf_poff;
      relocate_cd_boot_image(ram + CD_RELOC_BASE, copy_n, g_elf_vaddr, bias);
    }
    // V0.6.75 left 7F ELF at the load base. Slide PT_LOAD down so hopp+2
    // is the VAX entry mask, not the 'LF' of "ELF".
    if (have_elf && g_elf_poff > 0u && g_elf_poff < copy_n) {
      uint32_t payload = copy_n - g_elf_poff;
      if (g_elf_filesz && g_elf_filesz < payload) payload = g_elf_filesz;
      memmove(ram + CD_RELOC_BASE, ram + CD_RELOC_BASE + g_elf_poff, payload);
      copy_n = payload;
    }
  }

  if (ram && g_elf_memsz > g_elf_filesz) {
    uint32_t z0 = g_elf_filesz;
    uint32_t z1 = g_elf_memsz;
    if (CD_RELOC_BASE + z1 > ram_bytes)
      z1 = (ram_bytes > CD_RELOC_BASE) ? (uint32_t)(ram_bytes - CD_RELOC_BASE)
                                       : 0u;
    if (cons && CD_RELOC_BASE + z1 > cons) z1 = cons - CD_RELOC_BASE;
    if (z1 > z0) memset(ram + CD_RELOC_BASE + z0, 0, z1 - z0);
  }

  if (ram && CD_RELOC_BASE + 8u <= ram_bytes) {
    const uint8_t* t = ram + CD_RELOC_BASE;
    LOG("boot: CD /boot @7A0000 p_offset=%u p_filesz=%u bytes=%02X %02X %02X %02X %02X %02X %02X %02X",
        (unsigned)g_elf_poff, (unsigned)g_elf_filesz,
        (unsigned)t[0], (unsigned)t[1], (unsigned)t[2], (unsigned)t[3],
        (unsigned)t[4], (unsigned)t[5], (unsigned)t[6], (unsigned)t[7]);
  }

  auto& st = vax_cpu::state();
  // hoppabort: movl 4(ap),r6 / movab 2(r6),-(sp) / rei. R6 is the entry.
  if (st.r[6] == 0u || st.r[6] == g_elf_entry || st.r[6] == g_elf_vaddr)
    st.r[6] = CD_RELOC_BASE;

  g_cd_reloc = false;  // one-shot — later PC=2 is a real fault
  *npc = CD_RELOC_BASE + 2u;
  return true;
}

void log_elf_hopp(uint32_t npc) {
  LOG("boot: REI -> low PC=%08X (bad ELF e_entry/hoppabort?) e_entry=%08X p_vaddr=%08X load=%08X hopp=%08X",
      (unsigned)npc, (unsigned)g_elf_entry, (unsigned)g_elf_vaddr,
      (unsigned)g_load_dest, (unsigned)(g_elf_entry + 2u));
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
  g_saw_elf = false;
  g_elf_hdr_dest = 0;
  g_elf_entry = 0;
  g_elf_vaddr = 0;
  g_elf_filesz = 0;
  g_elf_memsz = 0;
  g_elf_poff = 0;
  g_elf_shoff = 0;
  g_elf_shentsize = 0;
  g_elf_shnum = 0;
  g_elf_shstrndx = 0;
  g_load_dest = 0xFFFFFFFFu;
  g_cd_reloc = false;
  g_cd_hdr_lbn = 0xFFFFFFFFu;
  g_cd_nsec = 0;
  g_cd_copied = 0;
  g_cd_patched = false;

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

#if VAX_MODEL == VAX_MODEL_KA630
  plant_ka630_console();
#endif

  st.r[0] = BDEV_UDA;
#if VAX_MODEL == VAX_MODEL_KA750
  // NetBSD FROM750 / SIMH vax750: R1 = DW750 UBA phys, R2 = Unibus UDA CSR.
  // Do not keep R1=0x20000000 (that is Q22 qba). See docs/VAX11750.md §3.
  st.r[1] = 0x00F30000u;
#else
  st.r[1] = 0x20000000u;
#endif
  st.r[2] = vax_mscp::CSR_IP_PA;
  st.r[3] = unit;
  st.r[5] = 0;
  st.r[6] = ROM_READ_PA;
  st.r[vax_cpu::R_SP] = 0x100000u;
  st.r[vax_cpu::R_PC] = 0x0Cu;

  LOG("boot: MSCP %c xxboot FROM750 PC=0x0C R0=%u R1=0x%08X R2=0x%08X R6=0x%08X",
      'A' + unit, (unsigned)st.r[0], (unsigned)st.r[1],
      (unsigned)st.r[2], (unsigned)st.r[6]);
  return true;
}

}  // namespace vax_boot
