#pragma once
#include <stdint.h>
#include <stddef.h>

// MicroVAX-style MMU: P0/P1/S0 regions + MAPEN + PTE walk (Phase 3).
namespace vax_mmu {

// IPR numbers (subset) used with MFPR/MTPR.
enum Ipr : uint32_t {
  IPR_KSP   = 0,
  IPR_ESP   = 1,
  IPR_SSP   = 2,
  IPR_USP   = 3,
  IPR_ISP   = 4,   // interrupt stack (NetBSD PR_ISP)
  IPR_P0BR  = 8,
  IPR_P0LR  = 9,
  IPR_P1BR  = 10,
  IPR_P1LR  = 11,
  IPR_SBR   = 12,
  IPR_SLR   = 13,
  IPR_MAPEN = 56,
  IPR_TBIA  = 57,
  IPR_TBIS  = 58
};

// PTE bit fields (VAX longword PTE).
enum {
  PTE_V    = 0x80000000u,
  PTE_ACC  = 0x78000000u,  // 4-bit protection (SIMH PTE_ACC)
  PTE_KW   = 0x10000000u,  // kernel write (acc=2)
  PTE_M    = 0x04000000u,
  PTE_PFN  = 0x001FFFFFu   // 21-bit PFN (512-byte pages)
};

// Last translate() miss. SIMH fill() checks protection before V:
//   acc=0 (PTE=0) → ACV; V=0 with prot set → TNV; region → LNV.
enum Fault : uint8_t {
  FLT_OK  = 0,
  FLT_ACV = 1,
  FLT_LNV = 2,
  FLT_TNV = 3
};

void reset();

// Physical memory probe used while walking PTEs (identity PA into guest RAM).
using PhysRead32Fn = uint32_t (*)(uint32_t pa);
using PhysWrite32Fn = void (*)(uint32_t pa, uint32_t v);
void set_phys_ops(PhysRead32Fn rd, PhysWrite32Fn wr);

bool mapen();
void set_mapen(bool on);

uint32_t get_ipr(uint32_t ipr);
void     set_ipr(uint32_t ipr, uint32_t value);

// Translate VA → PA. cur_mode is PSL<CUR> (0=kernel … 3=user).
// Returns false on ACV / TNV / LNV; last_fault() says which.
bool translate(uint32_t va, uint32_t* pa, bool write, uint32_t cur_mode = 0);
uint8_t last_fault();
void invalidate_tb();

// Self-check: map one page, read/write through it (requires phys ops + RAM).
bool selftest();

}  // namespace vax_mmu
