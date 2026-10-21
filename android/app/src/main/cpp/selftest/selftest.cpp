// On-device self-test for the OWC native core (adb push to /data/local/tmp).
//
//   owc_selftest probe          encoder probe: which configure stage/keys work
//   owc_selftest full [secs]    starts the real server (ports 47810-47812) and
//                               drives it with an in-process protocol client:
//                               discovery, handshake, busy reject, params,
//                               TCP streaming, ping/pong, keyframe request,
//                               reconfigure, UDP punch + fragments + FEC.
// Logs go to stderr and logcat (tag OWC).
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "abr.h"
#include "app_core.h"
#include "encoder/encoder_options.h"
#include "encoder/video_encoder.h"
#include "owc/protocol.h"
#include "util/clock.h"
#include "util/log.h"

using namespace owc;

namespace {

int g_fail = 0;
#define CHECK(cond, ...)                         \
  do {                                           \
    if (cond) {                                  \
      printf("  [ OK ] " __VA_ARGS__);           \
    } else {                                     \
      printf("  [FAIL] " __VA_ARGS__);           \
      ++g_fail;                                  \
    }                                            \
    printf("\n");                                \
    fflush(stdout);                              \
  } while (0)

int apiLevel() {
  char v[PROP_VALUE_MAX] = {0};
  __system_property_get("ro.build.version.sdk", v);
  return atoi(v) > 0 ? atoi(v) : 26;
}

// ------------------------------------------------------------------ probe
void runProbe() {
  const int api = apiLevel();
  printf("== encoder probe (API %d)\n", api);
  EncoderCapsSet caps;
  probeEncoders(caps, api);
  for (const EncoderCaps* c : {&caps.avc, &caps.hevc}) {
    printf("%s: %s available=%d hw=%d vendorKeys=%zu\n", c->mime.c_str(), c->name.c_str(),
           c->available, c->hardware, c->vendorKeys.size());
    for (const auto& k : c->vendorKeys)
      if (k.find("low-latency") != std::string::npos || k.find("qp") != std::string::npos ||
          k.find("intra") != std::string::npos || k.find("ltr") != std::string::npos)
        printf("    %s\n", k.c_str());
  }
  struct Case {
    const char* what;
    Codec codec;
    int w, h, fps, profile, bframes, qpMin, qpMax, intra;
  };
  const Case cases[] = {
      {"AVC 1080p30 Baseline (default)", Codec::H264, 1920, 1080, 30, 0, 0, 0, 0, 0},
      {"AVC 1080p60 High + 2 B-frames", Codec::H264, 1920, 1080, 60, 2, 2, 0, 0, 0},
      {"AVC 1080p30 Main + QP 20..40", Codec::H264, 1920, 1080, 30, 1, 0, 20, 40, 0},
      {"AVC 1080p30 intra-refresh 30", Codec::H264, 1920, 1080, 30, 0, 0, 0, 0, 30},
      {"AVC 2160p30 High", Codec::H264, 3840, 2160, 30, 2, 0, 0, 0, 0},
      {"HEVC 1080p30 Main", Codec::Hevc, 1920, 1080, 30, 0, 0, 0, 0, 0},
      {"HEVC 2160p30 Main", Codec::Hevc, 3840, 2160, 30, 0, 0, 0, 0, 0},
  };
  for (const auto& c : cases) {
    const EncoderCaps& ec = caps.get(c.codec);
    if (!ec.available) {
      printf("-- %s: codec unavailable\n", c.what);
      continue;
    }
    VideoEncoder::Params p;
    p.settings.codec = c.codec;
    p.settings.width = c.w;
    p.settings.height = c.h;
    p.settings.fps = c.fps;
    p.settings.bitrate = p.settings.bitrateCeiling = 8000000;
    p.settings.profile = c.profile;
    p.settings.bframes = c.bframes;
    p.settings.qpMin = c.qpMin;
    p.settings.qpMax = c.qpMax;
    p.settings.intraRefreshFrames = c.intra;
    int okStage = -1;
    for (int stage = 0; stage < kLadderStages && okStage < 0; ++stage) {
      VideoEncoder enc;
      std::string err;
      int64_t t0 = monoUs();
      if (enc.configure(p, ec, api, stage, err)) {
        okStage = stage;
        printf("-- %s: configured at stage %d in %.1f ms\n", c.what, stage, (monoUs() - t0) / 1000.0);
      } else {
        printf("-- %s: stage %d failed: %s\n", c.what, stage, err.c_str());
      }
    }
    CHECK(okStage >= 0, "%s configurable (stage %d)", c.what, okStage);
  }
}

// ------------------------------------------------------------------ ABR simulation
// Pure controller logic (no device features): 4 Hz ReceiverReports.
void runAbrSim() {
  printf("== ABR simulation\n");
  const int ceiling = 8000000;
  struct Sim {
    Abr abr;
    ReceiverReport rr;
    int64_t t = 0;
    int minAfter5s = 1 << 30;
    // One 250 ms report: fragments at the current target, `loss` fraction lost.
    void step(double loss, float grad, uint32_t queued, uint32_t encBps, uint32_t recvBps) {
      t += 250;
      const uint32_t frags = 20;
      const uint32_t lost = static_cast<uint32_t>(frags * loss + 0.5);
      rr.fragments_received += frags - lost;
      rr.fragments_lost += lost;
      if (lost >= 3) rr.frames_lost += 1;
      rr.delay_gradient_ms = grad;
      rr.recv_bitrate_bps = recvBps;
      Abr::Local l;
      l.queued = queued;
      l.queueAllowance = 1;
      l.encoderBps = encBps;
      abr.onReceiverReport(rr, l, t);
      if (t % 1000 == 0) abr.tick(l, t);
      if (t > 5000) minAfter5s = std::min(minAfter5s, abr.target());
    }
  };
  uint32_t rng = 1;
  auto rnd = [&] { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % 1000; };

  {  // A: static scene (VBR app-limited at 1.5 Mbps), zero loss, Wi-Fi power-save delay spikes
    Sim s;
    s.abr.reset(ceiling, 0);
    for (int i = 0; i < 240; ++i) s.step(0, rnd() < 300 ? 20.0f + rnd() % 80 : 0.0f, 0, 1500000, 1500000);
    CHECK(s.abr.target() == ceiling && s.minAfter5s >= ceiling * 85 / 100,
          "A app-limited + delay spikes, 0 loss: final %d kbps, min after 5 s %d kbps", s.abr.target() / 1000,
          s.minAfter5s / 1000);
  }
  {  // B: real queueing (delay overuse + queue growing), zero loss, encoder at target
    Sim s;
    s.abr.reset(ceiling, 0);
    int minSeen = 1 << 30;
    for (int i = 0; i < 80; ++i) {
      s.step(0, 15.0f, 3, s.abr.target(), s.abr.target());
      minSeen = std::min(minSeen, s.abr.target());
    }
    CHECK(minSeen >= static_cast<int>(ceiling * Abr::kDelayFloorFraction) - 1 && s.abr.target() < ceiling / 2,
          "B queueing+delay: backs off to %d kbps (floor %d kbps)", s.abr.target() / 1000,
          static_cast<int>(ceiling * Abr::kDelayFloorFraction) / 1000);
    // D: pressure removed -> recovers to the ceiling within ~5 s
    int64_t t0 = s.t, reached = -1;
    for (int i = 0; i < 40 && reached < 0; ++i) {
      s.step(0, 0, 0, s.abr.target(), s.abr.target());
      if (s.abr.target() == ceiling) reached = s.t - t0;
    }
    CHECK(reached >= 0 && reached <= 5000, "D recovery to ceiling in %lld ms", static_cast<long long>(reached));
  }
  {  // C: 10 % real loss -> goes below the delay floor, towards 500 kbps
    Sim s;
    s.abr.reset(ceiling, 0);
    for (int i = 0; i < 80; ++i) s.step(0.10 + (rnd() % 50) / 1000.0, 2.0f, 0, s.abr.target(), s.abr.target() * 9 / 10);
    CHECK(s.abr.target() < static_cast<int>(ceiling * Abr::kDelayFloorFraction),
          "C 10-15%% loss: target %d kbps (< delay floor), auto FEC %d%%", s.abr.target() / 1000,
          s.abr.autoFecPercent());
  }
  {  // E: TCP client without ReceiverReports: tick() alone climbs back up
    Abr a;
    a.reset(ceiling, 0);
    Abr::Local l;
    l.queued = 0;
    for (int64_t t = 1000; t <= 6000; t += 1000) a.tick(l, t);
    CHECK(a.target() == ceiling, "E no reports, clean queue: %d kbps after 6 s", a.target() / 1000);
  }
}

// ------------------------------------------------------------------ client helpers
struct PrintSink : EventSink {
  void onEvent(int type, int i, const std::string& s) override {
    printf("  [event] type=%d int=%d str='%s'\n", type, i, s.c_str());
    fflush(stdout);
    std::lock_guard<std::mutex> lk(mu);
    events.push_back(type);
  }
  std::mutex mu;
  std::vector<int> events;
};

int tcpConnect(const char* ip, uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int rcv = 64 * 1024;  // like a PC that reads promptly: no huge client-side buffer
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, ip, &a.sin_addr);
  if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

void sendMsg(int fd, MsgType t, const std::vector<uint8_t>& p) {
  std::vector<uint8_t> out;
  appendTcpMessage(out, t, p);
  size_t off = 0;
  while (off < out.size()) {
    ssize_t n = send(fd, out.data() + off, out.size() - off, MSG_NOSIGNAL);
    if (n <= 0) return;
    off += static_cast<size_t>(n);
  }
}

struct Client {
  int fd = -1;
  TcpFrameParser parser;
  // Reads until a message arrives or timeout; false on timeout/EOF.
  bool next(TcpFrameParser::Message& m, int timeoutMs) {
    int64_t end = monoMs() + timeoutMs;
    for (;;) {
      if (parser.next(m)) return true;
      int left = static_cast<int>(end - monoMs());
      if (left <= 0) return false;
      pollfd p{fd, POLLIN, 0};
      if (poll(&p, 1, left) <= 0) return false;
      uint8_t buf[65536];
      ssize_t n = recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) return false;
      parser.feed(buf, static_cast<size_t>(n));
    }
  }
  bool waitFor(MsgType t, TcpFrameParser::Message& m, int timeoutMs,
               std::map<MsgType, int>* seen = nullptr) {
    int64_t end = monoMs() + timeoutMs;
    while (monoMs() < end) {
      if (!next(m, static_cast<int>(end - monoMs()))) return false;
      if (seen) (*seen)[m.type]++;
      if (m.type == MsgType::Log) {
        LogMsg lm;
        if (decode(m.payload.data(), m.payload.size(), lm))
          printf("  [PC log %d] %s\n", static_cast<int>(lm.level), lm.text.c_str());
      }
      if (m.type == t) return true;
    }
    return false;
  }
};

std::string wlanIp() {
  ifaddrs* ifs = nullptr;
  std::string ip;
  if (getifaddrs(&ifs) != 0) return ip;
  for (ifaddrs* i = ifs; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
    char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, b, sizeof(b));
    ip = b;
    if (strncmp(i->ifa_name, "wlan", 4) == 0) break;
  }
  freeifaddrs(ifs);
  return ip;
}

void setParam(Client& c, ParamId id, ParamValue v, ParamState* out = nullptr) {
  SetParam sp;
  sp.id = id;
  sp.value = v;
  sendMsg(c.fd, MsgType::SetParam, encode(sp));
  TcpFrameParser::Message m;
  int64_t end = monoMs() + 2000;
  while (monoMs() < end && c.waitFor(MsgType::ParamState, m, 2000)) {
    ParamState ps;
    if (decode(m.payload.data(), m.payload.size(), ps) && ps.id == id) {
      if (out) *out = ps;
      return;
    }
  }
}

void command(Client& c, CommandId id, float x = 0, float y = 0) {
  Command cmd;
  cmd.cmd = id;
  cmd.x = x;
  cmd.y = y;
  sendMsg(c.fd, MsgType::Command, encode(cmd));
}

// ------------------------------------------------------------------ full
void runFull(int secs) {
  const uint16_t kCtl = 47810, kUdp = 47811, kDisc = 47812;
  const int api = apiLevel();
  PrintSink sink;
  AppCore core;
  StartOptions o;
  o.deviceName = "selftest";
  o.model = "SM-N9600";
  o.apiLevel = api;
  o.controlPort = kCtl;
  o.udpPort = kUdp;
  o.discoveryPort = kDisc;
  o.announcePort = 47813;  // keep test announcements away from a real PC on 47803
  o.javaCaps.apiLevel = api;
  o.testSyntheticSource = true;
  std::string err;
  printf("== full test\n");
  CHECK(core.start(o, &sink, err), "core start %s", err.c_str());
  CHECK(!core.start(o, &sink, err), "second start rejected");
  printf("  status: %s\n", core.statusJson().c_str());

  // --- announce (kind=3 broadcast every 2 s while idle)
  {
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(u, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(47813);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(u, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    DiscoveryReply rep;
    bool ok = false;
    int64_t end = monoMs() + 3000;
    while (!ok && monoMs() < end) {
      pollfd p{u, POLLIN, 0};
      if (poll(&p, 1, 500) <= 0) continue;
      uint8_t b[512];
      ssize_t n = recv(u, b, sizeof(b), 0);
      ok = n > 0 && decode(b, static_cast<size_t>(n), rep) && rep.announce;
    }
    CHECK(ok && rep.control_port == kCtl && !rep.busy, "announce received (kind=3, name='%s', busy=%d)",
          rep.device_name.c_str(), rep.busy);
    close(u);
  }

  // --- discovery
  {
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kDisc);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto req = encodeDiscoveryRequest();
    sendto(u, req.data(), req.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    pollfd p{u, POLLIN, 0};
    DiscoveryReply rep;
    bool ok = false;
    if (poll(&p, 1, 1000) > 0) {
      uint8_t b[512];
      ssize_t n = recv(u, b, sizeof(b), 0);
      ok = n > 0 && decode(b, static_cast<size_t>(n), rep);
    }
    CHECK(ok && rep.control_port == kCtl && !rep.busy, "discovery reply name='%s' busy=%d", rep.device_name.c_str(), rep.busy);
    close(u);
  }

  // --- TCP session
  Client c;
  c.fd = tcpConnect("127.0.0.1", kCtl);
  CHECK(c.fd >= 0, "tcp connect");
  Hello h;
  h.client_name = "selftest";
  h.transport = VideoTransport::Tcp;
  sendMsg(c.fd, MsgType::Hello, encode(h));
  TcpFrameParser::Message m;
  HelloAck ack;
  bool gotAck = c.waitFor(MsgType::HelloAck, m, 8000) && decode(m.payload.data(), m.payload.size(), ack);
  CHECK(gotAck && ack.status == HelloStatus::Ok && ack.session_id != 0,
        "HelloAck ok session=%08x codec_mask=0x%x api=%u", ack.session_id, ack.codec_mask, ack.android_api);
  Capabilities caps;
  bool gotCaps = c.waitFor(MsgType::Capabilities, m, 3000) && decode(m.payload.data(), m.payload.size(), caps);
  CHECK(gotCaps && !caps.cameras.empty(), "Capabilities: %zu cameras, %zu params, active %u", caps.cameras.size(),
        caps.params.size(), caps.active_camera);
  for (const auto& cam : caps.cameras)
    printf("    camera %s facing=%d level=%u orient=%d zoom=%.1f flash=%d sizes=%zu first=%ux%u@%u fpsRanges=%zu\n",
           cam.id.c_str(), static_cast<int>(cam.facing), cam.hw_level, cam.sensor_orientation, cam.max_zoom,
           cam.has_flash, cam.sizes.size(), cam.sizes.empty() ? 0 : cam.sizes[0].width,
           cam.sizes.empty() ? 0 : cam.sizes[0].height, cam.sizes.empty() ? 0 : cam.sizes[0].max_fps,
           cam.fps_ranges.size());
  for (const auto& pd : caps.params) {
    printf("    param %-22s type=%d flags=%d range=[%g..%g] step=%g cur=%lld/%g opts=%zu\n", toString(pd.id),
           static_cast<int>(pd.type), pd.flags, pd.min, pd.max, pd.step, static_cast<long long>(pd.current.i),
           pd.current.f, pd.options.size());
  }

  // --- busy
  {
    Client b;
    b.fd = tcpConnect("127.0.0.1", kCtl);
    HelloAck ba;
    bool ok = b.waitFor(MsgType::HelloAck, m, 2000) && decode(m.payload.data(), m.payload.size(), ba);
    CHECK(ok && ba.status == HelloStatus::Busy, "second client gets HelloAck{Busy}");
    close(b.fd);
  }

  // --- params
  ParamState ps;
  setParam(c, ParamId::Bitrate, ParamValue::Int(6000000), &ps);
  CHECK(ps.status == ParamStatus::Ok && ps.value.i == 6000000, "SetParam Bitrate -> ParamState ok");
  setParam(c, ParamId::Bitrate, ParamValue::Int(1), &ps);
  CHECK(ps.status == ParamStatus::OutOfRange, "SetParam Bitrate=1 -> OutOfRange (status %d)", static_cast<int>(ps.status));
  setParam(c, static_cast<ParamId>(999), ParamValue::Int(1), &ps);
  CHECK(ps.status == ParamStatus::Unsupported, "SetParam unknown id -> Unsupported");

  // --- streaming over TCP
  command(c, CommandId::StartStream);
  VideoFormat vf;
  std::map<MsgType, int> seen;
  bool gotFmt = c.waitFor(MsgType::VideoFormat, m, 8000, &seen) && decode(m.payload.data(), m.payload.size(), vf);
  CHECK(gotFmt, "VideoFormat epoch=%u %ux%u@%u codec=%d rot=%d mirror=%d csd=%zu", vf.epoch, vf.width, vf.height,
        vf.fps, static_cast<int>(vf.codec), vf.rotation_deg, vf.mirror, vf.csd.size());
  int frames = 0, keys = 0, configs = 0;
  uint64_t bytes = 0;
  double latSum = 0, latMax = 0;
  int64_t start = monoMs(), lastPing = 0, keyReqAt = 0, keyAfterReq = -1;
  int pongs = 0;
  double rttMin = 1e9;
  int stats = 0;
  Stats lastStats;
  while (monoMs() - start < secs * 1000) {
    const int64_t now = monoMs();
    if (now - lastPing > 500) {
      Ping p;
      p.t0_us = static_cast<uint64_t>(monoUs());
      sendMsg(c.fd, MsgType::Ping, encode(p));
      lastPing = now;
    }
    if (!keyReqAt && now - start > secs * 500) {
      command(c, CommandId::RequestKeyframe);
      keyReqAt = now;
    }
    if (!c.next(m, 200)) continue;
    seen[m.type]++;
    if (m.type == MsgType::VideoFrame) {
      VideoFrameHeader vh;
      const uint8_t* au;
      size_t n;
      if (!decodeVideoFrame(m.payload.data(), m.payload.size(), vh, au, n)) continue;
      ++frames;
      bytes += n;
      if (vh.flags & kFrameKey) {
        ++keys;
        if (keyReqAt && keyAfterReq < 0) keyAfterReq = monoMs() - keyReqAt;
      }
      if (vh.flags & kFrameConfig) ++configs;
      double lat = (monoUs() - static_cast<int64_t>(vh.capture_ts_us)) / 1000.0;
      latSum += lat;
      latMax = std::max(latMax, lat);
    } else if (m.type == MsgType::Pong) {
      Pong po;
      if (decode(m.payload.data(), m.payload.size(), po)) {
        ++pongs;
        rttMin = std::min(rttMin, (monoUs() - static_cast<int64_t>(po.t0_us)) / 1000.0);
      }
    } else if (m.type == MsgType::Stats) {
      ++stats;
      decode(m.payload.data(), m.payload.size(), lastStats);
    } else if (m.type == MsgType::Log) {
      LogMsg lm;
      if (decode(m.payload.data(), m.payload.size(), lm)) printf("  [PC log %d] %s\n", static_cast<int>(lm.level), lm.text.c_str());
    }
  }
  const double dur = (monoMs() - start) / 1000.0;
  CHECK(frames > secs * 20, "TCP frames: %d in %.1fs (%.1f fps), %d key, %d with SPS, %.0f kbps", frames, dur,
        frames / dur, keys, configs, bytes * 8 / dur / 1000);
  CHECK(frames > 0 && latMax < 1000, "capture->client latency avg %.1f ms max %.1f ms (same clock)",
        frames ? latSum / frames : 0.0, latMax);
  CHECK(pongs > 0, "Ping/Pong: %d pongs, min RTT %.2f ms", pongs, rttMin);
  CHECK(keyAfterReq >= 0 && keyAfterReq < 1000, "keyframe %lld ms after RequestKeyframe", static_cast<long long>(keyAfterReq));
  CHECK(stats >= secs - 1, "Stats: %d msgs; last cam %.1f enc %.1f send %.1f fps, %u kbps, dropped %u, queue %u, enc lat %.1f ms",
        stats, lastStats.camera_fps, lastStats.encode_fps, lastStats.send_fps, lastStats.bitrate_bps / 1000,
        lastStats.dropped_frames, lastStats.send_queue, lastStats.encode_latency_ms);
  printf("  status: %s\n", core.statusJson().c_str());
  if (vf.csd.size() > 5) {
    // Annex-B SPS: start code, NAL header 0x67, profile_idc, constraint flags, level_idc
    size_t sc = vf.csd[2] == 1 ? 3 : 4;
    printf("  SPS profile_idc=%u constraints=0x%02x level_idc=%u\n", vf.csd[sc + 1], vf.csd[sc + 2],
           vf.csd[sc + 3]);
  }

  // --- slow reader: stop reading for 2 s (latency policy must drop, then resync on a keyframe)
  {
    usleep(2000000);
    int64_t t = monoMs();
    double lastLat = 1e9;
    int keysAfter = 0, framesAfter = 0;
    Stats st;
    while (monoMs() - t < 2000) {
      if (!c.next(m, 200)) continue;
      if (m.type == MsgType::VideoFrame) {
        VideoFrameHeader vh;
        const uint8_t* au;
        size_t n;
        if (decodeVideoFrame(m.payload.data(), m.payload.size(), vh, au, n)) {
          ++framesAfter;
          if (vh.flags & kFrameKey) ++keysAfter;
          lastLat = (monoUs() - static_cast<int64_t>(vh.capture_ts_us)) / 1000.0;
        }
      } else if (m.type == MsgType::Stats) {
        decode(m.payload.data(), m.payload.size(), st);
      }
    }
    CHECK(st.dropped_frames > 0 && keysAfter > 0 && lastLat < 150,
          "slow reader: dropped %u, %d frames/%d keys after resume, latency back to %.1f ms",
          st.dropped_frames, framesAfter, keysAfter, lastLat);
  }

  // --- reconfigure: 1280x720
  setParam(c, ParamId::VideoWidth, ParamValue::Int(1280), &ps);
  setParam(c, ParamId::VideoHeight, ParamValue::Int(720), &ps);
  VideoFormat vf2;
  bool gotFmt2 = c.waitFor(MsgType::VideoFormat, m, 5000) && decode(m.payload.data(), m.payload.size(), vf2);
  CHECK(gotFmt2 && vf2.epoch > vf.epoch && vf2.width == 1280 && vf2.height == 720,
        "reconfigure -> VideoFormat epoch %u -> %u, %ux%u", vf.epoch, vf2.epoch, vf2.width, vf2.height);
  setParam(c, ParamId::Zoom, ParamValue::Real(2.0f), &ps);
  CHECK(ps.status == ParamStatus::Ok, "Zoom 2.0 live (status %d)", static_cast<int>(ps.status));
  setParam(c, ParamId::Torch, ParamValue::Bool(true), &ps);
  printf("  torch -> status %d\n", static_cast<int>(ps.status));
  setParam(c, ParamId::Torch, ParamValue::Bool(false), &ps);
  command(c, CommandId::FocusAt, 0.5f, 0.5f);
  if (ack.codec_mask & codecBit(Codec::Hevc)) {
    setParam(c, ParamId::VideoCodec, ParamValue::EnumV(static_cast<int32_t>(Codec::Hevc)), &ps);
    VideoFormat vf3;
    bool got = c.waitFor(MsgType::VideoFormat, m, 5000) && decode(m.payload.data(), m.payload.size(), vf3);
    CHECK(got && vf3.codec == Codec::Hevc && vf3.epoch > vf2.epoch && vf3.csd.size() > 20,
          "codec switch -> HEVC VideoFormat epoch %u csd %zu bytes", vf3.epoch, vf3.csd.size());
  }
  // Drain a bit
  int f2 = 0;
  int64_t t2 = monoMs();
  while (monoMs() - t2 < 1500)
    if (c.next(m, 200) && m.type == MsgType::VideoFrame) ++f2;
  CHECK(f2 > 20, "frames after reconfigure: %d in 1.5 s", f2);
  command(c, CommandId::StopStream);
  {
    Command cmd;
    cmd.cmd = CommandId::SetDeviceName;
    cmd.text = "renamed-by-pc";
    sendMsg(c.fd, MsgType::Command, encode(cmd));
    usleep(300000);
    std::lock_guard<std::mutex> lk(sink.mu);
    bool seen = std::find(sink.events.begin(), sink.events.end(), 7) != sink.events.end();
    CHECK(seen, "Command SetDeviceName -> app event 7 (DEVICE_NAME)");
  }
  sendMsg(c.fd, MsgType::Bye, {});
  // Expect EOF
  bool eof = !c.waitFor(MsgType::Hello, m, 3000);
  CHECK(eof, "server closes after Bye");
  close(c.fd);
  usleep(300000);

  // --- UDP session (needs a non-loopback address)
  std::string ip = wlanIp();
  if (ip.empty()) {
    printf("  (no non-loopback IPv4: UDP test skipped)\n");
  } else {
    Client u;
    u.fd = tcpConnect(ip.c_str(), kCtl);
    CHECK(u.fd >= 0, "tcp connect via %s", ip.c_str());
    Hello hu;
    hu.transport = VideoTransport::Udp;
    hu.client_name = "selftest-udp";
    sendMsg(u.fd, MsgType::Hello, encode(hu));
    HelloAck ua;
    bool ok = u.waitFor(MsgType::HelloAck, m, 3000) && decode(m.payload.data(), m.payload.size(), ua);
    CHECK(ok && ua.status == HelloStatus::Ok, "UDP session HelloAck");
    int us = socket(AF_INET, SOCK_DGRAM, 0);
    int rcv = 4 << 20;
    setsockopt(us, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    sockaddr_in pa{};
    pa.sin_family = AF_INET;
    pa.sin_port = htons(kUdp);
    inet_pton(AF_INET, ip.c_str(), &pa.sin_addr);
    UdpHeader ph;
    ph.type = UdpType::Punch;
    ph.session_id = ua.session_id;
    uint8_t pd[kUdpHeaderSize];
    writeUdpHeader(ph, pd);
    sendto(us, pd, sizeof(pd), 0, reinterpret_cast<sockaddr*>(&pa), sizeof(pa));
    bool punchAck = false;
    pollfd pp{us, POLLIN, 0};
    if (poll(&pp, 1, 1000) > 0) {
      uint8_t b[64];
      ssize_t n = recv(us, b, sizeof(b), 0);
      UdpHeader rh;
      punchAck = n > 0 && readUdpHeader(b, static_cast<size_t>(n), rh) && rh.type == UdpType::PunchAck &&
                 rh.session_id == ua.session_id;
    }
    CHECK(punchAck, "PunchAck received");
    setParam(u, ParamId::FecPercent, ParamValue::Int(20), &ps);
    CHECK(ps.status == ParamStatus::Ok, "FecPercent=20 accepted");
    command(u, CommandId::StartStream);
    FrameAssembler fa;
    int uf = 0, fecDg = 0, dg = 0;
    uint64_t ubytes = 0;
    int64_t us0 = monoMs();
    while (monoMs() - us0 < 4000) {
      // keep the TCP side drained
      while (u.parser.next(m)) {}
      pollfd fds[2] = {{us, POLLIN, 0}, {u.fd, POLLIN, 0}};
      if (poll(fds, 2, 100) <= 0) continue;
      if (fds[1].revents) {
        uint8_t b[65536];
        ssize_t n = recv(u.fd, b, sizeof(b), MSG_DONTWAIT);
        if (n > 0) u.parser.feed(b, static_cast<size_t>(n));
      }
      if (fds[0].revents) {
        uint8_t b[kUdpMaxDatagram];
        ssize_t n = recv(us, b, sizeof(b), MSG_DONTWAIT);
        UdpHeader vh;
        if (n <= 0 || !readUdpHeader(b, static_cast<size_t>(n), vh)) continue;
        ++dg;
        if (vh.type == UdpType::Fec) ++fecDg;
        FrameAssembler::Frame fr;
        if (fa.push(vh, b + kUdpHeaderSize, static_cast<size_t>(n) - kUdpHeaderSize, fr)) {
          ++uf;
          ubytes += fr.data.size();
        }
      }
    }
    CHECK(uf > 60, "UDP frames assembled: %d (%d datagrams, %d FEC), %.0f kbps, lost %llu", uf, dg, fecDg,
          ubytes * 8 / 4.0 / 1000, static_cast<unsigned long long>(fa.counters().frames_lost));
    CHECK(fecDg > 0, "FEC datagrams sent after data fragments");
    printf("  status: %s\n", core.statusJson().c_str());

    // ABR: ReceiverReports announcing 15 % fragment loss must lower the bitrate.
    {
      auto kbps = [&] {
        std::string s = core.statusJson();
        size_t p = s.find("\"bitrateKbps\":");
        return p == std::string::npos ? 0 : atoi(s.c_str() + p + 14);
      };
      const int before = kbps();
      ReceiverReport rr;
      for (int i = 0; i < 8; ++i) {
        rr.fragments_received += 85;
        rr.fragments_lost += 15;
        rr.frames_received += 8;
        rr.recv_bitrate_bps = static_cast<uint32_t>(before * 1000);
        sendMsg(u.fd, MsgType::ReceiverReport, encode(rr));
        int64_t tw = monoMs();
        while (monoMs() - tw < 250) {
          uint8_t b[65536];
          recv(us, b, sizeof(b), MSG_DONTWAIT);  // keep the UDP socket drained
          if (u.next(m, 20)) {}
        }
      }
      const int after = kbps();
      CHECK(after > 0 && after < before, "ABR: 15%% loss lowers bitrate %d -> %d kbps", before, after);
    }
    close(us);
    close(u.fd);  // abrupt close: server must clean up by itself
    usleep(500000);
    printf("  status after abrupt close: %s\n", core.statusJson().c_str());
  }

  int64_t t0 = monoMs();
  core.stop();
  CHECK(monoMs() - t0 < 3000, "core.stop() joined in %lld ms", static_cast<long long>(monoMs() - t0));
  printf("  status after stop: %s\n", core.statusJson().c_str());
  {
    std::lock_guard<std::mutex> lk(sink.mu);
    printf("  app events:");
    for (int e : sink.events) printf(" %d", e);
    printf("\n");
  }
}

}  // namespace

int main(int argc, char** argv) {
  log::mirrorToStderr(argc > 2 && strcmp(argv[argc - 1], "-v") == 0);
  const std::string mode = argc > 1 ? argv[1] : "probe";
  if (mode == "probe") runProbe();
  else if (mode == "abr") runAbrSim();
  else if (mode == "full") runFull(argc > 2 ? std::max(2, atoi(argv[2])) : 5);
  else {
    printf("usage: owc_selftest probe | full [secs] [-v]\n");
    return 2;
  }
  printf("== %s: %d failure(s)\n", mode.c_str(), g_fail);
  return g_fail ? 1 : 0;
}
