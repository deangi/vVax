#include "vax_mmu.h"
#include "platform.h"

#include <string.h>

namespace vax_mmu {

static constexpr uint32_t PAGE_SIZE = 512;
static constexpr uint32_t PAGE_SHIFT = 9;

static uint32_t g_p0br = 0, g_p0lr = 0;
static uint32_t g_p1br = 0, g_p1lr = 0;
static uint32_t g_sbr  = 0, g_slr  = 0;
static bool     g_mapen = false;

static PhysRead32Fn  g_rd = nullptr;
static PhysWrite32Fn g_wr = nullptr;

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
    case IPR_P0BR:  g_p0br = value; break;
    case IPR_P0LR:  g_p0lr = value; break;
    case IPR_P1BR:  g_p1br = value; break;
    case IPR_P1LR:  g_p1lr = value; break;
    case IPR_SBR:   g_sbr  = value; break;
    case IPR_SLR:   g_slr  = value; break;
    case IPR_MAPEN: set_mapen(value != 0); break;
    case IPR_TBIA:  invalidate_tb(); break;
    case IPR_TBIS:  invalidate_tb(); break;
    default: break;
  }
}

static bool walk_pte(uint32_t pte_pa, uint32_t va, uint32_t* pa, bool write) {
  if (!g_rd) return false;
  uint32_t pte = g_rd(pte_pa);
  if (!(pte & PTE_V)) return false;
  uint32_t pfn = pte & PTE_PFN;
  uint32_t phys = (pfn << PAGE_SHIFT) | (va & (PAGE_SIZE - 1));
  if (write && g_wr && !(pte & PTE_M)) {
    // Soft-set M bit (best-effort).
    g_wr(pte_pa, pte | PTE_M);
  }
  if (pa) *pa = phys;
  return true;
}

bool translate(uint32_t va, uint32_t* pa, bool write) {
  if (!g_mapen) {
    if (pa) *pa = va;
    return true;
  }
  if (!g_rd) {
    if (pa) *pa = va;
    return true;
  }

  // P0: 00000000–3FFFFFFF
  if (va < 0x40000000u) {
    uint32_t vpn = va >> PAGE_SHIFT;
    if (vpn >= g_p0lr) return false;
    return walk_pte(g_p0br + vpn * 4, va, pa, write);
  }

  // P1: 40000000–7FFFFFFF (grows toward lower addresses in length sense)
  if (va < 0x80000000u) {
    // VPN within P1: from top of P1 space. P1LR = length in pages.
    // Classic: offset from 0x80000000 downward.
    uint32_t off = 0x80000000u - va;
    uint32_t vpn = (off >> PAGE_SHIFT);
    if (vpn == 0 || vpn > g_p1lr) return false;
    // PTE index 0 is for highest page; use (P1LR - vpn)
    uint32_t idx = g_p1lr - vpn;
    return walk_pte(g_p1br + idx * 4, va, pa, write);
  }

  // S0: 80000000–BFFFFFFF
  if (va < 0xC0000000u) {
    uint32_t vpn = (va - 0x80000000u) >> PAGE_SHIFT;
    if (vpn >= g_slr) return false;
    return walk_pte(g_sbr + vpn * 4, va, pa, write);
  }

  // S1 / reserved — identity for console MMIO etc.
  if (pa) *pa = va;
  return true;
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

  // One P0 page: VPN0 → PFN for 0x3000
  uint32_t pfn = data_pa >> PAGE_SHIFT;
  g_wr(pte_pa, PTE_V | pfn);
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

  set_mapen(false);
  if (!translate(0x1234, &pa, false) || pa != 0x1234) {
    LOGE("MMU selftest: identity map fail");
    reset();
    return false;
  }

  reset();
  LOG("MMU selftest: PASS");
  return true;
}

}  // namespace vax_mmu
