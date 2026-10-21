// Throughput / latency of the v3 session crypto on this machine. Not a test:
//   owc_crypto_bench [seconds_per_measurement=0.3]
// Sizes: 1 KB (control message), 25 KB (typical P-frame), 200 KB (keyframe).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "owc/crypto.h"
#include "owc/protocol.h"

using namespace owc;
using Clock = std::chrono::steady_clock;

static volatile uint8_t g_sink = 0;
static double g_min_s = 0.3;

static void fill(uint8_t* p, size_t n, uint64_t seed) {
  for (size_t i = 0; i < n; ++i) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    p[i] = static_cast<uint8_t>(seed >> 56);
  }
}
static Key32 key32(uint64_t seed) {
  Key32 k;
  fill(k.data(), k.size(), seed);
  return k;
}

// Runs f() until g_min_s elapsed (after a short warm-up); returns seconds per call.
template <class F>
static double perCall(F&& f) {
  for (int i = 0; i < 3; ++i) f();
  uint64_t n = 0, batch = 1;
  const auto t0 = Clock::now();
  double el = 0;
  do {
    for (uint64_t i = 0; i < batch; ++i) f();
    n += batch;
    if (batch < (1u << 20)) batch *= 2;
    el = std::chrono::duration<double>(Clock::now() - t0).count();
  } while (el < g_min_s);
  return el / static_cast<double>(n);
}

static void row(const char* what, size_t bytes, double s) {
  if (bytes)
    std::printf("  %-34s %8zu B  %10.2f us  %9.1f MB/s\n", what, bytes, s * 1e6,
                static_cast<double>(bytes) / s / 1e6);
  else
    std::printf("  %-34s %10s  %10.2f us\n", what, "", s * 1e6);
}

static void benchCipher(Cipher c) {
  std::printf("%s\n", toString(c));
  TrafficKey tk;
  tk.cipher = c;
  fill(tk.key.data(), c == Cipher::Aes128Gcm ? 16 : 32, 1);
  fill(tk.iv.data(), tk.iv.size(), 2);
  Aead a;
  if (!a.init(tk)) {
    std::printf("  init failed\n");
    return;
  }
  uint8_t aad[8] = {60, 0, 0, 0, 1, 2, 3, 4};
  const size_t sizes[] = {1024, 25 * 1024, 200 * 1024};
  for (size_t n : sizes) {
    std::vector<uint8_t> buf(n);
    fill(buf.data(), n, n);
    uint8_t tag[16];
    uint64_t ctr = 0;
    const double seal = perCall([&] {
      a.seal(ctr++, aad, sizeof(aad), buf.data(), n, tag);
      g_sink = static_cast<uint8_t>(g_sink ^ tag[0]);
    });
    // open needs a valid record: time seal+open pairs and subtract the seal.
    const double pair = perCall([&] {
      a.seal(ctr, aad, sizeof(aad), buf.data(), n, tag);
      if (!a.open(ctr, aad, sizeof(aad), buf.data(), n, tag)) std::abort();
      ++ctr;
    });
    char label[64];
    std::snprintf(label, sizeof(label), "seal %zu KB", n / 1024);
    row(label, n, seal);
    std::snprintf(label, sizeof(label), "open %zu KB", n / 1024);
    row(label, n, pair - seal);
  }
  // TCP record layer as the phone uses it: VideoFrame header + AU via seal2 (incl. the copy).
  for (size_t n : {size_t{25 * 1024}, size_t{200 * 1024}}) {
    TcpSealer s;
    TcpOpener o;
    s.init(tk);
    o.init(tk);
    std::vector<uint8_t> hdr(kVideoFrameHeaderSize), au(n), out;
    fill(au.data(), n, 3);
    out.reserve(n + 64);
    const double t = perCall([&] {
      out.clear();
      s.seal2(MsgType::VideoFrame, hdr.data(), hdr.size(), au.data(), au.size(), out);
      g_sink = static_cast<uint8_t>(g_sink ^ out.back());
    });
    char label[64];
    std::snprintf(label, sizeof(label), "TcpSealer::seal2 VideoFrame %zu KB", n / 1024);
    row(label, n, t);
    // receive path: parser -> opener
    TcpSealer s2;
    s2.init(tk);
    TcpFrameParser::Message sealed, inner;
    const double r = perCall([&] {
      out.clear();
      s2.seal2(MsgType::VideoFrame, hdr.data(), hdr.size(), au.data(), au.size(), out);
      TcpFrameParser p;
      p.feed(out.data(), out.size());
      p.next(sealed);
      if (!o.open(sealed, inner)) std::abort();
    });
    std::snprintf(label, sizeof(label), "TcpOpener::open (parse+open) %zu KB", n / 1024);
    row(label, n, r - t);
  }
  // UDP: one full sealed datagram, and a 200 KB keyframe as datagrams.
  {
    UdpSealer s;
    UdpOpener o;
    s.init(tk);
    o.init(tk);
    UdpHeader h;
    h.session_id = 1;
    h.frag_count = 1;
    std::vector<uint8_t> d(kUdpMaxDatagram);
    const double t = perCall([&] {
      writeUdpHeader(h, d.data());
      size_t n = s.seal(d.data(), kUdpHeaderSize + kUdpMaxPayloadSealed, d.size());
      size_t out = 0;
      if (!n || !o.open(d.data(), n, out)) std::abort();
    });
    row("UDP datagram seal+open (1344 B)", kUdpMaxPayloadSealed, t);
    std::vector<uint8_t> au(200 * 1024);
    fill(au.data(), au.size(), 4);
    const double f = perCall([&] {
      packetize(h, au.data(), au.size(), [&](const uint8_t* p, size_t len) {
        std::memcpy(d.data(), p, len);
        size_t n = s.seal(d.data(), len, d.size());
        g_sink = static_cast<uint8_t>(g_sink ^ d[n - 1]);
      }, kUdpMaxPayloadSealed);
    });
    row("UDP 200 KB frame packetize+seal", au.size(), f);
  }
}

int main(int argc, char** argv) {
  if (argc > 1) g_min_s = std::atof(argv[1]);
  if (g_min_s <= 0) g_min_s = 0.3;
  std::printf("owc crypto bench  (hardware AES: %s, local mask 0x%x)\n",
              hardwareAesAvailable() ? "yes" : "no", localCipherMask());
  if (hardwareAesAvailable()) benchCipher(Cipher::Aes128Gcm);
  benchCipher(Cipher::ChaCha20Poly1305);

  std::printf("Key exchange / handshake\n");
  uint64_t seed = 10;
  Key32 peer = x25519Public(key32(99));
  row("x25519Public", 0, perCall([&] { g_sink = static_cast<uint8_t>(g_sink ^ x25519Public(key32(++seed))[0]); }));
  row("x25519 (shared secret)", 0, perCall([&] {
        Key32 s;
        x25519(key32(++seed), peer, s);
        g_sink = static_cast<uint8_t>(g_sink ^ s[0]);
      }));
  std::vector<uint8_t> hello(90), ack(140);
  fill(hello.data(), hello.size(), 5);
  fill(ack.data(), ack.size(), 6);
  const Key32 pk = key32(7);
  // One side's full work, paired path: ephemeral key, begin, confirmations, traffic keys.
  row("handshake, paired (one side)", 0, perCall([&] {
        Handshake h(Handshake::Role::Pc, key32(++seed));
        if (!h.begin(hello.data(), hello.size(), ack.data(), ack.size(), peer, Cipher::Aes128Gcm)) std::abort();
        h.usePairingKey(pk);
        Key32 a = h.pcConfirm(), b = h.phoneConfirm();
        SessionKeys k = h.keys();
        g_sink = static_cast<uint8_t>(g_sink ^ a[0] ^ b[0] ^ k.c2p_tcp.key[0]);
      }));
  const Key32 share = x25519Public(key32(8));  // any valid point works as the peer share
  row("handshake, CPace pairing (one side)", 0, perCall([&] {
        Handshake h(Handshake::Role::Phone, key32(++seed));
        if (!h.begin(hello.data(), hello.size(), ack.data(), ack.size(), peer, Cipher::Aes128Gcm)) std::abort();
        h.cpaceStart("123456", key32(++seed));
        if (!h.cpaceFinish(share)) std::abort();
        Key32 a = h.pcConfirm(), b = h.phoneConfirm(), n = h.newPairingKey();
        SessionKeys k = h.keys();
        g_sink = static_cast<uint8_t>(g_sink ^ a[0] ^ b[0] ^ n[0] ^ k.c2p_tcp.key[0]);
      }));
  std::printf("(sink %u)\n", static_cast<unsigned>(g_sink));
  return 0;
}
