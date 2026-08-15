#pragma once
#include <stdint.h>
#include <stddef.h>

// MicroVAX-style MMU: P0/P1/S0 regions + MAPEN + PTE walk (Phase 3).
namespace vax_mmu {

// IPR numbers (subset) used with MFPR/MTPR.
enum Ipr : uint32_t {
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
  PTE_M    = 0x04000000u,
  PTE_PFN  = 0x001FFFFFu   // 21-bit PFN (512-byte pages)
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

// Translate VA → PA. Returns false on invalid PTE / length fault.
bool translate(uint32_t va, uint32_t* pa, bool write);
void invalidate_tb();

// Self-check: map one page, read/write through it (requires phys ops + RAM).
bool selftest();

}  // namespace vax_mmu
