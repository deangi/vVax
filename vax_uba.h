#pragma once
#include <stdint.h>
#include <stddef.h>

// Minimal DW750 UBA for /boot MSCP (C3 start). Not a cmi0/uda0 attach.
// SIMH vax750: UBA0 @ 0xF30000, NEXSIZE 0x2000, UBA_NMAPR 512, valid 0x80000000.
// NetBSD ubareg.h: UBA_MAPOFF 0x800, NUBMREG 496 (first 496 of 512).
// /boot V0.7.0 also writes maps at UBA+0x2800 (0xF32800) — treat as alias.
namespace vax_uba {

static constexpr uint32_t UBA0_BASE      = 0x00F30000u;
static constexpr uint32_t NEXSIZE        = 0x2000u;
static constexpr uint32_t UBA0_NEX_END   = UBA0_BASE + NEXSIZE;  // 0xF32000
static constexpr uint32_t UBA0_END       = 0x00F34000u;          // hole + alias
static constexpr uint32_t MAP_OFF        = 0x800u;               // UBA_MAPOFF
static constexpr uint32_t MAP_BASE       = UBA0_BASE + MAP_OFF;  // 0xF30800
static constexpr uint32_t MAP_ALIAS      = 0x00F32800u;          // +0x2000+0x800
static constexpr uint32_t MAP_COUNT      = 512u;                 // SIMH; 496 used
static constexpr uint32_t MAP_VALID      = 0x80000000u;
static constexpr uint32_t MAP_PFN        = 0x001FFFFFu;
static constexpr uint32_t NEXUS_BASE     = 0x00F00000u;
static constexpr uint32_t NEXUS_END      = 0x00FC0000u;          // before Unibus mem
static constexpr uint32_t UNIMEM_BASE    = 0x00FC0000u;
static constexpr uint32_t UNIIO_BASE     = 0x00FFE000u;
static constexpr uint32_t UNIIO_END      = 0x01000000u;
static constexpr uint32_t WCS_BASE       = 0x00F00000u;
static constexpr uint32_t WCS_END        = 0x00F10000u;

void reset();

bool reg_hit(uint32_t pa);       // UBA0 CSR + both map windows
bool nexus_hit(uint32_t pa);     // CMI 0xF00000..0xFC0000 (empty TR / hole)
bool unibus_mem_hit(uint32_t pa);
bool io_page_hit(uint32_t pa);
bool wcs_hit(uint32_t pa);

uint8_t read8(uint32_t pa);
void    write8(uint32_t pa, uint8_t v);

bool map_ba(uint32_t ba, uint32_t* host);
bool map_unimem(uint32_t pa, uint32_t* host);

}  // namespace vax_uba
