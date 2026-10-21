// OWC v3 session crypto: see owc/crypto.h for the design (KEY SCHEDULE, TCP
// RECORDS, UDP DATAGRAMS). Portable; the hardware AES-GCM backends and the CPU
// probe live in aes_gcm_*.cpp / cpu_features.cpp.
#include "owc/crypto.h"

#include <algorithm>
#include <cstring>

#include "aes_gcm.h"
#include "monocypher.h"

namespace owc {

// ------------------------------------------------------------------ ciphers
const char* toString(Cipher c) {
  switch (c) {
    case Cipher::None: return "None";
    case Cipher::Aes128Gcm: return "AES-128-GCM";
    case Cipher::ChaCha20Poly1305: return "ChaCha20-Poly1305";
  }
  return "Unknown";
}

namespace {
const aesgcm::Backend* probeBackend() {
  // Check the CPU first: a backend TU is built with extended ISA flags.
  if (aesgcm::cpuHasX86AesClmul()) {
    if (const aesgcm::Backend* b = aesgcm::x86Backend()) return b;
  }
  if (aesgcm::cpuHasArmAesPmull()) {
    if (const aesgcm::Backend* b = aesgcm::armBackend()) return b;
  }
  return nullptr;
}
const aesgcm::Backend* hwBackend() {
  static const aesgcm::Backend* const b = probeBackend();
  return b;
}
}  // namespace

bool hardwareAesAvailable() { return hwBackend() != nullptr; }

uint32_t localCipherMask() {
  uint32_t m = cipherBit(Cipher::None) | cipherBit(Cipher::ChaCha20Poly1305);
  if (hardwareAesAvailable()) m |= cipherBit(Cipher::Aes128Gcm);
  return m;
}

bool chooseCipher(uint32_t pc_mask, uint32_t phone_mask, Cipher& out) {
  const uint32_t both = pc_mask & phone_mask;
  for (Cipher c : {Cipher::Aes128Gcm, Cipher::ChaCha20Poly1305, Cipher::None}) {
    if (both & cipherBit(c)) {
      out = c;
      return true;
    }
  }
  return false;
}

// --------------------------------------------------------------- primitives
void secureWipe(void* p, size_t n) {
  if (p && n) crypto_wipe(p, n);  // Monocypher: volatile writes, not elided
}

Key32 hkdfExtract(const uint8_t* salt, size_t salt_len, const uint8_t* ikm, size_t ikm_len) {
  // RFC 5869: an absent salt is HashLen zeros, which HMAC's zero padding
  // already produces for an empty key.
  return hmacSha256(salt, salt_len, ikm, ikm_len);
}

void hkdfExpand(const Key32& prk, const char* info, uint8_t* out, size_t n) {
  const size_t info_len = info ? std::strlen(info) : 0;
  std::vector<uint8_t> msg(kKeySize + info_len + 1);
  Key32 t{};
  size_t t_len = 0, done = 0;
  for (unsigned i = 1; done < n && i <= 255; ++i) {
    if (t_len) std::memcpy(msg.data(), t.data(), t_len);
    if (info_len) std::memcpy(msg.data() + t_len, info, info_len);
    msg[t_len + info_len] = static_cast<uint8_t>(i);
    t = hmacSha256(prk.data(), prk.size(), msg.data(), t_len + info_len + 1);
    t_len = kKeySize;
    const size_t take = std::min(kKeySize, n - done);
    std::memcpy(out + done, t.data(), take);
    done += take;
  }
  if (done < n) std::memset(out + done, 0, n - done);  // n > 255*32: contract violation
  secureWipe(t.data(), t.size());
  secureWipe(msg.data(), msg.size());
}

Key32 x25519Public(const Key32& secret) {
  Key32 pub;
  crypto_x25519_public_key(pub.data(), secret.data());
  return pub;
}

bool x25519(const Key32& secret, const Key32& peer_pub, Key32& shared) {
  crypto_x25519(shared.data(), secret.data(), peer_pub.data());
  uint8_t acc = 0;  // constant time all-zero check (low-order / invalid peer key)
  for (uint8_t b : shared) acc = static_cast<uint8_t>(acc | b);
  return acc != 0;
}

Key32 elligatorMap(const Key32& hidden) {
  Key32 curve;
  crypto_elligator_map(curve.data(), hidden.data());
  return curve;
}

// --------------------------------------------------------------------- AEAD
struct Aead::Impl {
  Cipher cipher = Cipher::None;
  const aesgcm::Backend* aes = nullptr;
  aesgcm::Key aes_key;
  uint8_t chacha_key[32];
  uint8_t iv[kAeadIvSize];

  // TLS 1.3 style: iv XOR (4 zero bytes || u64 counter big-endian).
  void nonce(uint64_t counter, uint8_t out[kAeadIvSize]) const {
    std::memcpy(out, iv, kAeadIvSize);
    for (size_t i = 0; i < 8; ++i)
      out[4 + i] = static_cast<uint8_t>(out[4 + i] ^ static_cast<uint8_t>(counter >> (56 - 8 * i)));
  }
};

Aead::Aead() : impl_(new Impl()) {}

Aead::~Aead() {
  if (impl_) secureWipe(impl_.get(), sizeof(Impl));
}

bool Aead::init(const TrafficKey& k) {
  secureWipe(impl_.get(), sizeof(Impl));
  impl_->cipher = Cipher::None;
  impl_->aes = nullptr;
  switch (k.cipher) {
    case Cipher::Aes128Gcm: {
      const aesgcm::Backend* b = hwBackend();
      if (!b) return false;
      b->init(impl_->aes_key, k.key.data());
      impl_->aes = b;
      break;
    }
    case Cipher::ChaCha20Poly1305:
      std::memcpy(impl_->chacha_key, k.key.data(), 32);
      break;
    default:
      return false;
  }
  std::memcpy(impl_->iv, k.iv.data(), kAeadIvSize);
  impl_->cipher = k.cipher;
  return true;
}

Cipher Aead::cipher() const { return impl_->cipher; }

void Aead::seal(uint64_t counter, const uint8_t* aad, size_t aad_len, uint8_t* p, size_t n,
                uint8_t* tag) const {
  const Impl& s = *impl_;
  uint8_t nonce[kAeadIvSize];
  s.nonce(counter, nonce);
  if (s.cipher == Cipher::Aes128Gcm) {
    s.aes->seal(s.aes_key, nonce, aad, aad_len, p, n, tag);
  } else if (s.cipher == Cipher::ChaCha20Poly1305) {
    crypto_aead_ctx ctx;  // fresh context per record = plain RFC 8439 (no ratchet)
    crypto_aead_init_ietf(&ctx, s.chacha_key, nonce);
    crypto_aead_write(&ctx, p, tag, aad, aad_len, p, n);
    crypto_wipe(&ctx, sizeof(ctx));
  } else {
    // Not initialised (init() failed / Cipher::None): never let plaintext out
    // looking like a record. The data is destroyed and the tag cannot verify.
    if (p && n) std::memset(p, 0, n);
    std::memset(tag, 0, kAeadTagSize);
  }
}

bool Aead::open(uint64_t counter, const uint8_t* aad, size_t aad_len, uint8_t* p, size_t n,
                const uint8_t* tag) const {
  const Impl& s = *impl_;
  uint8_t nonce[kAeadIvSize];
  s.nonce(counter, nonce);
  if (s.cipher == Cipher::Aes128Gcm) {
    uint8_t calc[kAeadTagSize];
    s.aes->open(s.aes_key, nonce, aad, aad_len, p, n, calc);
    const bool ok = crypto_verify16(calc, tag) == 0;
    secureWipe(calc, sizeof(calc));
    return ok;
  }
  if (s.cipher == Cipher::ChaCha20Poly1305) {
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, s.chacha_key, nonce);
    const int r = crypto_aead_read(&ctx, p, tag, aad, aad_len, p, n);
    crypto_wipe(&ctx, sizeof(ctx));
    return r == 0;
  }
  return false;
}

// ---------------------------------------------------------------- handshake
struct Handshake::Impl {
  Role role = Role::Pc;
  Key32 eph_secret{}, eph_pub{};
  bool begun = false;
  Cipher cipher = Cipher::None;
  Key32 th{};   // SHA256(Hello || HelloAck)
  Key32 ee{};   // X25519(eph, peer eph)
  bool have_share = false;
  Key32 cpace_secret{}, own_share{};
  bool ready = false;
  Key32 prk{};
  Key32 new_pairing_key{};

  void clearAuth() {
    ready = false;
    secureWipe(prk.data(), prk.size());
    secureWipe(new_pairing_key.data(), new_pairing_key.size());
  }
  Key32 expand(const char* label) const {
    Key32 out{};
    if (ready) hkdfExpand(prk, label, out.data(), out.size());
    return out;
  }
};

Handshake::Handshake(Role role, const Key32& eph_secret) : impl_(new Impl()) {
  impl_->role = role;
  impl_->eph_secret = eph_secret;
  crypto_x25519_public_key(impl_->eph_pub.data(), eph_secret.data());
}

Handshake::~Handshake() {
  if (impl_) secureWipe(impl_.get(), sizeof(Impl));
}

const Key32& Handshake::ephPublic() const { return impl_->eph_pub; }

bool Handshake::begin(const uint8_t* hello, size_t hello_len, const uint8_t* ack, size_t ack_len,
                      const Key32& peer_eph_pub, Cipher cipher) {
  Impl& s = *impl_;
  s.begun = false;
  s.have_share = false;
  s.clearAuth();
  if (cipher != Cipher::None && cipher != Cipher::Aes128Gcm && cipher != Cipher::ChaCha20Poly1305)
    return false;
  std::vector<uint8_t> t;
  t.reserve(hello_len + ack_len);
  if (hello_len) t.insert(t.end(), hello, hello + hello_len);
  if (ack_len) t.insert(t.end(), ack, ack + ack_len);
  s.th = sha256(t.data(), t.size());
  if (!x25519(s.eph_secret, peer_eph_pub, s.ee)) {
    secureWipe(s.ee.data(), s.ee.size());
    return false;
  }
  s.cipher = cipher;
  s.begun = true;
  return true;
}

void Handshake::usePairingKey(const Key32& pairing_key) {
  Impl& s = *impl_;
  s.have_share = false;
  s.clearAuth();
  if (!s.begun) return;
  uint8_t ikm[2 * kKeySize];
  std::memcpy(ikm, s.ee.data(), kKeySize);
  std::memcpy(ikm + kKeySize, pairing_key.data(), kKeySize);
  s.prk = hkdfExtract(s.th.data(), s.th.size(), ikm, sizeof(ikm));
  secureWipe(ikm, sizeof(ikm));
  s.ready = true;
}

Key32 Handshake::cpaceStart(const std::string& code, const Key32& rnd) {
  Impl& s = *impl_;
  s.clearAuth();
  s.have_share = false;
  secureWipe(s.own_share.data(), s.own_share.size());
  if (!s.begun) return Key32{};
  // G = elligatorMap(SHA256("OWC-CPACE-3" || code || th))
  static const char kTag[] = "OWC-CPACE-3";
  std::vector<uint8_t> m;
  m.reserve(sizeof(kTag) - 1 + code.size() + kKeySize);
  m.insert(m.end(), kTag, kTag + sizeof(kTag) - 1);
  m.insert(m.end(), code.begin(), code.end());
  m.insert(m.end(), s.th.begin(), s.th.end());
  Key32 hidden = sha256(m.data(), m.size());
  Key32 g = elligatorMap(hidden);
  s.cpace_secret = rnd;
  crypto_x25519(s.own_share.data(), s.cpace_secret.data(), g.data());
  secureWipe(m.data(), m.size());
  secureWipe(hidden.data(), hidden.size());
  secureWipe(g.data(), g.size());
  s.have_share = true;
  return s.own_share;
}

bool Handshake::cpaceFinish(const Key32& peer_share) {
  Impl& s = *impl_;
  s.clearAuth();
  if (!s.begun || !s.have_share) return false;
  s.have_share = false;  // one finish per cpaceStart
  Key32 k{};
  const bool ok = x25519(s.cpace_secret, peer_share, k);
  secureWipe(s.cpace_secret.data(), s.cpace_secret.size());
  if (!ok) {
    secureWipe(k.data(), k.size());
    return false;
  }
  const Key32& ya = s.role == Role::Pc ? s.own_share : peer_share;
  const Key32& yb = s.role == Role::Pc ? peer_share : s.own_share;
  uint8_t t[3 * kKeySize];
  std::memcpy(t, s.th.data(), kKeySize);
  std::memcpy(t + kKeySize, ya.data(), kKeySize);
  std::memcpy(t + 2 * kKeySize, yb.data(), kKeySize);
  const Key32 th2 = sha256(t, sizeof(t));
  uint8_t ikm[2 * kKeySize];
  std::memcpy(ikm, s.ee.data(), kKeySize);
  std::memcpy(ikm + kKeySize, k.data(), kKeySize);
  s.prk = hkdfExtract(th2.data(), th2.size(), ikm, sizeof(ikm));
  secureWipe(ikm, sizeof(ikm));
  secureWipe(k.data(), k.size());
  hkdfExpand(s.prk, "owc3 pairing key", s.new_pairing_key.data(), s.new_pairing_key.size());
  s.ready = true;
  return true;
}

bool Handshake::ready() const { return impl_->ready; }
Key32 Handshake::pcConfirm() const { return impl_->expand("owc3 pc confirm"); }
Key32 Handshake::phoneConfirm() const { return impl_->expand("owc3 phone confirm"); }
Key32 Handshake::newPairingKey() const {
  return impl_->ready ? impl_->new_pairing_key : Key32{};
}

SessionKeys Handshake::keys() const {
  const Impl& s = *impl_;
  SessionKeys out;
  if (!s.ready) return out;  // cipher None, all zeros
  out.cipher = s.cipher;
  if (s.cipher == Cipher::None) return out;
  const size_t klen = s.cipher == Cipher::Aes128Gcm ? 16 : 32;
  struct Slot { TrafficKey* k; const char* key_label; const char* iv_label; };
  const Slot slots[] = {
      {&out.c2p_tcp, "owc3 c2p tcp key", "owc3 c2p tcp iv"},
      {&out.p2c_tcp, "owc3 p2c tcp key", "owc3 p2c tcp iv"},
      {&out.c2p_udp, "owc3 c2p udp key", "owc3 c2p udp iv"},
      {&out.p2c_udp, "owc3 p2c udp key", "owc3 p2c udp iv"},
  };
  for (const Slot& sl : slots) {
    sl.k->cipher = s.cipher;
    hkdfExpand(s.prk, sl.key_label, sl.k->key.data(), klen);
    hkdfExpand(s.prk, sl.iv_label, sl.k->iv.data(), sl.k->iv.size());
  }
  return out;
}

// ---------------------------------------------------------------------- TCP
bool TcpSealer::init(const TrafficKey& k) {
  counter_ = 0;
  return aead_.init(k);
}

void TcpSealer::seal(MsgType inner, const uint8_t* payload, size_t n, std::vector<uint8_t>& out) {
  seal2(inner, payload, n, nullptr, 0, out);
}

void TcpSealer::seal2(MsgType inner, const uint8_t* a, size_t an, const uint8_t* b, size_t bn,
                      std::vector<uint8_t>& out) {
  const size_t body = 2 + an + bn;
  const size_t off = out.size();
  out.reserve(off + kTcpHeaderSize + body + kAeadTagSize);
  uint8_t pre[kTcpHeaderSize + 2];
  writeTcpHeader(pre, MsgType::Sealed, static_cast<uint32_t>(body + kAeadTagSize));
  const uint16_t t = static_cast<uint16_t>(inner);
  pre[kTcpHeaderSize] = static_cast<uint8_t>(t & 0xFFu);
  pre[kTcpHeaderSize + 1] = static_cast<uint8_t>(t >> 8);
  // insert() rather than resize(): no zero-fill pass over a whole video frame.
  out.insert(out.end(), pre, pre + sizeof(pre));
  if (an) out.insert(out.end(), a, a + an);
  if (bn) out.insert(out.end(), b, b + bn);
  out.insert(out.end(), kAeadTagSize, uint8_t{0});
  uint8_t* h = out.data() + off;
  aead_.seal(counter_, h, kTcpHeaderSize, h + kTcpHeaderSize, body, h + kTcpHeaderSize + body);
  ++counter_;
}

bool TcpOpener::init(const TrafficKey& k) {
  counter_ = 0;
  return aead_.init(k);
}

bool TcpOpener::open(TcpFrameParser::Message& sealed, TcpFrameParser::Message& inner) {
  if (sealed.type != MsgType::Sealed) return false;
  std::vector<uint8_t>& v = sealed.payload;
  if (v.size() < 2 + kAeadTagSize) return false;  // TcpFrameParser caps it at 16 MiB
  uint8_t h[kTcpHeaderSize];  // AAD: the header as it was on the wire (flags = 0)
  writeTcpHeader(h, MsgType::Sealed, static_cast<uint32_t>(v.size()));
  const size_t body = v.size() - kAeadTagSize;
  if (!aead_.open(counter_, h, sizeof(h), v.data(), body, v.data() + body)) return false;
  ++counter_;
  const MsgType t = static_cast<MsgType>(static_cast<uint16_t>(v[0] | (v[1] << 8)));
  if (t == MsgType::Sealed) return false;  // no nesting
  v.resize(body);
  v.erase(v.begin(), v.begin() + 2);
  inner.type = t;
  if (&inner != &sealed) inner.payload.swap(v);  // keep both buffers' capacity in play
  return true;
}

// ---------------------------------------------------------------------- UDP
namespace {
constexpr size_t kUdpSealedHead = kUdpHeaderSize + 8;  // header + counter = AAD

void putLe64(uint8_t* o, uint64_t v) {
  for (size_t i = 0; i < 8; ++i) o[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint64_t getLe64(const uint8_t* p) {
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

using Window = std::array<uint64_t, kReplayWindow / 64>;
static_assert(kReplayWindow % 64 == 0, "window is a whole number of words");

bool windowHas(const Window& w, uint64_t c) {
  const size_t i = static_cast<size_t>(c % kReplayWindow);
  return (w[i / 64] >> (i % 64)) & 1u;
}
void windowSet(Window& w, uint64_t c) {
  const size_t i = static_cast<size_t>(c % kReplayWindow);
  w[i / 64] |= uint64_t{1} << (i % 64);
}
// Clears the bits of counters (from, to]; the caller guarantees to - from < kReplayWindow.
void windowClear(Window& w, uint64_t from, uint64_t to) {
  uint64_t x = from + 1;
  while (x <= to) {
    const size_t i = static_cast<size_t>(x % kReplayWindow);
    const size_t bit = i % 64;
    const uint64_t take = std::min<uint64_t>(64 - bit, to - x + 1);
    const uint64_t mask = take == 64 ? ~uint64_t{0} : ((uint64_t{1} << take) - 1) << bit;
    w[i / 64] &= ~mask;
    x += take;
  }
}
}  // namespace

bool UdpSealer::init(const TrafficKey& k) {
  counter_ = 0;
  return aead_.init(k);
}

size_t UdpSealer::seal(uint8_t* buf, size_t len, size_t cap) {
  if (!buf || len < kUdpHeaderSize || len - kUdpHeaderSize > kUdpMaxPayloadSealed ||
      cap < len + kUdpSealOverhead || aead_.cipher() == Cipher::None)
    return 0;
  const size_t pl = len - kUdpHeaderSize;
  if (pl) std::memmove(buf + kUdpSealedHead, buf + kUdpHeaderSize, pl);
  buf[2] = kUdpVersionSealed;
  putLe64(buf + kUdpHeaderSize, counter_);
  aead_.seal(counter_, buf, kUdpSealedHead, buf + kUdpSealedHead, pl, buf + kUdpSealedHead + pl);
  ++counter_;
  return len + kUdpSealOverhead;
}

bool UdpOpener::init(const TrafficKey& k) {
  highest_ = 0;
  any_ = false;
  window_.fill(0);
  rejected_ = 0;
  return aead_.init(k);
}

bool UdpOpener::open(uint8_t* buf, size_t len, size_t& out_len) {
  if (!buf || len < kUdpSealedHead + kAeadTagSize ||
      static_cast<uint16_t>(buf[0] | (buf[1] << 8)) != kUdpMagic || buf[2] != kUdpVersionSealed) {
    ++rejected_;
    return false;
  }
  const uint64_t c = getLe64(buf + kUdpHeaderSize);
  // Cheap replay pre-check; the window only moves after authentication.
  if (any_ && c <= highest_ && (highest_ - c >= kReplayWindow || windowHas(window_, c))) {
    ++rejected_;
    return false;
  }
  const size_t pl = len - kUdpSealedHead - kAeadTagSize;
  if (!aead_.open(c, buf, kUdpSealedHead, buf + kUdpSealedHead, pl, buf + kUdpSealedHead + pl)) {
    ++rejected_;
    return false;
  }
  if (!any_ || c > highest_) {
    if (!any_ || c - highest_ >= kReplayWindow) window_.fill(0);
    else windowClear(window_, highest_, c);
    highest_ = c;
    any_ = true;
  }
  windowSet(window_, c);
  if (pl) std::memmove(buf + kUdpHeaderSize, buf + kUdpSealedHead, pl);
  buf[2] = kUdpVersion;
  out_len = kUdpHeaderSize + pl;
  return true;
}

}  // namespace owc
