#pragma once
#include <stdint.h>
#include <stddef.h>

// Minimal DW750 UBA for /boot MSCP (C3 start). Not a cmi0/uda0 attach.
// SIMH vax750: UBA0 @ 0xF30000, 512 maps, valid 0x80000000; I/O page 0xFFE000.
namespace vax_uba {

static constexpr uint32_t UBA0_BASE      = 0x00F30000u;
static constexpr uint32_t UBA0_END       = 0x00F31000u;  // CSRs + 512 maps
static constexpr uint32_t MAP_BASE       = 0x00F30800u;
static constexpr uint32_t MAP_COUNT      = 512u;
static constexpr uint32_t MAP_VALID      = 0x80000000u;
static constexpr uint32_t MAP_PFN        = 0x001FFFFFu;
static constexpr uint32_t UNIMEM_BASE    = 0x00FC0000u;
static constexpr uint32_t UNIIO_BASE     = 0x00FFE000u;
static constexpr uint32_t UNIIO_END      = 0x01000000u;
static constexpr uint32_t WCS_BASE       = 0x00F00000u;
static constexpr uint32_t WCS_END        = 0x00F10000u;

void reset();

bool reg_hit(uint32_t pa);       // UBA0 CSR + map file
bool unibus_mem_hit(uint32_t pa);
bool io_page_hit(uint32_t pa);   // 0xFFE000.. excluding decoded CSRs
bool wcs_hit(uint32_t pa);

uint8_t read8(uint32_t pa);
void    write8(uint32_t pa, uint8_t v);

// 18-bit Unibus ba → host phys via map[ba>>9]. False if invalid.
bool map_ba(uint32_t ba, uint32_t* host);
bool map_unimem(uint32_t pa, uint32_t* host);

}  // namespace vax_uba
