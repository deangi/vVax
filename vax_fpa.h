#pragma once
#include <stdint.h>

// VAX F_floating / D_floating via host IEEE double. D has 3 extra fraction
// bits vs double; that is enough for NetBSD userland (CVTLD / libc %f).
// Study: VARM CMP/CVT condition codes and Open SIMH VAX/vax_fpa.c packing.
namespace vax_fpa {

enum {
  FLT_OVRFLO = 0x8,
  FLT_DIVZRO = 0x9,
  FLT_UNDFLO = 0xA
};

enum Pack {
  PACK_OK = 0,
  PACK_OVF = 1,
  PACK_UNF = 2
};

// 6-bit short literal → F_floating bits (also D low longword, hi=0).
uint32_t shortlit_f(uint8_t lit);

// reserved=true if sign=1 and exponent=0 (dirty zero).
bool unpack_f(uint32_t bits, double* out, bool* reserved);
bool unpack_d(uint32_t lo, uint32_t hi, double* out, bool* reserved);

Pack pack_f(double v, uint32_t* bits);
Pack pack_d(double v, uint32_t* lo, uint32_t* hi);

}  // namespace vax_fpa
