// AES-128-GCM with x86 AES-NI + PCLMULQDQ (+ SSSE3 PSHUFB). 8 blocks per batch:
// pipelined CTR and aggregated Karatsuba GHASH (gcm_impl.inc.h). Runs only after
// cpuHasX86AesClmul(). MSVC accepts the intrinsics without flags; GCC/Clang get
// -maes -mpclmul -mssse3 for this file only (protocol/CMakeLists.txt).
// Keep this TU free of C++ standard headers (see aes_gcm.h).
#include "aes_gcm.h"

#if (defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))) || \
    ((defined(__x86_64__) || defined(__i386__)) && defined(__AES__) && defined(__PCLMUL__) && \
     defined(__SSSE3__))
#define OWC_HAVE_AESNI 1
#endif

#if defined(OWC_HAVE_AESNI)
#include <emmintrin.h>
#include <string.h>
#include <tmmintrin.h>
#include <wmmintrin.h>

namespace owc {
namespace aesgcm {
namespace {

#if defined(_MSC_VER) && !defined(__clang__)
#define OWC_INLINE __forceinline
#else
#define OWC_INLINE inline __attribute__((always_inline))
#endif

typedef __m128i V;
const unsigned kW = 8;

OWC_INLINE V ld(const uint8_t* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); }
OWC_INLINE void st(uint8_t* p, V v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v); }
OWC_INLINE V vx(V a, V b) { return _mm_xor_si128(a, b); }
OWC_INLINE V vor(V a, V b) { return _mm_or_si128(a, b); }
inline V vzero() { return _mm_setzero_si128(); }
OWC_INLINE V rev(V a) {
  return _mm_shuffle_epi8(a, _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
}
template <int n> OWC_INLINE V sl64(V a) { return _mm_slli_epi64(a, n); }
template <int n> OWC_INLINE V sr64(V a) { return _mm_srli_epi64(a, n); }
OWC_INLINE V upLane(V a) { return _mm_slli_si128(a, 8); }
OWC_INLINE V downLane(V a) { return _mm_srli_si128(a, 8); }
OWC_INLINE V swapLanes(V a) { return _mm_shuffle_epi32(a, 0x4E); }
OWC_INLINE V clLo(V a, V b) { return _mm_clmulepi64_si128(a, b, 0x00); }
OWC_INLINE V clHi(V a, V b) { return _mm_clmulepi64_si128(a, b, 0x11); }

// Counter kept byte-reversed: the low 32-bit lane is the big-endian inc32 word.
struct Ctr {
  V r;
};
inline Ctr ctrFromJ0(const uint8_t j0[16]) { Ctr c; c.r = rev(ld(j0)); return c; }
OWC_INLINE V ctrBlock(const Ctr& c, unsigned i) {
  return rev(_mm_add_epi32(c.r, _mm_cvtsi32_si128(static_cast<int>(i))));
}
OWC_INLINE void ctrAdd(Ctr& c, unsigned k) { c.r = _mm_add_epi32(c.r, _mm_cvtsi32_si128(static_cast<int>(k))); }

struct Rk {
  V k[11];
};
OWC_INLINE void loadRk(Rk& r, const uint8_t* p) {
  for (unsigned i = 0; i < 11; ++i) r.k[i] = ld(p + 16 * i);
}
OWC_INLINE V aes1(const Rk& r, V s) {
  s = vx(s, r.k[0]);
  for (unsigned i = 1; i < 10; ++i) s = _mm_aesenc_si128(s, r.k[i]);
  return _mm_aesenclast_si128(s, r.k[10]);
}
OWC_INLINE void aesW(const Rk& r, V s[kW]) {
  for (unsigned b = 0; b < kW; ++b) s[b] = vx(s[b], r.k[0]);
  for (unsigned i = 1; i < 10; ++i) {
    const V k = r.k[i];
    for (unsigned b = 0; b < kW; ++b) s[b] = _mm_aesenc_si128(s[b], k);
  }
  for (unsigned b = 0; b < kW; ++b) s[b] = _mm_aesenclast_si128(s[b], r.k[10]);
}

inline V expandStep(V k, V g) {
  g = _mm_shuffle_epi32(g, 0xFF);
  k = vx(k, _mm_slli_si128(k, 4));
  k = vx(k, _mm_slli_si128(k, 4));
  k = vx(k, _mm_slli_si128(k, 4));
  return vx(k, g);
}
void expandKey(uint8_t out[176], const uint8_t key[16]) {
  V k[11];
  k[0] = ld(key);
#define OWC_EXPAND(i, rcon) k[i] = expandStep(k[i - 1], _mm_aeskeygenassist_si128(k[i - 1], rcon))
  OWC_EXPAND(1, 0x01); OWC_EXPAND(2, 0x02); OWC_EXPAND(3, 0x04); OWC_EXPAND(4, 0x08);
  OWC_EXPAND(5, 0x10); OWC_EXPAND(6, 0x20); OWC_EXPAND(7, 0x40); OWC_EXPAND(8, 0x80);
  OWC_EXPAND(9, 0x1B); OWC_EXPAND(10, 0x36);
#undef OWC_EXPAND
  for (unsigned i = 0; i < 11; ++i) st(out + 16 * i, k[i]);
}

#include "gcm_impl.inc.h"

const Backend kBackend = {"AES-NI+PCLMUL", gcmInit, gcmSeal, gcmOpen};

}  // namespace

const Backend* x86Backend() { return &kBackend; }

}  // namespace aesgcm
}  // namespace owc

#else

namespace owc {
namespace aesgcm {
const Backend* x86Backend() { return nullptr; }
}  // namespace aesgcm
}  // namespace owc

#endif
