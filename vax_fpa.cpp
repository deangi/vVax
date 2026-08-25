#include "vax_fpa.h"

#include <math.h>

namespace vax_fpa {

uint32_t shortlit_f(uint8_t lit) {
  lit &= 0x3Fu;
  // VARM short float: 0 → 0.5, then 1/16 steps with exponent in bits 5:3.
  return 0x00004000u | ((uint32_t)(lit & 7u) << 4) | ((uint32_t)(lit >> 3) << 7);
}

bool unpack_f(uint32_t bits, double* out, bool* reserved) {
  const uint32_t sign = (bits >> 15) & 1u;
  const uint32_t exp = (bits >> 7) & 0xFFu;
  const uint32_t frac = ((bits & 0x7Fu) << 16) | (bits >> 16);
  if (reserved) *reserved = false;
  if (exp == 0) {
    if (sign) {
      if (reserved) *reserved = true;
      *out = 0.0;
      return false;
    }
    *out = 0.0;
    return true;
  }
  // 1.frac * 2^(exp-129)
  double v = ldexp((double)((1u << 23) | frac), (int)exp - 152);
  *out = sign ? -v : v;
  return true;
}

bool unpack_d(uint32_t lo, uint32_t hi, double* out, bool* reserved) {
  const uint32_t sign = (lo >> 15) & 1u;
  const uint32_t exp = (lo >> 7) & 0xFFu;
  const uint64_t frac =
      ((uint64_t)(lo & 0x7Fu) << 48) | ((uint64_t)(lo >> 16) << 32) | (uint64_t)hi;
  if (reserved) *reserved = false;
  if (exp == 0) {
    if (sign) {
      if (reserved) *reserved = true;
      *out = 0.0;
      return false;
    }
    *out = 0.0;
    return true;
  }
  double v = ldexp((double)((1ULL << 55) | frac), (int)exp - 184);
  *out = sign ? -v : v;
  return true;
}

static Pack pack_fd(double v, int frac_bits, uint32_t* lo, uint32_t* hi) {
  if (!isfinite(v) || fabs(v) > 8.0e37) {
    *lo = 0;
    if (hi) *hi = 0;
    return PACK_OVF;
  }
  if (v == 0.0) {
    *lo = 0;
    if (hi) *hi = 0;
    return PACK_OK;
  }
  const uint32_t sign = (v < 0.0) ? 1u : 0u;
  int e = 0;
  double m = frexp(fabs(v), &e);  // [0.5, 1) * 2^e
  int vexp = e + 128;
  if (vexp <= 0) {
    *lo = 0;
    if (hi) *hi = 0;
    return PACK_UNF;
  }
  if (vexp >= 256) {
    *lo = 0;
    if (hi) *hi = 0;
    return PACK_OVF;
  }
  const int sig_bits = frac_bits + 1;  // hidden 1
  double scaled = ldexp(m, sig_bits);
  uint64_t raw = (uint64_t)(scaled + 0.5);
  if (raw >= (1ULL << sig_bits)) {
    raw >>= 1;
    vexp++;
    if (vexp >= 256) {
      *lo = 0;
      if (hi) *hi = 0;
      return PACK_OVF;
    }
  }
  const uint64_t frac = raw & ((1ULL << frac_bits) - 1ULL);
  if (frac_bits == 23) {
    const uint32_t hi7 = (uint32_t)(frac >> 16) & 0x7Fu;
    const uint32_t lo16 = (uint32_t)frac & 0xFFFFu;
    *lo = (sign << 15) | ((uint32_t)vexp << 7) | hi7 | (lo16 << 16);
    if (hi) *hi = 0;
  } else {
    const uint32_t hi7 = (uint32_t)(frac >> 48) & 0x7Fu;
    const uint32_t mid = (uint32_t)(frac >> 32) & 0xFFFFu;
    *lo = (sign << 15) | ((uint32_t)vexp << 7) | hi7 | (mid << 16);
    if (hi) *hi = (uint32_t)frac;
  }
  return PACK_OK;
}

Pack pack_f(double v, uint32_t* bits) {
  return pack_fd(v, 23, bits, nullptr);
}

Pack pack_d(double v, uint32_t* lo, uint32_t* hi) {
  return pack_fd(v, 55, lo, hi);
}

}  // namespace vax_fpa
