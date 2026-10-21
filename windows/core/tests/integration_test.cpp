// owc_core integration test: in-process FakePhone (loopback, ephemeral ports)
// <-> owc::Client. Verifies handshake, authentication / pairing (protocol v3:
// X25519 + CPace, mutual key confirmation), encryption of TCP and UDP (each
// cipher, Off, attacks: forged confirmation, tampered/plaintext TCP records,
// tampered/replayed UDP datagrams, spoofed punches), discovery, TCP and UDP
// (+loss, +FEC) streaming, decoded frame counters, shared-memory output,
// B-frame handling, transforms, Busy / NoResponse errors; prints measured
// latencies (also per cipher). Every streaming scenario authenticates with a
// pairing preloaded on the fake phone (AuthProof path) and is encrypted with
// the default cipher unless it says otherwise.
#include "test_util.h"

#include "decoder.h"
#include "record_setup.h"
#include "transform.h"
#include "util.h"

#include "fake_phone.h"
#include "pattern.h"

#include <owc/core.h>
#include <owc/shm.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace owc;

namespace {

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// This test PC's identity and the key the fake phone has for it (preloaded).
const ClientId kTestClientId = {0x0c, 0x11, 0xe2, 0x7a, 0x55, 0x01, 0x9b, 0x3d,
                                0x42, 0x6f, 0x80, 0x13, 0xc4, 0x29, 0xd7, 0x6e};
const Key32 kTestKey = {0x51, 0x2e, 0x93, 0x04, 0xaa, 0x7c, 0x18, 0xf0, 0x3b, 0x66, 0xd1,
                        0x82, 0x0e, 0x49, 0xb5, 0x27, 0xc8, 0x1d, 0x74, 0x9a, 0x36, 0xef,
                        0x05, 0x58, 0xbc, 0x63, 0x20, 0x8f, 0xd2, 0x47, 0x9e, 0x11};

// The fake phone knows kTestClientId with kTestKey.
fake::FakePhoneConfig& preload(fake::FakePhoneConfig& pc) {
  fake::FakePhoneConfig::PairedClient c;
  c.client_id = kTestClientId;
  c.key = kTestKey;
  c.name = "integration-test";
  pc.paired_clients.push_back(c);
  return pc;
}

// The client holds the key for this phone.
void authorize(ConnectParams& cp, const fake::FakePhone& phone) {
  cp.client_id = kTestClientId;
  PairedPhone pp;
  pp.phone_id = phone.phoneId();
  pp.key = kTestKey;
  cp.paired.push_back(pp);
}

struct Recorder : ClientEvents {
  std::mutex mx;
  std::vector<uint32_t> counters;  // strip values of decoded frames
  uint32_t unreadable = 0;
  uint32_t nonIncreasing = 0;
  uint32_t lastW = 0, lastH = 0;
  int lastRot = 0;
  uint32_t lastColor = 99;
  bool lastFull = false;
  bool caps = false, connected = false, format = false;
  std::vector<ClientState> states;
  DisconnectReason lastReason = DisconnectReason::None;
  std::string lastMsg;
  std::atomic<int> frames{0};
  int rotateBack = 0;  // undo this rotation before reading the strip

  void onStateChanged(ClientState s, DisconnectReason r, const std::string& m) override {
    std::lock_guard<std::mutex> lk(mx);
    states.push_back(s);
    if (r != DisconnectReason::None) {
      lastReason = r;
      lastMsg = m;
    }
  }
  void onConnected(const DeviceInfo&) override { connected = true; }
  void onCapabilities(const Capabilities&) override { caps = true; }
  void onVideoFormat(const VideoFormat&) override { format = true; }
  void onFrame(const DecodedFrame& f) override {
    uint32_t v = 0;
    bool ok;
    if (rotateBack) {
      uint32_t bw, bh;
      const int back = detail::normalizeRotation(360 - rotateBack);
      detail::transformedSize(f.width, f.height, back, bw, bh);
      std::vector<uint8_t> tmp(static_cast<size_t>(bw) * bh * 3 / 2);
      detail::transformNV12(f.y, f.y_stride, f.uv, f.uv_stride, f.width, f.height, back, false,
                            tmp.data(), bw, tmp.data() + static_cast<size_t>(bw) * bh, bw);
      ok = fake::readCounterStrip(tmp.data(), bw, bw, bh, v);
    } else {
      ok = fake::readCounterStrip(f.y, f.y_stride, f.width, f.height, v);
    }
    std::lock_guard<std::mutex> lk(mx);
    lastW = f.width;
    lastH = f.height;
    lastColor = f.color_matrix;
    lastFull = f.full_range;
    lastRot = f.rotation_applied;
    if (!ok) {
      ++unreadable;
    } else {
      if (!counters.empty() && v <= counters.back()) ++nonIncreasing;
      counters.push_back(v);
    }
    ++frames;
  }
  void reset() {
    std::lock_guard<std::mutex> lk(mx);
    counters.clear();
    unreadable = nonIncreasing = 0;
    frames = 0;
  }
};

struct ShmCheck {
  uint32_t frames = 0, readable = 0, nonIncreasing = 0;
  uint32_t colorMatrix = 99;
  bool fullRange = false;
  uint32_t last = 0;
  bool have = false;
  double maxPublishToRead = 0;
};

// What ClientOptions::cipher negotiates against a fake phone on this machine
// (same CPU: the phone runs localCipherMask()).
Cipher expectedCipher(CipherPreference p) {
  switch (p) {
    case CipherPreference::Off: return Cipher::None;
    case CipherPreference::ChaCha20Poly1305: return Cipher::ChaCha20Poly1305;
    case CipherPreference::Aes128Gcm:
    case CipherPreference::Auto: break;
  }
  return hardwareAesAvailable() ? Cipher::Aes128Gcm : Cipher::ChaCha20Poly1305;
}

void printStats(const char* tag, const ClientStats& s) {
  std::printf("  [%s] crypto %s decrypt avg %.1f us max %.1f us/frame, udp rejected %llu\n", tag,
              toString(s.cipher), s.decrypt_us_avg, s.decrypt_us_max,
              static_cast<unsigned long long>(s.udp_auth_failures));
  std::printf(
      "  [%s] state=%s decoder='%s' hw=%d lowlat=%d bframes=%d sps_patched=%d\n"
      "  [%s] rx %.1f fps %.0f kbps | decoded %llu (%.1f fps) published %llu | lost %llu "
      "recovered(FEC) %llu frag rx %llu lost %llu | kf req %u\n"
      "  [%s] rtt %.2f ms (min %.2f) offset %lld us | jitter %.2f ms grad %.2f ms\n"
      "  [%s] LATENCY arrival->decoded avg %.2f p95 %.2f ms | capture->publish avg %.2f p95 %.2f "
      "ms | capture->arrival avg %.2f ms | decode %.2f ms (readback %.2f) | decoder delay avg %.2f "
      "max %u frames\n",
      tag, toString(s.state), s.decoder_name.c_str(), s.decoder_hardware, s.decoder_low_latency,
      s.stream_has_bframes, s.sps_patched, tag, s.rx_fps, s.rx_bitrate_bps / 1000.0,
      static_cast<unsigned long long>(s.frames_decoded), s.decoded_fps,
      static_cast<unsigned long long>(s.frames_published), static_cast<unsigned long long>(s.frames_lost),
      static_cast<unsigned long long>(s.frames_recovered_fec),
      static_cast<unsigned long long>(s.fragments_received),
      static_cast<unsigned long long>(s.fragments_lost), s.keyframe_requests, tag, s.rtt_ms,
      s.rtt_min_ms, static_cast<long long>(s.clock_offset_us), s.jitter_ms, s.delay_gradient_ms,
      tag, s.arrival_to_decoded_ms_avg, s.arrival_to_decoded_ms_p95, s.capture_to_publish_ms_avg,
      s.capture_to_publish_ms_p95, s.capture_to_arrival_ms_avg, s.decode_ms_avg, s.readback_ms_avg,
      s.decoder_delay_frames_avg, s.decoder_delay_frames_max);
  std::fflush(stdout);
}

struct Scenario {
  const char* name = "";
  VideoTransport transport = VideoTransport::Tcp;
  double loss = 0;
  int fec = 0;
  uint32_t bframes = 0;
  bool patchSps = true;
  uint32_t width = 1280, height = 720, bitrate = 4000000;
  uint32_t needFrames = 90;
  int timeoutMs = 30000;
  bool checkShm = true;
  uint32_t jitterMs = 0;
  DecoderMode decoder = DecoderMode::Auto;
  bool inlinePs = true;          // Android-like IDRs (SPS/PPS inline, Key|Config)
  const char* replay = nullptr;  // tests/data file: real-phone stream (no counter strip)
  uint32_t replayFps = 30;
  int16_t rotation = 0;          // VideoFormat rotation hint (output w/h swap for 90/270)
  bool overload = false;         // stream faster than this PC can present: latency must stay bounded
  CipherPreference cipher = CipherPreference::Auto;
  uint32_t udpAttack = 0;        // fake phone: tampered copy + replay after every Nth datagram
  fake::FakePhone::Counters* phoneOut = nullptr;  // phone counters at the end (optional)
};

std::string dataFile(const char* name) { return std::string(OWC_TEST_DATA_DIR) + "/" + name; }

ClientStats runScenario(const Scenario& sc) {
  char* only = nullptr;
  size_t onlyLen = 0;
  if (_dupenv_s(&only, &onlyLen, "OWC_IT_ONLY") == 0 && only) {
    const bool skip = !strstr(sc.name, only);
    free(only);
    if (skip) return ClientStats();
  }
  std::printf("--- scenario %s ---\n", sc.name);
  fake::FakePhoneConfig pc;
  pc.bind_address = "127.0.0.1";
  pc.control_port = pc.udp_port = pc.discovery_port = 0;
  pc.width = sc.width;
  pc.height = sc.height;
  pc.bitrate = sc.bitrate;
  pc.bframes = sc.bframes;
  pc.loss_percent = sc.loss;
  pc.fec_percent = sc.fec;
  pc.keyframe_interval_ms = 0;  // keyframes only at start / on request
  pc.inline_param_sets = sc.inlinePs;
  pc.rotation_deg = sc.rotation;
  pc.udp_attack_every = sc.udpAttack;
  if (sc.replay) {
    pc.replay_file = dataFile(sc.replay);
    pc.replay_fps = sc.replayFps;
  }
  const bool strip = sc.replay == nullptr;
  pc.logger = [](const std::string& s) { std::printf("    phone: %s\n", s.c_str()); };
  fake::FakePhone phone;
  std::string err;
  if (!phone.start(preload(pc), err)) {
    CHECK_MSG(false, "fakephone start: %s", err.c_str());
    return ClientStats();
  }
  Recorder rec;
  ClientOptions o;
  o.udp_bind_address = "127.0.0.1";
  o.auto_reconnect = false;
  o.patch_sps = sc.patchSps;
  o.jitter_buffer_ms = sc.jitterMs;
  o.decoder_mode = sc.decoder;
  o.cipher = sc.cipher;
  Client client(&rec, o);
  ConnectParams cp;
  cp.host = "127.0.0.1";
  cp.port = phone.controlPort();
  authorize(cp, phone);
  cp.transport = sc.transport;
  cp.client_name = "integration-test";
  CHECK(client.connect(cp));

  ShmReader reader;
  ShmCheck shm;
  std::vector<uint8_t> buf(OWC_SHM_SLOT_BYTES);
  uint64_t lastSeen = 0;
  const int64_t t0 = detail::steadyMs();
  while (detail::steadyMs() - t0 < sc.timeoutMs) {
    if (static_cast<uint32_t>(rec.frames.load()) >= sc.needFrames + 10) break;
    if (client.state() == ClientState::Failed) break;
    if (sc.checkShm && (reader.isOpen() || reader.open())) {
      ShmReader::FrameInfo fi;
      if (reader.readLatest(lastSeen, buf.data(), buf.size(), fi)) {
        lastSeen = fi.frame_number;
        ++shm.frames;
        shm.colorMatrix = fi.color_matrix;
        shm.fullRange = fi.full_range;
        uint32_t v;
        if (fake::readCounterStrip(buf.data(), fi.width, fi.width, fi.height, v)) {
          ++shm.readable;
          if (shm.have && v <= shm.last) ++shm.nonIncreasing;
          shm.last = v;
          shm.have = true;
        }
        const double ms = static_cast<double>(qpcNow() - fi.publish_qpc) * 1000.0 /
                          static_cast<double>(detail::qpcFreq());
        if (ms > shm.maxPublishToRead) shm.maxPublishToRead = ms;
      }
    }
    sleepMs(1);
  }
  const ClientStats st = client.stats();
  printStats(sc.name, st);
  for (const LogEntry& l : client.logs(0))
    if (l.level >= LogLevel::Warn)
      std::printf("    log[%s]: %s\n", l.source == LogSource::Phone ? "phone" : "pc", l.text.c_str());
  auto pcnt = phone.counters();
  if (sc.phoneOut) *sc.phoneOut = pcnt;
  CHECK_MSG(st.cipher == expectedCipher(sc.cipher) && pcnt.last_cipher == static_cast<uint8_t>(st.cipher),
            "cipher %s (phone %u), expected %s", toString(st.cipher), pcnt.last_cipher,
            toString(expectedCipher(sc.cipher)));
  CHECK_MSG(pcnt.tcp_open_failures == 0 && pcnt.punches_rejected == 0, "phone: %llu bad records, %llu bad punches",
            static_cast<unsigned long long>(pcnt.tcp_open_failures),
            static_cast<unsigned long long>(pcnt.punches_rejected));
  if (st.cipher != Cipher::None && st.frames_received > 0) CHECK(st.decrypt_us_avg > 0);
  if (st.cipher == Cipher::None) CHECK(st.decrypt_us_avg == 0 && st.udp_auth_failures == 0);
  if (!sc.udpAttack) CHECK_MSG(st.udp_auth_failures == 0, "udp auth failures %llu",
                               static_cast<unsigned long long>(st.udp_auth_failures));
  std::printf("  [%s] phone: encoded %llu sent %llu key %llu dgrams %llu dropped %llu fec %llu kfreq %llu rr %llu\n",
              sc.name, static_cast<unsigned long long>(pcnt.frames_encoded),
              static_cast<unsigned long long>(pcnt.frames_sent), static_cast<unsigned long long>(pcnt.keyframes),
              static_cast<unsigned long long>(pcnt.datagrams_sent), static_cast<unsigned long long>(pcnt.datagrams_dropped),
              static_cast<unsigned long long>(pcnt.fec_datagrams), static_cast<unsigned long long>(pcnt.keyframe_requests),
              static_cast<unsigned long long>(pcnt.receiver_reports));
  {
    std::lock_guard<std::mutex> lk(rec.mx);
    std::printf("  [%s] decoded frames %d, strip readable %zu, unreadable %u, non-increasing %u, out %ux%u\n",
                sc.name, rec.frames.load(), rec.counters.size(), rec.unreadable, rec.nonIncreasing,
                rec.lastW, rec.lastH);
    CHECK(rec.connected && rec.caps && rec.format);
    const bool swap = sc.rotation == 90 || sc.rotation == 270;
    CHECK_MSG(rec.lastW == (swap ? sc.height : sc.width) && rec.lastH == (swap ? sc.width : sc.height),
              "output %ux%u", rec.lastW, rec.lastH);
    if (strip) {
      CHECK_MSG(rec.counters.size() >= sc.needFrames, "only %zu frames decoded", rec.counters.size());
      CHECK_MSG(rec.nonIncreasing == 0, "%u non-increasing counters", rec.nonIncreasing);
      CHECK_MSG(rec.unreadable <= rec.counters.size() / 20, "%u unreadable strips", rec.unreadable);
    } else {
      // real-phone stream: every received frame must be decoded, no keyframe loop
      CHECK_MSG(static_cast<uint32_t>(rec.frames.load()) >= sc.needFrames, "only %d frames decoded",
                rec.frames.load());
      if (!sc.overload) {
        CHECK_MSG(st.frames_decoded + 3 >= st.frames_received, "decoded %llu of %llu received",
                  static_cast<unsigned long long>(st.frames_decoded),
                  static_cast<unsigned long long>(st.frames_received));
        CHECK_MSG(st.keyframe_requests <= 2, "%u keyframe requests", st.keyframe_requests);
      } else {
        // latency-first: bounded capture->publish even though the PC can't keep up
        std::printf("  [%s] OVERLOAD rx %.1f fps, decoded %.1f fps, published %.1f fps, not presented "
                    "%llu, backlog drops %llu, cap->pub avg %.1f p95 %.1f ms (decode %.1f ms)\n",
                    sc.name, st.rx_fps, st.decoded_fps, st.published_fps,
                    static_cast<unsigned long long>(st.frames_not_presented),
                    static_cast<unsigned long long>(st.backlog_drops), st.capture_to_publish_ms_avg,
                    st.capture_to_publish_ms_p95, st.decode_ms_avg);
        // avg always bounded; p95 tight except for forced HW on a slow-read-back
        // iGPU (GPU stalls, notably in Debug builds): there only a sanity bound
        // (the original bug was 7-20 SECONDS).
        const double bound = 150.0 + 2 * st.decode_ms_avg;
        CHECK_MSG(st.capture_to_publish_ms_avg < bound, "capture->publish avg %.1f ms (decode %.1f ms)",
                  st.capture_to_publish_ms_avg, st.decode_ms_avg);
        const double p95Bound = sc.decoder == DecoderMode::Hardware ? 600.0 : bound;
        CHECK_MSG(st.capture_to_publish_ms_p95 < p95Bound, "capture->publish p95 %.1f ms (decode %.1f ms)",
                  st.capture_to_publish_ms_p95, st.decode_ms_avg);
        // Published fps depends on CPU headroom (parallel builds can halve it); the
        // property under test is "keeps presenting with bounded latency".
        CHECK_MSG(st.published_fps >= 4.0, "published only %.1f fps", st.published_fps);
      }
    }
  }
  if (sc.checkShm) {
    std::printf("  [%s] shm: frames read %u, strip readable %u, non-increasing %u, max publish->read %.2f ms\n",
                sc.name, shm.frames, shm.readable, shm.nonIncreasing, shm.maxPublishToRead);
    if (strip) CHECK_MSG(shm.readable >= sc.needFrames / 2, "shm readable frames %u", shm.readable);
    else CHECK_MSG(shm.frames >= sc.needFrames / 2, "shm frames %u", shm.frames);
    std::printf("  [%s] colour: frame matrix %u full %d | shm matrix %u full %d | stats %u\n",
                sc.name, rec.lastColor, rec.lastFull ? 1 : 0, shm.colorMatrix, shm.fullRange ? 1 : 0,
                st.color_matrix);
    CHECK(shm.colorMatrix == rec.lastColor && shm.fullRange == rec.lastFull);
    CHECK(st.shm_publishing && st.shm_error.empty());
    if (strip) CHECK(shm.nonIncreasing == 0);
  }
  CHECK(st.clock_synced);
  CHECK_MSG(std::llabs(st.clock_offset_us - pc.clock_offset_us) < 2000, "offset error %lld us",
            static_cast<long long>(st.clock_offset_us - pc.clock_offset_us));
  CHECK(st.capture_to_publish_ms_avg > 0 && st.capture_to_publish_ms_avg < 500);
  CHECK(pcnt.receiver_reports > 0);
  client.disconnect();
  for (int i = 0; i < 200 && client.state() != ClientState::Idle; ++i) sleepMs(5);
  CHECK(client.state() == ClientState::Idle);
  return st;
}

void testDiscovery() {
  fake::FakePhoneConfig pc;
  pc.bind_address = "127.0.0.1";
  pc.control_port = pc.udp_port = pc.discovery_port = 0;
  pc.device_name = "DiscoPhone";
  pc.logger = [](const std::string&) {};
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(preload(pc), err));
  Discovery::Options o;
  o.port = phone.discoveryPort();
  o.broadcast = false;
  o.extra_targets = {"127.0.0.1"};
  o.bind_address = "127.0.0.1";
  o.listen_announcements = false;  // never touch the real 47803 / LAN / %APPDATA% in tests
  o.subnet_sweep = false;
  o.persist_known = false;
  auto devs = Discovery::scanOnce(600, o);
  CHECK(devs.size() == 1);
  if (!devs.empty()) {
    CHECK(devs[0].ip == "127.0.0.1" && devs[0].name == "DiscoPhone" && devs[0].model == "OWC-Fake");
    CHECK(devs[0].control_port == phone.controlPort() && devs[0].udp_port == phone.udpPort());
    CHECK(!devs[0].busy);
  }
  // continuous mode + callback
  Discovery d;
  std::atomic<int> calls{0};
  o.interval_ms = 200;
  CHECK(d.start(o, [&](const std::vector<DiscoveredDevice>&) { ++calls; }));
  for (int i = 0; i < 100 && d.devices().empty(); ++i) sleepMs(10);
  CHECK(d.devices().size() == 1);
  CHECK(calls >= 1);
  d.stop();
  auto bc = Discovery::broadcastAddresses();
  std::printf("  broadcast addresses:");
  for (auto& b : bc) std::printf(" %s", b.c_str());
  std::printf("\n");
}

void testBusyAndNoResponse() {
  fake::FakePhoneConfig pc;
  pc.bind_address = "127.0.0.1";
  pc.control_port = pc.udp_port = pc.discovery_port = 0;
  pc.enable_discovery = false;
  pc.logger = [](const std::string&) {};
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(preload(pc), err));
  ClientOptions o;
  o.auto_reconnect = false;
  o.auto_start_stream = false;
  Recorder r1, r2;
  Client c1(&r1, o), c2(&r2, o);
  ConnectParams cp;
  cp.port = phone.controlPort();
  authorize(cp, phone);
  c1.connect(cp);
  for (int i = 0; i < 300 && c1.state() != ClientState::Connected; ++i) sleepMs(5);
  CHECK(c1.state() == ClientState::Connected);
  DeviceInfo di;
  CHECK(c1.deviceInfo(di) && di.session_id != 0 && di.model == "OWC-Fake");
  Capabilities caps;
  for (int i = 0; i < 100 && !c1.capabilities(caps); ++i) sleepMs(5);
  CHECK(!caps.params.empty() && caps.cameras.size() == 1);
  // setParam round trip
  CHECK(c1.setParam(ParamId::Bitrate, ParamValue::Int(2500000)));
  ParamValue pv;
  for (int i = 0; i < 100; ++i) {
    if (c1.paramValue(ParamId::Bitrate, pv) && pv.i == 2500000) break;
    sleepMs(5);
  }
  CHECK(pv.i == 2500000);
  c2.connect(cp);
  for (int i = 0; i < 400 && c2.state() != ClientState::Failed; ++i) sleepMs(5);
  CHECK(c2.state() == ClientState::Failed);
  CHECK(c2.lastDisconnectReason() == DisconnectReason::Busy);
  c1.disconnect();

  // NoResponse: a TCP listener that accepts but never answers (like adb forward
  // with the app not running).
  detail::winsockInit();
  SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  bind(l, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  listen(l, 8);
  int al = sizeof(a);
  getsockname(l, reinterpret_cast<sockaddr*>(&a), &al);
  ClientOptions o3 = o;
  o3.handshake_timeout_ms = 200;
  o3.handshake_attempts = 2;
  Recorder r3;
  Client c3(&r3, o3);
  ConnectParams cp3;
  cp3.client_id = kTestClientId;
  cp3.port = ntohs(a.sin_port);
  const int64_t t0 = detail::steadyMs();
  c3.connect(cp3);
  for (int i = 0; i < 400 && c3.state() != ClientState::Failed; ++i) sleepMs(5);
  CHECK(c3.state() == ClientState::Failed);
  CHECK(c3.lastDisconnectReason() == DisconnectReason::NoResponse);
  std::printf("  NoResponse after %lld ms: %s\n", static_cast<long long>(detail::steadyMs() - t0),
              r3.lastMsg.c_str());
  closesocket(l);

  // ConnectFailed: nothing listening
  Recorder r4;
  Client c4(&r4, o);
  ConnectParams cp4;
  cp4.client_id = kTestClientId;
  cp4.port = Adb::pickFreeTcpPort();
  c4.connect(cp4);
  for (int i = 0; i < 400 && c4.state() != ClientState::Failed; ++i) sleepMs(5);
  CHECK(c4.state() == ClientState::Failed);
  CHECK(c4.lastDisconnectReason() == DisconnectReason::ConnectFailed);
}

void testTransformAndControls() {
  fake::FakePhoneConfig pc;
  pc.bind_address = "127.0.0.1";
  pc.control_port = pc.udp_port = pc.discovery_port = 0;
  pc.enable_discovery = false;
  pc.width = 640;
  pc.height = 480;
  pc.logger = [](const std::string&) {};
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(preload(pc), err));
  Recorder rec;
  ClientOptions o;
  o.auto_reconnect = false;
  o.publish_to_shm = false;
  OutputTransform t;
  t.rotation_deg = 90;
  o.transform = t;
  rec.rotateBack = 90;
  Client c(&rec, o);
  ConnectParams cp;
  cp.port = phone.controlPort();
  authorize(cp, phone);
  c.connect(cp);
  for (int i = 0; i < 1000 && rec.frames < 20; ++i) sleepMs(5);
  {
    std::lock_guard<std::mutex> lk(rec.mx);
    CHECK(rec.lastW == 480 && rec.lastH == 640 && rec.lastRot == 90);
    CHECK_MSG(rec.counters.size() >= 15, "rot90 readable %zu", rec.counters.size());
  }
  // latest-frame copy
  FrameCopy fc;
  CHECK(c.copyLatestFrame(fc, 0) && fc.width == 480 && fc.height == 640 && fc.nv12.size() == 480 * 640 * 3 / 2);
  CHECK(!c.copyLatestFrame(fc, fc.frame_number) || fc.frame_number > 0);
  // colour grading end to end (graded in place in the rotation buffer): the
  // black & white look reaches the frames, then switching it off passes through
  {
    ClientOptions go = c.options();
    go.color.enabled = true;
    go.color.lut = "builtin:bw";
    c.setOptions(go);
    ClientStats gs;
    for (int i = 0; i < 400; ++i) {
      gs = c.stats();
      if (gs.grade_active) break;
      sleepMs(5);
    }
    CHECK_MSG(gs.grade_active && gs.grade_lut_size == 33 && gs.grade_error.empty(),
              "grade active %d lut %u err '%s'", gs.grade_active, gs.grade_lut_size, gs.grade_error.c_str());
    std::printf("  grade: apply %.2f ms, bake %.1f ms, look '%s'\n", gs.grade_ms_avg, gs.grade_bake_ms,
                gs.grade_lut_title.c_str());
    sleepMs(100);  // a frame or two after the table switch
    CHECK(c.copyLatestFrame(fc, 0));
    int worst = 0;
    for (size_t i = static_cast<size_t>(fc.width) * fc.height; i < fc.nv12.size(); ++i)
      worst = std::max(worst, std::abs(fc.nv12[i] - 128));
    CHECK_MSG(worst <= 2, "graded bw frame: max |UV-128| %d", worst);
    go.color.enabled = false;
    c.setOptions(go);
    for (int i = 0; i < 400 && c.stats().grade_active; ++i) sleepMs(5);
    CHECK(!c.stats().grade_active);
  }
  // live resolution change via SetParam (new epoch)
  t.rotation_deg = 0;
  c.setTransform(t);
  rec.rotateBack = 0;
  c.setParam(ParamId::VideoWidth, ParamValue::Int(1280));
  c.setParam(ParamId::VideoHeight, ParamValue::Int(720));
  rec.reset();
  for (int i = 0; i < 1000; ++i) {
    {
      std::lock_guard<std::mutex> lk(rec.mx);
      if (rec.lastW == 1280 && rec.counters.size() > 10) break;
    }
    sleepMs(5);
  }
  {
    std::lock_guard<std::mutex> lk(rec.mx);
    CHECK_MSG(rec.lastW == 1280 && rec.lastH == 720, "after SetParam %ux%u", rec.lastW, rec.lastH);
  }
  VideoFormat vf;
  CHECK(c.videoFormat(vf) && vf.width == 1280 && vf.epoch >= 2);
  // stop / start stream
  CHECK(c.stopStream());
  for (int i = 0; i < 200 && c.state() != ClientState::Connected; ++i) sleepMs(5);
  CHECK(c.state() == ClientState::Connected);
  sleepMs(200);
  const int before = rec.frames;
  sleepMs(200);
  CHECK(rec.frames == before);
  CHECK(c.startStream());
  for (int i = 0; i < 400 && rec.frames < before + 10; ++i) sleepMs(5);
  CHECK(rec.frames >= before + 10);
  CHECK(c.requestKeyframe());
  auto logs = c.logs(0);
  CHECK(!logs.empty());
  bool phoneLog = false;
  for (auto& l : logs) phoneLog |= l.source == LogSource::Phone;
  CHECK(phoneLog);
  c.disconnect();
}

uint16_t freeUdpPort() {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  int l = sizeof(a);
  getsockname(s, reinterpret_cast<sockaddr*>(&a), &l);
  closesocket(s);
  return ntohs(a.sin_port);
}

// Screen-off phones: only announcements (kind=3) and unicast sweeps reach them.
void testDiscoveryAnnounceAndSweep() {
  // (a) announce only: the phone does not answer requests at all
  const uint16_t annPort = freeUdpPort();
  fake::FakePhoneConfig pc;
  pc.bind_address = "127.0.0.1";
  pc.control_port = pc.udp_port = pc.discovery_port = 0;
  pc.enable_discovery = false;
  pc.device_name = "AnnouncePhone";
  pc.announce_target = "127.0.0.1:" + std::to_string(annPort);
  pc.announce_interval_ms = 200;
  pc.logger = [](const std::string&) {};
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(preload(pc), err));
  Discovery::Options o;
  o.broadcast = false;
  o.subnet_sweep = false;
  o.persist_known = false;
  o.bind_address = "127.0.0.1";
  o.announce_bind_address = "127.0.0.1";
  o.announce_port = annPort;
  Discovery d;
  CHECK(d.start(o));
  CHECK(d.announceListening());
  for (int i = 0; i < 200 && d.devices().empty(); ++i) sleepMs(10);
  auto devs = d.devices();
  CHECK(devs.size() == 1);
  if (!devs.empty()) {
    CHECK(devs[0].via_announce && devs[0].name == "AnnouncePhone" && devs[0].ip == "127.0.0.1");
    CHECK(devs[0].control_port == phone.controlPort() && devs[0].udp_port == phone.udpPort());
  }
  auto known = d.knownAddresses();
  CHECK(known.size() == 1 && known[0] == "127.0.0.1");
  // a second listener on the same port (another OWC process) also works (SO_REUSEADDR)
  Discovery d2;
  CHECK(d2.start(o));
  d2.stop();
  d.stop();
  phone.stop();

  // (b) unicast sweep of a /29 on loopback: the phone answers unicast requests
  // only (no broadcast, no extra targets, no announcements)
  fake::FakePhoneConfig pc2 = pc;
  pc2.enable_discovery = true;
  pc2.announce_target.clear();
  pc2.device_name = "SweepPhone";
  fake::FakePhone phone2;
  CHECK(phone2.start(pc2, err));
  Discovery::Options s;
  s.port = phone2.discoveryPort();
  s.broadcast = false;
  s.listen_announcements = false;
  s.persist_known = false;
  s.bind_address = "127.0.0.1";
  s.subnet_sweep = true;
  s.sweep_min_prefix = 32;  // ignore the real interfaces: only the explicit subnet
  s.sweep_subnets = {"127.0.0.0/29"};
  auto found = Discovery::scanOnce(600, s);
  CHECK(found.size() == 1 && !found.empty() && found[0].name == "SweepPhone" && !found[0].via_announce);
  // known-address path + file persistence
  char tmp[MAX_PATH];
  GetTempPathA(MAX_PATH, tmp);
  const std::string file = std::string(tmp) + "owc_known_test_" + std::to_string(GetCurrentProcessId()) + ".txt";
  CHECK(Discovery::saveKnownAddresses({"10.0.0.1", "127.0.0.1"}, file));
  auto loaded = Discovery::loadKnownAddresses(file);
  CHECK(loaded.size() == 2 && loaded[1] == "127.0.0.1");
  Discovery::Options k = s;
  k.subnet_sweep = false;
  k.persist_known = true;
  k.known_file = file;
  auto viaKnown = Discovery::scanOnce(600, k);
  CHECK(viaKnown.size() == 1);
  DeleteFileA(file.c_str());
  for (auto& sn : Discovery::sweepSubnets())
    std::printf("  sweep subnet: %s on '%s' (%s)\n", sn.cidr.c_str(), sn.adapter.c_str(), sn.address.c_str());
}

// Codec negotiation with a fake HEVC-capable phone (its encoder is H.264 only).
ClientStats codecRun(const char* tag, const DecoderCapabilities* caps, int hevcMode,
                     Recorder& rec, uint64_t& logId, Client*& keep) {
  (void)keep;
  Client::overrideDecoderCapabilitiesForTest(caps);
  fake::FakePhoneConfig pc;
  pc.bind_address = "127.0.0.1";
  pc.control_port = pc.udp_port = pc.discovery_port = 0;
  pc.enable_discovery = false;
  pc.width = 640;
  pc.height = 480;
  pc.codec_mask = codecBit(Codec::H264) | codecBit(Codec::Hevc);
  pc.hevc_mode = hevcMode;
  pc.logger = [](const std::string&) {};
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(preload(pc), err));
  ClientOptions o;
  o.auto_reconnect = false;
  o.publish_to_shm = false;
  Client c(&rec, o);
  ConnectParams cp;
  cp.port = phone.controlPort();
  authorize(cp, phone);
  c.connect(cp);
  for (int i = 0; i < 1600 && rec.frames < 40; ++i) sleepMs(5);
  const ClientStats s = c.stats();
  std::printf("  [%s] frames %d, requested %s, selection '%s'\n", tag, rec.frames.load(),
              s.requested_codec == Codec::Hevc ? "HEVC" : "H.264", s.codec_selection.c_str());
  for (const LogEntry& l : c.logs(logId))
    if (l.text.find("codec") != std::string::npos || l.text.find("HEVC") != std::string::npos)
      std::printf("    log: %s\n", l.text.c_str());
  c.disconnect();
  Client::overrideDecoderCapabilitiesForTest(nullptr);
  return s;
}

void testCodecNegotiation() {
  DecoderCapabilities hevcHw;  // pretend this PC has a hardware HEVC decoder
  hevcHw.h264.hardware = hevcHw.h264.software = true;
  hevcHw.hevc.hardware = hevcHw.hevc.software = true;
  DecoderCapabilities noHevc = hevcHw;
  noHevc.hevc = DecoderCapabilities::Entry();
  uint64_t logId = 0;
  Client* keep = nullptr;
  {  // (a) PC can decode HEVC, phone advertises it but REJECTS SetParam -> H.264
    Recorder rec;
    const ClientStats s = codecRun("phone rejects HEVC", &hevcHw, 0, rec, logId, keep);
    CHECK(rec.frames >= 40);
    CHECK(s.requested_codec == Codec::H264);
    CHECK_MSG(s.codec_selection.find("HEVC failed earlier") != std::string::npos, "%s",
              s.codec_selection.c_str());
  }
  {  // (b) phone accepts HEVC but the stream can't be decoded as HEVC -> runtime fallback
    Recorder rec;
    const ClientStats s = codecRun("HEVC decode fails", &hevcHw, 1, rec, logId, keep);
    CHECK_MSG(rec.frames >= 40, "frames %d", rec.frames.load());
    CHECK(s.requested_codec == Codec::H264);
    CHECK(s.has_format && s.format.codec == Codec::H264);
    CHECK_MSG(s.codec_selection.find("HEVC failed earlier") != std::string::npos, "%s",
              s.codec_selection.c_str());
  }
  {  // (c) no HEVC decoder on this PC: Auto stays on H.264 with the install hint
    Recorder rec;
    const ClientStats s = codecRun("no HEVC decoder", &noHevc, 1, rec, logId, keep);
    CHECK(rec.frames >= 40);
    CHECK(s.requested_codec == Codec::H264);
    CHECK_MSG(s.codec_selection == "auto: H.264 (no HEVC decoder: install 'HEVC Video Extensions')",
              "%s", s.codec_selection.c_str());
  }
}

// ---------------------------------------------------------------- recording
struct Mp4Info {
  bool ok = false;
  uint32_t samples = 0, width = 0, height = 0;
  double duration_s = 0, fps = 0;
  bool firstKey = false;
  bool h264 = false;
  std::string err;
};

// Opens an MP4 with the MF Source Reader (no decoding: native compressed type).
Mp4Info inspectMp4(const std::string& path) {
  Mp4Info info;
  detail::MfThreadScope mf;
  Microsoft::WRL::ComPtr<IMFSourceReader> r;
  HRESULT hr = MFCreateSourceReaderFromURL(detail::utf8ToWide(path).c_str(), nullptr, &r);
  if (FAILED(hr)) {
    info.err = "open " + detail::hresultToString(hr);
    return info;
  }
  PROPVARIANT var;
  PropVariantInit(&var);
  if (SUCCEEDED(r->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                            MF_PD_DURATION, &var)))
    info.duration_s = static_cast<double>(var.uhVal.QuadPart) / 1e7;
  PropVariantClear(&var);
  Microsoft::WRL::ComPtr<IMFMediaType> t;
  const DWORD vs = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  if (FAILED(r->GetNativeMediaType(vs, 0, &t))) {
    info.err = "no video stream";
    return info;
  }
  UINT32 w = 0, h = 0, num = 0, den = 1;
  MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
  MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
  GUID sub{};
  t->GetGUID(MF_MT_SUBTYPE, &sub);
  info.width = w;
  info.height = h;
  info.fps = den ? static_cast<double>(num) / den : 0;
  info.h264 = sub == MFVideoFormat_H264 || sub == MFVideoFormat_H264_ES;
  r->SetCurrentMediaType(vs, nullptr, t.Get());
  for (;;) {
    DWORD idx = 0, flags = 0;
    LONGLONG ts = 0;
    Microsoft::WRL::ComPtr<IMFSample> s;
    if (FAILED(r->ReadSample(vs, 0, &idx, &flags, &ts, &s))) break;
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    if (!s) continue;
    if (info.samples == 0) {
      UINT32 clean = 0;
      s->GetUINT32(MFSampleExtension_CleanPoint, &clean);
      info.firstKey = clean != 0;
    }
    ++info.samples;
  }
  info.ok = info.samples > 0;
  return info;
}

std::string tempPath(const char* name) {
  char tmp[MAX_PATH];
  GetTempPathA(MAX_PATH, tmp);
  return std::string(tmp) + "owc_rec_" + std::to_string(GetCurrentProcessId()) + "_" + name;
}

void printRec(const char* tag, const RecordStatus& st, const Mp4Info& m) {
  std::printf("  [%s] written %llu (received %llu, dropped %llu, missing %llu, gaps %u), capture %.1f fps "
              "over %.2f s, media %.2f s, %.2f MB, slowmo x%.1f | MP4: %u samples %ux%u %.0f fps %.2f s "
              "firstKey %d %s\n",
              tag, static_cast<unsigned long long>(st.frames_written),
              static_cast<unsigned long long>(st.frames_received),
              static_cast<unsigned long long>(st.frames_dropped),
              static_cast<unsigned long long>(st.frames_missing), st.gaps, st.capture_fps,
              st.capture_span_s, st.media_duration_s, static_cast<double>(st.bytes) / 1e6, st.slowmo_factor,
              m.samples, m.width, m.height, m.fps, m.duration_s, m.firstKey ? 1 : 0, m.err.c_str());
  for (auto& f : st.files) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExA(f.c_str(), GetFileExInfoStandard, &fa);
    std::printf("    file %s (%lu bytes)\n", f.c_str(), fa.nFileSizeLow);
  }
  if (!st.last_error.empty()) std::printf("    last error: %s\n", st.last_error.c_str());
}

void testRecording() {
  // Real Note 9 720p frames replayed at 240 fps (slow-motion source).
  fake::FakePhoneConfig pc;
  pc.bind_address = "127.0.0.1";
  pc.control_port = pc.udp_port = pc.discovery_port = 0;
  pc.enable_discovery = false;
  pc.replay_file = std::string(OWC_TEST_DATA_DIR) + "/note9_hs_720p60.h264";
  pc.replay_fps = 240;
  pc.rotation_deg = 90;
  pc.logger = [](const std::string&) {};
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(preload(pc), err));
  Recorder rec;
  ClientOptions o;
  o.auto_reconnect = false;
  o.publish_to_shm = false;
  o.max_present_fps = 60;  // decimated preview, as prepareSlowMotion() sets
  Client c(&rec, o);
  ConnectParams cp;
  cp.port = phone.controlPort();
  authorize(cp, phone);
  c.connect(cp);
  for (int i = 0; i < 1000 && rec.frames < 20; ++i) sleepMs(5);
  CHECK(rec.frames >= 20);

  // (1) real time, ~2 s
  RecordOptions ro;
  ro.path = tempPath("realtime.mp4");
  CHECK(c.startRecording(ro, &err));
  CHECK(!c.startRecording(ro, &err));  // already recording
  const int64_t t0 = detail::steadyMs();
  sleepMs(2000);
  RecordStatus st = c.stopRecording();
  const double wall = static_cast<double>(detail::steadyMs() - t0) / 1000.0;
  Mp4Info m = inspectMp4(ro.path);
  printRec("realtime 240 fps", st, m);
  CHECK(m.ok && m.firstKey && m.h264 && m.width == 1280 && m.height == 720);
  CHECK(st.last_error.empty() && st.files.size() == 1);
  CHECK_MSG(m.samples == st.frames_written, "mp4 %u vs written %llu", m.samples,
            static_cast<unsigned long long>(st.frames_written));
  CHECK_MSG(st.frames_written > 400, "only %llu frames at 240 fps", static_cast<unsigned long long>(st.frames_written));
  CHECK(st.frames_dropped == 0 && st.frames_missing == 0);
  CHECK_MSG(std::fabs(m.duration_s - st.capture_span_s) < 0.1 && m.duration_s > wall - 0.6 && m.duration_s < wall + 0.2,
            "duration %.2f s vs capture span %.2f s, wall %.2f s", m.duration_s, st.capture_span_s, wall);
  CHECK_MSG(st.capture_fps > 200 && st.capture_fps < 260, "capture fps %.1f", st.capture_fps);

  // (2) slow motion at 30 fps, ~1 s of capture -> ~8 s of playback
  RecordOptions so;
  so.path = tempPath("slowmo.mp4");
  so.mode = RecordMode::SlowMotion;
  so.slowmo_output_fps = 30;
  CHECK(c.startRecording(so, &err));
  sleepMs(1000);
  st = c.stopRecording();
  m = inspectMp4(so.path);
  printRec("slow motion 240->30", st, m);
  CHECK(m.ok && m.firstKey && m.samples == st.frames_written);
  CHECK_MSG(std::fabs(m.duration_s - static_cast<double>(m.samples) / 30.0) < 0.05,
            "slowmo duration %.3f s for %u frames", m.duration_s, m.samples);
  CHECK(std::fabs(m.fps - 30.0) < 0.5);
  CHECK(st.slowmo_factor > 6.5 && st.slowmo_factor < 8.7);

  // (3) present=false: decoding stops while recording, resumes after
  RecordOptions np = ro;
  np.path = tempPath("nopresent.mp4");
  np.present = false;
  CHECK(c.startRecording(np, &err));
  sleepMs(300);
  const int before = rec.frames;
  sleepMs(500);
  const int during = rec.frames - before;
  st = c.stopRecording();
  for (int i = 0; i < 400 && rec.frames < before + during + 20; ++i) sleepMs(5);
  m = inspectMp4(np.path);
  std::printf("  [present=false] frames presented during recording %d, written %llu\n", during,
              static_cast<unsigned long long>(st.frames_written));
  CHECK(during <= 1 && st.frames_written > 100 && m.samples == st.frames_written);
  CHECK(rec.frames >= before + during + 20);  // preview resumed
  c.disconnect();
  for (auto& f : {ro.path, so.path, np.path}) DeleteFileA(f.c_str());
}

// ---------------------------------------------------------------- recording setup
struct SetupPhone {
  fake::FakePhone phone;
  Recorder rec;
  std::unique_ptr<Client> c;
  bool start(uint32_t codecMask, int hevcMode, float hevcFpsScale) {
    fake::FakePhoneConfig pc;
    pc.bind_address = "127.0.0.1";
    pc.control_port = pc.udp_port = pc.discovery_port = 0;
    pc.enable_discovery = false;
    pc.width = 640;
    pc.height = 480;
    pc.codec_mask = codecMask;
    pc.hevc_mode = hevcMode;
    pc.hevc_stats_fps_scale = hevcFpsScale;
    pc.logger = [](const std::string&) {};
    std::string err;
    if (!phone.start(preload(pc), err)) return false;
    ClientOptions o;
    o.auto_reconnect = false;
    o.publish_to_shm = false;
    c.reset(new Client(&rec, o));
    ConnectParams cp;
    cp.port = phone.controlPort();
    authorize(cp, phone);
    c->connect(cp);
    for (int i = 0; i < 1000 && rec.frames < 15; ++i) sleepMs(5);
    return rec.frames >= 15;
  }
  int64_t param(ParamId id) {
    ParamValue v;
    return c->paramValue(id, v) ? v.i : -999;
  }
};

void testRecordSetupQuality() {
  SetupPhone t;
  CHECK(t.start(codecBit(Codec::H264), 0, 1.0f));
  const ParamId knobs[] = {ParamId::Bitrate, ParamId::BitrateMode, ParamId::AdaptiveBitrate,
                           ParamId::LatencyDropPolicy, ParamId::MaxSendQueueFrames,
                           ParamId::KeyframeIntervalMs, ParamId::FecPercent};
  std::vector<int64_t> before;
  for (ParamId id : knobs) before.push_back(t.param(id));
  RecordPreset p;  // Quality, RealTime, keep 640x480@30
  std::string err;
  CHECK(t.c->prepareRecording(p, &err));
  sleepMs(800);
  RecordSetupStatus st = t.c->recordSetupStatus();
  std::printf("  [quality] %s\n", st.summary.c_str());
  CHECK(st.active && st.codec == Codec::H264 && st.preview_available && st.width == 640 && st.fps == 30);
  CHECK(t.param(ParamId::Bitrate) == static_cast<int64_t>(detail::qualityBitrate(640, 480, 30)));
  CHECK(t.param(ParamId::BitrateMode) == 1 && t.param(ParamId::AdaptiveBitrate) == 0 &&
        t.param(ParamId::LatencyDropPolicy) == 1 && t.param(ParamId::MaxSendQueueFrames) == 32 &&
        t.param(ParamId::KeyframeIntervalMs) == 2000 && t.param(ParamId::FecPercent) == 0);
  const int f0 = t.rec.frames;
  sleepMs(500);
  CHECK(t.rec.frames > f0 + 5);  // preview still running (H.264 decodable)
  CHECK(t.c->endRecordingSetup());
  sleepMs(800);
  CHECK(!t.c->recordSetupStatus().active);
  for (size_t i = 0; i < before.size(); ++i)
    CHECK_MSG(t.param(knobs[i]) == before[i], "param %s: %lld after restore, %lld before",
              owc::toString(knobs[i]), static_cast<long long>(t.param(knobs[i])),
              static_cast<long long>(before[i]));
  CHECK(!t.c->endRecordingSetup());  // nothing to restore
  t.c->disconnect();
}

void testRecordSetupHevc(bool sustain) {
  DecoderCapabilities noHevc;  // this PC: H.264 only
  noHevc.h264.hardware = noHevc.h264.software = true;
  Client::overrideDecoderCapabilitiesForTest(&noHevc);
  SetupPhone t;
  // hevc_mode 1: the phone "switches" to HEVC (H.264 bits labelled HEVC; fine:
  // with no PC decoder the preview is off and nothing decodes them).
  CHECK(t.start(codecBit(Codec::H264) | codecBit(Codec::Hevc), 1, sustain ? 1.0f : 0.5f));
  RecordPreset p;
  std::string err;
  CHECK(t.c->prepareRecording(p, &err));
  RecordSetupStatus st = t.c->recordSetupStatus();
  std::printf("  [hevc %s] %s\n", sustain ? "ok" : "slow", st.summary.c_str());
  CHECK(st.codec == Codec::Hevc && !st.preview_available);
  CHECK(st.summary.find("sem preview (HEVC sem decoder)") != std::string::npos);
  sleepMs(1000);
  VideoFormat vf;
  CHECK(t.c->videoFormat(vf) && vf.codec == Codec::Hevc);
  const int f0 = t.rec.frames;
  sleepMs(600);
  CHECK_MSG(t.rec.frames <= f0 + 1, "preview should be off: %d -> %d", f0, t.rec.frames.load());
  if (!sustain) {
    // phone reports encode_fps = 50% of 30 -> fallback to H.264 within ~3 s
    for (int i = 0; i < 100 && t.c->recordSetupStatus().codec != Codec::H264; ++i) sleepMs(50);
    st = t.c->recordSetupStatus();
    std::printf("  [hevc slow] after watch: %s\n", st.summary.c_str());
    CHECK(st.codec == Codec::H264 && st.preview_available);
    CHECK(st.summary.find("HEVC não sustentou 30 fps") != std::string::npos);
    const int f1 = t.rec.frames;
    for (int i = 0; i < 200 && t.rec.frames < f1 + 10; ++i) sleepMs(10);
    CHECK(t.rec.frames >= f1 + 10);  // preview back on H.264
    CHECK(t.c->videoFormat(vf) && vf.codec == Codec::H264);
  } else {
    sleepMs(2500);  // watch window passes without fallback
    CHECK(t.c->recordSetupStatus().codec == Codec::Hevc);
  }
  CHECK(t.c->endRecordingSetup());
  sleepMs(800);
  CHECK(t.param(ParamId::VideoCodec) == 0);  // restored
  const int f2 = t.rec.frames;
  for (int i = 0; i < 300 && t.rec.frames < f2 + 10; ++i) sleepMs(10);
  CHECK_MSG(t.rec.frames >= f2 + 10, "preview did not resume after endRecordingSetup");
  t.c->disconnect();
  Client::overrideDecoderCapabilitiesForTest(nullptr);
}

// Resolution change mid-recording: the file is closed and name_2.mp4 started.
void testRecordingEpochSplit() {
  fake::FakePhoneConfig pc;
  pc.bind_address = "127.0.0.1";
  pc.control_port = pc.udp_port = pc.discovery_port = 0;
  pc.enable_discovery = false;
  pc.width = 640;
  pc.height = 480;
  pc.logger = [](const std::string&) {};
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(preload(pc), err));
  Recorder rec;
  ClientOptions o;
  o.auto_reconnect = false;
  o.publish_to_shm = false;
  Client c(&rec, o);
  ConnectParams cp;
  cp.port = phone.controlPort();
  authorize(cp, phone);
  c.connect(cp);
  for (int i = 0; i < 1000 && rec.frames < 10; ++i) sleepMs(5);
  RecordOptions ro;
  ro.path = tempPath("split.mp4");
  CHECK(c.startRecording(ro, &err));
  sleepMs(1000);
  c.setParam(ParamId::VideoWidth, ParamValue::Int(1280));
  c.setParam(ParamId::VideoHeight, ParamValue::Int(720));
  sleepMs(1500);
  const RecordStatus st = c.stopRecording();
  std::printf("  [split] files %zu, written %llu, last error '%s'\n", st.files.size(),
              static_cast<unsigned long long>(st.frames_written), st.last_error.c_str());
  CHECK(st.files.size() >= 2);
  uint32_t total = 0;
  for (size_t i = 0; i < st.files.size(); ++i) {
    const Mp4Info m = inspectMp4(st.files[i]);
    std::printf("    %s: %u samples %ux%u %.2f s firstKey %d\n", st.files[i].c_str(), m.samples, m.width,
                m.height, m.duration_s, m.firstKey ? 1 : 0);
    CHECK(m.ok && m.firstKey);
    total += m.samples;
    DeleteFileA(st.files[i].c_str());
  }
  CHECK(total == st.frames_written);
  if (st.files.size() >= 2) {
    CHECK(st.files[1].find("split_2.mp4") != std::string::npos);
  }
  c.disconnect();
}

// Another producer owns the bus: the client keeps decoding, reports shm_error,
// and takes over within ~2 s once the bus is released.
void testShmBusOwnedByOther() {
  ShmWriter other;
  CHECK(other.open());
  fake::FakePhoneConfig pc;
  pc.bind_address = "127.0.0.1";
  pc.control_port = pc.udp_port = pc.discovery_port = 0;
  pc.enable_discovery = false;
  pc.width = 640;
  pc.height = 480;
  pc.logger = [](const std::string&) {};
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(preload(pc), err));
  Recorder rec;
  ClientOptions o;
  o.auto_reconnect = false;
  Client c(&rec, o);
  ConnectParams cp;
  cp.port = phone.controlPort();
  authorize(cp, phone);
  c.connect(cp);
  for (int i = 0; i < 1000 && rec.frames < 15; ++i) sleepMs(5);
  ClientStats s = c.stats();
  CHECK(rec.frames >= 15);
  CHECK(!s.shm_publishing);
  CHECK_MSG(s.shm_error.find("another OWC producer") != std::string::npos, "shm_error='%s'",
            s.shm_error.c_str());
  std::printf("  shm_error: %s\n", s.shm_error.c_str());
  other.close();
  for (int i = 0; i < 400 && !c.stats().shm_publishing; ++i) sleepMs(10);
  s = c.stats();
  CHECK(s.shm_publishing && s.shm_error.empty());
  ShmReader r;
  CHECK(r.open() && r.header()->producer_pid == GetCurrentProcessId());
  c.disconnect();
}

// ---------------------------------------------------------------- pairing (protocol v3)
constexpr const char* kCode = "246810";

struct PairRec : Recorder {
  std::mutex pmx;
  std::vector<PairingInfo> prompts;  // onPairing
  std::vector<PhoneId> rejected;     // onKeyRejected
  bool paired = false, pairedBeforeConnected = false;
  PhoneId pairedPhone{};
  Key32 pairedKey{};
  DeviceInfo pairedInfo;
  std::atomic<int> connects{0};

  void onPairing(const PairingInfo& i) override {
    std::lock_guard<std::mutex> lk(pmx);
    prompts.push_back(i);
  }
  void onPaired(const PhoneId& id, const Key32& key, const DeviceInfo& di) override {
    std::lock_guard<std::mutex> lk(pmx);
    paired = true;
    pairedBeforeConnected = connects.load() == 0;
    pairedPhone = id;
    pairedKey = key;
    pairedInfo = di;
  }
  void onKeyRejected(const PhoneId& id) override {
    std::lock_guard<std::mutex> lk(pmx);
    rejected.push_back(id);
  }
  void onConnected(const DeviceInfo& d) override {
    Recorder::onConnected(d);
    ++connects;
  }
  size_t promptCount() {
    std::lock_guard<std::mutex> lk(pmx);
    return prompts.size();
  }
  bool sawState(ClientState s) {
    std::lock_guard<std::mutex> lk(mx);
    for (ClientState x : states)
      if (x == s) return true;
    return false;
  }
};

// A phone that knows nobody, with a fixed code.
fake::FakePhoneConfig pairPhoneConfig() {
  fake::FakePhoneConfig pc;
  pc.bind_address = "127.0.0.1";
  pc.control_port = pc.udp_port = pc.discovery_port = 0;
  pc.enable_discovery = false;
  pc.width = 640;
  pc.height = 480;
  pc.pair_code = kCode;
  pc.logger = [](const std::string& s) { std::printf("    phone: %s\n", s.c_str()); };
  return pc;
}

ClientOptions pairOptions(bool autoReconnect) {
  ClientOptions o;
  o.auto_reconnect = autoReconnect;
  o.reconnect_delay_ms = 200;
  o.publish_to_shm = false;
  return o;
}

ConnectParams freshParams(const fake::FakePhone& phone, const ClientId& id) {
  ConnectParams cp;
  cp.port = phone.controlPort();
  cp.client_id = id;
  cp.client_name = "pairing-test";
  return cp;
}

bool waitFor(const std::function<bool()>& pred, int ms) {
  const int64_t t0 = detail::steadyMs();
  while (detail::steadyMs() - t0 < ms) {
    if (pred()) return true;
    sleepMs(5);
  }
  return pred();
}

bool waitState(Client& c, ClientState s, int ms) {
  return waitFor([&] { return c.state() == s; }, ms);
}

bool waitSession(Client& c, int ms) {
  return waitFor([&] {
    const ClientState s = c.state();
    return s == ClientState::Connected || s == ClientState::Streaming;
  }, ms);
}

void testConnectRequiresClientId() {
  Client c(nullptr, pairOptions(false));
  ConnectParams cp;  // client_id all zeros
  cp.port = Adb::pickFreeTcpPort();  // nothing listens: never reaches a real phone
  CHECK(!c.connect(cp));
  const ClientId a = newClientId(), b = newClientId();
  CHECK(a != ClientId{} && b != ClientId{} && a != b);
  cp.client_id = a;
  CHECK(c.connect(cp));
  c.disconnect();
}

// First connection of a new PC: code shown, typed, key delivered; nothing else
// (no Ping, no StartStream) reaches the phone before AuthResult Ok.
void testPairingFirstTime() {
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(pairPhoneConfig(), err));
  PairRec rec;
  Client c(&rec, pairOptions(true));
  const ClientId me = newClientId();
  CHECK(c.connect(freshParams(phone, me)));
  CHECK(waitState(c, ClientState::Pairing, 3000));
  PairingInfo pi = c.pairingInfo();
  CHECK(pi.active && !pi.code_pending && !pi.wrong_code);
  CHECK(pi.phone_id == phone.phoneId() && pi.device_name == "FakePhone" && pi.model == "OWC-Fake");
  CHECK_MSG(pi.attempts_left == kPairCodeAttempts && pi.code_lifetime_s == kPairCodeLifetimeS,
            "attempts %u lifetime %u", pi.attempts_left, pi.code_lifetime_s);
  CHECK_MSG(pi.ms_left > 110000 && pi.ms_left <= 120000u, "ms_left %u", pi.ms_left);
  CHECK(rec.promptCount() == 1);
  CHECK(phone.lastPairCode() == kCode);
  DeviceInfo di;
  CHECK(!c.deviceInfo(di) && !rec.connected);
  sleepMs(600);  // the client must stay quiet (no Ping before authentication)
  auto pc = phone.counters();
  CHECK_MSG(pc.unauth_msgs == 0 && pc.pings == 0, "unauth msgs %llu pings %llu",
            static_cast<unsigned long long>(pc.unauth_msgs), static_cast<unsigned long long>(pc.pings));
  CHECK(c.state() == ClientState::Pairing);  // not bounded by handshake/idle timeouts
  CHECK(c.submitPairingCode(std::string(" ") + kCode + " "));
  CHECK(!c.submitPairingCode(kCode));  // already being checked (or already paired)
  CHECK(waitSession(c, 3000));
  CHECK(waitFor([&] { return rec.frames.load() >= 10; }, 5000));
  {
    std::lock_guard<std::mutex> lk(rec.pmx);
    CHECK(rec.paired && rec.pairedBeforeConnected);
    CHECK(rec.pairedPhone == phone.phoneId() && rec.pairedInfo.phone_id == phone.phoneId());
    const auto known = phone.pairedClients();
    CHECK(known.size() == 1 && !known.empty() && known[0].client_id == me && known[0].key == rec.pairedKey &&
          known[0].name == "pairing-test");
    CHECK(rec.rejected.empty());
    const ConnectParams now = c.connectParams();
    CHECK(now.paired.size() == 1 && !now.paired.empty() && now.paired[0].phone_id == phone.phoneId() &&
          now.paired[0].key == rec.pairedKey);
  }
  CHECK(c.deviceInfo(di) && di.phone_id == phone.phoneId());
  CHECK(!c.pairingInfo().active);
  pc = phone.counters();
  CHECK(pc.pair_begins == 1 && pc.pair_ok == 1 && pc.unauth_msgs == 0 && pc.auth_proofs == 0);
  c.disconnect();
  CHECK(waitState(c, ClientState::Idle, 2000));
}

// A paired PC reconnects with AuthProof only (auto-reconnect and a new Client
// that got the key from onPaired, like the app after a restart).
void testPairingReconnectUsesProof() {
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(pairPhoneConfig(), err));
  PairRec rec;
  Client c(&rec, pairOptions(true));
  const ClientId me = newClientId();
  CHECK(c.connect(freshParams(phone, me)));
  CHECK(waitState(c, ClientState::Pairing, 3000));
  CHECK(c.submitPairingCode(kCode));
  CHECK(waitSession(c, 3000));
  CHECK(waitFor([&] { return rec.frames.load() >= 5; }, 5000));
  phone.kickClient();  // connection lost without Bye -> auto-reconnect
  CHECK(waitFor([&] { return rec.connects.load() >= 2; }, 6000));
  CHECK(waitFor([&] { return phone.streaming(); }, 3000));
  auto pc = phone.counters();
  CHECK_MSG(pc.pair_begins == 1 && pc.auth_proofs == 1 && pc.auth_ok == 1,
            "pair_begins %llu proofs %llu ok %llu", static_cast<unsigned long long>(pc.pair_begins),
            static_cast<unsigned long long>(pc.auth_proofs), static_cast<unsigned long long>(pc.auth_ok));
  CHECK(rec.promptCount() == 1 && c.stats().reconnect_count >= 1);
  Key32 key;
  {
    std::lock_guard<std::mutex> lk(rec.pmx);
    key = rec.pairedKey;
  }
  c.disconnect();
  CHECK(waitState(c, ClientState::Idle, 2000));

  PairRec rec2;
  Client c2(&rec2, pairOptions(true));  // the phone may still be closing the last session (Busy)
  ConnectParams cp = freshParams(phone, me);
  PairedPhone pp;
  pp.phone_id = phone.phoneId();
  pp.key = key;
  cp.paired.push_back(pp);
  CHECK(c2.connect(cp));
  CHECK(waitSession(c2, 3000));
  pc = phone.counters();
  CHECK(pc.pair_begins == 1 && pc.auth_ok == 2);
  CHECK(rec2.promptCount() == 0 && !rec2.paired && rec2.rejected.empty());
  CHECK(!rec2.sawState(ClientState::Pairing));
  c2.disconnect();
}

void testPairingWrongThenRight() {
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(pairPhoneConfig(), err));
  PairRec rec;
  Client c(&rec, pairOptions(false));
  CHECK(!c.submitPairingCode(kCode));  // not pairing
  CHECK(c.connect(freshParams(phone, newClientId())));
  CHECK(waitState(c, ClientState::Pairing, 3000));
  CHECK(!c.submitPairingCode("12345"));
  CHECK(!c.submitPairingCode("12a456"));
  CHECK(!c.submitPairingCode("1234567"));
  CHECK(!c.submitPairingCode(""));
  CHECK(c.submitPairingCode("000000"));
  CHECK(waitFor([&] { return c.pairingInfo().wrong_code; }, 3000));
  const PairingInfo pi = c.pairingInfo();
  CHECK_MSG(pi.active && !pi.code_pending && pi.attempts_left == kPairCodeAttempts - 1, "attempts left %u",
            pi.attempts_left);
  CHECK(c.state() == ClientState::Pairing);
  {
    std::lock_guard<std::mutex> lk(rec.pmx);
    CHECK(rec.prompts.size() == 2 && rec.prompts.size() >= 2 && rec.prompts[1].wrong_code &&
          rec.prompts[1].attempts_left == kPairCodeAttempts - 1);
  }
  CHECK(c.submitPairingCode(kCode));
  CHECK(waitSession(c, 3000));
  CHECK(rec.paired);
  const auto pc = phone.counters();
  CHECK(pc.pair_wrong == 1 && pc.pair_ok == 1 && pc.pair_begins == 1);
  c.disconnect();
}

// The PC holds a key the phone no longer accepts: forget it, pair again on the
// same connection.
void testPairingStaleKey() {
  const ClientId me = newClientId();
  Key32 oldKey{}, phoneKey{};
  for (size_t i = 0; i < oldKey.size(); ++i) {
    oldKey[i] = static_cast<uint8_t>(i + 1);
    phoneKey[i] = static_cast<uint8_t>(0xA0 + i);
  }
  fake::FakePhoneConfig cfg = pairPhoneConfig();
  fake::FakePhoneConfig::PairedClient known;
  known.client_id = me;
  known.key = phoneKey;  // the phone knows this PC, but with another key
  cfg.paired_clients.push_back(known);
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(cfg, err));

  // (a) HelloAck paired=true -> AuthProof -> NotPaired -> onKeyRejected -> PairBegin
  PairRec rec;
  Client c(&rec, pairOptions(true));
  ConnectParams cp = freshParams(phone, me);
  PairedPhone pp;
  pp.phone_id = phone.phoneId();
  pp.key = oldKey;
  cp.paired.push_back(pp);
  CHECK(c.connect(cp));
  CHECK(waitState(c, ClientState::Pairing, 3000));
  auto pc = phone.counters();
  CHECK(pc.auth_proofs == 1 && pc.auth_not_paired == 1 && pc.pair_begins == 1);
  {
    std::lock_guard<std::mutex> lk(rec.pmx);
    CHECK(rec.rejected.size() == 1 && !rec.rejected.empty() && rec.rejected[0] == phone.phoneId());
  }
  CHECK(c.connectParams().paired.empty());
  CHECK(c.submitPairingCode(kCode));
  CHECK(waitSession(c, 3000));
  {
    std::lock_guard<std::mutex> lk(rec.pmx);
    CHECK(rec.paired && rec.pairedKey != oldKey && rec.pairedKey != phoneKey);
    CHECK(c.connectParams().paired.size() == 1);
  }
  c.disconnect();
  CHECK(waitState(c, ClientState::Idle, 2000));

  // (b) the PC was removed on the phone (HelloAck paired=false): no AuthProof
  // at all, the key is dropped and a pairing starts.
  phone.setPairedClients({});
  PairRec rec2;
  Client c2(&rec2, pairOptions(true));
  ConnectParams cp2 = freshParams(phone, me);
  pp.key = rec.pairedKey;
  cp2.paired.push_back(pp);
  CHECK(c2.connect(cp2));
  CHECK(waitState(c2, ClientState::Pairing, 3000));
  pc = phone.counters();
  CHECK(pc.auth_proofs == 1 && pc.pair_begins == 2);
  CHECK(rec2.rejected.size() == 1 && c2.connectParams().paired.empty());
  CHECK(c2.cancelPairing());
  CHECK(waitState(c2, ClientState::Idle, 2000));
}

// Too many wrong codes: Failed (PairingLocked) and NO auto-reconnect.
void testPairingLockedNoReconnect() {
  fake::FakePhoneConfig cfg = pairPhoneConfig();
  cfg.pair_attempts = 2;
  cfg.pair_lockout_s = 30;
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(cfg, err));
  PairRec rec;
  Client c(&rec, pairOptions(true));  // auto-reconnect ON: must still not retry
  const ConnectParams cp = freshParams(phone, newClientId());
  CHECK(c.connect(cp));
  CHECK(waitState(c, ClientState::Pairing, 3000));
  CHECK(c.pairingInfo().attempts_left == 2);
  CHECK(c.submitPairingCode("111111"));
  CHECK(waitFor([&] { return c.pairingInfo().wrong_code; }, 3000));
  CHECK(c.submitPairingCode("222222"));
  CHECK(waitState(c, ClientState::Failed, 3000));
  CHECK(c.lastDisconnectReason() == DisconnectReason::PairingLocked);
  PairingInfo pi = c.pairingInfo();
  CHECK_MSG(pi.last_status == AuthStatus::Locked && pi.retry_after_s == 30 && !pi.active,
            "status %d retry %u", static_cast<int>(pi.last_status), pi.retry_after_s);
  {
    std::lock_guard<std::mutex> lk(rec.mx);
    CHECK_MSG(rec.lastMsg.find("retry in 30 s") != std::string::npos, "message '%s'", rec.lastMsg.c_str());
  }
  sleepMs(1500);
  CHECK(c.state() == ClientState::Failed);
  CHECK(!rec.sawState(ClientState::Reconnecting));
  auto pc = phone.counters();
  CHECK(pc.pair_begins == 1 && pc.pair_locked == 1 && pc.pair_wrong == 2);

  // A new connect() during the cooldown: the phone answers Locked at once.
  CHECK(c.connect(cp));
  CHECK(waitFor([&] { return phone.counters().pair_begins == 2; }, 3000));
  CHECK(waitState(c, ClientState::Failed, 3000));
  pi = c.pairingInfo();
  CHECK(c.lastDisconnectReason() == DisconnectReason::PairingLocked);
  CHECK_MSG(pi.retry_after_s >= 1 && pi.retry_after_s <= 30, "retry %u", pi.retry_after_s);
  sleepMs(800);
  CHECK(phone.counters().pair_begins == 2 && c.state() == ClientState::Failed);
}

void testCancelPairing() {
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(pairPhoneConfig(), err));
  PairRec rec;
  Client c(&rec, pairOptions(true));
  const ConnectParams cp = freshParams(phone, newClientId());
  CHECK(!c.cancelPairing());  // not pairing
  CHECK(c.connect(cp));
  CHECK(waitState(c, ClientState::Pairing, 3000));
  CHECK(c.cancelPairing());
  CHECK(waitState(c, ClientState::Idle, 2000));
  CHECK(c.lastDisconnectReason() == DisconnectReason::UserRequested);
  CHECK(!c.pairingInfo().active);
  sleepMs(500);
  CHECK(c.state() == ClientState::Idle && phone.counters().pair_begins == 1);
  // the phone is free again (the cancelled connection was closed)
  CHECK(c.connect(cp));
  CHECK(waitState(c, ClientState::Pairing, 3000));
  CHECK(phone.counters().pair_begins == 2);
  CHECK(c.cancelPairing());
  CHECK(waitState(c, ClientState::Idle, 2000));
}

void testPairingExpiredAndRejected() {
  {  // the code expires on the phone
    fake::FakePhoneConfig cfg = pairPhoneConfig();
    cfg.pair_code_lifetime_s = 1;
    fake::FakePhone phone;
    std::string err;
    CHECK(phone.start(cfg, err));
    PairRec rec;
    Client c(&rec, pairOptions(true));
    CHECK(c.connect(freshParams(phone, newClientId())));
    CHECK(waitState(c, ClientState::Pairing, 3000));
    CHECK(c.pairingInfo().code_lifetime_s == 1);
    CHECK(waitState(c, ClientState::Failed, 4000));
    CHECK(c.lastDisconnectReason() == DisconnectReason::PairingExpired);
    sleepMs(800);
    CHECK(c.state() == ClientState::Failed && phone.counters().pair_begins == 1);
    CHECK(phone.counters().pair_expired == 1);
  }
  {  // "cancel" tapped on the phone
    fake::FakePhone phone;
    std::string err;
    CHECK(phone.start(pairPhoneConfig(), err));
    PairRec rec;
    Client c(&rec, pairOptions(true));
    CHECK(c.connect(freshParams(phone, newClientId())));
    CHECK(waitState(c, ClientState::Pairing, 3000));
    phone.rejectPairing();
    CHECK(waitState(c, ClientState::Failed, 3000));
    CHECK(c.lastDisconnectReason() == DisconnectReason::PairingRejected);
    CHECK(c.pairingInfo().last_status == AuthStatus::Rejected);
    sleepMs(800);
    CHECK(c.state() == ClientState::Failed && phone.counters().pair_begins == 1);
  }
}

// The PC is removed on the phone while connected (the phone sends Bye and
// closes): the auto-reconnect must NOT start a pairing (no new code on the
// phone) but end in Failed/NotPaired. Same for a refused key on auto-reconnect.
void testAutoReconnectNeverPairs() {
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(pairPhoneConfig(), err));
  PairRec rec;
  Client c(&rec, pairOptions(true));
  const ClientId me = newClientId();
  CHECK(c.connect(freshParams(phone, me)));
  CHECK(waitState(c, ClientState::Pairing, 3000));
  CHECK(c.submitPairingCode(kCode));
  CHECK(waitSession(c, 3000));
  CHECK(waitFor([&] { return rec.frames.load() >= 5; }, 5000));

  // (a) removed on the phone: Bye -> auto-reconnect -> HelloAck paired=false -> Failed
  phone.setPairedClients({});
  CHECK(waitState(c, ClientState::Failed, 6000));
  CHECK_MSG(c.lastDisconnectReason() == DisconnectReason::NotPaired, "reason %s",
            toString(c.lastDisconnectReason()));
  CHECK(rec.sawState(ClientState::Reconnecting));
  {
    std::lock_guard<std::mutex> lk(rec.pmx);
    CHECK(rec.rejected.size() == 1 && !rec.rejected.empty() && rec.rejected[0] == phone.phoneId());
  }
  CHECK(c.connectParams().paired.empty());
  sleepMs(1000);
  auto pc = phone.counters();
  CHECK_MSG(pc.pair_begins == 1 && c.state() == ClientState::Failed, "pair_begins %llu state %s",
            static_cast<unsigned long long>(pc.pair_begins), toString(c.state()));
  CHECK(rec.promptCount() == 1);

  // (b) a user connect() may pair again; then the key goes stale and an
  // auto-reconnect gets NotPaired for its AuthProof: Failed, still no PairBegin
  CHECK(c.connect(freshParams(phone, me)));
  CHECK(waitState(c, ClientState::Pairing, 3000));
  CHECK(phone.counters().pair_begins == 2);
  CHECK(c.submitPairingCode(kCode));
  CHECK(waitSession(c, 3000));
  fake::FakePhoneConfig::PairedClient other;
  other.client_id = me;
  other.key[0] = 0x5A;  // the phone now holds another key for this PC
  phone.setPairedClients({other});
  phone.kickClient();
  CHECK(waitFor([&] { return c.state() == ClientState::Failed; }, 6000));
  CHECK(c.lastDisconnectReason() == DisconnectReason::NotPaired);
  sleepMs(800);
  pc = phone.counters();
  CHECK_MSG(pc.pair_begins == 2 && pc.auth_not_paired == 1 && c.state() == ClientState::Failed,
            "pair_begins %llu not_paired %llu", static_cast<unsigned long long>(pc.pair_begins),
            static_cast<unsigned long long>(pc.auth_not_paired));
}

// A raw "phone" answering Hello with a fixed HelloAck, to check the client's
// order of checks: a v1 phone (status Ok, version 1, zero trailer) and a v2
// phone must be a VersionMismatch; a v3 phone answering status Ok is a protocol
// error (never "authenticated"); a cipher the PC did not offer (downgrade to
// None) or an invalid ephemeral key is a protocol error.
DisconnectReason helloAckOutcome(const HelloAck& reply, std::string& msg) {
  detail::winsockInit();
  SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  bind(l, reinterpret_cast<sockaddr*>(&a), sizeof(a));
  listen(l, 4);
  int al = sizeof(a);
  getsockname(l, reinterpret_cast<sockaddr*>(&a), &al);
  std::atomic<bool> stop{false};
  std::thread srv([&] {
    while (!stop) {
      fd_set r;
      FD_ZERO(&r);
      FD_SET(l, &r);
      timeval tv{0, 50000};
      if (select(0, &r, nullptr, nullptr, &tv) <= 0) continue;
      SOCKET s = accept(l, nullptr, nullptr);
      if (s == INVALID_SOCKET) continue;
      char buf[512];
      recv(s, buf, sizeof buf, 0);  // the Hello
      std::vector<uint8_t> out;
      appendTcpMessage(out, MsgType::HelloAck, encode(reply));
      send(s, reinterpret_cast<const char*>(out.data()), static_cast<int>(out.size()), 0);
      sleepMs(300);
      closesocket(s);
    }
  });
  Recorder rec;
  ClientOptions o = pairOptions(false);
  Client c(&rec, o);
  ConnectParams cp;
  cp.port = ntohs(a.sin_port);
  cp.client_id = newClientId();
  c.connect(cp);
  waitState(c, ClientState::Failed, 4000);
  const DisconnectReason r = c.lastDisconnectReason();
  {
    std::lock_guard<std::mutex> lk(rec.mx);
    msg = rec.lastMsg;
  }
  CHECK(!rec.connected);
  stop = true;
  srv.join();
  closesocket(l);
  return r;
}

void testHelloAckChecks() {
  std::string msg;
  HelloAck v1;
  v1.version = 1;
  v1.status = HelloStatus::Ok;  // what a v1 phone sends; its v2 trailer decodes as zeros
  DisconnectReason r = helloAckOutcome(v1, msg);
  CHECK_MSG(r == DisconnectReason::VersionMismatch, "v1 phone: %s (%s)", toString(r), msg.c_str());
  HelloAck v1busy = v1;
  v1busy.status = HelloStatus::Busy;
  r = helloAckOutcome(v1busy, msg);
  CHECK_MSG(r == DisconnectReason::VersionMismatch, "v1 busy phone: %s", toString(r));
  HelloAck okV3;
  okV3.status = HelloStatus::Ok;  // v2+ must say AuthRequired
  r = helloAckOutcome(okV3, msg);
  CHECK_MSG(r == DisconnectReason::ProtocolError, "v3 status Ok: %s (%s)", toString(r), msg.c_str());
  HelloAck v2;  // a v2 phone (no key exchange): update needed
  v2.version = 2;
  v2.status = HelloStatus::AuthRequired;
  r = helloAckOutcome(v2, msg);
  CHECK_MSG(r == DisconnectReason::VersionMismatch, "v2 phone: %s (%s)", toString(r), msg.c_str());
  // Downgrade: the "phone" picks no encryption although the PC did not offer it.
  HelloAck none;
  none.status = HelloStatus::AuthRequired;
  none.cipher = static_cast<uint8_t>(Cipher::None);
  r = helloAckOutcome(none, msg);
  CHECK_MSG(r == DisconnectReason::ProtocolError && msg.find("did not offer") != std::string::npos,
            "downgrade to None: %s (%s)", toString(r), msg.c_str());
  HelloAck bogus = none;
  bogus.cipher = 9;
  r = helloAckOutcome(bogus, msg);
  CHECK_MSG(r == DisconnectReason::ProtocolError, "unknown cipher: %s (%s)", toString(r), msg.c_str());
  // A valid cipher but an all-zero ephemeral key (X25519 gives an all-zero secret).
  HelloAck zeroKey = none;
  zeroKey.cipher = static_cast<uint8_t>(Cipher::ChaCha20Poly1305);
  r = helloAckOutcome(zeroKey, msg);
  CHECK_MSG(r == DisconnectReason::ProtocolError, "zero ephemeral key: %s (%s)", toString(r), msg.c_str());
}

// ---------------------------------------------------------------- encryption (protocol v3)
// A client with the preloaded key, against a phone configured by `tweak`.
struct CryptoRun {
  fake::FakePhone phone;
  PairRec rec;
  std::unique_ptr<Client> c;
  bool start(const std::function<void(fake::FakePhoneConfig&)>& tweak, CipherPreference cipher,
             VideoTransport tr = VideoTransport::Tcp, bool autoReconnect = false) {
    fake::FakePhoneConfig pc;
    pc.bind_address = "127.0.0.1";
    pc.control_port = pc.udp_port = pc.discovery_port = 0;
    pc.enable_discovery = false;
    pc.width = 640;
    pc.height = 480;
    pc.logger = [](const std::string& s) { std::printf("    phone: %s\n", s.c_str()); };
    if (tweak) tweak(pc);
    std::string err;
    if (!phone.start(preload(pc), err)) return false;
    ClientOptions o = pairOptions(autoReconnect);
    o.udp_bind_address = "127.0.0.1";
    o.cipher = cipher;
    c.reset(new Client(&rec, o));
    ConnectParams cp;
    cp.port = phone.controlPort();
    cp.transport = tr;
    authorize(cp, phone);
    return c->connect(cp);
  }
};

// Every cipher (AES only with hardware AES) and Off, TCP and UDP: negotiated as
// asked, frames decode, the phone's counters agree.
void testCipherNegotiation() {
  std::vector<CipherPreference> prefs = {CipherPreference::Auto, CipherPreference::ChaCha20Poly1305,
                                         CipherPreference::Off};
  if (hardwareAesAvailable()) prefs.push_back(CipherPreference::Aes128Gcm);
  else std::printf("  (no AES-NI/PCLMULQDQ on this CPU: AES-128-GCM not tested)\n");
  for (VideoTransport tr : {VideoTransport::Tcp, VideoTransport::Udp}) {
    for (CipherPreference p : prefs) {
      CryptoRun t;
      CHECK(t.start(nullptr, p, tr));
      const bool ok = waitFor([&] { return t.rec.frames.load() >= 20; }, 8000);
      const ClientStats s = t.c->stats();
      DeviceInfo di;
      CHECK(t.c->deviceInfo(di));
      std::printf("  [%s %s] -> %s, frames %d, decrypt avg %.1f max %.1f us, punched %d\n",
                  tr == VideoTransport::Udp ? "UDP" : "TCP", toString(p), toString(s.cipher), t.rec.frames.load(),
                  s.decrypt_us_avg, s.decrypt_us_max, s.udp_punched ? 1 : 0);
      CHECK_MSG(ok, "%s: only %d frames", toString(p), t.rec.frames.load());
      CHECK(s.cipher == expectedCipher(p) && di.cipher == s.cipher);
      CHECK(t.phone.counters().last_cipher == static_cast<uint8_t>(s.cipher));
      CHECK(s.cipher == Cipher::None ? s.decrypt_us_avg == 0 : s.decrypt_us_avg > 0);
      CHECK(s.udp_auth_failures == 0 && t.phone.counters().punches_rejected == 0);
      if (tr == VideoTransport::Udp) CHECK(s.udp_punched);
      t.c->disconnect();
    }
  }
  // The phone cannot run what the PC forces: VersionMismatch with a clear message, no retry.
  {
    CryptoRun t;
    CHECK(t.start([](fake::FakePhoneConfig& pc) { pc.cipher_mask = cipherBit(Cipher::ChaCha20Poly1305); },
                  CipherPreference::Off, VideoTransport::Tcp, true));
    CHECK(waitState(*t.c, ClientState::Failed, 4000));
    CHECK(t.c->lastDisconnectReason() == DisconnectReason::VersionMismatch);
    std::lock_guard<std::mutex> lk(t.rec.mx);
    CHECK_MSG(t.rec.lastMsg.find("encryption") != std::string::npos, "message '%s'", t.rec.lastMsg.c_str());
  }
  // A phone that only runs ChaCha serves an Auto PC with ChaCha.
  {
    CryptoRun t;
    CHECK(t.start([](fake::FakePhoneConfig& pc) { pc.cipher_mask = cipherBit(Cipher::ChaCha20Poly1305); },
                  CipherPreference::Auto));
    CHECK(waitFor([&] { return t.rec.frames.load() >= 5; }, 6000));
    CHECK(t.c->stats().cipher == Cipher::ChaCha20Poly1305);
    t.c->disconnect();
  }
}

// AuthResult Ok with a confirmation that does not match: whoever answered does
// not hold the key. Failed/PhoneNotAuthentic, no auto-reconnect, key KEPT.
void testPhoneNotAuthentic() {
  {  // paired path (AuthProof)
    CryptoRun t;
    CHECK(t.start([](fake::FakePhoneConfig& pc) { pc.forge_phone_confirm = true; }, CipherPreference::Auto,
                  VideoTransport::Tcp, true));
    CHECK(waitState(*t.c, ClientState::Failed, 4000));
    CHECK_MSG(t.c->lastDisconnectReason() == DisconnectReason::PhoneNotAuthentic, "reason %s",
              toString(t.c->lastDisconnectReason()));
    sleepMs(800);
    CHECK(t.c->state() == ClientState::Failed && !t.rec.sawState(ClientState::Reconnecting));
    CHECK(!t.rec.connected && t.rec.connects.load() == 0);
    const ConnectParams cp = t.c->connectParams();
    CHECK(cp.paired.size() == 1 && !cp.paired.empty() && cp.paired[0].key == kTestKey);
    {
      std::lock_guard<std::mutex> lk(t.rec.pmx);
      CHECK(t.rec.rejected.empty() && !t.rec.paired);
    }
    {
      std::lock_guard<std::mutex> lk(t.rec.mx);
      std::printf("  paired path: %s\n", t.rec.lastMsg.c_str());
    }
    const auto pc = t.phone.counters();
    CHECK(pc.auth_ok == 1 && pc.pings == 0 && pc.sessions == 1);
  }
  {  // pairing path (CPace): the right code, but a forged phone confirmation -> no key stored
    fake::FakePhoneConfig cfg = pairPhoneConfig();
    cfg.forge_phone_confirm = true;
    fake::FakePhone phone;
    std::string err;
    CHECK(phone.start(cfg, err));
    PairRec rec;
    Client c(&rec, pairOptions(true));
    CHECK(c.connect(freshParams(phone, newClientId())));
    CHECK(waitState(c, ClientState::Pairing, 3000));
    CHECK(c.submitPairingCode(kCode));
    CHECK(waitState(c, ClientState::Failed, 3000));
    CHECK(c.lastDisconnectReason() == DisconnectReason::PhoneNotAuthentic);
    {
      std::lock_guard<std::mutex> lk(rec.pmx);
      CHECK(!rec.paired);
    }
    CHECK(c.connectParams().paired.empty() && !rec.connected);
    sleepMs(500);
    CHECK(phone.counters().pair_begins == 1 && c.state() == ClientState::Failed);
  }
}

// One TCP record of the phone fails to open (bit flip) / a plaintext message
// after authentication: ProtocolError, the session is dropped.
void testTcpRecordAttacks() {
  for (int plain = 0; plain < 2; ++plain) {
    CryptoRun t;
    CHECK(t.start([plain](fake::FakePhoneConfig& pc) {
      if (plain) pc.tcp_plain_after_frames = 15;
      else pc.tcp_tamper_after_frames = 15;
    }, CipherPreference::Auto));
    CHECK(waitState(*t.c, ClientState::Failed, 8000));
    const DisconnectReason r = t.c->lastDisconnectReason();
    std::string msg;
    {
      std::lock_guard<std::mutex> lk(t.rec.mx);
      msg = t.rec.lastMsg;
    }
    std::printf("  [%s] %s: %s, frames before %d\n", plain ? "plaintext" : "tampered", toString(r), msg.c_str(),
                t.rec.frames.load());
    CHECK(r == DisconnectReason::ProtocolError);
    CHECK(msg.find(plain ? "unencrypted" : "failed authentication") != std::string::npos);
    // Tampered: frame 15 itself is bad. Plaintext: the Log follows a valid frame 15.
    const int frames = t.rec.frames.load();
    CHECK(frames >= 1 && frames <= (plain ? 15 : 14));
  }
}

// UDP: tampered and replayed datagrams are dropped and counted, the stream
// continues intact; a plaintext punch from another socket is ignored by the
// phone (it cannot redirect the stream).
void testUdpAttacks() {
  Scenario sc;
  sc.name = "UDP tampered+replayed datagrams";
  sc.transport = VideoTransport::Udp;
  sc.width = 640;
  sc.height = 480;
  sc.bitrate = 1500000;
  sc.udpAttack = 5;
  sc.needFrames = 90;
  sc.checkShm = false;
  fake::FakePhone::Counters pc;
  sc.phoneOut = &pc;
  const ClientStats st = runScenario(sc);
  if (st.frames_received == 0) return;  // filtered by OWC_IT_ONLY
  const uint64_t attacks = pc.udp_tampered_sent + pc.udp_replays_sent;
  std::printf("  attacks sent: %llu tampered + %llu replays, rejected by the PC %llu, frames lost %llu\n",
              static_cast<unsigned long long>(pc.udp_tampered_sent),
              static_cast<unsigned long long>(pc.udp_replays_sent),
              static_cast<unsigned long long>(st.udp_auth_failures),
              static_cast<unsigned long long>(st.frames_lost));
  CHECK(pc.udp_tampered_sent > 20 && pc.udp_replays_sent > 20);
  // Loopback: every attack datagram arrives and must be rejected (a few may still
  // be in flight when the stats are taken).
  CHECK_MSG(st.udp_auth_failures + 4 >= attacks && st.udp_auth_failures <= attacks,
            "rejected %llu of %llu", static_cast<unsigned long long>(st.udp_auth_failures),
            static_cast<unsigned long long>(attacks));
  CHECK_MSG(st.frames_lost <= 2, "frames lost %llu", static_cast<unsigned long long>(st.frames_lost));

  // Spoofed punch: plaintext with the right session id from another socket.
  CryptoRun t;
  CHECK(t.start(nullptr, CipherPreference::Auto, VideoTransport::Udp));
  CHECK(waitFor([&] { return t.rec.frames.load() >= 10; }, 6000));
  DeviceInfo di;
  CHECK(t.c->deviceInfo(di));
  SOCKET spy = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in me{};
  me.sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.1", &me.sin_addr);
  bind(spy, reinterpret_cast<sockaddr*>(&me), sizeof(me));
  DWORD tmo = 300;
  setsockopt(spy, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
  UdpHeader h;
  h.type = UdpType::Punch;
  h.session_id = di.session_id;
  uint8_t b[kUdpHeaderSize];
  writeUdpHeader(h, b);
  sockaddr_in ph = me;
  ph.sin_port = htons(t.phone.udpPort());
  for (int i = 0; i < 3; ++i)
    sendto(spy, reinterpret_cast<const char*>(b), static_cast<int>(sizeof(b)), 0,
           reinterpret_cast<const sockaddr*>(&ph), sizeof(ph));
  char rb[2048];
  const int got = recv(spy, rb, sizeof(rb), 0);
  closesocket(spy);
  CHECK_MSG(got <= 0, "the spoofed punch got %d bytes back", got);
  CHECK(waitFor([&] { return t.phone.counters().punches_rejected >= 3; }, 2000));
  const int f0 = t.rec.frames.load();
  CHECK(waitFor([&] { return t.rec.frames.load() >= f0 + 10; }, 3000));  // still streaming to the PC
  t.c->disconnect();
}

// A key created by a v2 build (same 32-byte format) authenticates in v3.
void testV2StoredKeyStillValid() {
  const Key32 v2Key = {0x9c, 0x02, 0x7e, 0x41, 0xd5, 0x13, 0x88, 0x6a, 0x20, 0xf7, 0x3c,
                       0xb9, 0x64, 0x0e, 0xa1, 0x5d, 0xc2, 0x37, 0x99, 0x48, 0x0b, 0xee,
                       0x71, 0x16, 0x83, 0xda, 0x2f, 0x54, 0xbb, 0x06, 0x6d, 0xf1};
  fake::FakePhoneConfig cfg = pairPhoneConfig();
  fake::FakePhoneConfig::PairedClient known;
  known.client_id = kTestClientId;
  known.key = v2Key;
  known.name = "paired by a v2 build";
  cfg.paired_clients.push_back(known);
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(cfg, err));
  PairRec rec;
  Client c(&rec, pairOptions(false));
  ConnectParams cp = freshParams(phone, kTestClientId);
  PairedPhone pp;
  pp.phone_id = phone.phoneId();
  pp.key = v2Key;
  cp.paired.push_back(pp);
  CHECK(c.connect(cp));
  CHECK(waitSession(c, 3000));
  CHECK(waitFor([&] { return rec.frames.load() >= 5; }, 5000));
  const auto pc = phone.counters();
  CHECK(pc.auth_proofs == 1 && pc.auth_ok == 1 && pc.pair_begins == 0);
  CHECK(rec.promptCount() == 0 && !rec.paired && rec.rejected.empty());
  CHECK(c.connectParams().paired.size() == 1 && c.connectParams().paired[0].key == v2Key);
  c.disconnect();
}

// Capture->PC latency and decrypt cost per cipher (same stream, software
// decoder for comparable decode times). Encryption must not regress latency
// meaningfully: decrypt is microseconds per frame.
void testLatencyByCipher() {
  struct Row { const char* name; CipherPreference p; };
  std::vector<Row> rows = {{"off", CipherPreference::Off}, {"chacha20-poly1305", CipherPreference::ChaCha20Poly1305}};
  if (hardwareAesAvailable()) rows.push_back({"aes-128-gcm", CipherPreference::Aes128Gcm});
  for (VideoTransport tr : {VideoTransport::Tcp, VideoTransport::Udp}) {
    std::vector<ClientStats> res;
    std::vector<std::string> names;
    for (const Row& r : rows) {
      names.push_back(std::string(tr == VideoTransport::Udp ? "LATENCY UDP" : "LATENCY TCP") +
                      " 1080p30 8 Mbps cipher " + r.name);
      Scenario sc;
      sc.name = names.back().c_str();
      sc.transport = tr;
      sc.width = 1920;
      sc.height = 1080;
      sc.bitrate = 8000000;
      sc.decoder = DecoderMode::Software;
      sc.cipher = r.p;
      sc.needFrames = 150;
      sc.checkShm = false;
      res.push_back(runScenario(sc));
    }
    if (res[0].frames_received == 0) continue;  // filtered by OWC_IT_ONLY
    std::printf("SUMMARY %s latency by cipher (1080p30 8 Mbps, SW decoder):\n",
                tr == VideoTransport::Udp ? "UDP" : "TCP");
    for (size_t i = 0; i < rows.size(); ++i)
      std::printf("  %-18s capture->publish avg %6.2f p95 %6.2f ms | capture->arrival %6.2f ms | decrypt avg %7.1f "
                  "max %7.1f us/frame\n",
                  rows[i].name, res[i].capture_to_publish_ms_avg, res[i].capture_to_publish_ms_p95,
                  res[i].capture_to_arrival_ms_avg, res[i].decrypt_us_avg, res[i].decrypt_us_max);
    for (size_t i = 1; i < rows.size(); ++i) {
      if (res[i].frames_received == 0) continue;
      CHECK_MSG(res[i].decrypt_us_avg < 1000.0, "%s: decrypt %.1f us/frame", rows[i].name, res[i].decrypt_us_avg);
      const double off = res[0].capture_to_publish_ms_avg;
      CHECK_MSG(res[i].capture_to_publish_ms_avg <= off + std::max(5.0, off * 0.25),
                "%s: capture->publish %.2f ms vs %.2f ms without encryption", rows[i].name,
                res[i].capture_to_publish_ms_avg, off);
    }
  }
}

// The phone counts wrong codes globally: a new code may allow fewer attempts.
void testPairingGlobalAttempts() {
  fake::FakePhone phone;
  std::string err;
  CHECK(phone.start(pairPhoneConfig(), err));
  PairRec rec;
  // Auto-reconnect: the phone may still be closing the cancelled connection
  // (Busy); a Busy attempt does not use up the connect()'s right to pair.
  Client c(&rec, pairOptions(true));
  const ConnectParams cp = freshParams(phone, newClientId());
  CHECK(c.connect(cp));
  CHECK(waitState(c, ClientState::Pairing, 3000));
  CHECK(c.pairingInfo().attempts_left == kPairCodeAttempts);
  CHECK(c.submitPairingCode("999999"));
  CHECK(waitFor([&] { return c.pairingInfo().wrong_code; }, 3000));
  CHECK(c.cancelPairing());
  CHECK(waitState(c, ClientState::Idle, 2000));
  CHECK(c.connect(cp));
  CHECK(waitState(c, ClientState::Pairing, 5000));
  CHECK_MSG(c.pairingInfo().attempts_left == kPairCodeAttempts - 1, "attempts %u", c.pairingInfo().attempts_left);
  CHECK(c.submitPairingCode(kCode));
  CHECK(waitSession(c, 3000));
  c.disconnect();
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  // Private shm bus per run (never the real one): must be set before the first
  // owc::shmName() call, which caches it for the process.
  wchar_t shmBus[64];
  swprintf(shmBus, 64, L"Local\\OwcTest_core_%lu", GetCurrentProcessId());
  SetEnvironmentVariableW(L"OWC_SHM_NAME", shmBus);
  std::printf("shm bus: %ls\n", shmName());
  RUN(testDiscovery);
  RUN(testDiscoveryAnnounceAndSweep);
  RUN(testBusyAndNoResponse);
  RUN(testConnectRequiresClientId);
  RUN(testPairingFirstTime);
  RUN(testPairingReconnectUsesProof);
  RUN(testPairingWrongThenRight);
  RUN(testPairingStaleKey);
  RUN(testPairingLockedNoReconnect);
  RUN(testCancelPairing);
  RUN(testPairingExpiredAndRejected);
  RUN(testAutoReconnectNeverPairs);
  RUN(testPairingGlobalAttempts);
  RUN(testHelloAckChecks);
  RUN(testCipherNegotiation);
  RUN(testPhoneNotAuthentic);
  RUN(testTcpRecordAttacks);
  RUN(testUdpAttacks);
  RUN(testV2StoredKeyStillValid);
  RUN(testTransformAndControls);
  RUN(testShmBusOwnedByOther);
  RUN(testCodecNegotiation);
  RUN(testRecording);
  RUN(testRecordingEpochSplit);
  RUN(testRecordSetupQuality);
  std::printf("[ RUN  ] testRecordSetupHevc(sustained)\n");
  testRecordSetupHevc(true);
  std::printf("[ RUN  ] testRecordSetupHevc(fallback)\n");
  testRecordSetupHevc(false);

  Scenario tcp;
  tcp.name = "TCP 720p30 sps-patch";
  const ClientStats a = runScenario(tcp);

  Scenario tcpNoPatch = tcp;
  tcpNoPatch.name = "TCP 720p30 no-patch strip-ps";
  tcpNoPatch.patchSps = false;
  tcpNoPatch.inlinePs = false;  // csd only via VideoFormat: client prepends it to IDRs
  const ClientStats b = runScenario(tcpNoPatch);

  Scenario sw = tcp;
  sw.name = "TCP 720p30 software-decoder";
  sw.decoder = DecoderMode::Software;
  const ClientStats g = runScenario(sw);
  CHECK(g.frames_decoded == 0 || !g.decoder_hardware);

  // Regression (real Note 9 / Qualcomm streams): IDRs carry SPS/PPS inline and
  // are flagged kFrameKey|kFrameConfig; they used to be swallowed as config.
  Scenario r1 = tcp;
  r1.name = "REPLAY Note9 high-speed 720p60 rot90";
  r1.replay = "note9_hs_720p60.h264";
  r1.replayFps = 60;
  r1.rotation = 90;
  r1.width = 1280;
  r1.height = 720;
  r1.needFrames = 120;
  const ClientStats rp1 = runScenario(r1);
  Scenario r2 = r1;
  r2.name = "REPLAY Note9 1080p30 rot90";
  r2.replay = "note9_1080p30.h264";
  r2.replayFps = 30;
  r2.width = 1920;
  r2.height = 1080;
  r2.needFrames = 60;
  const ClientStats rp2 = runScenario(r2);
  if (rp1.frames_decoded && rp2.frames_decoded)
    std::printf("SUMMARY real-phone replay: 720p60 decoder delay %.2f/%u, 1080p30 %.2f/%u frames\n",
                rp1.decoder_delay_frames_avg, rp1.decoder_delay_frames_max,
                rp2.decoder_delay_frames_avg, rp2.decoder_delay_frames_max);

  // Regression: stream faster than the PC can present (phone 1080p120 @ 40 Mbps
  // made capture->publish grow to 7-20 s). Real 1080p frames replayed at 120 fps.
  Scenario ov = r2;
  ov.name = "OVERLOAD replay 1080p at 120 fps TCP hardware";
  ov.replayFps = 120;
  ov.overload = true;
  ov.decoder = DecoderMode::Hardware;  // ~21 ms/frame incl. read-back on the Iris Xe
  ov.needFrames = 60;
  ov.timeoutMs = 15000;
  runScenario(ov);
  Scenario ovu = ov;
  ovu.name = "OVERLOAD replay 1080p at 120 fps UDP auto";
  ovu.transport = VideoTransport::Udp;
  ovu.decoder = DecoderMode::Auto;
  runScenario(ovu);

  Scenario hd = tcp;
  hd.name = "TCP 1080p30 hardware";
  hd.width = 1920;
  hd.height = 1080;
  hd.bitrate = 8000000;
  hd.decoder = DecoderMode::Hardware;
  const ClientStats h1 = runScenario(hd);
  Scenario hdsw = hd;
  hdsw.name = "TCP 1080p30 software-decoder";
  hdsw.decoder = DecoderMode::Software;
  const ClientStats h2 = runScenario(hdsw);

  Scenario au = hd;
  au.name = "TCP 1080p30 auto-decoder";
  au.decoder = DecoderMode::Auto;
  au.needFrames = 160;
  const ClientStats h3 = runScenario(au);
  std::printf("  auto selection: %s (now %s)\n", h3.decoder_selection.c_str(), h3.decoder_name.c_str());
  CHECK_MSG(h3.frames_decoded == 0 || !h3.decoder_selection.empty(), "auto benchmark did not finish");

  Scenario udpLoss;
  udpLoss.name = "UDP 5% loss";
  udpLoss.transport = VideoTransport::Udp;
  udpLoss.loss = 5;
  udpLoss.width = 640;
  udpLoss.height = 480;
  udpLoss.bitrate = 1500000;
  udpLoss.timeoutMs = 90000;
  const ClientStats c = runScenario(udpLoss);
  if (c.frames_decoded) {
    CHECK(c.frames_lost > 0);
    CHECK(c.keyframe_requests > 0);
    CHECK(c.fragments_lost > 0);
  }

  Scenario udpFec = udpLoss;
  udpFec.name = "UDP 5% loss + 20% FEC";
  udpFec.fec = 20;
  const ClientStats d = runScenario(udpFec);
  CHECK_MSG(d.frames_decoded == 0 || d.frames_recovered_fec > 0, "no FEC recovery");

  Scenario bf = tcp;
  bf.name = "TCP B-frames=2";
  bf.bframes = 2;
  const ClientStats e = runScenario(bf);
  if (e.frames_decoded) {
    CHECK(e.stream_has_bframes);
    CHECK(e.decoder_low_latency);
    // far below a DPB (~16 frames); avg ~1 frame
    CHECK_MSG(e.decoder_delay_frames_avg <= 2.0 && e.decoder_delay_frames_max <= 6,
              "B-frame decoder delay avg %.2f max %u", e.decoder_delay_frames_avg,
              e.decoder_delay_frames_max);
  }

  Scenario bfsw = bf;
  bfsw.name = "TCP B-frames=2 software-decoder";
  bfsw.decoder = DecoderMode::Software;
  const ClientStats e2 = runScenario(bfsw);
  CHECK(e2.frames_decoded == 0 ||
        (e2.decoder_delay_frames_avg <= 2.0 && e2.decoder_delay_frames_max <= 6));

  RUN(testLatencyByCipher);

  Scenario jb = tcp;
  jb.name = "TCP jitter buffer 60 ms";
  jb.jitterMs = 60;
  jb.checkShm = true;
  const ClientStats f = runScenario(jb);
  if (f.frames_decoded && a.frames_decoded)
    CHECK(f.capture_to_publish_ms_avg > a.capture_to_publish_ms_avg + 30);

  std::printf("\nSUMMARY decoder delay (frames, avg/max): sps-patch %.2f/%u, no-patch %.2f/%u, "
              "B-frames %.2f/%u\n",
              a.decoder_delay_frames_avg, a.decoder_delay_frames_max, b.decoder_delay_frames_avg,
              b.decoder_delay_frames_max, e.decoder_delay_frames_avg, e.decoder_delay_frames_max);
  std::printf("SUMMARY capture->publish avg/p95 ms: TCP %.2f/%.2f, TCP no-patch %.2f/%.2f, UDP-loss "
              "%.2f/%.2f, UDP-FEC %.2f/%.2f, B-frames %.2f/%.2f, JB60 %.2f/%.2f\n",
              a.capture_to_publish_ms_avg, a.capture_to_publish_ms_p95, b.capture_to_publish_ms_avg,
              b.capture_to_publish_ms_p95, c.capture_to_publish_ms_avg, c.capture_to_publish_ms_p95,
              d.capture_to_publish_ms_avg, d.capture_to_publish_ms_p95, e.capture_to_publish_ms_avg,
              e.capture_to_publish_ms_p95, f.capture_to_publish_ms_avg, f.capture_to_publish_ms_p95);
  std::printf("SUMMARY decode ms (incl. readback): HW %.2f (readback %.2f), SW %.2f\n",
              a.decode_ms_avg, a.readback_ms_avg, g.decode_ms_avg);
  std::printf("SUMMARY 1080p decode ms: HW %.2f (readback %.2f) capture->publish %.2f | SW %.2f "
              "capture->publish %.2f\n",
              h1.decode_ms_avg, h1.readback_ms_avg, h1.capture_to_publish_ms_avg, h2.decode_ms_avg,
              h2.capture_to_publish_ms_avg);
  std::printf("SUMMARY arrival->decoded avg/p95 ms: TCP %.2f/%.2f, no-patch %.2f/%.2f, B-frames %.2f/%.2f\n",
              a.arrival_to_decoded_ms_avg, a.arrival_to_decoded_ms_p95, b.arrival_to_decoded_ms_avg,
              b.arrival_to_decoded_ms_p95, e.arrival_to_decoded_ms_avg, e.arrival_to_decoded_ms_p95);
  std::printf("%s (%d failure(s))\n", owctest::failures() ? "FAILED" : "ALL PASSED", owctest::failures());
  return owctest::failures() ? 1 : 0;
}
