#include "vax_mmu.h"
#include "platform.h"

#include <string.h>

namespace vax_mmu {

static constexpr uint32_t PAGE_SIZE = 512;
static constexpr uint32_t PAGE_SHIFT = 9;
// VAX physical address width is 30 bits (SIMH PAMASK). With MAPEN off the
// virtual address is used as a physical address after this mask — so
// KERNBASE (0x80000000) aliases to PA 0, which is how NetBSD /boot loads
// the kernel at its linked S0 addresses without an S0 page table yet.
static constexpr uint32_t PAMASK = 0x3FFFFFFFu;

static constexpr uint32_t BR_MASK = 0xFFFFFFFCu;
static constexpr uint32_t LR_MASK = 0x003FFFFFu;  // 22-bit region length (SIMH)
static constexpr uint32_t VA_S0   = 0x80000000u;

static uint32_t g_p0br = 0, g_p0lr = 0;
static uint32_t g_p1br = 0, g_p1lr = 0;
static uint32_t g_sbr  = 0, g_slr  = 0;
static bool     g_mapen = false;

static PhysRead32Fn  g_rd = nullptr;
static PhysWrite32Fn g_wr = nullptr;
static uint8_t       g_fault = FLT_OK;

// SIMH vax_mmu.c cvtacc[]: PTE access field → {user,supv,exec,kern} R in
// bits 0–3 and W in bits 4–7. Index 0/1 (no access / reserved) are 0 so a
// zero PTE is ACV, not TNV. NetBSD Xtransl_v (TNV) ORs PG_V and REIs —
// safe only when protection is already set (ref-bit simulation).
static const uint8_t cvtacc[16] = {
  0x00, 0x00, 0x11, 0x01, 0xFF, 0x33, 0x13, 0x03,
  0x77, 0x37, 0x17, 0x07, 0x7F, 0x3F, 0x1F, 0x0F
};

uint8_t last_fault() { return g_fault; }

void set_phys_ops(PhysRead32Fn rd, PhysWrite32Fn wr) {
  g_rd = rd;
  g_wr = wr;
}

void reset() {
  g_p0br = g_p0lr = 0;
  g_p1br = g_p1lr = 0;
  g_sbr = g_slr = 0;
  g_mapen = false;
}

bool mapen() { return g_mapen; }
void set_mapen(bool on) {
  g_mapen = on;
  invalidate_tb();
}

void invalidate_tb() {
  // Soft TB not cached yet — walks are direct.
}

uint32_t get_ipr(uint32_t ipr) {
  switch (ipr) {
    case IPR_P0BR:  return g_p0br;
    case IPR_P0LR:  return g_p0lr;
    case IPR_P1BR:  return g_p1br;
    case IPR_P1LR:  return g_p1lr;
    case IPR_SBR:   return g_sbr;
    case IPR_SLR:   return g_slr;
    case IPR_MAPEN: return g_mapen ? 1u : 0u;
    default:        return 0;
  }
}

void set_ipr(uint32_t ipr, uint32_t value) {
  switch (ipr) {
    case IPR_P0BR:  g_p0br = value & BR_MASK; break;
    case IPR_P0LR:  g_p0lr = value & LR_MASK; break;  // drop AST_PCB etc.
    case IPR_P1BR:  g_p1br = value & BR_MASK; break;
    case IPR_P1LR:  g_p1lr = value & LR_MASK; break;
    case IPR_SBR:   g_sbr  = value & BR_MASK; break;
    case IPR_SLR:   g_slr  = value & LR_MASK; break;
    case IPR_MAPEN: set_mapen(value != 0); break;
    case IPR_TBIA:  invalidate_tb(); break;
    case IPR_TBIS:  invalidate_tb(); break;
    default: break;
  }
}

static bool pte_allows(uint32_t pte, bool write, uint32_t mode) {
  uint8_t bits = cvtacc[(pte >> 27) & 0xFu];
  uint8_t need = (uint8_t)(1u << (mode & 3u));
  if (write) need = (uint8_t)(need << 4);
  return (bits & need) != 0;
}

static bool walk_pte(uint32_t pte_pa, uint32_t va, uint32_t* pa, bool write,
                     uint32_t mode) {
  if (!g_rd) {
    g_fault = FLT_ACV;
    return false;
  }
  uint32_t pte = g_rd(pte_pa);
  // SIMH fill(): access before valid. Zero PTE (acc=0) is ACV so NetBSD
  // Xaccess_v → uvm_fault. TNV is only for V=0 with protection already set
  // (pmap_simulref re-validates and REIs).
  if (!pte_allows(pte, write, mode)) {
    g_fault = FLT_ACV;
    return false;
  }
  if (!(pte & PTE_V)) {
    g_fault = FLT_TNV;
    return false;
  }
  uint32_t pfn = pte & PTE_PFN;
  uint32_t phys = (pfn << PAGE_SHIFT) | (va & (PAGE_SIZE - 1));
  if (write && g_wr && !(pte & PTE_M)) {
    g_wr(pte_pa, pte | PTE_M);
  }
  if (pa) *pa = phys;
  return true;
}

// S0: SBR is a physical address (NetBSD: Sysmap - KERNBASE).
static bool translate_s0(uint32_t va, uint32_t* pa, bool write, uint32_t mode) {
  uint32_t vpn = (va - VA_S0) >> PAGE_SHIFT;
  if (vpn >= (g_slr & LR_MASK)) {
    g_fault = FLT_LNV;
    return false;
  }
  return walk_pte((g_sbr & BR_MASK) + vpn * 4, va, pa, write, mode);
}

// P0/P1 page tables live in S0 (SIMH: ppte must be sys). Physical P0BR
// (selftest) is allowed when the PTE address has bit 31 clear.
// PTE fetch is a kernel read regardless of the data access mode.
static bool walk_process_pte(uint32_t pte_va, uint32_t va, uint32_t* pa,
                             bool write, uint32_t mode) {
  uint32_t pte_pa = pte_va;
  if (pte_va & VA_S0) {
    if (!translate_s0(pte_va, &pte_pa, false, 0)) return false;
  }
  return walk_pte(pte_pa, va, pa, write, mode);
}

bool translate(uint32_t va, uint32_t* pa, bool write, uint32_t cur_mode) {
  g_fault = FLT_OK;
  if (!g_mapen) {
    if (pa) *pa = va & PAMASK;
    return true;
  }
  if (!g_rd) {
    if (pa) *pa = va & PAMASK;
    return true;
  }

  // P0: 00000000–3FFFFFFF. P0LR = mapped pages from 0 (SIMH: vpn >= P0LR → LNV).
  if (va < 0x40000000u) {
    uint32_t vpn = va >> PAGE_SHIFT;
    if (vpn >= (g_p0lr & LR_MASK)) {
      g_fault = FLT_LNV;
      return false;
    }
    return walk_process_pte((g_p0br & BR_MASK) + vpn * 4, va, pa, write, cur_mode);
  }

  // P1: 40000000–7FFFFFFF. P1LR = unmapped pages from 0x40000000 upward
  // (SIMH: ptidx < (P1LR<<2)+0x800000 → LNV). NPTEPERREG (0x200000) = empty P1.
  if (va < VA_S0) {
    uint32_t p1_idx = (va - 0x40000000u) >> PAGE_SHIFT;
    if (p1_idx < (g_p1lr & LR_MASK)) {
      g_fault = FLT_LNV;
      return false;
    }
    return walk_process_pte((g_p1br & BR_MASK) + p1_idx * 4, va, pa, write, cur_mode);
  }

  // S0: 80000000–BFFFFFFF
  if (va < 0xC0000000u)
    return translate_s0(va, pa, write, cur_mode);

  // S1 (VA<31:30>=11) is reserved. VARM: length violation, not identity.
  // NetBSD CAS RAS uses CASMAGIC=0xFEDABABE so an interrupted CAS traps
  // here (T_PTELEN) and restarts at cas32_ras_start.
  g_fault = FLT_LNV;
  return false;
}

bool selftest() {
  // Requires phys ops already installed and guest RAM large enough.
  // Layout: PTE page at PA 0x2000; data page at PA 0x3000; map VA 0x0000 → 0x3000.
  if (!g_rd || !g_wr) {
    LOGE("MMU selftest: no phys ops");
    return false;
  }

  reset();
  const uint32_t pte_pa  = 0x2000;
  const uint32_t data_pa = 0x3000;
  const uint32_t data_va = 0x0000;

  // One P0 page: VPN0 → PFN for 0x3000 (KW so ACV-before-V does not fire)
  uint32_t pfn = data_pa >> PAGE_SHIFT;
  g_wr(pte_pa, PTE_V | PTE_KW | pfn);
  g_wr(data_pa, 0);
  g_wr(data_pa + 4, 0);

  g_p0br = pte_pa;
  g_p0lr = 1;
  set_mapen(true);

  uint32_t pa = 0;
  if (!translate(data_va, &pa, false) || pa != data_pa) {
    LOGE("MMU selftest: translate read fail pa=%08X", (unsigned)pa);
    reset();
    return false;
  }

  // Write via translated address path is exercised by CPU; here poke phys and
  // confirm reverse translate for write sets M.
  if (!translate(data_va, &pa, true) || pa != data_pa) {
    LOGE("MMU selftest: translate write fail");
    reset();
    return false;
  }
  uint32_t pte = g_rd(pte_pa);
  if (!(pte & PTE_M)) {
    LOGE("MMU selftest: M bit not set");
    reset();
    return false;
  }

  // Length fault
  if (translate(PAGE_SIZE, &pa, false)) {
    LOGE("MMU selftest: expected length fault");
    reset();
    return false;
  }
  if (translate(0xC0000000u, &pa, false) || translate(0xFEDABABEu, &pa, true)) {
    LOGE("MMU selftest: expected S1 length fault");
    reset();
    return false;
  }
  if (last_fault() != FLT_LNV) {
    LOGE("MMU selftest: S1 not LNV");
    reset();
    return false;
  }

  // Zero PTE: protection field 0 → ACV (NetBSD uvm_fault), not TNV.
  g_wr(pte_pa, 0);
  if (translate(data_va, &pa, true) || last_fault() != FLT_ACV) {
    LOGE("MMU selftest: zero PTE should be ACV");
    reset();
    return false;
  }
  // KW, V=0: TNV (ref-bit simulation path).
  g_wr(pte_pa, PTE_KW | pfn);
  if (translate(data_va, &pa, true) || last_fault() != FLT_TNV) {
    LOGE("MMU selftest: V=0 KW should be TNV");
    reset();
    return false;
  }

  set_mapen(false);
  if (!translate(0x1234, &pa, false) || pa != 0x1234) {
    LOGE("MMU selftest: identity map fail");
    reset();
    return false;
  }
  // MAPEN-off PAMASK: S0 / KERNBASE → low physical (NetBSD loadfile path).
  if (!translate(0x80001234u, &pa, false) || pa != 0x1234u) {
    LOGE("MMU selftest: S0 PAMASK alias fail pa=%08X", (unsigned)pa);
    reset();
    return false;
  }
  if (!translate(0x20000000u, &pa, false) || pa != 0x20000000u) {
    LOGE("MMU selftest: Q22 PAMASK fail pa=%08X", (unsigned)pa);
    reset();
    return false;
  }

  reset();
  LOG("MMU selftest: PASS");
  return true;
}

}  // namespace vax_mmu
