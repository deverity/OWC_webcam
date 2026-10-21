// Tests for owc/crypto.h: known-answer vectors (X25519 RFC 7748, HKDF RFC 5869,
// ChaCha20-Poly1305 RFC 8439, AES-128-GCM GCM spec / NIST CAVP), a software
// reference AES-GCM cross-check, tampering, the handshake (paired + CPace), the
// TCP and UDP record layers (replay window) and sealed UDP + FEC reassembly.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "owc/crypto.h"
#include "owc/protocol.h"

using namespace owc;

static int g_checks = 0, g_fails = 0, g_tests = 0, g_test_fails = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_fails;                                                                 \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                            \
  } while (0)

// ------------------------------------------------------------------ helpers
struct Rng {  // xorshift64*, deterministic
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
  uint64_t next() {
    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
    return s * 2685821657736338717ull;
  }
  size_t below(size_t n) { return n ? static_cast<size_t>(next() % n) : 0; }
  void fill(uint8_t* p, size_t n) { for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(next() >> 56); }
  std::vector<uint8_t> bytes(size_t n) { std::vector<uint8_t> v(n); fill(v.data(), n); return v; }
  Key32 key() { Key32 k; fill(k.data(), k.size()); return k; }
};

static std::vector<uint8_t> hexv(const char* h) {
  std::vector<uint8_t> v(std::strlen(h) / 2);
  CHECK(fromHex(std::string(h, v.size() * 2), v.data(), v.size()));
  return v;
}
static Key32 hex32(const char* h) {
  Key32 k{};
  CHECK(std::strlen(h) == 64 && fromHex(h, k.data(), k.size()));
  return k;
}
static TrafficKey makeKey(Cipher c, const std::vector<uint8_t>& key, const std::vector<uint8_t>& iv) {
  TrafficKey t;
  t.cipher = c;
  std::copy(key.begin(), key.end(), t.key.begin());
  std::copy(iv.begin(), iv.end(), t.iv.begin());
  return t;
}
static TrafficKey randKey(Rng& r, Cipher c) {
  TrafficKey t;
  t.cipher = c;
  r.fill(t.key.data(), c == Cipher::Aes128Gcm ? 16 : 32);
  r.fill(t.iv.data(), t.iv.size());
  return t;
}
static std::vector<Cipher> localCiphers() {
  std::vector<Cipher> v;
  if (localCipherMask() & cipherBit(Cipher::Aes128Gcm)) v.push_back(Cipher::Aes128Gcm);
  v.push_back(Cipher::ChaCha20Poly1305);
  return v;
}

// ------------------------------------------------- reference AES-128-GCM
// Bit-by-bit SP 800-38D + FIPS 197, tests only (computed S-box, no tables to
// mistype). Used to cross-check the hardware backend on random inputs.
namespace ref {
static uint8_t xt(uint8_t a) { return static_cast<uint8_t>((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }
static uint8_t gmul8(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
  return r;
}
static const std::array<uint8_t, 256>& sbox() {
  static std::array<uint8_t, 256> s = [] {
    std::array<uint8_t, 256> t{};
    for (int x = 0; x < 256; ++x) {
      uint8_t inv = 0;
      for (int y = 1; y < 256 && x; ++y)
        if (gmul8(static_cast<uint8_t>(x), static_cast<uint8_t>(y)) == 1) { inv = static_cast<uint8_t>(y); break; }
      uint8_t b = inv, r = inv;
      for (int i = 0; i < 4; ++i) { b = static_cast<uint8_t>((b << 1) | (b >> 7)); r ^= b; }
      t[static_cast<size_t>(x)] = static_cast<uint8_t>(r ^ 0x63);
    }
    return t;
  }();
  return s;
}
struct Aes {
  uint8_t rk[176];
  explicit Aes(const uint8_t* key) {
    const auto& S = sbox();
    std::memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 4; i < 44; ++i) {
      uint8_t t[4];
      std::memcpy(t, rk + 4 * (i - 1), 4);
      if (i % 4 == 0) {
        uint8_t u = t[0];
        t[0] = static_cast<uint8_t>(S[t[1]] ^ rcon); t[1] = S[t[2]]; t[2] = S[t[3]]; t[3] = S[u];
        rcon = xt(rcon);
      }
      for (int j = 0; j < 4; ++j) rk[4 * i + j] = static_cast<uint8_t>(rk[4 * (i - 4) + j] ^ t[j]);
    }
  }
  void enc(const uint8_t in[16], uint8_t out[16]) const {
    const auto& S = sbox();
    uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = static_cast<uint8_t>(in[i] ^ rk[i]);
    for (int r = 1; r <= 10; ++r) {
      uint8_t t[16];
      for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) t[row + 4 * c] = S[s[row + 4 * ((c + row) % 4)]];
      if (r < 10) {
        for (int c = 0; c < 4; ++c) {
          uint8_t* a = t + 4 * c;
          uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
          a[0] = static_cast<uint8_t>(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
          a[1] = static_cast<uint8_t>(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
          a[2] = static_cast<uint8_t>(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
          a[3] = static_cast<uint8_t>(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
        }
      }
      for (int i = 0; i < 16; ++i) s[i] = static_cast<uint8_t>(t[i] ^ rk[16 * r + i]);
    }
    std::memcpy(out, s, 16);
  }
};
static void ghMul(uint8_t x[16], const uint8_t h[16]) {
  uint8_t z[16] = {0}, v[16];
  std::memcpy(v, h, 16);
  for (int i = 0; i < 128; ++i) {
    if ((x[i / 8] >> (7 - i % 8)) & 1)
      for (int j = 0; j < 16; ++j) z[j] ^= v[j];
    bool lsb = v[15] & 1;
    for (int j = 15; j > 0; --j) v[j] = static_cast<uint8_t>((v[j] >> 1) | (v[j - 1] << 7));
    v[0] >>= 1;
    if (lsb) v[0] ^= 0xE1;
  }
  std::memcpy(x, z, 16);
}
static void ghash(uint8_t y[16], const uint8_t h[16], const uint8_t* p, size_t n) {
  for (size_t off = 0; off < n; off += 16) {
    for (size_t j = 0; j < 16 && off + j < n; ++j) y[j] ^= p[off + j];
    ghMul(y, h);
  }
}
// Returns ciphertext || tag.
static std::vector<uint8_t> seal(const uint8_t key[16], const uint8_t nonce[12], const std::vector<uint8_t>& aad,
                                 const std::vector<uint8_t>& pt) {
  Aes a(key);
  uint8_t h[16] = {0}, j[16];
  a.enc(h, h);
  std::memcpy(j, nonce, 12); j[12] = 0; j[13] = 0; j[14] = 0; j[15] = 1;
  std::vector<uint8_t> out(pt);
  uint8_t ctr[16];
  std::memcpy(ctr, j, 16);
  for (size_t off = 0; off < out.size(); off += 16) {
    for (int k = 15; k >= 12; --k) if (++ctr[k]) break;
    uint8_t ks[16];
    a.enc(ctr, ks);
    for (size_t i = 0; i < 16 && off + i < out.size(); ++i) out[off + i] ^= ks[i];
  }
  uint8_t y[16] = {0};
  ghash(y, h, aad.data(), aad.size());
  ghash(y, h, out.data(), out.size());
  uint8_t lb[16];
  uint64_t al = aad.size() * 8ull, cl = out.size() * 8ull;
  for (int i = 0; i < 8; ++i) { lb[i] = static_cast<uint8_t>(al >> (56 - 8 * i)); lb[8 + i] = static_cast<uint8_t>(cl >> (56 - 8 * i)); }
  ghash(y, h, lb, 16);
  uint8_t ej[16];
  a.enc(j, ej);
  for (int i = 0; i < 16; ++i) out.push_back(static_cast<uint8_t>(y[i] ^ ej[i]));
  return out;
}
}  // namespace ref

// -------------------------------------------------------------- primitives
static void testX25519() {
  {  // RFC 7748 5.2
    Key32 out;
    x25519(hex32("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
           hex32("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"), out);
    CHECK(out == hex32("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));
    x25519(hex32("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"),
           hex32("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493"), out);
    CHECK(out == hex32("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"));
    // iterated: k = u = 9; (k, u) <- (X25519(k, u), k)
    Key32 k{}, u{};
    k[0] = 9; u[0] = 9;
    for (int i = 1; i <= 1000; ++i) {
      Key32 r;
      CHECK(x25519(k, u, r));
      u = k; k = r;
      if (i == 1) CHECK(k == hex32("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"));
    }
    CHECK(k == hex32("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51"));
  }
  {  // RFC 7748 6.1
    Key32 a = hex32("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    Key32 b = hex32("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    Key32 ka = x25519Public(a), kb = x25519Public(b);
    CHECK(ka == hex32("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
    CHECK(kb == hex32("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
    Key32 s1, s2;
    CHECK(x25519(a, kb, s1) && x25519(b, ka, s2) && s1 == s2);
    CHECK(s1 == hex32("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
  }
  {  // low-order points -> all-zero output -> false
    Key32 a = hex32("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"), s;
    Key32 zero{}, one{};
    one[0] = 1;
    CHECK(!x25519(a, zero, s) && s == Key32{});
    CHECK(!x25519(a, one, s));
    // order-8 point (RFC 7748 sec. 7 / libsodium blocklist)
    CHECK(!x25519(a, hex32("e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800"), s));
  }
  {  // elligator: deterministic, ignores the 2 top bits, different inputs -> different points
    Rng r(5);
    Key32 h = r.key(), h2 = h;
    h2[31] ^= 0xC0;
    CHECK(elligatorMap(h) == elligatorMap(h) && elligatorMap(h) == elligatorMap(h2));
    Key32 h3 = h;
    h3[0] ^= 1;
    CHECK(elligatorMap(h3) != elligatorMap(h));
  }
  {  // Elligator 2 direct map: (hidden, curve point) pairs from Monocypher's
     // tests/tis-ci-vectors.h (CC0), so every platform build is checked.
    static const char* kElligator[] = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0000000000000000000000000000000000000000000000000000000000000000",
        "00000000000000000000000000000000000000000000000000000000000000c0",
        "0000000000000000000000000000000000000000000000000000000000000000",
        "673a505e107189ee54ca93310ac42e4545e9e59050aaac6f8b5f64295c8ec02f",
        "242ae39ef158ed60f20b89396d7d7eef5374aba15dc312a6aea6d1e57cacf85e",
        "922688fa428d42bc1fa8806998fbc5959ae801817e85a42a45e8ec25a0d7545a",
        "696f341266c64bcfa7afa834f8c34b2730be11c932e08474d1a22f26ed82410b",
        "0d3b0eb88b74ed13d5f6a130e03c4ad607817057dc227152827c0506a538bbba",
        "0b00df174d9fb0b6ee584d2cf05613130bad18875268c38b377e86dfefef177f",
        "01a3ea5658f4e00622eeacf724e0bd82068992fae66ed2b04a8599be16662ef5",
        "7ae4c58bc647b5646c9f5ae4c2554ccbf7c6e428e7b242a574a5a9c293c21f7e",
        "69599ab5a829c3e9515128d368da7354a8b69fcee4e34d0a668b783b6cae550f",
        "09024abaaef243e3b69366397e8dfc1fdc14a0ecc7cf497cbe4f328839acce69",
        "9172922f96d2fa41ea0daf961857056f1656ab8406db80eaeae76af58f8c9f50",
        "beab745a2a4b4e7f1a7335c3ffcdbd85139f3a72b667a01ee3e3ae0e530b3372",
        "6850a20ac5b6d2fa7af7042ad5be234d3311b9fb303753dd2b610bd566983281",
        "1287388eb2beeff706edb9cf4fcfdd35757f22541b61528570b86e8915be1530",
        "84417826c0e80af7cb25a73af1ba87594ff7048a26248b5757e52f2824e068f1",
        "51acd2e8910e7d28b4993db7e97e2b995005f26736f60dcdde94bdf8cb542251",
        "b0fbe152849f49034d2fa00ccc7b960fad7b30b6c4f9f2713eb01c147146ad31",
        "98508bb3590886af3be523b61c3d0ce6490bb8b27029878caec57e4c750f993d",
        "a0ca9ff75afae65598630b3b93560834c7f4dd29a557aa29c7becd49aeef3753",
        "3c5fad0516bb8ec53da1c16e910c23f792b971c7e2a0ee57d57c32e3655a646b",
    };
    for (size_t i = 0; i + 1 < sizeof(kElligator) / sizeof(kElligator[0]); i += 2)
      CHECK(elligatorMap(hex32(kElligator[i])) == hex32(kElligator[i + 1]));
  }
}

// Fixed-input run of the whole KEY SCHEDULE. The constants were produced by this
// implementation and independently re-derived from the owc/crypto.h text with
// Python (hashlib/hmac + an RFC 7748 X25519); every platform must reproduce them.
static bool sameKeys(const SessionKeys& a, const SessionKeys& b);
static bool g_print_kat = false;
static void katCheck(const char* name, const uint8_t* p, size_t n, const char* want) {
  const std::string got = toHex(p, n);
  if (g_print_kat) std::printf("  %s = %s\n", name, got.c_str());
  if (got != want) {
    ++g_fails;
    std::printf("  FAIL KAT %s: got %s\n", name, got.c_str());
  }
  ++g_checks;
}
static Key32 seqKey(uint8_t first) {
  Key32 k;
  for (size_t i = 0; i < k.size(); ++i) k[i] = static_cast<uint8_t>(first + i);
  return k;
}
static void testHandshakeKat() {
  const std::string hello = "OWC hello KAT", ack = "OWC ack KAT";
  const auto* hb = reinterpret_cast<const uint8_t*>(hello.data());
  const auto* ab = reinterpret_cast<const uint8_t*>(ack.data());
  Handshake pc(Handshake::Role::Pc, seqKey(0x01)), ph(Handshake::Role::Phone, seqKey(0x41));
  katCheck("pc_eph_pub", pc.ephPublic().data(), 32,
           "07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c");
  katCheck("phone_eph_pub", ph.ephPublic().data(), 32,
           "64b101b1d0be5a8704bd078f9895001fc03e8e9f9522f188dd128d9846d48466");
  CHECK(pc.begin(hb, hello.size(), ab, ack.size(), ph.ephPublic(), Cipher::ChaCha20Poly1305));
  CHECK(ph.begin(hb, hello.size(), ab, ack.size(), pc.ephPublic(), Cipher::ChaCha20Poly1305));
  // paired
  pc.usePairingKey(seqKey(0x80));
  ph.usePairingKey(seqKey(0x80));
  Key32 v = pc.pcConfirm();
  katCheck("paired.pc_confirm", v.data(), 32,
           "a0074a3f6d7c62084d14142ec806434be62470385049e13afb4ec351a56b6c46");
  v = pc.phoneConfirm();
  katCheck("paired.phone_confirm", v.data(), 32,
           "7919ae7ce78bd28119ca0626fb28d9aaed4db65990957e55514772192e37e6b3");
  SessionKeys k = pc.keys();
  CHECK(sameKeys(k, ph.keys()));
  katCheck("paired.c2p_tcp.key", k.c2p_tcp.key.data(), 32,
           "9a4adf1bdc496bcb083ae71e4e7cbd4b7830a9a5db3828978a29967fb62cb704");
  katCheck("paired.c2p_tcp.iv", k.c2p_tcp.iv.data(), 12, "74a46e77f168e0cca12e196d");
  katCheck("paired.p2c_udp.key", k.p2c_udp.key.data(), 32,
           "8d3cb50e0939580f6712899c30daac09d2cc5fe3de56a6b86b1031546b0ff2fc");
  katCheck("paired.p2c_udp.iv", k.p2c_udp.iv.data(), 12, "269b31717bce907f6e3edf47");
  // CPace with code "123456"
  Key32 yb = ph.cpaceStart("123456", seqKey(0xE0));
  Key32 ya = pc.cpaceStart("123456", seqKey(0xC0));
  katCheck("cpace.Ya", ya.data(), 32,
           "3cf612cd8087301a678fd4733de78873b6752ef6eb8adf149bfc06feff3ee235");
  katCheck("cpace.Yb", yb.data(), 32,
           "d92afc30a847529c78f7ac1f8616b310a3e569c9daf0eba39bb6ca21eece006b");
  CHECK(pc.cpaceFinish(yb) && ph.cpaceFinish(ya));
  v = pc.pcConfirm();
  katCheck("cpace.pc_confirm", v.data(), 32,
           "4a87ace29de57db6733f9dcf58d138b9ad3d68560de6064696f0efb160bea69b");
  v = ph.phoneConfirm();
  katCheck("cpace.phone_confirm", v.data(), 32,
           "1730974399b911081ae4fd8726df3b15523baf7941f29d2c56973c8bb2669cdf");
  v = pc.newPairingKey();
  katCheck("cpace.new_pairing_key", v.data(), 32,
           "72bf6746f2128a071576bc7f7d3d7fbe64a7e8837f480e91ecb8557fc4b77c64");
  k = ph.keys();
  katCheck("cpace.p2c_tcp.key", k.p2c_tcp.key.data(), 32,
           "410ee8e851ac1c37b81c0414838af045a214d1631ed976a3aa89365204a517ff");
  katCheck("cpace.c2p_udp.iv", k.c2p_udp.iv.data(), 12, "9e0bf59b4a267c108a05a297");
  // the same with AES-128-GCM: 16-byte keys, prefix of the 32-byte HKDF output
  Handshake pa(Handshake::Role::Pc, seqKey(0x01));
  CHECK(pa.begin(hb, hello.size(), ab, ack.size(), ph.ephPublic(), Cipher::Aes128Gcm));
  pa.usePairingKey(seqKey(0x80));
  SessionKeys ka = pa.keys();
  katCheck("paired.aes.c2p_tcp.key", ka.c2p_tcp.key.data(), 32,
           "9a4adf1bdc496bcb083ae71e4e7cbd4b00000000000000000000000000000000");
}

static void testHkdf() {
  struct Case { const char *ikm, *salt, *info; size_t L; const char *prk, *okm; };
  const Case cases[] = {
      {"0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "000102030405060708090a0b0c", "f0f1f2f3f4f5f6f7f8f9", 42,
       "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
       "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"},
      {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
       "303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f",
       "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f"
       "909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
       "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
       "e0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
       82, "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244",
       "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09"
       "da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87"},
      {"0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "", "", 42,
       "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04",
       "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"},
  };
  for (const Case& c : cases) {
    auto ikm = hexv(c.ikm), salt = hexv(c.salt), info = hexv(c.info);
    Key32 prk = hkdfExtract(salt.data(), salt.size(), ikm.data(), ikm.size());
    CHECK(prk == hex32(c.prk));
    std::string infoStr(info.begin(), info.end());  // the RFC infos contain no NUL byte
    std::vector<uint8_t> okm(c.L);
    hkdfExpand(prk, infoStr.c_str(), okm.data(), okm.size());
    CHECK(okm == hexv(c.okm));
    // a shorter output is a prefix
    std::vector<uint8_t> part(7);
    hkdfExpand(prk, infoStr.c_str(), part.data(), part.size());
    CHECK(std::equal(part.begin(), part.end(), okm.begin()));
  }
  // null info == empty info
  Key32 prk = hex32("19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04");
  uint8_t a[42], b[42];
  hkdfExpand(prk, nullptr, a, sizeof(a));
  hkdfExpand(prk, "", b, sizeof(b));
  CHECK(std::memcmp(a, b, sizeof(a)) == 0 && std::memcmp(a, hexv("8da4e775a563c18f715f").data(), 10) == 0);
  // secureWipe
  uint8_t w[33];
  std::memset(w, 0xAB, sizeof(w));
  secureWipe(w, sizeof(w));
  bool zero = true;
  for (uint8_t x : w) zero = zero && x == 0;
  CHECK(zero);
}

// Flips one bit of ciphertext / tag / aad / counter: open must fail each time.
static void tamperChecks(const TrafficKey& tk, Rng& r, size_t n) {
  Aead s, o;
  CHECK(s.init(tk) && o.init(tk));
  auto pt = r.bytes(n), aad = r.bytes(1 + r.below(40));
  auto ct = pt;
  uint8_t tag[16];
  const uint64_t ctr = r.next() >> 8;
  s.seal(ctr, aad.data(), aad.size(), ct.data(), ct.size(), tag);
  {
    auto x = ct;
    CHECK(o.open(ctr, aad.data(), aad.size(), x.data(), x.size(), tag) && x == pt);
  }
  if (n) {
    auto x = ct;
    x[r.below(n)] ^= static_cast<uint8_t>(1u << r.below(8));
    CHECK(!o.open(ctr, aad.data(), aad.size(), x.data(), x.size(), tag));
  }
  {
    auto x = ct;
    uint8_t t[16];
    std::memcpy(t, tag, 16);
    t[r.below(16)] ^= static_cast<uint8_t>(1u << r.below(8));
    CHECK(!o.open(ctr, aad.data(), aad.size(), x.data(), x.size(), t));
  }
  {
    auto x = ct;
    auto a = aad;
    a[r.below(a.size())] ^= static_cast<uint8_t>(1u << r.below(8));
    CHECK(!o.open(ctr, a.data(), a.size(), x.data(), x.size(), tag));
    CHECK(!o.open(ctr, aad.data(), aad.size() - 1, x.data(), x.size(), tag));  // truncated aad
  }
  {
    auto x = ct;
    CHECK(!o.open(ctr ^ (uint64_t{1} << r.below(64)), aad.data(), aad.size(), x.data(), x.size(), tag));
  }
  if (n) {
    auto x = ct;  // truncated ciphertext
    CHECK(!o.open(ctr, aad.data(), aad.size(), x.data(), x.size() - 1, tag));
  }
  {
    TrafficKey other = tk;  // other key
    other.key[0] ^= 1;
    Aead w;
    CHECK(w.init(other));
    auto x = ct;
    CHECK(!w.open(ctr, aad.data(), aad.size(), x.data(), x.size(), tag));
  }
}

static void testChaCha() {
  // RFC 8439 2.8.2: nonce = 07000000 || 4041424344454647, so iv = nonce, counter 0.
  const char* pt =
      "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
      "sunscreen would be it.";
  std::vector<uint8_t> p(pt, pt + std::strlen(pt));
  CHECK(p.size() == 114);
  auto aad = hexv("50515253c0c1c2c3c4c5c6c7");
  TrafficKey tk = makeKey(Cipher::ChaCha20Poly1305,
                          hexv("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"),
                          hexv("070000004041424344454647"));
  Aead a;
  CHECK(a.init(tk) && a.cipher() == Cipher::ChaCha20Poly1305);
  auto c = p;
  uint8_t tag[16];
  a.seal(0, aad.data(), aad.size(), c.data(), c.size(), tag);
  CHECK(c == hexv("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b"
                  "1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
                  "3ff4def08e4b7a9de576d26586cec64b6116"));
  CHECK(std::memcmp(tag, hexv("1ae10b594f09e26a7e902ecbd0600691").data(), 16) == 0);
  CHECK(a.open(0, aad.data(), aad.size(), c.data(), c.size(), tag) && c == p);
  Rng r(81);
  for (size_t n : {size_t{0}, size_t{1}, size_t{63}, size_t{64}, size_t{65}, size_t{1000}, size_t{25000}})
    tamperChecks(randKey(r, Cipher::ChaCha20Poly1305), r, n);
}

static void testAesGcm() {
  if (!hardwareAesAvailable()) {
    std::printf("  (no hardware AES on this CPU: Aes128Gcm not offered, vectors skipped)\n");
    CHECK(!(localCipherMask() & cipherBit(Cipher::Aes128Gcm)));
    Aead a;
    TrafficKey tk;
    tk.cipher = Cipher::Aes128Gcm;
    CHECK(!a.init(tk));
    return;
  }
  struct V { const char *key, *nonce, *in, *ad, *ct, *tag; };
  static const V vecs[] = {
#include "aes_gcm_vectors.inc"
  };
  int n = 0;
  for (const V& v : vecs) {
    auto key = hexv(v.key), nonce = hexv(v.nonce), in = hexv(v.in), ad = hexv(v.ad), ct = hexv(v.ct),
         tag = hexv(v.tag);
    TrafficKey tk = makeKey(Cipher::Aes128Gcm, key, nonce);
    Aead a;
    CHECK(a.init(tk));
    auto x = in;
    uint8_t t[16];
    a.seal(0, ad.data(), ad.size(), x.data(), x.size(), t);
    CHECK(x == ct);
    CHECK(std::memcmp(t, tag.data(), 16) == 0);
    CHECK(a.open(0, ad.data(), ad.size(), x.data(), x.size(), tag.data()) && x == in);
    // the reference implementation agrees with the vectors too
    auto rs = ref::seal(key.data(), nonce.data(), ad, in);
    CHECK(rs.size() == ct.size() + 16 && std::equal(ct.begin(), ct.end(), rs.begin()) &&
          std::equal(tag.begin(), tag.end(), rs.begin() + static_cast<std::ptrdiff_t>(ct.size())));
    ++n;
  }
  CHECK(n == 69);
  // Random lengths 0..300 (+ some large ones): hardware == reference, open(seal(x)) == x.
  Rng r(4242);
  std::vector<size_t> lens;
  for (size_t i = 0; i <= 300; ++i) lens.push_back(i);
  for (size_t big : {size_t{1023}, size_t{1024}, size_t{1025}, size_t{4096 + 5}, size_t{25000}, size_t{65536 + 129}})
    lens.push_back(big);
  for (size_t len : lens) {
    TrafficKey tk = randKey(r, Cipher::Aes128Gcm);
    Aead a;
    CHECK(a.init(tk));
    auto pt = r.bytes(len), aad = r.bytes(r.below(3) == 0 ? r.below(300) : r.below(48));
    const uint64_t ctr = r.next();
    uint8_t nonce[12];
    std::memcpy(nonce, tk.iv.data(), 12);
    for (int i = 0; i < 8; ++i) nonce[4 + i] = static_cast<uint8_t>(nonce[4 + i] ^ (ctr >> (56 - 8 * i)));
    auto x = pt;
    uint8_t t[16];
    a.seal(ctr, aad.data(), aad.size(), x.data(), x.size(), t);
    auto rs = ref::seal(tk.key.data(), nonce, aad, pt);
    CHECK(std::equal(x.begin(), x.end(), rs.begin()) && std::memcmp(t, rs.data() + len, 16) == 0);
    CHECK(a.open(ctr, aad.data(), aad.size(), x.data(), x.size(), t) && x == pt);
  }
  for (size_t n2 : {size_t{0}, size_t{1}, size_t{15}, size_t{16}, size_t{127}, size_t{128}, size_t{129}, size_t{25000}})
    tamperChecks(randKey(r, Cipher::Aes128Gcm), r, n2);
}

static void testAeadMisc() {
  Rng r(9);
  for (Cipher c : localCiphers()) {
    // nonce = iv XOR (0^4 || BE64(counter)): counter c with iv equals counter 0 with iv'
    TrafficKey tk = randKey(r, c);
    const uint64_t ctr = 0x0102030405060708ull;
    TrafficKey tk2 = tk;
    for (int i = 0; i < 8; ++i) tk2.iv[4 + i] = static_cast<uint8_t>(tk2.iv[4 + i] ^ (ctr >> (56 - 8 * i)));
    Aead a, b;
    CHECK(a.init(tk) && b.init(tk2) && a.cipher() == c);
    auto p = r.bytes(100), q = p;
    uint8_t t1[16], t2[16];
    a.seal(ctr, p.data(), 5, p.data() + 5, 95, t1);
    b.seal(0, q.data(), 5, q.data() + 5, 95, t2);
    CHECK(p == q && std::memcmp(t1, t2, 16) == 0);
    // re-init with another key replaces the old one
    TrafficKey tk3 = randKey(r, c);
    CHECK(a.init(tk3));
    auto z = r.bytes(10), z0 = z;
    a.seal(1, nullptr, 0, z.data(), z.size(), t1);
    Aead d;
    CHECK(d.init(tk3) && d.open(1, nullptr, 0, z.data(), z.size(), t1) && z == z0);
    // empty everything
    a.seal(2, nullptr, 0, nullptr, 0, t1);
    CHECK(d.open(2, nullptr, 0, nullptr, 0, t1));
    t1[3] ^= 1;
    CHECK(!d.open(2, nullptr, 0, nullptr, 0, t1));
  }
  // None / uninitialised
  Aead n;
  TrafficKey none;
  CHECK(!n.init(none) && n.cipher() == Cipher::None);
  uint8_t tag[16], buf[4] = {1, 2, 3, 4};
  n.seal(0, nullptr, 0, buf, 4, tag);
  CHECK(buf[0] == 0 && buf[3] == 0);  // an uninitialised Aead destroys, never leaks
  CHECK(!n.open(0, nullptr, 0, buf, 4, tag));
  TrafficKey bogus;
  bogus.cipher = static_cast<Cipher>(9);
  CHECK(!n.init(bogus));
}

static void testCipherSelection() {
  const uint32_t aes = cipherBit(Cipher::Aes128Gcm), cc = cipherBit(Cipher::ChaCha20Poly1305),
                 none = cipherBit(Cipher::None);
  static_assert(cipherBit(Cipher::Aes128Gcm) == 2 && cipherBit(Cipher::ChaCha20Poly1305) == 4 &&
                    cipherBit(Cipher::None) == 1,
                "wire values of the cipher mask");
  Cipher c = Cipher::None;
  CHECK(chooseCipher(aes | cc | none, aes | cc | none, c) && c == Cipher::Aes128Gcm);
  CHECK(chooseCipher(aes | cc, cc | none, c) && c == Cipher::ChaCha20Poly1305);
  CHECK(chooseCipher(cc, aes | cc | none, c) && c == Cipher::ChaCha20Poly1305);
  CHECK(chooseCipher(none, aes | cc | none, c) && c == Cipher::None);
  c = Cipher::ChaCha20Poly1305;
  CHECK(!chooseCipher(aes, cc | none, c) && c == Cipher::ChaCha20Poly1305);
  CHECK(!chooseCipher(0, aes | cc | none, c));
  CHECK(!chooseCipher(0xF0, 0xF0, c));  // unknown bits are never chosen
  const uint32_t local = localCipherMask();
  CHECK((local & none) && (local & cc) && ((local & aes) != 0) == hardwareAesAvailable());
  CHECK(std::strcmp(toString(Cipher::Aes128Gcm), "AES-128-GCM") == 0);
  CHECK(std::strcmp(toString(Cipher::ChaCha20Poly1305), "ChaCha20-Poly1305") == 0);
  CHECK(std::strcmp(toString(Cipher::None), "None") == 0);
  CHECK(std::strcmp(toString(static_cast<Cipher>(7)), "Unknown") == 0);
  std::printf("  local ciphers: mask=0x%x hardware AES=%s\n", local, hardwareAesAvailable() ? "yes" : "no");
}

// ---------------------------------------------------------------- handshake
struct Peers {
  std::unique_ptr<Handshake> pc, ph;
  std::vector<uint8_t> hello, ack;
};

static Peers makePeers(Rng& r, Cipher c, const std::string& client = "pc") {
  Peers p;
  p.pc.reset(new Handshake(Handshake::Role::Pc, r.key()));
  p.ph.reset(new Handshake(Handshake::Role::Phone, r.key()));
  Hello h;
  h.client_name = client;
  r.fill(h.client_id.data(), h.client_id.size());
  h.eph_pub = p.pc->ephPublic();
  h.cipher_mask = localCipherMask() & ~cipherBit(Cipher::None);
  HelloAck a;
  a.status = HelloStatus::AuthRequired;
  a.session_id = 1234;
  a.device_name = "phone";
  r.fill(a.phone_id.data(), a.phone_id.size());
  a.eph_pub = p.ph->ephPublic();
  a.cipher = static_cast<uint8_t>(c);
  p.hello = encode(h);
  p.ack = encode(a);
  // each side takes the peer key from the decoded message, as the integration does
  Hello hd;
  HelloAck ad;
  CHECK(decode(p.hello.data(), p.hello.size(), hd) && decode(p.ack.data(), p.ack.size(), ad));
  CHECK(p.pc->begin(p.hello.data(), p.hello.size(), p.ack.data(), p.ack.size(), ad.eph_pub, c));
  CHECK(p.ph->begin(p.hello.data(), p.hello.size(), p.ack.data(), p.ack.size(), hd.eph_pub, c));
  return p;
}

static bool sameTk(const TrafficKey& a, const TrafficKey& b) {
  return a.cipher == b.cipher && a.key == b.key && a.iv == b.iv;
}
static bool sameKeys(const SessionKeys& a, const SessionKeys& b) {
  return a.cipher == b.cipher && sameTk(a.c2p_tcp, b.c2p_tcp) && sameTk(a.p2c_tcp, b.p2c_tcp) &&
         sameTk(a.c2p_udp, b.c2p_udp) && sameTk(a.p2c_udp, b.p2c_udp);
}
static bool keysDistinct(const SessionKeys& k) {
  const TrafficKey* t[4] = {&k.c2p_tcp, &k.p2c_tcp, &k.c2p_udp, &k.p2c_udp};
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j)
      if (t[i]->key == t[j]->key || t[i]->iv == t[j]->iv) return false;
  return true;
}

static void testHandshake() {
  Rng r(31337);
  for (Cipher c : {Cipher::ChaCha20Poly1305, Cipher::Aes128Gcm, Cipher::None}) {
    {  // paired path
      Peers p = makePeers(r, c);
      CHECK(!p.pc->ready() && p.pc->keys().cipher == Cipher::None && p.pc->pcConfirm() == Key32{});
      Key32 pk = r.key();
      p.pc->usePairingKey(pk);
      p.ph->usePairingKey(pk);
      CHECK(p.pc->ready() && p.ph->ready());
      CHECK(p.pc->pcConfirm() == p.ph->pcConfirm() && p.pc->phoneConfirm() == p.ph->phoneConfirm());
      CHECK(p.pc->pcConfirm() != p.pc->phoneConfirm() && p.pc->pcConfirm() != Key32{});
      CHECK(p.pc->newPairingKey() == Key32{});
      SessionKeys a = p.pc->keys(), b = p.ph->keys();
      CHECK(sameKeys(a, b) && a.cipher == c);
      if (c != Cipher::None) {
        CHECK(keysDistinct(a));
        CHECK(a.c2p_tcp.cipher == c && a.p2c_udp.cipher == c);
        if (c == Cipher::Aes128Gcm) {
          bool tailZero = true;
          for (size_t i = 16; i < 32; ++i) tailZero = tailZero && a.c2p_tcp.key[i] == 0;
          CHECK(tailZero);
        }
      } else {
        CHECK(a.c2p_tcp.key == (std::array<uint8_t, 32>{}));
      }
      // wrong pairing key on one side: nothing matches
      Peers q = makePeers(r, c);
      Key32 other = pk;
      other[5] ^= 0x40;
      q.pc->usePairingKey(pk);
      q.ph->usePairingKey(other);
      CHECK(q.pc->pcConfirm() != q.ph->pcConfirm() && q.pc->phoneConfirm() != q.ph->phoneConfirm());
      if (c != Cipher::None) CHECK(!sameKeys(q.pc->keys(), q.ph->keys()));
      // same pairing key, new connection: different keys (fresh ephemerals)
      Peers s = makePeers(r, c);
      s.pc->usePairingKey(pk);
      s.ph->usePairingKey(pk);
      CHECK(s.pc->pcConfirm() == s.ph->pcConfirm() && s.pc->pcConfirm() != p.pc->pcConfirm());
    }
    {  // CPace, right code
      Peers p = makePeers(r, c);
      const std::string code = "042917";
      Key32 yb = p.ph->cpaceStart(code, r.key());  // phone: AuthResult{CodeShown, Yb}
      Key32 ya = p.pc->cpaceStart(code, r.key());  // PC after the user typed the code
      CHECK(ya != yb && ya != Key32{} && yb != Key32{});
      CHECK(p.pc->cpaceFinish(yb) && p.ph->cpaceFinish(ya));
      CHECK(p.pc->ready() && p.ph->ready());
      CHECK(p.pc->pcConfirm() == p.ph->pcConfirm() && p.pc->phoneConfirm() == p.ph->phoneConfirm());
      CHECK(p.pc->newPairingKey() == p.ph->newPairingKey() && p.pc->newPairingKey() != Key32{});
      CHECK(sameKeys(p.pc->keys(), p.ph->keys()));
      if (c != Cipher::None) CHECK(keysDistinct(p.pc->keys()));
      // the new pairing key works on the paired path of a later connection
      const Key32 stored = p.pc->newPairingKey();
      Peers n = makePeers(r, c);
      n.pc->usePairingKey(stored);
      n.ph->usePairingKey(p.ph->newPairingKey());
      CHECK(n.pc->pcConfirm() == n.ph->pcConfirm());
      // a second finish without a new start is refused
      CHECK(!p.pc->cpaceFinish(yb) && !p.pc->ready());
    }
    {  // CPace, wrong code, then a retry with fresh shares
      Peers p = makePeers(r, c);
      Key32 yb = p.ph->cpaceStart("111111", r.key());
      Key32 ya = p.pc->cpaceStart("111112", r.key());
      CHECK(p.pc->cpaceFinish(yb) && p.ph->cpaceFinish(ya));  // shares are valid points...
      CHECK(p.pc->pcConfirm() != p.ph->pcConfirm());          // ...but nothing matches
      CHECK(p.pc->phoneConfirm() != p.ph->phoneConfirm());
      CHECK(p.pc->newPairingKey() != p.ph->newPairingKey());
      if (c != Cipher::None) CHECK(!sameKeys(p.pc->keys(), p.ph->keys()));
      // phone: AuthResult{WrongCode, fresh Yb}; PC retries with the right code
      Key32 yb2 = p.ph->cpaceStart("111111", r.key());
      CHECK(!p.ph->ready() && yb2 != yb);
      Key32 ya2 = p.pc->cpaceStart("111111", r.key());
      CHECK(p.pc->cpaceFinish(yb2) && p.ph->cpaceFinish(ya2));
      CHECK(p.pc->pcConfirm() == p.ph->pcConfirm() && p.pc->newPairingKey() == p.ph->newPairingKey());
      CHECK(sameKeys(p.pc->keys(), p.ph->keys()));
      // same code, same randomness, but another connection: different shares and keys
      Peers q = makePeers(r, c);
      Rng r1(77), r2(77);
      Key32 qa = q.pc->cpaceStart("111111", r1.key());
      Peers q2 = makePeers(r, c);
      Key32 qa2 = q2.pc->cpaceStart("111111", r2.key());
      CHECK(qa != qa2);  // G depends on the transcript
    }
    {  // invalid shares / peers
      Peers p = makePeers(r, c);
      p.pc->cpaceStart("123456", r.key());
      CHECK(!p.pc->cpaceFinish(Key32{}) && !p.pc->ready());
      Key32 one{};
      one[0] = 1;
      p.pc->cpaceStart("123456", r.key());
      CHECK(!p.pc->cpaceFinish(one));
      Handshake h(Handshake::Role::Pc, r.key());
      CHECK(!h.cpaceFinish(r.key()));  // no start
      CHECK(!h.begin(p.hello.data(), p.hello.size(), p.ack.data(), p.ack.size(), Key32{}, c));
      CHECK(!h.ready());
      h.usePairingKey(r.key());  // ignored before a successful begin
      CHECK(!h.ready() && h.pcConfirm() == Key32{});
    }
    {  // different transcripts -> different keys
      Rng r1(500), r2(500);
      Peers a = makePeers(r1, c, "pc-A");
      Peers b = makePeers(r2, c, "pc-B");  // same ephemerals, different Hello bytes
      CHECK(a.pc->ephPublic() == b.pc->ephPublic());
      Key32 pk{};
      pk[0] = 1;
      a.pc->usePairingKey(pk);
      b.pc->usePairingKey(pk);
      CHECK(a.pc->pcConfirm() != b.pc->pcConfirm());
      // the two ends saw different Hello bytes (tampering): confirmations differ
      Handshake pc(Handshake::Role::Pc, r.key()), ph(Handshake::Role::Phone, r.key());
      const uint8_t h1[] = {1, 2, 3}, h2[] = {1, 2, 4}, ack[] = {9, 9};
      CHECK(pc.begin(h1, sizeof(h1), ack, sizeof(ack), ph.ephPublic(), c));
      CHECK(ph.begin(h2, sizeof(h2), ack, sizeof(ack), pc.ephPublic(), c));
      pc.usePairingKey(pk);
      ph.usePairingKey(pk);
      CHECK(pc.pcConfirm() != ph.pcConfirm() && pc.phoneConfirm() != ph.phoneConfirm());
      CHECK(ph.begin(h1, sizeof(h1), ack, sizeof(ack), pc.ephPublic(), c));  // same bytes: match
      ph.usePairingKey(pk);
      pc.usePairingKey(pk);
      CHECK(pc.pcConfirm() == ph.pcConfirm() && sameKeys(pc.keys(), ph.keys()));
    }
  }
  {  // invalid cipher value
    Handshake h(Handshake::Role::Pc, r.key());
    Key32 peer = x25519Public(r.key());
    CHECK(!h.begin(nullptr, 0, nullptr, 0, peer, static_cast<Cipher>(3)));
    CHECK(h.begin(nullptr, 0, nullptr, 0, peer, Cipher::ChaCha20Poly1305));
  }
}

// ---------------------------------------------------------------------- TCP
static void testTcpRecords() {
  Rng r(2024);
  for (Cipher c : localCiphers()) {
    Peers p = makePeers(r, c);
    Key32 pk = r.key();
    p.pc->usePairingKey(pk);
    p.ph->usePairingKey(pk);
    SessionKeys kp = p.pc->keys(), kh = p.ph->keys();
    TcpSealer phoneTx;
    TcpOpener pcRx;
    CHECK(phoneTx.init(kh.p2c_tcp) && pcRx.init(kp.p2c_tcp));
    // a stream of messages, including seal2 (VideoFrame header + AU) and empty payloads
    struct Msg { MsgType t; std::vector<uint8_t> a, b; };
    std::vector<Msg> msgs;
    msgs.push_back({MsgType::Capabilities, r.bytes(700), {}});
    msgs.push_back({MsgType::Pong, r.bytes(16), {}});
    msgs.push_back({MsgType::Bye, {}, {}});
    msgs.push_back({MsgType::VideoFrame, r.bytes(kVideoFrameHeaderSize), r.bytes(25000)});
    msgs.push_back({MsgType::VideoFrame, r.bytes(kVideoFrameHeaderSize), r.bytes(200000)});
    msgs.push_back({MsgType::Log, r.bytes(1), {}});
    std::vector<uint8_t> wire;
    for (const Msg& m : msgs) {
      if (m.b.empty()) phoneTx.seal(m.t, m.a.data(), m.a.size(), wire);
      else phoneTx.seal2(m.t, m.a.data(), m.a.size(), m.b.data(), m.b.size(), wire);
    }
    CHECK(phoneTx.counter() == msgs.size());
    TcpFrameParser parser;
    size_t off = 0, got = 0;
    TcpFrameParser::Message sealed, inner;
    while (off < wire.size()) {
      size_t n = std::min(wire.size() - off, 1 + r.below(70000));
      CHECK(parser.feed(wire.data() + off, n));
      off += n;
      while (parser.next(sealed)) {
        CHECK(sealed.type == MsgType::Sealed);
        CHECK(pcRx.open(sealed, inner));
        CHECK(got < msgs.size());
        if (got < msgs.size()) {
          std::vector<uint8_t> want = msgs[got].a;
          want.insert(want.end(), msgs[got].b.begin(), msgs[got].b.end());
          CHECK(inner.type == msgs[got].t && inner.payload == want);
        }
        ++got;
      }
    }
    CHECK(got == msgs.size() && pcRx.counter() == msgs.size());
    // wire format: header{Sealed, 0, 2+n+16}, then ciphertext of u16 inner type
    {
      TcpSealer s;
      CHECK(s.init(kh.p2c_tcp));
      std::vector<uint8_t> w = {0xEE};  // seal appends
      auto pl = r.bytes(10);
      s.seal(MsgType::Stats, pl.data(), pl.size(), w);
      CHECK(w.size() == 1 + 8 + 2 + 10 + 16 && w[0] == 0xEE);
      CHECK(w[1] == 60 && w[2] == 0 && w[3] == 0 && w[4] == 0 && w[5] == 28 && w[6] == 0 && w[7] == 0 && w[8] == 0);
      CHECK(!std::equal(pl.begin(), pl.end(), w.begin() + 11));  // encrypted
    }
    // seal2 == seal of the concatenation (same key, same counter)
    {
      TcpSealer s1, s2;
      CHECK(s1.init(kh.p2c_tcp) && s2.init(kh.p2c_tcp));
      auto a = r.bytes(17), b = r.bytes(3000), ab = a;
      ab.insert(ab.end(), b.begin(), b.end());
      std::vector<uint8_t> w1, w2;
      s1.seal2(MsgType::VideoFrame, a.data(), a.size(), b.data(), b.size(), w1);
      s2.seal(MsgType::VideoFrame, ab.data(), ab.size(), w2);
      CHECK(w1 == w2);
      std::vector<uint8_t> w3;  // empty second part
      s1.seal2(MsgType::Ping, a.data(), a.size(), nullptr, 0, w3);
      std::vector<uint8_t> w4;
      s2.seal(MsgType::Ping, a.data(), a.size(), w4);
      CHECK(w3 == w4);
    }
    // out of order, replay, tamper, wrong direction, plain message
    auto record = [&](TcpSealer& s, MsgType t, size_t n) {
      std::vector<uint8_t> w;
      auto pl = r.bytes(n);
      s.seal(t, pl.data(), pl.size(), w);
      TcpFrameParser ps;
      TcpFrameParser::Message m;
      ps.feed(w.data(), w.size());
      ps.next(m);
      return m;
    };
    {
      TcpSealer s;
      TcpOpener o;
      CHECK(s.init(kh.p2c_tcp) && o.init(kp.p2c_tcp));
      auto m0 = record(s, MsgType::Stats, 50), m1 = record(s, MsgType::Stats, 50);
      TcpFrameParser::Message out;
      auto m1c = m1;
      CHECK(!o.open(m1c, out));  // record 1 before record 0
      auto m0c = m0;
      CHECK(o.open(m0c, out) && out.type == MsgType::Stats);
      m0c = m0;
      CHECK(!o.open(m0c, out));  // replay of record 0
      m1c = m1;
      m1c.payload[r.below(m1c.payload.size())] ^= 4;
      CHECK(!o.open(m1c, out));  // tampered
      m1c = m1;
      CHECK(o.open(m1c, m1c) && m1c.type == MsgType::Stats && m1c.payload.size() == 50);  // in place
      TcpFrameParser::Message plain;
      plain.type = MsgType::Ping;
      plain.payload = r.bytes(8);
      CHECK(!o.open(plain, out));
      TcpFrameParser::Message tiny;
      tiny.type = MsgType::Sealed;
      tiny.payload = r.bytes(17);
      CHECK(!o.open(tiny, out));
      // a phone->PC record does not open with the PC->phone key
      TcpOpener wrong;
      CHECK(wrong.init(kp.c2p_tcp));
      auto m2 = record(s, MsgType::Stats, 10);
      CHECK(!wrong.open(m2, out));
      // nested Sealed is refused
      TcpSealer s2;
      TcpOpener o2;
      CHECK(s2.init(kh.p2c_tcp) && o2.init(kp.p2c_tcp));
      auto nest = record(s2, MsgType::Sealed, 30);
      CHECK(!o2.open(nest, out));
    }
    // PC -> phone direction
    {
      TcpSealer pcTx;
      TcpOpener phoneRx;
      CHECK(pcTx.init(kp.c2p_tcp) && phoneRx.init(kh.c2p_tcp));
      for (int i = 0; i < 20; ++i) {
        auto m = record(pcTx, MsgType::SetParam, r.below(40));
        TcpFrameParser::Message out;
        CHECK(phoneRx.open(m, out) && out.type == MsgType::SetParam);
      }
    }
  }
  TcpSealer s;
  TrafficKey none;
  CHECK(!s.init(none));
}

// ---------------------------------------------------------------------- UDP
static std::vector<uint8_t> sealedDatagram(UdpSealer& s, const UdpHeader& h, const std::vector<uint8_t>& pl) {
  std::vector<uint8_t> b(kUdpMaxDatagram);
  writeUdpHeader(h, b.data());
  std::copy(pl.begin(), pl.end(), b.begin() + kUdpHeaderSize);
  size_t n = s.seal(b.data(), kUdpHeaderSize + pl.size(), b.size());
  b.resize(n);
  return b;
}

static void testUdpRecords() {
  Rng r(77);
  for (Cipher c : localCiphers()) {
    Peers p = makePeers(r, c);
    Key32 pk = r.key();
    p.pc->usePairingKey(pk);
    p.ph->usePairingKey(pk);
    SessionKeys kp = p.pc->keys(), kh = p.ph->keys();
    UdpSealer tx;
    UdpOpener rx;
    CHECK(tx.init(kh.p2c_udp) && rx.init(kp.p2c_udp));
    UdpHeader h;
    h.type = UdpType::Video;
    h.session_id = 99;
    h.frame_seq = 5;
    h.frag_count = 1;
    {  // round trip + layout
      auto pl = r.bytes(kUdpMaxPayloadSealed);
      h.frame_size = static_cast<uint32_t>(pl.size());
      auto d = sealedDatagram(tx, h, pl);
      CHECK(d.size() == kUdpMaxDatagram);  // max sealed payload fills the datagram exactly
      CHECK(d[2] == kUdpVersionSealed && d[32] == 0 && d[33] == 0);  // counter 0, LE
      UdpHeader tmp;
      CHECK(!readUdpHeader(d.data(), d.size(), tmp));  // sealed datagrams are not v1
      size_t n = 0;
      CHECK(rx.open(d.data(), d.size(), n) && n == kUdpHeaderSize + pl.size());
      UdpHeader o;
      CHECK(readUdpHeader(d.data(), n, o) && o.session_id == 99 && o.frame_seq == 5 && o.frame_size == pl.size());
      CHECK(std::equal(pl.begin(), pl.end(), d.begin() + kUdpHeaderSize));
    }
    {  // too big / too small / no room
      std::vector<uint8_t> b(kUdpMaxDatagram + 100);
      writeUdpHeader(h, b.data());
      CHECK(tx.seal(b.data(), kUdpHeaderSize + kUdpMaxPayloadSealed + 1, b.size()) == 0);
      CHECK(tx.seal(b.data(), kUdpHeaderSize - 1, b.size()) == 0);
      CHECK(tx.seal(b.data(), kUdpHeaderSize + 10, kUdpHeaderSize + 10 + kUdpSealOverhead - 1) == 0);
      CHECK(tx.seal(b.data(), kUdpHeaderSize, kUdpHeaderSize + kUdpSealOverhead) == kUdpHeaderSize + kUdpSealOverhead);
    }
    {  // punch: empty payload
      UdpSealer pcTx;
      UdpOpener phoneRx;
      CHECK(pcTx.init(kp.c2p_udp) && phoneRx.init(kh.c2p_udp));
      UdpHeader ph;
      ph.type = UdpType::Punch;
      ph.session_id = 99;
      auto d = sealedDatagram(pcTx, ph, {});
      CHECK(d.size() == kUdpHeaderSize + kUdpSealOverhead);
      size_t n = 0;
      CHECK(phoneRx.open(d.data(), d.size(), n) && n == kUdpHeaderSize);
      UdpHeader o;
      CHECK(readUdpHeader(d.data(), n, o) && o.type == UdpType::Punch && o.session_id == 99);
      // a punch from someone without the key
      UdpSealer evil;
      TrafficKey ek = randKey(r, c);
      CHECK(evil.init(ek));
      auto e = sealedDatagram(evil, ph, {});
      CHECK(!phoneRx.open(e.data(), e.size(), n));
      // a plain v1 punch
      std::vector<uint8_t> v1(kUdpHeaderSize);
      writeUdpHeader(ph, v1.data());
      CHECK(!phoneRx.open(v1.data(), v1.size(), n));
      CHECK(phoneRx.rejected() == 2);
    }
    {  // replay window
      UdpSealer s;
      UdpOpener o;
      CHECK(s.init(kh.p2c_udp) && o.init(kp.p2c_udp));
      std::vector<std::vector<uint8_t>> d;
      for (int i = 0; i < 3000; ++i) d.push_back(sealedDatagram(s, h, r.bytes(r.below(64))));
      auto tryOpen = [&](size_t i) {
        auto b = d[i];
        size_t n = 0;
        return o.open(b.data(), b.size(), n);
      };
      CHECK(tryOpen(10));
      CHECK(!tryOpen(10));          // duplicate
      CHECK(tryOpen(5));            // older, within window
      CHECK(!tryOpen(5));
      CHECK(tryOpen(11) && tryOpen(9) && tryOpen(0));
      CHECK(tryOpen(1033));         // highest = 1033: window is (9, 1033]
      CHECK(!tryOpen(9));           // 1033 - 9 = 1024 -> too old
      CHECK(tryOpen(10 + 1) == false);  // seen before the jump, still inside the window
      CHECK(tryOpen(12));           // 1033 - 12 = 1021: inside, unseen
      CHECK(tryOpen(1032) && !tryOpen(1033) && !tryOpen(1032));
      CHECK(tryOpen(2000));         // window (976, 2000]
      CHECK(!tryOpen(1033) && !tryOpen(976));  // seen; 2000 - 976 = 1024: too old
      CHECK(tryOpen(977) && tryOpen(1500) && !tryOpen(1500));
      // reorder inside the window: everything in (2999-1024, 2999] opens once in any order
      std::vector<size_t> idx;
      for (size_t i = 2001; i < 3000; ++i) idx.push_back(i);
      for (size_t i = idx.size(); i > 1; --i) std::swap(idx[i - 1], idx[r.below(i)]);
      bool all = true;
      for (size_t i : idx) all = tryOpen(i) && all;
      CHECK(all);
      bool none = true;
      for (size_t i : idx) none = !tryOpen(i) && none;
      CHECK(none);
      CHECK(!tryOpen(1975));                // 2999 - 1975 = 1024: too old
      CHECK(tryOpen(1976) && !tryOpen(1976));  // 1023 behind, never seen: accepted once
      CHECK(!tryOpen(2000) && !tryOpen(1500));
    }
    {  // a jump larger than the window forgets everything older
      UdpSealer s;
      UdpOpener o;
      CHECK(s.init(kh.p2c_udp) && o.init(kp.p2c_udp));
      std::vector<std::vector<uint8_t>> d;
      for (int i = 0; i < 2600; ++i) d.push_back(sealedDatagram(s, h, r.bytes(3)));
      auto tryOpen = [&](size_t i) {
        auto b = d[i];
        size_t n = 0;
        return o.open(b.data(), b.size(), n);
      };
      CHECK(tryOpen(1500) && tryOpen(1499) && tryOpen(2599));
      CHECK(!tryOpen(1499) && !tryOpen(1575) && tryOpen(1576) && !tryOpen(2599));
      CHECK(tryOpen(2598) && tryOpen(1600));
    }
    {  // a failed authentication does not move the window
      UdpSealer s;
      UdpOpener o;
      CHECK(s.init(kh.p2c_udp) && o.init(kp.p2c_udp));
      auto d0 = sealedDatagram(s, h, r.bytes(30));
      std::vector<std::vector<uint8_t>> later;
      for (int i = 0; i < 2000; ++i) later.push_back(sealedDatagram(s, h, r.bytes(5)));
      auto forged = later.back();
      forged[forged.size() - 1] ^= 1;  // counter 2000 but bad tag
      size_t n = 0;
      CHECK(!o.open(forged.data(), forged.size(), n));
      CHECK(o.open(d0.data(), d0.size(), n));  // counter 0 still acceptable
      auto hdr = later[0];
      hdr[8] ^= 1;  // header byte (frame_seq) is authenticated
      CHECK(!o.open(hdr.data(), hdr.size(), n));
      auto ctrb = later[1];
      ctrb[kUdpHeaderSize] ^= 1;  // counter is authenticated
      CHECK(!o.open(ctrb.data(), ctrb.size(), n));
      auto shortb = later[2];
      CHECK(!o.open(shortb.data(), kUdpHeaderSize + kUdpSealOverhead - 1, n));
      CHECK(o.rejected() == 4);
    }
  }
  UdpSealer s;
  UdpOpener o;
  TrafficKey none;
  CHECK(!s.init(none) && !o.init(none));
  std::vector<uint8_t> b(100);
  CHECK(s.seal(b.data(), 40, 100) == 0);
}

// Sender fragments with kUdpMaxPayloadSealed, seals, some datagrams are lost and
// the rest arrive shuffled; FEC must rebuild every frame from the opened plaintext.
static void testSealedFec() {
  Rng r(1999);
  for (Cipher c : localCiphers()) {
    Peers p = makePeers(r, c);
    p.pc->cpaceStart("000000", r.key());
    Key32 ya = p.pc->cpaceStart("314159", r.key());
    Key32 yb = p.ph->cpaceStart("314159", r.key());
    CHECK(p.pc->cpaceFinish(yb) && p.ph->cpaceFinish(ya));
    SessionKeys kp = p.pc->keys(), kh = p.ph->keys();
    UdpSealer tx;
    UdpOpener rx;
    CHECK(tx.init(kh.p2c_udp) && rx.init(kp.p2c_udp));
    FrameAssembler fa(4);
    const size_t sizes[] = {1, 100, kUdpMaxPayloadSealed - 1, kUdpMaxPayloadSealed, kUdpMaxPayloadSealed + 1,
                            5000, 25000, 25000, 200000, 77777, 3 * kUdpMaxPayloadSealed};
    uint32_t seq = 1;
    uint64_t recovered = 0;
    for (size_t round = 0; round < 3; ++round) {
      for (size_t sz : sizes) {
        auto au = r.bytes(sz);
        UdpHeader b;
        b.session_id = 7;
        b.frame_seq = seq++;
        b.epoch = 3;
        b.flags = kFrameKey;  // every frame decodable on its own
        b.capture_ts_us = 1000 + seq;
        std::vector<std::vector<uint8_t>> data, par;
        auto sink = [&](std::vector<std::vector<uint8_t>>& into) {
          return [&into, &tx](const uint8_t* d, size_t len) {
            std::vector<uint8_t> buf(kUdpMaxDatagram);
            std::memcpy(buf.data(), d, len);
            size_t n = tx.seal(buf.data(), len, buf.size());
            CHECK(n == len + kUdpSealOverhead && n <= kUdpMaxDatagram);
            buf.resize(n);
            into.push_back(std::move(buf));
          };
        };
        size_t count = packetize(b, au.data(), au.size(), sink(data), kUdpMaxPayloadSealed);
        CHECK(count == (sz + kUdpMaxPayloadSealed - 1) / kUdpMaxPayloadSealed);
        uint8_t M = fecParityCount(static_cast<uint16_t>(count), 25);
        size_t np = packetizeFec(b, au.data(), au.size(), M, sink(par), kUdpMaxPayloadSealed);
        CHECK(np == M);
        // lose up to parity_count data fragments per FEC block (always recoverable)
        FecLayout lay = fecLayout(static_cast<uint16_t>(count), M);
        std::vector<char> keep(count, 1);
        for (unsigned bl = 0; bl < lay.nblocks; ++bl) {
          const FecBlock& B = lay.block[bl];
          unsigned drop = static_cast<unsigned>(r.below(B.parity_count + 1u));
          for (unsigned i = 0; i < drop; ++i) keep[B.data_first + r.below(B.data_count)] = 0;
        }
        std::vector<std::vector<uint8_t>*> order;
        for (size_t i = 0; i < count; ++i) if (keep[i]) order.push_back(&data[i]);
        for (auto& d : par) order.push_back(&d);
        for (size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[r.below(i)]);
        bool delivered = false;
        for (auto* d : order) {
          size_t n = 0;
          CHECK(rx.open(d->data(), d->size(), n));
          UdpHeader h;
          CHECK(readUdpHeader(d->data(), n, h));
          FrameAssembler::Frame f;
          if (fa.push(h, d->data() + kUdpHeaderSize, n - kUdpHeaderSize, f)) {
            CHECK(!delivered && f.data == au && f.frame_seq == b.frame_seq);
            delivered = true;
          }
        }
        CHECK(delivered);
      }
    }
    recovered = fa.counters().fragments_recovered;
    CHECK(recovered > 0 && fa.counters().frames_lost == 0);
    std::printf("  %s: %llu frames, %llu fragments rebuilt by FEC\n", toString(c),
                static_cast<unsigned long long>(fa.counters().frames_complete),
                static_cast<unsigned long long>(recovered));
  }
}

static void run(const char* name, void (*fn)()) {
  int before = g_fails;
  ++g_tests;
  fn();
  bool ok = g_fails == before;
  if (!ok) ++g_test_fails;
  std::printf("[%s] %s\n", ok ? " OK " : "FAIL", name);
}

int main(int argc, char** argv) {
  g_print_kat = argc > 1 && std::strcmp(argv[1], "--print-kat") == 0;
  run("cipher selection", testCipherSelection);
  run("x25519 (RFC 7748) + elligator", testX25519);
  run("hkdf-sha256 (RFC 5869) + wipe", testHkdf);
  run("chacha20-poly1305 (RFC 8439)", testChaCha);
  run("aes-128-gcm (NIST/McGrew-Viega + reference)", testAesGcm);
  run("aead misc", testAeadMisc);
  run("handshake", testHandshake);
  run("handshake known answers", testHandshakeKat);
  run("tcp records", testTcpRecords);
  run("udp records + replay window", testUdpRecords);
  run("sealed udp + fec", testSealedFec);
  std::printf("%d tests, %d failed; %d checks, %d failed\n", g_tests, g_test_fails, g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
