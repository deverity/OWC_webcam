// owc_core integration test: in-process FakePhone (loopback, ephemeral ports)
// <-> owc::Client. Verifies handshake, discovery, TCP and UDP (+loss, +FEC)
// streaming, decoded frame counters, shared-memory output, B-frame handling,
// transforms, Busy / NoResponse errors; prints measured latencies.
#include "test_util.h"

#include "transform.h"
#include "util.h"

#include "fake_phone.h"
#include "pattern.h"

#include <owc/core.h>
#include <owc/shm.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace owc;

namespace {

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

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

void printStats(const char* tag, const ClientStats& s) {
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
  if (sc.replay) {
    pc.replay_file = dataFile(sc.replay);
    pc.replay_fps = sc.replayFps;
  }
  const bool strip = sc.replay == nullptr;
  pc.logger = [](const std::string& s) { std::printf("    phone: %s\n", s.c_str()); };
  fake::FakePhone phone;
  std::string err;
  if (!phone.start(pc, err)) {
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
  Client client(&rec, o);
  ConnectParams cp;
  cp.host = "127.0.0.1";
  cp.port = phone.controlPort();
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
        CHECK_MSG(st.capture_to_publish_ms_p95 < 150.0 + 2 * st.decode_ms_avg,
                  "capture->publish p95 %.1f ms (decode %.1f ms)", st.capture_to_publish_ms_p95,
                  st.decode_ms_avg);
        CHECK_MSG(st.published_fps >= 10.0, "published only %.1f fps", st.published_fps);
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
  CHECK(phone.start(pc, err));
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
  CHECK(phone.start(pc, err));
  ClientOptions o;
  o.auto_reconnect = false;
  o.auto_start_stream = false;
  Recorder r1, r2;
  Client c1(&r1, o), c2(&r2, o);
  ConnectParams cp;
  cp.port = phone.controlPort();
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
  CHECK(phone.start(pc, err));
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
  CHECK(phone.start(pc, err));
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
  CHECK(phone.start(pc, err));
  ClientOptions o;
  o.auto_reconnect = false;
  o.publish_to_shm = false;
  Client c(&rec, o);
  ConnectParams cp;
  cp.port = phone.controlPort();
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
  CHECK(phone.start(pc, err));
  Recorder rec;
  ClientOptions o;
  o.auto_reconnect = false;
  Client c(&rec, o);
  ConnectParams cp;
  cp.port = phone.controlPort();
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
  RUN(testTransformAndControls);
  RUN(testShmBusOwnedByOther);
  RUN(testCodecNegotiation);

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
