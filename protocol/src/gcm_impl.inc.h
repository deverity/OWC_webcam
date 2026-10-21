// AES-128-GCM (NIST SP 800-38D, 96-bit IV) written once against a tiny set of
// ISA primitives. Included INSIDE an anonymous namespace by aes_gcm_x86.cpp
// and aes_gcm_arm.cpp after they define:
//
//   V                      128-bit vector
//   kW                     blocks per batch (4 or 8; <= 8)
//   ld(p) st(p,v) vx(a,b) vor(a,b) vzero()
//   rev(v)                 full byte reversal (block <-> GHASH domain)
//   sl64<n>(v) sr64<n>(v)  shift each 64-bit lane
//   upLane(v)              {0, v.lo}   (128-bit shift left by 64)
//   downLane(v)            {v.hi, 0}   (128-bit shift right by 64)
//   swapLanes(v)           {v.hi, v.lo}
//   clLo(a,b) clHi(a,b)    64x64 -> 128 carry-less multiply of the low / high lanes
//   Ctr ctrFromJ0(j0) V ctrBlock(c,i) ctrAdd(c,k)   inc32 counter blocks
//   Rk loadRk(rk,bytes) aes1(rk,v) aesW(rk,V s[kW]) expandKey(bytes176,key16)
//   OWC_INLINE             force-inline marker for the per-block helpers
//
// GHASH representation: every block is byte-reversed, so the 128-bit lane value
// holds the field element with x^0 at bit 127 ("reflected"). A carry-less
// product of two such values is the reflected 255-bit product; shifting it left
// by one gives the reflected 256-bit product, which kreduce() folds modulo
// x^128 + x^7 + x^2 + x + 1 (the shift/xor reduction of Gueron & Kounavis,
// written with 64-bit lanes). Products are accumulated unreduced (Karatsuba,
// 3 multiplies per block) and reduced once per batch: aggregated reduction
// with precomputed H^1..H^kW.

struct Kara {
  V lo, hi, mid;
};

OWC_INLINE V karaHelper(V h) { return vx(h, swapLanes(h)); }

template <bool kFirst>
OWC_INLINE void kmul(Kara& a, V x, V h, V hk) {
  const V lo = clLo(x, h);
  const V hi = clHi(x, h);
  const V mid = clLo(karaHelper(x), hk);
  if (kFirst) {
    a.lo = lo; a.hi = hi; a.mid = mid;
  } else {
    a.lo = vx(a.lo, lo); a.hi = vx(a.hi, hi); a.mid = vx(a.mid, mid);
  }
}

OWC_INLINE V kreduce(const Kara& a) {
  // Karatsuba recombination into the 256-bit product [hi:lo].
  const V mid = vx(a.mid, vx(a.lo, a.hi));
  V lo = vx(a.lo, upLane(mid));
  V hi = vx(a.hi, downLane(mid));
  // Reflected 255-bit -> 256-bit: shift [hi:lo] left by one.
  const V loC = sr64<63>(lo), hiC = sr64<63>(hi);
  lo = vor(sl64<1>(lo), upLane(loC));
  hi = vor(vor(sl64<1>(hi), upLane(hiC)), downLane(loC));
  // lo holds the coefficients x^128..x^255 (reflected). result = hi ^ lo*(1+x+x^2+x^7):
  // first fold the bits that the right shifts below push past x^127, then shift.
  const V t = vx(vx(sl64<63>(lo), sl64<62>(lo)), sl64<57>(lo));
  const V d = vx(lo, upLane(t));
  const V u = vx(vx(sl64<63>(d), sl64<62>(d)), sl64<57>(d));
  const V m = vx(vx(vx(d, sr64<1>(d)), vx(sr64<2>(d), sr64<7>(d))), downLane(u));
  return vx(hi, m);
}

OWC_INLINE V gfmul(V x, V h) {
  Kara a;
  kmul<true>(a, x, h, karaHelper(h));
  return kreduce(a);
}

// acc = (acc ^ b0)*H^k ^ b1*H^(k-1) ^ ... ^ b(k-1)*H, blocks as stored (not reversed).
// ghashN: k known at compile time (the batch path, fully unrolled); ghashK: tails.
template <unsigned k>
OWC_INLINE V ghashN(V acc, const uint8_t* p, const Key& K) {
  Kara a;
  kmul<true>(a, vx(rev(ld(p)), acc), ld(K.h + 16 * (k - 1)), ld(K.hk + 16 * (k - 1)));
  for (unsigned i = 1; i < k; ++i)
    kmul<false>(a, rev(ld(p + 16 * i)), ld(K.h + 16 * (k - 1 - i)), ld(K.hk + 16 * (k - 1 - i)));
  return kreduce(a);
}

inline V ghashK(V acc, const uint8_t* p, unsigned k, const Key& K) {
  Kara a;
  kmul<true>(a, vx(rev(ld(p)), acc), ld(K.h + 16 * (k - 1)), ld(K.hk + 16 * (k - 1)));
  for (unsigned i = 1; i < k; ++i)
    kmul<false>(a, rev(ld(p + 16 * i)), ld(K.h + 16 * (k - 1 - i)), ld(K.hk + 16 * (k - 1 - i)));
  return kreduce(a);
}

inline V ghashBytes(V acc, const uint8_t* p, size_t n, const Key& K) {
  while (n >= 16 * kW) {
    acc = ghashN<kW>(acc, p, K);
    p += 16 * kW;
    n -= 16 * kW;
  }
  if (n) {
    uint8_t buf[16 * kW];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, p, n);
    acc = ghashK(acc, buf, static_cast<unsigned>((n + 15) / 16), K);
  }
  return acc;
}

inline void putBe64(uint8_t* o, uint64_t v) {
  for (int i = 7; i >= 0; --i) { o[i] = static_cast<uint8_t>(v & 0xFF); v >>= 8; }
}

inline V finishTag(V acc, size_t aad_len, size_t n, const Key& K) {
  uint8_t lb[16];
  putBe64(lb, static_cast<uint64_t>(aad_len) * 8);
  putBe64(lb + 8, static_cast<uint64_t>(n) * 8);
  return rev(ghashK(acc, lb, 1, K));
}

void gcmInit(Key& K, const uint8_t key[16]) {
  expandKey(K.rk, key);
  Rk rk;
  loadRk(rk, K.rk);
  uint8_t zero[16];
  memset(zero, 0, sizeof(zero));
  const V h = rev(aes1(rk, ld(zero)));
  V p = h;
  for (unsigned i = 0; i < 8; ++i) {
    st(K.h + 16 * i, p);
    st(K.hk + 16 * i, karaHelper(p));
    p = gfmul(p, h);
  }
}

inline Ctr startCtr(const uint8_t nonce[12], const Rk& rk, V& ekj0) {
  uint8_t j0[16];
  memcpy(j0, nonce, 12);
  j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
  Ctr c = ctrFromJ0(j0);
  ekj0 = aes1(rk, ctrBlock(c, 0));
  ctrAdd(c, 1);  // data starts at inc32(J0)
  return c;
}

OWC_INLINE void keystreamW(const Rk& rk, Ctr& c, V s[kW]) {
  for (unsigned i = 0; i < kW; ++i) s[i] = ctrBlock(c, i);
  ctrAdd(c, kW);
  aesW(rk, s);
}

void gcmSeal(const Key& K, const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
             uint8_t* p, size_t n, uint8_t tag[16]) {
  Rk rk;
  loadRk(rk, K.rk);
  V ekj0;
  Ctr c = startCtr(nonce, rk, ekj0);
  V acc = ghashBytes(vzero(), aad, aad_len, K);
  const size_t total = n;
  // GHASH of batch i is issued after the AES of batch i+1 so the two
  // independent chains overlap in the pipeline.
  const uint8_t* pending = nullptr;
  while (n >= 16 * kW) {
    V s[kW];
    keystreamW(rk, c, s);
    for (unsigned i = 0; i < kW; ++i) st(p + 16 * i, vx(ld(p + 16 * i), s[i]));
    if (pending) acc = ghashN<kW>(acc, pending, K);
    pending = p;
    p += 16 * kW;
    n -= 16 * kW;
  }
  if (pending) acc = ghashN<kW>(acc, pending, K);
  if (n) {
    uint8_t buf[16 * kW];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, p, n);
    V s[kW];
    keystreamW(rk, c, s);
    const unsigned nb = static_cast<unsigned>((n + 15) / 16);
    for (unsigned i = 0; i < nb; ++i) st(buf + 16 * i, vx(ld(buf + 16 * i), s[i]));
    memcpy(p, buf, n);
    memset(buf + n, 0, 16 * nb - n);  // GHASH pads the ciphertext with zeros
    acc = ghashK(acc, buf, nb, K);
  }
  st(tag, vx(finishTag(acc, aad_len, total, K), ekj0));
}

void gcmOpen(const Key& K, const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
             uint8_t* p, size_t n, uint8_t tag[16]) {
  Rk rk;
  loadRk(rk, K.rk);
  V ekj0;
  Ctr c = startCtr(nonce, rk, ekj0);
  V acc = ghashBytes(vzero(), aad, aad_len, K);
  const size_t total = n;
  while (n >= 16 * kW) {
    acc = ghashN<kW>(acc, p, K);  // independent of the AES below: they overlap
    V s[kW];
    keystreamW(rk, c, s);
    for (unsigned i = 0; i < kW; ++i) st(p + 16 * i, vx(ld(p + 16 * i), s[i]));
    p += 16 * kW;
    n -= 16 * kW;
  }
  if (n) {
    uint8_t buf[16 * kW];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, p, n);
    const unsigned nb = static_cast<unsigned>((n + 15) / 16);
    acc = ghashK(acc, buf, nb, K);
    V s[kW];
    keystreamW(rk, c, s);
    for (unsigned i = 0; i < nb; ++i) st(buf + 16 * i, vx(ld(buf + 16 * i), s[i]));
    memcpy(p, buf, n);
  }
  st(tag, vx(finishTag(acc, aad_len, total, K), ekj0));
}
