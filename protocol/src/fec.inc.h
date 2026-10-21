// Reed-Solomon FEC (GF(2^8), systematic Cauchy). Included INSIDE namespace owc by
// protocol.cpp (kept in one translation unit so consumers only compile protocol.cpp).
// Original implementation. Multiplication by a constant is done with 8 "xtime planes"
// of each source shard (word-wise, endian independent) instead of byte table lookups.

namespace fecimpl {

struct Tables {
  uint8_t exp[512];
  uint8_t log[256];
  uint8_t inv[256];
  Tables() {
    unsigned x = 1;
    for (unsigned i = 0; i < 255; ++i) {
      exp[i] = static_cast<uint8_t>(x);
      log[x] = static_cast<uint8_t>(i);
      x <<= 1;
      if (x & 0x100u) x ^= 0x11Du;
    }
    for (unsigned i = 255; i < 512; ++i) exp[i] = exp[i - 255];
    log[0] = 0;
    inv[0] = 0;
    for (unsigned a = 1; a < 256; ++a) inv[a] = exp[255 - log[a]];
  }
};
inline const Tables& tab() {
  static const Tables t;
  return t;
}
inline uint8_t gmul(uint8_t a, uint8_t b) {
  if (!a || !b) return 0;
  const Tables& t = tab();
  return t.exp[t.log[a] + t.log[b]];
}
// Cauchy coefficient for parity j, data i in a block with m parity shards.
inline uint8_t coef(size_t j, size_t i, size_t m) {
  return tab().inv[(j ^ (m + i)) & 0xFFu];
}

inline uint64_t xtime64(uint64_t x) {
  return ((x & 0x7F7F7F7F7F7F7F7Full) << 1) ^ (((x >> 7) & 0x0101010101010101ull) * 0x1Dull);
}
// pl[0..W) holds the source; fills pl[b*W..] with source * 2^b.
inline void buildPlanes(uint64_t* pl, size_t W) {
  for (unsigned b = 1; b < 8; ++b) {
    const uint64_t* a = pl + (b - 1) * W;
    uint64_t* d = pl + b * W;
    for (size_t w = 0; w < W; ++w) d[w] = xtime64(a[w]);
  }
}
// dst ^= c * source
inline void accum(uint64_t* dst, const uint64_t* pl, size_t W, uint8_t c) {
  for (unsigned b = 0; b < 8; ++b) {
    if (!((c >> b) & 1u)) continue;
    const uint64_t* p = pl + b * W;
    for (size_t w = 0; w < W; ++w) dst[w] ^= p[w];
  }
}

// parity: m*W zeroed words. load(i, dst) fills W words (zero padded).
template <class Load>
void encodeBlock(size_t k, size_t m, size_t W, Load load, uint64_t* parity) {
  std::vector<uint64_t> pl(8 * W);
  for (size_t i = 0; i < k; ++i) {
    load(i, pl.data());
    buildPlanes(pl.data(), W);
    for (size_t j = 0; j < m; ++j) accum(parity + j * W, pl.data(), W, coef(j, i, m));
  }
}

// In-place Gauss-Jordan inverse of an e x e matrix over GF(256). Returns false if singular.
inline bool invert(std::vector<uint8_t>& a, size_t e) {
  const Tables& t = tab();
  std::vector<uint8_t> inv(e * e, 0);
  for (size_t i = 0; i < e; ++i) inv[i * e + i] = 1;
  for (size_t c = 0; c < e; ++c) {
    size_t piv = c;
    while (piv < e && a[piv * e + c] == 0) ++piv;
    if (piv == e) return false;
    if (piv != c)
      for (size_t x = 0; x < e; ++x) {
        std::swap(a[piv * e + x], a[c * e + x]);
        std::swap(inv[piv * e + x], inv[c * e + x]);
      }
    uint8_t s = t.inv[a[c * e + c]];
    for (size_t x = 0; x < e; ++x) {
      a[c * e + x] = gmul(a[c * e + x], s);
      inv[c * e + x] = gmul(inv[c * e + x], s);
    }
    for (size_t r = 0; r < e; ++r) {
      uint8_t f = a[r * e + c];
      if (r == c || f == 0) continue;
      for (size_t x = 0; x < e; ++x) {
        a[r * e + x] = static_cast<uint8_t>(a[r * e + x] ^ gmul(f, a[c * e + x]));
        inv[r * e + x] = static_cast<uint8_t>(inv[r * e + x] ^ gmul(f, inv[c * e + x]));
      }
    }
  }
  a.swap(inv);
  return true;
}

// Reconstructs the missing data shards of one block. have_data[i]/have_par[j] flag
// availability. ld(i,dst)/lp(j,dst) load a shard as W words; st(i,words) stores a
// recovered data shard. Returns false if fewer than k shards are available.
template <class LD, class LP, class ST>
bool recoverBlock(size_t k, size_t m, size_t W, const std::vector<uint8_t>& have_data,
                  const std::vector<uint8_t>& have_par, LD ld, LP lp, ST st) {
  std::vector<size_t> missing, chosen;
  for (size_t i = 0; i < k; ++i)
    if (!have_data[i]) missing.push_back(i);
  const size_t e = missing.size();
  if (e == 0) return true;
  for (size_t j = 0; j < m && chosen.size() < e; ++j)
    if (have_par[j]) chosen.push_back(j);
  if (chosen.size() < e) return false;

  std::vector<uint64_t> R(e * W), pl(8 * W), out(e * W, 0);
  for (size_t r = 0; r < e; ++r) lp(chosen[r], R.data() + r * W);
  for (size_t i = 0; i < k; ++i) {
    if (!have_data[i]) continue;
    ld(i, pl.data());
    buildPlanes(pl.data(), W);
    for (size_t r = 0; r < e; ++r) accum(R.data() + r * W, pl.data(), W, coef(chosen[r], i, m));
  }
  std::vector<uint8_t> A(e * e);
  for (size_t r = 0; r < e; ++r)
    for (size_t u = 0; u < e; ++u) A[r * e + u] = coef(chosen[r], missing[u], m);
  if (!invert(A, e)) return false;
  for (size_t r = 0; r < e; ++r) {
    std::memcpy(pl.data(), R.data() + r * W, W * sizeof(uint64_t));
    buildPlanes(pl.data(), W);
    for (size_t u = 0; u < e; ++u) accum(out.data() + u * W, pl.data(), W, A[u * e + r]);
  }
  for (size_t u = 0; u < e; ++u) st(missing[u], out.data() + u * W);
  return true;
}

}  // namespace fecimpl

FecLayout fecLayout(uint16_t frag_count, uint8_t parity_total) {
  FecLayout L;
  if (frag_count == 0 || parity_total == 0) return L;
  for (unsigned n = 1; n <= kFecMaxBlocks && n <= parity_total && n <= frag_count; ++n) {
    unsigned kb = (frag_count + n - 1) / n, mb = (parity_total + n - 1) / n;
    if (kb + mb > 255) continue;
    unsigned dq = frag_count / n, dr = frag_count % n, pq = parity_total / n, pr = parity_total % n;
    unsigned df = 0, pf = 0;
    for (unsigned b = 0; b < n; ++b) {
      unsigned dc = dq + (b < dr ? 1u : 0u), pc = pq + (b < pr ? 1u : 0u);
      L.block[b].data_first = static_cast<uint16_t>(df);
      L.block[b].data_count = static_cast<uint16_t>(dc);
      L.block[b].parity_first = static_cast<uint16_t>(pf);
      L.block[b].parity_count = static_cast<uint16_t>(pc);
      df += dc;
      pf += pc;
    }
    L.nblocks = static_cast<uint8_t>(n);
    return L;
  }
  return L;
}

uint8_t fecParityCount(uint16_t frag_count, int percent) {
  if (percent <= 0 || frag_count == 0) return 0;
  uint64_t m = (static_cast<uint64_t>(frag_count) * static_cast<uint64_t>(percent) + 99u) / 100u;
  if (m < 2) m = 2;
  if (m > 255) m = 255;
  return static_cast<uint8_t>(m);
}

size_t fecShardSize(size_t au_len, size_t max_payload) {
  if (au_len == 0) return 0;
  if (max_payload == 0) max_payload = 1;
  if (max_payload > kUdpMaxPayload) max_payload = kUdpMaxPayload;
  return au_len <= max_payload ? au_len : max_payload;
}

bool fecEncode(const uint8_t* au, size_t au_len, uint8_t parity_total, std::vector<uint8_t>& parity,
               size_t max_payload) {
  parity.clear();
  if (au_len == 0 || parity_total == 0 || !au) return false;
  if (max_payload == 0) max_payload = 1;
  if (max_payload > kUdpMaxPayload) max_payload = kUdpMaxPayload;
  if (au_len > kMaxTcpPayload) return false;
  const size_t S = fecShardSize(au_len, max_payload);
  const size_t count = au_len <= max_payload ? 1 : (au_len + max_payload - 1) / max_payload;
  if (count > 0xFFFFu) return false;
  FecLayout L = fecLayout(static_cast<uint16_t>(count), parity_total);
  if (L.nblocks == 0) return false;
  const size_t W = (S + 7) / 8;
  parity.assign(static_cast<size_t>(parity_total) * S, 0);
  std::vector<uint64_t> pw;
  for (unsigned b = 0; b < L.nblocks; ++b) {
    const FecBlock& B = L.block[b];
    pw.assign(static_cast<size_t>(B.parity_count) * W, 0);
    fecimpl::encodeBlock(B.data_count, B.parity_count, W,
                         [&](size_t i, uint64_t* dst) {
                           size_t g = B.data_first + i;
                           size_t off = g * S;
                           size_t n = std::min(S, au_len - off);
                           std::memset(dst, 0, W * sizeof(uint64_t));
                           std::memcpy(dst, au + off, n);
                         },
                         pw.data());
    for (size_t j = 0; j < B.parity_count; ++j)
      std::memcpy(parity.data() + (B.parity_first + j) * S, pw.data() + j * W, S);
  }
  return true;
}
