// On-device self-test for the OWC native core (adb push to /data/local/tmp).
//
//   owc_selftest probe          encoder probe: which configure stage/keys work
//   owc_selftest full [secs]    starts the real server (ports 47810-47812) and
//                               drives it with an in-process protocol client:
//                               discovery, handshake, pairing/authentication
//                               (the code is read from the app event sink),
//                               busy reject, params, TCP streaming, ping/pong,
//                               keyframe request, reconfigure, UDP punch +
//                               fragments + FEC, unpair, cancel, lockout.
//                               Pairing data: /data/local/tmp/owc_selftest_data.
// Logs go to stderr and logcat (tag OWC).
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
  struct Ev {
    int type = 0, i = 0;
    std::string s;
  };
  void onEvent(int type, int i, const std::string& s) override {
    std::string shown = s;  // PAIRING_CODE = "code\nname"
    for (auto& ch : shown)
      if (ch == '\n') ch = '|';
    printf("  [event] type=%d int=%d str='%s'\n", type, i, shown.c_str());
    fflush(stdout);
    {
      std::lock_guard<std::mutex> lk(mu);
      events.push_back({type, i, s});
    }
    cv.notify_all();
  }
  size_t count() {
    std::lock_guard<std::mutex> lk(mu);
    return events.size();
  }
  bool has(int type, size_t from = 0) {
    std::lock_guard<std::mutex> lk(mu);
    for (size_t k = from; k < events.size(); ++k)
      if (events[k].type == type) return true;
    return false;
  }
  // Waits for the first event `type` at index >= from.
  bool wait(int type, size_t from, int timeoutMs, Ev* out = nullptr) {
    std::unique_lock<std::mutex> lk(mu);
    auto pred = [&] {
      for (size_t k = from; k < events.size(); ++k)
        if (events[k].type == type) {
          if (out) *out = events[k];
          return true;
        }
      return false;
    };
    return cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), pred);
  }
  std::mutex mu;
  std::condition_variable cv;
  std::vector<Ev> events;
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
  bool eof = false;
  TcpFrameParser parser;
  std::vector<std::string> logs;  // every LogMsg text seen by waitFor()
  // Reads until a message arrives or timeout; false on timeout/EOF.
  bool next(TcpFrameParser::Message& m, int timeoutMs) {
    int64_t end = monoMs() + timeoutMs;
    for (;;) {
      if (parser.next(m)) return true;
      if (eof) return false;
      int left = static_cast<int>(end - monoMs());
      if (left <= 0) return false;
      pollfd p{fd, POLLIN, 0};
      if (poll(&p, 1, left) <= 0) return false;
      uint8_t buf[65536];
      ssize_t n = recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        eof = true;
        return false;
      }
      parser.feed(buf, static_cast<size_t>(n));
    }
  }
  // Discards messages until the server closes (true) or timeout (false).
  bool waitClosed(int timeoutMs) {
    TcpFrameParser::Message m;
    int64_t end = monoMs() + timeoutMs;
    while (!eof && monoMs() < end) next(m, static_cast<int>(std::max<int64_t>(1, end - monoMs())));
    return eof;
  }
  bool waitFor(MsgType t, TcpFrameParser::Message& m, int timeoutMs,
               std::map<MsgType, int>* seen = nullptr) {
    int64_t end = monoMs() + timeoutMs;
    while (monoMs() < end) {
      if (!next(m, static_cast<int>(end - monoMs()))) return false;
      if (seen) (*seen)[m.type]++;
      if (m.type == MsgType::Log) {
        LogMsg lm;
        if (decode(m.payload.data(), m.payload.size(), lm)) {
          printf("  [PC log %d] %s\n", static_cast<int>(lm.level), lm.text.c_str());
          logs.push_back(lm.text);
        }
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

// ------------------------------------------------------------------ pairing helpers
// The test plays a PC: identity persisted "on the PC" = client id + key per phone.
struct PcIdentity {
  Id16 id{};
  Key32 key{};
  Id16 phoneId{};
};

template <size_t N>
bool isZero(const std::array<uint8_t, N>& a) {
  return std::all_of(a.begin(), a.end(), [](uint8_t b) { return b == 0; });
}

// Connect + Hello; true when HelloAck{AuthRequired} arrives.
bool helloAuth(Client& c, const std::string& ip, uint16_t port, const PcIdentity& pc, VideoTransport tr,
               const char* name, HelloAck& ack) {
  c.fd = tcpConnect(ip.c_str(), port);
  if (c.fd < 0) return false;
  Hello h;
  h.client_name = name;
  h.transport = tr;
  h.client_id = pc.id;
  sendMsg(c.fd, MsgType::Hello, encode(h));
  TcpFrameParser::Message m;
  return c.waitFor(MsgType::HelloAck, m, 8000) && decode(m.payload.data(), m.payload.size(), ack) &&
         ack.status == HelloStatus::AuthRequired;
}

bool readAuth(Client& c, AuthResult& r, int timeoutMs = 3000) {
  TcpFrameParser::Message m;
  return c.waitFor(MsgType::AuthResult, m, timeoutMs) && decode(m.payload.data(), m.payload.size(), r);
}

void sendProof(Client& c, const PcIdentity& pc, const HelloAck& ack, bool corrupt = false) {
  AuthProof p;
  p.mac = authMac(pc.key, ack.phone_id, pc.id, ack.nonce);
  if (corrupt) p.mac[0] ^= 1;
  sendMsg(c.fd, MsgType::AuthProof, encode(p));
}

void sendCode(Client& c, const std::string& code) {
  PairCode pc;
  pc.code = code;
  sendMsg(c.fd, MsgType::PairCode, encode(pc));
}

std::string wrongCode(std::string code) {
  if (code.empty()) return "000000";
  code[0] = static_cast<char>('0' + (code[0] - '0' + 1) % 10);
  return code;
}

std::string codeFrom(const PrintSink::Ev& ev) { return ev.s.substr(0, ev.s.find('\n')); }

// UDP Punch with `sid`; true if a matching PunchAck arrives within timeoutMs.
bool punch(const std::string& ip, uint16_t port, uint32_t sid, int timeoutMs) {
  int us = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in pa{};
  pa.sin_family = AF_INET;
  pa.sin_port = htons(port);
  inet_pton(AF_INET, ip.c_str(), &pa.sin_addr);
  UdpHeader ph;
  ph.type = UdpType::Punch;
  ph.session_id = sid;
  uint8_t pd[kUdpHeaderSize];
  writeUdpHeader(ph, pd);
  sendto(us, pd, sizeof(pd), 0, reinterpret_cast<sockaddr*>(&pa), sizeof(pa));
  bool ok = false;
  const int64_t end = monoMs() + timeoutMs;
  while (!ok && monoMs() < end) {
    pollfd pp{us, POLLIN, 0};
    if (poll(&pp, 1, static_cast<int>(std::max<int64_t>(1, end - monoMs()))) <= 0) break;
    uint8_t b[64];
    ssize_t n = recv(us, b, sizeof(b), 0);
    UdpHeader rh;
    ok = n > 0 && readUdpHeader(b, static_cast<size_t>(n), rh) && rh.type == UdpType::PunchAck &&
         rh.session_id == sid;
  }
  close(us);
  return ok;
}

// True if `code` appears in `text` as a standalone digit run.
bool containsCode(const std::string& text, const std::string& code) {
  for (size_t pos = text.find(code); pos != std::string::npos; pos = text.find(code, pos + 1)) {
    const bool before = pos > 0 && isdigit(static_cast<unsigned char>(text[pos - 1]));
    const size_t e = pos + code.size();
    const bool after = e < text.size() && isdigit(static_cast<unsigned char>(text[e]));
    if (!before && !after) return true;
  }
  return false;
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
  const std::string dataDir = "/data/local/tmp/owc_selftest_data";
  mkdir(dataDir.c_str(), 0700);
  unlink((dataDir + "/owc_pairing.txt").c_str());  // every run starts unpaired
  CHECK(!core.start(o, &sink, err), "start refused before setDataDir (%s)", err.c_str());
  err.clear();
  CHECK(core.setDataDir(dataDir, err), "setDataDir %s %s", dataDir.c_str(), err.c_str());
  CHECK(core.pairedClientsJson() == "[]", "no paired PCs initially");
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

  // --- TCP session: Hello -> AuthRequired; nothing works before authentication
  PcIdentity pc;
  arc4random_buf(pc.id.data(), pc.id.size());
  Client c;
  HelloAck ack;
  TcpFrameParser::Message m;
  const size_t ev0 = sink.count();
  bool gotAck = helloAuth(c, "127.0.0.1", kCtl, pc, VideoTransport::Tcp, "selftest", ack);
  CHECK(gotAck && ack.session_id != 0 && !isZero(ack.phone_id) && !isZero(ack.nonce) && !ack.paired,
        "HelloAck{AuthRequired} session=%08x paired=%d codec_mask=0x%x api=%u", ack.session_id, ack.paired,
        ack.codec_mask, ack.android_api);
  {
    SetParam sp;
    sp.id = ParamId::Bitrate;
    sp.value = ParamValue::Int(6000000);
    sendMsg(c.fd, MsgType::SetParam, encode(sp));
    command(c, CommandId::StartStream);
    Ping p;
    p.t0_us = static_cast<uint64_t>(monoUs());
    sendMsg(c.fd, MsgType::Ping, encode(p));
    Hello dup;
    dup.client_id = pc.id;
    sendMsg(c.fd, MsgType::Hello, encode(dup));
    const bool punched = punch("127.0.0.1", kUdp, ack.session_id, 500);
    int replies = 0;
    while (c.next(m, 700)) ++replies;
    CHECK(replies == 0 && !punched && !c.eof && !sink.has(kEvClientConnected, ev0),
          "before authentication: SetParam/Command/Ping/Hello ignored (%d replies), punch unanswered (%d), "
          "no CLIENT_CONNECTED", replies, punched);
  }
  {  // busy while the first client is still unauthenticated
    Client b;
    b.fd = tcpConnect("127.0.0.1", kCtl);
    HelloAck ba;
    bool ok = b.waitFor(MsgType::HelloAck, m, 2000) && decode(m.payload.data(), m.payload.size(), ba);
    CHECK(ok && ba.status == HelloStatus::Busy, "second client gets Busy while the first authenticates");
    close(b.fd);
  }
  std::string code;
  {  // pairing: wrong code, then the right one
    const size_t mark = sink.count();
    sendMsg(c.fd, MsgType::PairBegin, {});
    AuthResult r;
    bool ok = readAuth(c, r);
    CHECK(ok && r.status == AuthStatus::CodeShown && r.attempts_left == kPairCodeAttempts &&
              r.seconds == kPairCodeLifetimeS,
          "PairBegin -> CodeShown (%u attempts, %u s)", r.attempts_left, r.seconds);
    PrintSink::Ev ev;
    ok = sink.wait(kEvPairingCode, mark, 2000, &ev);
    code = ok ? codeFrom(ev) : "";
    const size_t nl = ev.s.find('\n');
    const std::string name = nl == std::string::npos ? "" : ev.s.substr(nl + 1);
    CHECK(ok && code.size() == kPairCodeDigits && name == "selftest" && ev.i == kPairCodeLifetimeS,
          "PAIRING_CODE event: %zu digits, PC '%s', %d s", code.size(), name.c_str(), ev.i);
    CHECK(core.statusJson().find("\"state\":\"pairing\"") != std::string::npos, "status state = pairing");
    sendCode(c, wrongCode(code));
    ok = readAuth(c, r);
    CHECK(ok && r.status == AuthStatus::WrongCode && r.attempts_left == kPairCodeAttempts - 1,
          "wrong code -> WrongCode (%u left)", r.attempts_left);
    sendCode(c, code);
    ok = readAuth(c, r);
    CHECK(ok && r.status == AuthStatus::Ok && !isZero(r.key), "right code -> AuthResult{Ok} with a key");
    pc.key = r.key;
    pc.phoneId = ack.phone_id;
    PrintSink::Ev end;
    CHECK(sink.wait(kEvPairingEnded, mark, 2000, &end) && end.i == kPairingPaired && end.s == "selftest",
          "PAIRING_ENDED paired '%s'", end.s.c_str());
    CHECK(sink.wait(kEvClientConnected, mark, 2000), "CLIENT_CONNECTED only after authentication");
    const std::string list = core.pairedClientsJson();
    CHECK(list.find(toHex(pc.id.data(), kIdSize)) != std::string::npos &&
              list.find("\"selftest\"") != std::string::npos &&
              list.find(toHex(pc.key.data(), kKeySize)) == std::string::npos,
          "paired list (no key): %s", list.c_str());
  }
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
    CHECK(sink.has(kEvDeviceName), "Command SetDeviceName -> app event 7 (DEVICE_NAME)");
  }
  sendMsg(c.fd, MsgType::Bye, {});
  // Expect EOF
  bool eof = !c.waitFor(MsgType::Hello, m, 3000);
  CHECK(eof, "server closes after Bye");
  close(c.fd);
  {
    bool leaked = false;
    for (const auto& l : c.logs) leaked = leaked || containsCode(l, code);
    CHECK(!code.empty() && !leaked, "pairing code never appears in PC log lines (%zu lines)", c.logs.size());
  }
  usleep(300000);

  // --- reconnect: AuthProof with the stored key (a bad one first, same connection)
  {
    Client r;
    HelloAck a2;
    const size_t mark = sink.count();
    bool ok = helloAuth(r, "127.0.0.1", kCtl, pc, VideoTransport::Tcp, "selftest", a2);
    CHECK(ok && a2.paired && a2.phone_id == pc.phoneId && a2.nonce != ack.nonce,
          "reconnect: HelloAck paired=%d, same phone_id, fresh nonce", a2.paired);
    sendProof(r, pc, a2, true);
    AuthResult ar;
    ok = readAuth(r, ar);
    CHECK(ok && ar.status == AuthStatus::NotPaired && !r.eof, "bad mac -> NotPaired, connection kept open");
    CHECK(!punch("127.0.0.1", kUdp, a2.session_id, 400), "punch before authentication not answered");
    sendProof(r, pc, a2);
    ok = readAuth(r, ar);
    CHECK(ok && ar.status == AuthStatus::Ok && isZero(ar.key), "good AuthProof -> Ok (key all zeros)");
    Capabilities c2;
    CHECK(r.waitFor(MsgType::Capabilities, m, 3000) && decode(m.payload.data(), m.payload.size(), c2),
          "Capabilities after AuthProof");
    CHECK(punch("127.0.0.1", kUdp, a2.session_id, 1000), "punch after authentication answered");
    CHECK(!sink.has(kEvPairingCode, mark), "no pairing code shown for a paired PC");
    sendMsg(r.fd, MsgType::Bye, {});
    CHECK(r.waitClosed(3000), "closed after Bye");
    close(r.fd);
    usleep(300000);
  }

  // --- UDP session (needs a non-loopback address)
  std::string ip = wlanIp();
  if (ip.empty()) {
    printf("  (no non-loopback IPv4: UDP test skipped)\n");
  } else {
    Client u;
    HelloAck ua;
    bool ok = helloAuth(u, ip, kCtl, pc, VideoTransport::Udp, "selftest-udp", ua);
    CHECK(ok && ua.paired, "UDP session: Hello via %s -> AuthRequired (paired)", ip.c_str());
    sendProof(u, pc, ua);
    AuthResult uar;
    ok = readAuth(u, uar) && uar.status == AuthStatus::Ok;
    CHECK(ok, "UDP session authenticated with AuthProof");
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

  // --- unpairing a connected PC drops its session
  {
    Client r;
    HelloAck a;
    bool ok = helloAuth(r, "127.0.0.1", kCtl, pc, VideoTransport::Tcp, "selftest", a);
    sendProof(r, pc, a);
    AuthResult ar;
    ok = ok && readAuth(r, ar) && ar.status == AuthStatus::Ok;
    CHECK(ok, "authenticated again for the unpair test");
    const size_t mark = sink.count();
    CHECK(core.unpairClient(toHex(pc.id.data(), kIdSize)), "unpairClient(id)");
    const bool bye = r.waitFor(MsgType::Bye, m, 3000);
    CHECK(bye && r.waitClosed(3000), "unpaired PC: session dropped (Bye + close)");
    CHECK(sink.wait(kEvClientDisconnected, mark, 2000), "CLIENT_DISCONNECTED after unpair");
    CHECK(core.pairedClientsJson() == "[]", "paired list empty after unpair");
    close(r.fd);
    usleep(300000);
  }
  // --- the old key is refused; PairBegin on the same connection; cancel on the phone
  {
    Client r;
    HelloAck a;
    bool ok = helloAuth(r, "127.0.0.1", kCtl, pc, VideoTransport::Tcp, "selftest", a);
    CHECK(ok && !a.paired, "after unpair: HelloAck paired=0");
    sendProof(r, pc, a);
    AuthResult ar;
    ok = readAuth(r, ar);
    CHECK(ok && ar.status == AuthStatus::NotPaired, "old key -> NotPaired");
    const size_t mark = sink.count();
    sendMsg(r.fd, MsgType::PairBegin, {});
    ok = readAuth(r, ar) && ar.status == AuthStatus::CodeShown;
    CHECK(ok && sink.wait(kEvPairingCode, mark, 2000), "PairBegin on the same connection -> CodeShown");
    core.cancelPairing();
    ok = readAuth(r, ar);
    CHECK(ok && ar.status == AuthStatus::Rejected && r.waitClosed(3000), "cancel on the phone -> Rejected + close");
    PrintSink::Ev end;
    CHECK(sink.wait(kEvPairingEnded, mark, 2000, &end) && end.i == kPairingRejected, "PAIRING_ENDED rejected (%d)",
          end.i);
    close(r.fd);
    usleep(300000);
  }
  // --- too many wrong codes: Locked, then PairBegin refused during the lockout
  {
    Client r;
    HelloAck a;
    const size_t mark = sink.count();
    bool ok = helloAuth(r, "127.0.0.1", kCtl, pc, VideoTransport::Tcp, "selftest", a);
    sendMsg(r.fd, MsgType::PairBegin, {});
    AuthResult ar;
    ok = ok && readAuth(r, ar) && ar.status == AuthStatus::CodeShown && ar.attempts_left == kPairCodeAttempts;
    PrintSink::Ev ev;
    ok = ok && sink.wait(kEvPairingCode, mark, 2000, &ev);
    const std::string bad = wrongCode(ok ? codeFrom(ev) : "");
    int wrong = 0;
    for (int i = 0; i < kPairCodeAttempts - 1; ++i) {
      sendCode(r, bad);
      if (readAuth(r, ar) && ar.status == AuthStatus::WrongCode && ar.attempts_left == kPairCodeAttempts - 1 - i)
        ++wrong;
    }
    sendCode(r, bad);
    const bool locked = readAuth(r, ar) && ar.status == AuthStatus::Locked && ar.seconds == kPairLockoutS;
    CHECK(ok && wrong == kPairCodeAttempts - 1 && locked && r.waitClosed(3000),
          "%d wrong codes -> WrongCode x%d, then Locked (%u s) + close", kPairCodeAttempts, wrong, ar.seconds);
    PrintSink::Ev end;
    CHECK(sink.wait(kEvPairingEnded, mark, 2000, &end) && end.i == kPairingFailed, "PAIRING_ENDED failed (%d)", end.i);
    close(r.fd);
    usleep(300000);
    Client r2;
    HelloAck a2;
    const size_t mark2 = sink.count();
    ok = helloAuth(r2, "127.0.0.1", kCtl, pc, VideoTransport::Tcp, "selftest", a2);
    sendMsg(r2.fd, MsgType::PairBegin, {});
    ok = ok && readAuth(r2, ar);
    CHECK(ok && ar.status == AuthStatus::Locked && ar.seconds > 0 && ar.seconds <= kPairLockoutS &&
              r2.waitClosed(3000) && !sink.has(kEvPairingCode, mark2),
          "PairBegin during the lockout -> Locked (%u s left) + close, no code shown", ar.seconds);
    close(r2.fd);
    usleep(300000);
  }
  // --- an unauthenticated connection is closed after kAuthTimeoutS
  {
    Client r;
    HelloAck a;
    bool ok = helloAuth(r, "127.0.0.1", kCtl, pc, VideoTransport::Tcp, "selftest", a);
    const int64_t ta = monoMs();
    const bool closed = r.waitClosed(kAuthTimeoutS * 1000 + 4000);
    const int64_t dt = monoMs() - ta;
    CHECK(ok && closed && dt >= kAuthTimeoutS * 1000 - 500, "unauthenticated connection closed after %lld ms (%u s)",
          static_cast<long long>(dt), static_cast<unsigned>(kAuthTimeoutS));
    close(r.fd);
    usleep(300000);
  }

  int64_t t0 = monoMs();
  core.stop();
  CHECK(monoMs() - t0 < 3000, "core.stop() joined in %lld ms", static_cast<long long>(monoMs() - t0));
  printf("  status after stop: %s\n", core.statusJson().c_str());
  {
    std::lock_guard<std::mutex> lk(sink.mu);
    printf("  app events:");
    for (const auto& e : sink.events) printf(" %d", e.type);
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
