// OWC session crypto (protocol v3): ephemeral X25519 key exchange, CPace PAKE
// for pairing, HKDF-SHA256 key schedule, AEAD record layers for TCP and UDP.
// Shared by the Android NDK core and the Windows client; part of owc_protocol.
//
// Ciphers (fastest first):
//   Aes128Gcm         hardware only: x86 AES-NI + PCLMULQDQ, ARMv8 AES + PMULL
//                     (arm64, and 32-bit ARM on ARMv8 cores), detected at
//                     runtime. Multi-block pipelined CTR + aggregated GHASH.
//   ChaCha20Poly1305  RFC 8439 (IETF, 96-bit nonce); always available.
//   None              no encryption (authentication still mandatory). Only
//                     when the PC explicitly offers nothing else.
// X25519, Elligator 2 and ChaCha20-Poly1305 come from the vendored Monocypher
// (protocol/third_party/monocypher). Randomness is NEVER generated here: every
// call that needs it takes caller-provided bytes from the platform CSPRNG
// (arc4random_buf on Android, BCryptGenRandom on Windows).
//
// ============================================================ KEY SCHEDULE
// HKDF = RFC 5869 with SHA-256; labels are ASCII without terminator.
//   th  = SHA256(Hello payload || HelloAck payload)   exact wire bytes, no TCP header
//   ee  = X25519(own eph secret, peer eph pub)         all-zero -> abort
// Paired:
//   prk = HKDF-Extract(salt = th, ikm = ee || pairing_key)
// Pairing (CPace on X25519; Ya = PC's share, Yb = phone's share):
//   G   = elligatorMap(SHA256("OWC-CPACE-3" || code || th))
//   Ya  = X25519(a, G), Yb = X25519(b, G)   a/b = fresh caller randomness per try
//   K   = X25519(a, Yb) = X25519(b, Ya)      all-zero -> treated as a wrong code
//   th2 = SHA256(th || Ya || Yb)
//   prk = HKDF-Extract(salt = th2, ikm = ee || K)
//   newPairingKey = HKDF-Expand(prk, "owc3 pairing key", 32)
// Both:
//   pcConfirm     = HKDF-Expand(prk, "owc3 pc confirm", 32)
//   phoneConfirm  = HKDF-Expand(prk, "owc3 phone confirm", 32)
//   for d in {c2p, p2c}, ch in {tcp, udp}:
//     key = HKDF-Expand(prk, "owc3 <d> <ch> key", 16 (AES) | 32 (ChaCha))
//     iv  = HKDF-Expand(prk, "owc3 <d> <ch> iv", 12)
//   AEAD nonce = iv XOR (4 zero bytes || u64 counter big-endian)   (TLS 1.3 style)
// c2p = PC -> phone, p2c = phone -> PC. Four independent keys: TCP and UDP
// never share a nonce space.
//
// ============================================================ TCP RECORDS
// After AuthResult{Ok} (which itself is plain), every TCP message in both
// directions is one record:
//   TcpHeader{type = Sealed, flags = 0, length = 2 + n + 16}
//   ciphertext( u16 inner_type || inner payload (n bytes) ) || tag[16]
//   AAD = the 8 TcpHeader bytes; counter = implicit, 0,1,2... per direction.
// The phone seals from its first message after AuthResult{Ok}; the PC from its
// first message after receiving it. A record that fails to open, or any
// non-Sealed message after that point, is fatal for the connection.
// With Cipher::None messages stay plain.
//
// ============================================================ UDP DATAGRAMS
//   UdpHeader (32 bytes, version = kUdpVersionSealed) || u64 counter (LE)
//   || ciphertext(payload) || tag[16]
//   AAD = the 32 header bytes || the 8 counter bytes; counter = per direction,
//   starts at 0, explicit (datagrams get lost/reordered). The receiver keeps a
//   sliding replay window of kReplayWindow counters. Punch (c2p) and PunchAck
//   (p2c) are sealed too (empty plaintext): the phone only answers / sends
//   video to a punch that opens with the session key, so nobody else can
//   redirect the stream. With Cipher::None datagrams stay v1.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <owc/protocol.h>

namespace owc {

enum class Cipher : uint8_t { None = 0, Aes128Gcm = 1, ChaCha20Poly1305 = 2 };
constexpr uint32_t cipherBit(Cipher c) { return 1u << static_cast<uint8_t>(c); }
const char* toString(Cipher);

// Ciphers this machine can run: always None | ChaCha20Poly1305, plus
// Aes128Gcm when the CPU has the AES + carry-less multiply instructions.
uint32_t localCipherMask();
bool hardwareAesAvailable();
// Phone side: the first of Aes128Gcm, ChaCha20Poly1305, None present in both
// masks. Returns false if the intersection is empty. The PC forces a cipher by
// offering only that bit (default: localCipherMask() minus None).
bool chooseCipher(uint32_t pc_mask, uint32_t phone_mask, Cipher& out);

constexpr size_t kAeadTagSize = 16;
constexpr size_t kAeadIvSize = 12;
constexpr uint8_t kUdpVersionSealed = 2;
constexpr size_t kUdpSealOverhead = 8 + kAeadTagSize;  // counter + tag
constexpr size_t kUdpMaxPayloadSealed = kUdpMaxPayload - kUdpSealOverhead;
constexpr size_t kReplayWindow = 1024;

// ---------------------------------------------------------------- primitives
// Exposed for tests and the handshake. hkdfExpand: n <= 255*32.
Key32 hkdfExtract(const uint8_t* salt, size_t salt_len, const uint8_t* ikm, size_t ikm_len);
void hkdfExpand(const Key32& prk, const char* info, uint8_t* out, size_t n);
Key32 x25519Public(const Key32& secret);                                // RFC 7748 (clamps)
bool x25519(const Key32& secret, const Key32& peer_pub, Key32& shared);  // false if all-zero
Key32 elligatorMap(const Key32& hidden);                                 // Monocypher crypto_elligator_map
// Overwrites memory in a way the compiler cannot elide.
void secureWipe(void* p, size_t n);

struct TrafficKey {
  Cipher cipher = Cipher::None;
  std::array<uint8_t, 32> key{};  // first 16 bytes used by Aes128Gcm
  std::array<uint8_t, kAeadIvSize> iv{};
};
struct SessionKeys {
  Cipher cipher = Cipher::None;
  TrafficKey c2p_tcp, p2c_tcp, c2p_udp, p2c_udp;
};

// One direction, one key. Not thread-safe (one owner thread). Expensive key
// setup (AES round keys, GHASH powers) is done once in init().
class Aead {
 public:
  Aead();
  ~Aead();  // wipes the key schedule
  Aead(const Aead&) = delete;
  Aead& operator=(const Aead&) = delete;
  bool init(const TrafficKey& k);  // false for Cipher::None
  // In place. `p` may alias nothing else. tag: 16 bytes out / in.
  void seal(uint64_t counter, const uint8_t* aad, size_t aad_len, uint8_t* p, size_t n,
            uint8_t* tag) const;
  // Constant-time tag check; on failure the buffer content is unspecified.
  bool open(uint64_t counter, const uint8_t* aad, size_t aad_len, uint8_t* p, size_t n,
            const uint8_t* tag) const;
  Cipher cipher() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------- handshake
// Same object on both sides; Role selects which share/confirmation is "own".
// Call order:
//   Handshake hs(role, eph_secret);  hello.eph_pub = hs.ephPublic() (PC) /
//                                    ack.eph_pub = hs.ephPublic() (phone)
//   hs.begin(hello_bytes, ack_bytes, peer_eph_pub, cipher)
//   paired:  hs.usePairingKey(stored)                       -> confirms + keys
//   pairing: phone: Yb = hs.cpaceStart(code, rnd)  (again with fresh rnd after
//                   each wrong code)  ... hs.cpaceFinish(Ya)
//            PC:    Ya = hs.cpaceStart(code, rnd); hs.cpaceFinish(Yb)
//            cpaceFinish false = invalid share (treat as a wrong code)
//   then pcConfirm()/phoneConfirm()/keys() and, after pairing, newPairingKey().
// A failed CPace try can be followed by cpaceStart again (new code attempt).
// The destructor wipes every secret.
class Handshake {
 public:
  enum class Role : uint8_t { Pc, Phone };
  Handshake(Role role, const Key32& eph_secret);
  ~Handshake();
  Handshake(const Handshake&) = delete;
  Handshake& operator=(const Handshake&) = delete;

  const Key32& ephPublic() const;
  // false: peer key gives an all-zero ee (abort the connection).
  bool begin(const uint8_t* hello, size_t hello_len, const uint8_t* ack, size_t ack_len,
             const Key32& peer_eph_pub, Cipher cipher);
  void usePairingKey(const Key32& pairing_key);
  Key32 cpaceStart(const std::string& code, const Key32& rnd);  // returns own share
  bool cpaceFinish(const Key32& peer_share);
  bool ready() const;  // usePairingKey or a successful cpaceFinish happened

  Key32 pcConfirm() const;
  Key32 phoneConfirm() const;
  Key32 newPairingKey() const;  // pairing path only (zeros otherwise)
  SessionKeys keys() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------- TCP
// Sealer: one per sending direction (phone: Sender thread; PC: the thread that
// writes the control socket, under its send lock). Appends a whole outer
// Sealed message to `out` (does not clear it). seal2 = payload given in two
// parts (e.g. VideoFrame header + access unit) to avoid a copy.
class TcpSealer {
 public:
  bool init(const TrafficKey& k);
  void seal(MsgType inner, const uint8_t* payload, size_t n, std::vector<uint8_t>& out);
  void seal2(MsgType inner, const uint8_t* a, size_t an, const uint8_t* b, size_t bn,
             std::vector<uint8_t>& out);
  uint64_t counter() const { return counter_; }

 private:
  Aead aead_;
  uint64_t counter_ = 0;
};

// Opener: one per receiving direction, on the thread that parses the socket.
// `sealed` must be a MsgType::Sealed message; on success `inner` holds the
// original type and payload. false = fatal (close the connection).
class TcpOpener {
 public:
  bool init(const TrafficKey& k);
  bool open(TcpFrameParser::Message& sealed, TcpFrameParser::Message& inner);
  uint64_t counter() const { return counter_; }

 private:
  Aead aead_;
  uint64_t counter_ = 0;
};

// ---------------------------------------------------------------- UDP
// Sealer: input = a plain v1 datagram (header + payload <= kUdpMaxPayloadSealed)
// in `buf` with capacity `cap` >= len + kUdpSealOverhead; sealed in place.
// Returns the sealed length (0 if it does not fit).
class UdpSealer {
 public:
  bool init(const TrafficKey& k);
  size_t seal(uint8_t* buf, size_t len, size_t cap);

 private:
  Aead aead_;
  uint64_t counter_ = 0;
};

// Opener: input = a received datagram; on success it is rewritten in place
// into the plain v1 layout (version byte 1, payload plaintext) of length
// `out_len`. false = drop it (not sealed, bad tag, replayed, too old).
class UdpOpener {
 public:
  bool init(const TrafficKey& k);
  bool open(uint8_t* buf, size_t len, size_t& out_len);
  uint64_t rejected() const { return rejected_; }

 private:
  Aead aead_;
  uint64_t highest_ = 0;
  bool any_ = false;
  std::array<uint64_t, kReplayWindow / 64> window_{};
  uint64_t rejected_ = 0;
};

}  // namespace owc
