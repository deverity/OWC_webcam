// AES-128-GCM with the ARMv8 Cryptography Extensions (AESE/AESMC + PMULL), for
// AArch64 and for AArch32 on ARMv8 cores (armeabi-v7a builds). Pipelined CTR +
// aggregated Karatsuba GHASH (gcm_impl.inc.h): 8 blocks per batch on AArch64
// (32 vector registers), 4 on AArch32 (16 q registers).
//
// protocol/CMakeLists.txt compiles this file alone with the crypto ISA flags
// (arm64: -march=armv8-a+crypto; armv7: -march=armv8-a -mfpu=crypto-neon-fp-armv8);
// without them the backend is compiled out. Everything here runs only after
// cpuHasArmAesPmull(), and the TU includes no C++ standard header (aes_gcm.h).
#include "aes_gcm.h"

#if (defined(__aarch64__) || defined(__arm__)) && !defined(_MSC_VER) && \
    !defined(__ARM_BIG_ENDIAN) && (defined(__ARM_FEATURE_AES) || defined(__ARM_FEATURE_CRYPTO))
#define OWC_HAVE_ARMCE 1
#endif

#if defined(OWC_HAVE_ARMCE)
#include <arm_neon.h>
#include <string.h>

namespace owc {
void secureWipe(void* p, size_t n);  // crypto.cpp (declared here: no C++ headers in this TU)

namespace aesgcm {
namespace {

#if defined(_MSC_VER) && !defined(__clang__)
#define OWC_INLINE __forceinline
#else
#define OWC_INLINE inline __attribute__((always_inline))
#endif

typedef uint8x16_t V;
#if defined(__aarch64__)
const unsigned kW = 8;
#else
const unsigned kW = 4;
#endif

OWC_INLINE uint64x2_t u64(V a) { return vreinterpretq_u64_u8(a); }
OWC_INLINE V u8(uint64x2_t a) { return vreinterpretq_u8_u64(a); }

OWC_INLINE V ld(const uint8_t* p) { return vld1q_u8(p); }
OWC_INLINE void st(uint8_t* p, V v) { vst1q_u8(p, v); }
OWC_INLINE V vx(V a, V b) { return veorq_u8(a, b); }
OWC_INLINE V vor(V a, V b) { return vorrq_u8(a, b); }
inline V vzero() { return vdupq_n_u8(0); }
OWC_INLINE V rev(V a) {
  const V r = vrev64q_u8(a);
  return vextq_u8(r, r, 8);
}
template <int n> OWC_INLINE V sl64(V a) { return u8(vshlq_n_u64(u64(a), n)); }
template <int n> OWC_INLINE V sr64(V a) { return u8(vshrq_n_u64(u64(a), n)); }
OWC_INLINE V upLane(V a) { return vextq_u8(vzero(), a, 8); }
OWC_INLINE V downLane(V a) { return vextq_u8(a, vzero(), 8); }
OWC_INLINE V swapLanes(V a) { return vextq_u8(a, a, 8); }

#if defined(__aarch64__)
OWC_INLINE V clLo(V a, V b) {
  return vreinterpretq_u8_p128(vmull_p64(static_cast<poly64_t>(vgetq_lane_u64(u64(a), 0)),
                                         static_cast<poly64_t>(vgetq_lane_u64(u64(b), 0))));
}
OWC_INLINE V clHi(V a, V b) {
  return vreinterpretq_u8_p128(vmull_high_p64(vreinterpretq_p64_u8(a), vreinterpretq_p64_u8(b)));
}
#else
// AArch32: arm_neon.h has no vmull_p64 there, so VMULL.P64 (Qd = Dn x Dm) directly.
OWC_INLINE V pmull(uint64x1_t a, uint64x1_t b) {
  uint64x2_t r;
  __asm__("vmull.p64 %q0, %P1, %P2" : "=w"(r) : "w"(a), "w"(b));
  return u8(r);
}
OWC_INLINE V clLo(V a, V b) { return pmull(vget_low_u64(u64(a)), vget_low_u64(u64(b))); }
OWC_INLINE V clHi(V a, V b) { return pmull(vget_high_u64(u64(a)), vget_high_u64(u64(b))); }
#endif

// Counter kept with each 32-bit word byte-swapped: lane 3 is the inc32 word.
struct Ctr {
  uint32x4_t le;
};
OWC_INLINE uint32x4_t inc(unsigned i) { return vsetq_lane_u32(i, vdupq_n_u32(0), 3); }
inline Ctr ctrFromJ0(const uint8_t j0[16]) {
  Ctr c;
  c.le = vreinterpretq_u32_u8(vrev32q_u8(ld(j0)));
  return c;
}
OWC_INLINE V ctrBlock(const Ctr& c, unsigned i) {
  return vrev32q_u8(vreinterpretq_u8_u32(vaddq_u32(c.le, inc(i))));
}
OWC_INLINE void ctrAdd(Ctr& c, unsigned k) { c.le = vaddq_u32(c.le, inc(k)); }

struct Rk {
  V k[11];
};
OWC_INLINE void loadRk(Rk& r, const uint8_t* p) {
  for (unsigned i = 0; i < 11; ++i) r.k[i] = ld(p + 16 * i);
}
// AESE = AddRoundKey + SubBytes + ShiftRows; AESMC = MixColumns.
OWC_INLINE V aes1(const Rk& r, V s) {
  for (unsigned i = 0; i < 9; ++i) s = vaesmcq_u8(vaeseq_u8(s, r.k[i]));
  return veorq_u8(vaeseq_u8(s, r.k[9]), r.k[10]);
}
OWC_INLINE void aesW(const Rk& r, V s[kW]) {
  for (unsigned i = 0; i < 9; ++i) {
    const V k = r.k[i];
    for (unsigned b = 0; b < kW; ++b) s[b] = vaesmcq_u8(vaeseq_u8(s[b], k));
  }
  for (unsigned b = 0; b < kW; ++b) s[b] = veorq_u8(vaeseq_u8(s[b], r.k[9]), r.k[10]);
}

// SubWord through AESE with a zero key: with the word in every column,
// ShiftRows is a no-op, so lane 0 is SubBytes(word). Constant time.
inline uint32_t subWord(uint32_t w) {
  const V s = vaeseq_u8(vreinterpretq_u8_u32(vdupq_n_u32(w)), vzero());
  return vgetq_lane_u32(vreinterpretq_u32_u8(s), 0);
}
void expandKey(uint8_t out[176], const uint8_t key[16]) {
  static const uint8_t kRcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};
  uint32_t w[44];
  memcpy(w, key, 16);  // words in memory byte order (little-endian lanes)
  for (unsigned i = 4; i < 44; ++i) {
    uint32_t t = w[i - 1];
    if (i % 4 == 0) {
      t = subWord(t);
      t = ((t >> 8) | (t << 24)) ^ kRcon[i / 4 - 1];  // RotWord on bytes, then Rcon
    }
    w[i] = w[i - 4] ^ t;
  }
  memcpy(out, w, 176);
  secureWipe(w, sizeof(w));
}

#include "gcm_impl.inc.h"

const Backend kBackend = {"ARMv8-CE AES+PMULL", gcmInit, gcmSeal, gcmOpen};

}  // namespace

const Backend* armBackend() { return &kBackend; }

}  // namespace aesgcm
}  // namespace owc

#else

namespace owc {
namespace aesgcm {
const Backend* armBackend() { return nullptr; }
}  // namespace aesgcm
}  // namespace owc

#endif
