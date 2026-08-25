#pragma once
#include <stdint.h>
#include <stddef.h>

// DW750 UBA + MS750 MCTL (C3/C5). Not a full cmi0/uda0 attach.
// SIMH vax750: MCTL TR0 @ 0xF20000 (3 CSRs), ROM @ 0xF20400 (no dump here),
// UBA0 TR8 @ 0xF30000, NEXSIZE 0x2000, UBA_NMAPR 512, valid 0x80000000.
// NetBSD ubareg.h: UBA_MAPOFF 0x800, NUBMREG 496 (first 496 of 512).
// /boot also writes maps at UBA+0x2800 (0xF32800) — alias only, not TR9.
// TR9 (0xF32000) must MCHK so VMS/NetBSD do not invent UBA1 (UMEM @ 0xF80000).
namespace vax_uba {

static constexpr uint32_t UBA0_BASE      = 0x00F30000u;
static constexpr uint32_t NEXSIZE        = 0x2000u;
static constexpr uint32_t UBA0_NEX_END   = UBA0_BASE + NEXSIZE;  // 0xF32000
static constexpr uint32_t MAP_OFF        = 0x800u;               // UBA_MAPOFF
static constexpr uint32_t MAP_BASE       = UBA0_BASE + MAP_OFF;  // 0xF30800
static constexpr uint32_t MAP_ALIAS      = 0x00F32800u;          // TR9+0x800
static constexpr uint32_t MAP_COUNT      = 512u;                 // SIMH; 496 used
static constexpr uint32_t MAP_VALID      = 0x80000000u;
static constexpr uint32_t MAP_PFN        = 0x001FFFFFu;
static constexpr uint32_t MCTL_BASE      = 0x00F20000u;
static constexpr uint32_t MCTL_END       = 0x00F2000Cu;          // MCSR0/1/2
static constexpr uint32_t ROM_BASE       = 0x00F20400u;
static constexpr uint32_t ROM_END        = 0x00F20800u;          // 1 KiB, no dump
static constexpr uint32_t NEXUS_BASE     = 0x00F00000u;
static constexpr uint32_t NEXUS_END      = 0x00FC0000u;          // before Unibus mem
static constexpr uint32_t UNIMEM_BASE    = 0x00FC0000u;
static constexpr uint32_t UNIIO_BASE     = 0x00FFE000u;
static constexpr uint32_t UNIIO_END      = 0x01000000u;
static constexpr uint32_t WCS_BASE       = 0x00F00000u;
static constexpr uint32_t WCS_END        = 0x00F10000u;

void reset();
void set_ram_bytes(size_t ram_bytes);

bool reg_hit(uint32_t pa);       // UBA0 TR8 + map alias at 0xF32800
bool mctl_hit(uint32_t pa);      // MS750 TR0 CSRs
bool rom_hit(uint32_t pa);       // 0xF20400 window (reads 0; no DEC image)
bool nexus_hit(uint32_t pa);     // CMI 0xF00000..0xFC0000 (empty TR / hole)
bool unibus_mem_hit(uint32_t pa);
bool io_page_hit(uint32_t pa);
bool wcs_hit(uint32_t pa);

uint8_t read8(uint32_t pa);
void    write8(uint32_t pa, uint8_t v);

bool map_ba(uint32_t ba, uint32_t* host);
bool map_unimem(uint32_t pa, uint32_t* host);

}  // namespace vax_uba
