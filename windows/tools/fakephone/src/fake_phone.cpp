#include "fake_phone.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <owc/protocol.h>

#include "h264_encoder.h"
#include "pattern.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace owc {
namespace fake {

namespace {

int64_t pcUs() {
  static const int64_t f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v.QuadPart;
  }();
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return (v.QuadPart / f) * 1000000 + ((v.QuadPart % f) * 1000000) / f;
}

bool wsInit() {
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    WSADATA d;
    ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
  });
  return ok;
}

SOCKET bindSock(int type, const std::string& addr, uint16_t port, std::string& err) {
  SOCKET s = socket(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
  if (s == INVALID_SOCKET) { err = "socket failed"; return s; }
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, addr.c_str(), &a.sin_addr);
  if (type == SOCK_DGRAM) {
    BOOL f = FALSE;
    DWORD br = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &f, sizeof(f), nullptr, 0, &br, nullptr, nullptr);
  }
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    err = "bind " + addr + ":" + std::to_string(port) + " failed (" +
          std::to_string(WSAGetLastError()) + ")";
    closesocket(s);
    return INVALID_SOCKET;
  }
  return s;
}

uint16_t localPort(SOCKET s) {
  sockaddr_in a{};
  int l = sizeof(a);
  getsockname(s, reinterpret_cast<sockaddr*>(&a), &l);
  return ntohs(a.sin_port);
}

bool readable(SOCKET s, int ms) {
  fd_set r;
  FD_ZERO(&r);
  FD_SET(s, &r);
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  return select(0, &r, nullptr, nullptr, &tv) > 0;
}

// Splits SPS/PPS out of an Annex-B AU. Returns AU without parameter sets.
void splitParamSets(const uint8_t* p, size_t n, std::vector<uint8_t>& ps, std::vector<uint8_t>& rest) {
  ps.clear();
  rest.clear();
  size_t i = 0;
  auto isSc = [&](size_t k, size_t& len) {
    if (k + 3 <= n && p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 1) { len = 3; return true; }
    if (k + 4 <= n && p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 0 && p[k + 3] == 1) { len = 4; return true; }
    return false;
  };
  size_t sc = 0;
  while (i < n && !isSc(i, sc)) ++i;
  while (i < n) {
    const size_t start = i + sc;
    size_t j = start, sc2 = 0;
    while (j < n && !isSc(j, sc2)) ++j;
    if (start < j) {
      const int t = p[start] & 0x1F;
      std::vector<uint8_t>& dst = (t == 7 || t == 8) ? ps : rest;
      if (t != 9) {  // drop access unit delimiters
        static const uint8_t code[4] = {0, 0, 0, 1};
        dst.insert(dst.end(), code, code + 4);
        dst.insert(dst.end(), p + start, p + j);
      }
    }
    i = j;
    sc = sc2;
  }
}

}  // namespace

struct FakePhone::Impl {
  FakePhoneConfig cfg;
  std::atomic<bool> stopFlag{false};
  SOCKET lsock = INVALID_SOCKET, usock = INVALID_SOCKET, dsock = INVALID_SOCKET;
  uint16_t cport = 0, uport = 0, dport = 0;
  std::thread acceptTh, udpTh, discTh, sessionTh, streamTh, annTh;

  // session
  std::mutex sendMx;
  SOCKET client = INVALID_SOCKET;
  std::atomic<bool> sessionActive{false};
  std::atomic<uint32_t> sessionId{0};
  std::atomic<int> transport{0};
  std::mutex udpMx;
  bool haveUdpPeer = false;
  sockaddr_in udpPeer{};

  // stream params (pmx)
  mutable std::mutex pmx;
  uint32_t width = 0, height = 0, fps = 0, bitrate = 0, bframes = 0, kfMs = 0;
  int profile = 0, fec = 0;
  int codec = 0;  // VideoCodec param (0 H.264, 1 HEVC label)
  // Stored-only params (quality knobs; no effect on the synthetic encoder).
  bool adaptiveBitrate = true;
  int dropPolicy = 0, sendQueue = 2, bitrateMode = 0;
  float zoom = 1.0f;
  bool torch = false;
  std::string name;

  std::atomic<bool> streaming{false};
  std::atomic<bool> reconfigure{false};
  std::atomic<bool> kfRequest{false};
  std::atomic<bool> bitrateDirty{false};
  uint32_t epoch = 0;

  mutable std::mutex cmx;
  Counters c;
  std::mt19937 rng{12345};      // session ids (session thread)
  std::mt19937 lossRng{777};    // loss simulation (stream thread)

  // authentication (amx)
  mutable std::mutex amx;
  Id16 phoneId{};
  std::vector<FakePhoneConfig::PairedClient> pairings;
  std::string lastCode;
  int64_t lockUntilUs = 0;
  // Like the phone: wrong codes are counted GLOBALLY (across codes and
  // connections) until a lockout or a success, forgotten after 10 min.
  int wrongCodes = 0;
  int64_t lastWrongUs = 0;
  Id16 authClient{};                       // client of the current authenticated session
  std::atomic<bool> sessionAuthed{false};
  std::atomic<bool> unpairKick{false};     // the connected PC was removed: Bye + close
  std::atomic<bool> rejectReq{false};
  std::atomic<bool> kick{false};
  std::random_device secureRng;  // codes, keys, nonces (rand_s on MSVC); session thread only

  void log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (cfg.logger) cfg.logger(buf);
    else {
      printf("[fakephone] %s\n", buf);
      fflush(stdout);  // pairing codes must show up at once (console or redirected)
    }
  }
  void vlog(const char* fmt, ...) {
    if (!cfg.verbose) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log("%s", buf);
  }

  int64_t phoneUs() const { return pcUs() + cfg.clock_offset_us; }

  // ---------------------------------------------------------------- sending
  bool sendBytes(SOCKET s, const uint8_t* p, size_t n) {
    while (n) {
      int k = send(s, reinterpret_cast<const char*>(p), static_cast<int>(n), 0);
      if (k <= 0) return false;
      p += k;
      n -= static_cast<size_t>(k);
    }
    return true;
  }
  bool sendMsg(MsgType t, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> b;
    appendTcpMessage(b, t, payload);
    std::lock_guard<std::mutex> lk(sendMx);
    if (client == INVALID_SOCKET) return false;
    return sendBytes(client, b.data(), b.size());
  }

  // ---------------------------------------------------------------- caps
  ParamDesc desc(ParamId id, ValueType t, double mn, double mx, double st, ParamValue cur,
                 bool reconf) {
    ParamDesc d;
    d.id = id;
    d.type = t;
    d.flags = kParamSupported | (reconf ? kParamReconfigures : 0);
    d.min = mn;
    d.max = mx;
    d.step = st;
    d.current = cur;
    return d;
  }
  Capabilities caps() {
    std::lock_guard<std::mutex> lk(pmx);
    Capabilities cp;
    CameraInfo cam;
    cam.id = "0";
    cam.facing = Facing::Back;
    cam.hw_level = 1;
    cam.sensor_orientation = 90;
    cam.max_zoom = 4.0f;
    cam.has_flash = true;
    cam.sizes = {{640, 480, 30}, {1280, 720, 60}, {1920, 1080, 30}};
    cam.fps_ranges.emplace_back(static_cast<uint16_t>(15), static_cast<uint16_t>(30));
    cam.fps_ranges.emplace_back(static_cast<uint16_t>(30), static_cast<uint16_t>(30));
    cp.cameras.push_back(cam);
    cp.params.push_back(desc(ParamId::CameraIndex, ValueType::Int32, 0, 0, 1, ParamValue::Int(0), true));
    cp.params.push_back(desc(ParamId::VideoWidth, ValueType::Int32, 160, 1920, 2, ParamValue::Int(static_cast<int32_t>(width)), true));
    cp.params.push_back(desc(ParamId::VideoHeight, ValueType::Int32, 120, 1080, 2, ParamValue::Int(static_cast<int32_t>(height)), true));
    cp.params.push_back(desc(ParamId::Fps, ValueType::Int32, 1, 60, 1, ParamValue::Int(static_cast<int32_t>(fps)), true));
    ParamDesc vc = desc(ParamId::VideoCodec, ValueType::Enum, 0, 2, 1, ParamValue::EnumV(codec), true);
    vc.options.push_back(EnumOption{0, "H.264"});
    if (cfg.codec_mask & codecBit(Codec::Hevc)) vc.options.push_back(EnumOption{1, "HEVC"});
    cp.params.push_back(vc);
    cp.params.push_back(desc(ParamId::Bitrate, ValueType::Int32, 100000, 50000000, 100000, ParamValue::Int(static_cast<int32_t>(bitrate)), false));
    cp.params.push_back(desc(ParamId::KeyframeIntervalMs, ValueType::Int32, 0, 10000, 100, ParamValue::Int(static_cast<int32_t>(kfMs)), true));
    ParamDesc prof = desc(ParamId::EncoderProfile, ValueType::Enum, 0, 2, 1, ParamValue::EnumV(profile), true);
    prof.options = {EnumOption{0, "Baseline"}, EnumOption{1, "Main"}, EnumOption{2, "High"}};
    cp.params.push_back(prof);
    cp.params.push_back(desc(ParamId::BFrames, ValueType::Int32, 0, 2, 1, ParamValue::Int(static_cast<int32_t>(bframes)), true));
    cp.params.push_back(desc(ParamId::FecPercent, ValueType::Int32, -1, 100, 1, ParamValue::Int(fec), false));
    cp.params.push_back(desc(ParamId::AdaptiveBitrate, ValueType::Bool, 0, 1, 1, ParamValue::Bool(adaptiveBitrate), false));
    ParamDesc dp = desc(ParamId::LatencyDropPolicy, ValueType::Enum, 0, 1, 1, ParamValue::EnumV(dropPolicy), false);
    dp.options = {EnumOption{0, "drop"}, EnumOption{1, "queue"}};
    cp.params.push_back(dp);
    cp.params.push_back(desc(ParamId::MaxSendQueueFrames, ValueType::Int32, 1, 60, 1, ParamValue::Int(sendQueue), false));
    ParamDesc bm = desc(ParamId::BitrateMode, ValueType::Enum, 0, 1, 1, ParamValue::EnumV(bitrateMode), false);
    bm.options = {EnumOption{0, "CBR"}, EnumOption{1, "VBR"}};  // like the phone's AVC encoder
    cp.params.push_back(bm);
    cp.params.push_back(desc(ParamId::Zoom, ValueType::Float, 1.0, 4.0, 0.1, ParamValue::Real(zoom), false));
    cp.params.push_back(desc(ParamId::Torch, ValueType::Bool, 0, 1, 1, ParamValue::Bool(torch), false));
    return cp;
  }

  // ---------------------------------------------------------------- params
  ParamState applyParam(const SetParam& sp) {
    ParamState ps;
    ps.id = sp.id;
    ps.value = sp.value;
    std::lock_guard<std::mutex> lk(pmx);
    const int64_t v = sp.value.i;
    auto inRange = [&](int64_t lo, int64_t hi) { return v >= lo && v <= hi; };
    switch (sp.id) {
      case ParamId::VideoWidth:
        if (!inRange(160, 1920) || (v & 1)) { ps.status = ParamStatus::OutOfRange; ps.value = ParamValue::Int(static_cast<int32_t>(width)); break; }
        width = static_cast<uint32_t>(v); reconfigure = true; break;
      case ParamId::VideoHeight:
        if (!inRange(120, 1080) || (v & 1)) { ps.status = ParamStatus::OutOfRange; ps.value = ParamValue::Int(static_cast<int32_t>(height)); break; }
        height = static_cast<uint32_t>(v); reconfigure = true; break;
      case ParamId::Fps:
        if (!inRange(1, 60)) { ps.status = ParamStatus::OutOfRange; ps.value = ParamValue::Int(static_cast<int32_t>(fps)); break; }
        fps = static_cast<uint32_t>(v); reconfigure = true; break;
      case ParamId::Bitrate:
        if (!inRange(100000, 50000000)) { ps.status = ParamStatus::OutOfRange; ps.value = ParamValue::Int(static_cast<int32_t>(bitrate)); break; }
        bitrate = static_cast<uint32_t>(v); bitrateDirty = true; break;
      case ParamId::KeyframeIntervalMs:
        kfMs = static_cast<uint32_t>(v < 0 ? 0 : v); reconfigure = true; break;
      case ParamId::EncoderProfile:
        profile = static_cast<int>(v < 0 ? 0 : v > 2 ? 2 : v); reconfigure = true;
        ps.value = ParamValue::EnumV(profile); break;
      case ParamId::BFrames:
        bframes = static_cast<uint32_t>(v < 0 ? 0 : v > 2 ? 2 : v);
        if (bframes && profile == 0) profile = 1;
        reconfigure = true;
        ps.value = ParamValue::Int(static_cast<int32_t>(bframes)); break;
      case ParamId::FecPercent:
        fec = static_cast<int>(v < -1 ? -1 : v > 100 ? 100 : v); break;
      case ParamId::AdaptiveBitrate:
        adaptiveBitrate = v != 0; ps.value = ParamValue::Bool(adaptiveBitrate); break;
      case ParamId::LatencyDropPolicy:
        dropPolicy = v ? 1 : 0; ps.value = ParamValue::EnumV(dropPolicy); break;
      case ParamId::MaxSendQueueFrames:
        if (!inRange(1, 60)) { ps.status = ParamStatus::OutOfRange; ps.value = ParamValue::Int(sendQueue); break; }
        sendQueue = static_cast<int>(v); break;
      case ParamId::BitrateMode:
        if (v != 0 && v != 1) { ps.status = ParamStatus::Unsupported; ps.value = ParamValue::EnumV(bitrateMode); break; }
        bitrateMode = static_cast<int>(v); break;
      case ParamId::Zoom:
        zoom = sp.value.f; break;
      case ParamId::Torch:
        torch = sp.value.i != 0; break;
      case ParamId::VideoCodec:
        if (v == 0 || (v == 1 && (cfg.codec_mask & codecBit(Codec::Hevc)) && cfg.hevc_mode == 1)) {
          if (codec != static_cast<int>(v)) reconfigure = true;
          codec = static_cast<int>(v);
        } else {
          ps.status = ParamStatus::Unsupported;
        }
        ps.value = ParamValue::EnumV(codec);
        break;
      case ParamId::CameraIndex:
        if (v != 0) ps.status = ParamStatus::OutOfRange;
        ps.value = ParamValue::Int(0); break;
      default:
        ps.status = ParamStatus::Unsupported;
        break;
    }
    return ps;
  }

  void resetParams() {
    std::lock_guard<std::mutex> lk(pmx);
    width = cfg.width;
    height = cfg.height;
    fps = cfg.fps;
    bitrate = cfg.bitrate;
    bframes = cfg.bframes;
    profile = cfg.bframes && cfg.profile == 0 ? 1 : cfg.profile;
    kfMs = cfg.keyframe_interval_ms;
    fec = cfg.fec_percent;
    codec = 0;
    adaptiveBitrate = true;
    dropPolicy = 0;
    sendQueue = 2;
    bitrateMode = 0;
    zoom = 1.0f;
    torch = false;
  }

  // ---------------------------------------------------------------- threads
  void acceptMain() {
    while (!stopFlag) {
      if (!readable(lsock, 100)) continue;
      SOCKET s = accept(lsock, nullptr, nullptr);
      if (s == INVALID_SOCKET) continue;
      BOOL nd = TRUE;
      setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
      if (sessionActive) {
        rejectBusy(s);
        continue;
      }
      if (sessionTh.joinable()) sessionTh.join();
      sessionActive = true;
      sessionTh = std::thread([this, s] { sessionMain(s); });
    }
  }

  bool readHello(SOCKET s, TcpFrameParser& parser, Hello& h) {
    const int64_t deadline = pcUs() + 3000000;
    uint8_t buf[4096];
    while (pcUs() < deadline && !stopFlag) {
      TcpFrameParser::Message m;
      if (parser.next(m)) return m.type == MsgType::Hello && decode(m.payload.data(), m.payload.size(), h);
      if (!readable(s, 50)) continue;
      int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
      if (n <= 0) return false;
      parser.feed(buf, static_cast<size_t>(n));
    }
    return false;
  }

  void rejectBusy(SOCKET s) {
    TcpFrameParser p;
    Hello h;
    if (readHello(s, p, h)) {
      HelloAck a;
      a.status = HelloStatus::Busy;
      std::vector<uint8_t> b;
      appendTcpMessage(b, MsgType::HelloAck, encode(a));
      sendBytes(s, b.data(), b.size());
    }
    std::lock_guard<std::mutex> lk(cmx);
    ++c.busy_rejects;
    closesocket(s);
  }

  void sessionMain(SOCKET s) {
    TcpFrameParser parser;
    Hello h;
    if (!readHello(s, parser, h)) {
      closesocket(s);
      sessionActive = false;
      return;
    }
    HelloAck ack;
    ack.status = HelloStatus::AuthRequired;
    if (h.magic != kMagic) ack.status = HelloStatus::BadMagic;
    else if (h.version != kProtocolVersion) ack.status = HelloStatus::VersionMismatch;
    {
      std::lock_guard<std::mutex> lk(amx);
      ack.phone_id = phoneId;
      ack.paired = findPairing(h.client_id) != nullptr;
    }
    randomBytes(ack.nonce.data(), ack.nonce.size());
    std::uniform_int_distribution<uint32_t> d(1, 0xFFFFFFFEu);
    ack.session_id = d(rng);
    ack.udp_port = uport;
    {
      std::lock_guard<std::mutex> lk(pmx);
      ack.device_name = name;
    }
    ack.model = cfg.model;
    ack.android_api = 34;
    ack.codec_mask = cfg.codec_mask;
    {
      std::lock_guard<std::mutex> lk(sendMx);
      client = s;
    }
    sendMsg(MsgType::HelloAck, encode(ack));
    kick = false;
    rejectReq = false;
    unpairKick = false;
    if (ack.status != HelloStatus::AuthRequired || !authenticate(s, parser, h, ack)) {
      Sleep(50);
      {
        std::lock_guard<std::mutex> lk(sendMx);
        client = INVALID_SOCKET;
      }
      closesocket(s);
      sessionActive = false;
      return;
    }
    sessionId = ack.session_id;
    transport = static_cast<int>(h.transport);
    {
      std::lock_guard<std::mutex> lk(udpMx);
      haveUdpPeer = false;
    }
    {
      std::lock_guard<std::mutex> lk(cmx);
      ++c.sessions;
    }
    log("client '%s' connected (%s)", h.client_name.c_str(), h.transport == VideoTransport::Udp ? "UDP" : "TCP");
    resetParams();
    sendMsg(MsgType::Capabilities, encode(caps()));
    LogMsg lm;
    lm.text = "fakephone ready";
    sendMsg(MsgType::Log, encode(lm));

    uint8_t buf[16384];
    int64_t nextStats = pcUs() + 1000000;
    uint64_t lastEnc = 0;
    bool run = true;
    {
      std::lock_guard<std::mutex> lk(amx);
      authClient = h.client_id;
    }
    sessionAuthed = true;
    while (run && !stopFlag) {
      if (kick.exchange(false)) break;
      if (unpairKick.exchange(false)) {  // like the phone: removing a connected PC drops it
        log("this PC was removed: Bye");
        sendMsg(MsgType::Bye, {});
        Sleep(20);
        break;
      }
      if (pcUs() >= nextStats) {
        nextStats += 1000000;
        Stats st;
        uint64_t enc;
        {
          std::lock_guard<std::mutex> lk(cmx);
          enc = c.frames_encoded;
        }
        st.camera_fps = st.encode_fps = st.send_fps = static_cast<float>(enc - lastEnc);
        {
          std::lock_guard<std::mutex> lk(pmx);
          if (codec == 1) st.encode_fps *= cfg.hevc_stats_fps_scale;
        }
        lastEnc = enc;
        {
          std::lock_guard<std::mutex> lk(pmx);
          st.bitrate_bps = bitrate;
        }
        st.encode_latency_ms = 5.0f;
        st.battery_pct = 87;
        st.battery_temp_c = 31.5f;
        st.charging = true;
        st.thermal_status = 0;
        sendMsg(MsgType::Stats, encode(st));
      }
      TcpFrameParser::Message m;
      while (run && parser.next(m)) run = handle(m);
      if (!run) break;
      if (!readable(s, 20)) continue;
      int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
      if (n <= 0) break;
      if (!parser.feed(buf, static_cast<size_t>(n))) break;
    }
    sessionAuthed = false;
    stopStream();
    {
      std::lock_guard<std::mutex> lk(sendMx);
      client = INVALID_SOCKET;
    }
    closesocket(s);
    log("client disconnected");
    sessionActive = false;
  }

  // ---------------------------------------------------------------- auth
  void randomBytes(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i += 4) {
      const uint32_t v = secureRng();
      for (size_t k = 0; k < 4 && i + k < n; ++k) p[i + k] = static_cast<uint8_t>(v >> (8 * k));
    }
  }
  // amx held.
  const FakePhoneConfig::PairedClient* findPairing(const Id16& clientId) const {
    for (const auto& pc : pairings)
      if (pc.client_id == clientId) return &pc;
    return nullptr;
  }
  // amx held. Format: one "client_hex key_hex name" per line.
  void savePairings() {
    if (cfg.pairings_file.empty()) return;
    std::ofstream f(cfg.pairings_file, std::ios::trunc);
    for (const auto& pc : pairings)
      f << toHex(pc.client_id.data(), pc.client_id.size()) << ' ' << toHex(pc.key.data(), pc.key.size())
        << ' ' << pc.name << '\n';
  }
  // amx held.
  void loadPairings() {
    if (cfg.pairings_file.empty()) return;
    std::ifstream f(cfg.pairings_file);
    std::string line;
    while (std::getline(f, line)) {
      FakePhoneConfig::PairedClient pc;
      const size_t a = line.find(' ');
      const size_t b = a == std::string::npos ? a : line.find(' ', a + 1);
      if (b == std::string::npos || !fromHex(line.substr(0, a), pc.client_id.data(), pc.client_id.size()) ||
          !fromHex(line.substr(a + 1, b - a - 1), pc.key.data(), pc.key.size()))
        continue;
      pc.name = line.substr(b + 1);
      if (!findPairing(pc.client_id)) pairings.push_back(pc);
    }
  }
  void authResult(AuthStatus st, uint8_t attempts, uint16_t seconds, const Key32& key = Key32{}) {
    AuthResult r;
    r.status = st;
    r.attempts_left = attempts;
    r.seconds = seconds;
    r.key = key;
    sendMsg(MsgType::AuthResult, encode(r));
  }
  void count(uint64_t Counters::*field) {
    std::lock_guard<std::mutex> lk(cmx);
    ++(c.*field);
  }

  // Protocol v2 PAIRING, phone side. true = authenticated (the session may start).
  bool authenticate(SOCKET s, TcpFrameParser& parser, const Hello& h, const HelloAck& ack) {
    int64_t deadline = pcUs() + int64_t(cfg.auth_timeout_s) * 1000000;
    bool codeActive = false;
    std::string code;
    int attempts = 0;
    int64_t codeExpiry = 0;
    uint8_t buf[4096];
    if (std::all_of(h.client_id.begin(), h.client_id.end(), [](uint8_t b) { return b == 0; })) {
      log("client '%s' has no client_id: Rejected", h.client_name.c_str());
      authResult(AuthStatus::Rejected, 0, 0);
      return false;
    }
    while (!stopFlag) {
      const int64_t now = pcUs();
      if (kick.exchange(false)) return false;
      if (codeActive && rejectReq.exchange(false)) {
        log("pairing cancelled on the phone");
        authResult(AuthStatus::Rejected, 0, 0);
        count(&Counters::pair_rejected);
        return false;
      }
      if (codeActive && now >= codeExpiry) {
        log("pairing code expired");
        authResult(AuthStatus::Expired, 0, 0);
        count(&Counters::pair_expired);
        return false;
      }
      if (!codeActive && now >= deadline) {
        log("authentication timeout: closing");
        return false;
      }
      TcpFrameParser::Message m;
      if (!parser.next(m)) {
        if (!readable(s, 20)) continue;
        const int got = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (got <= 0 || !parser.feed(buf, static_cast<size_t>(got))) return false;
        continue;
      }
      const uint8_t* p = m.payload.data();
      const size_t n = m.payload.size();
      switch (m.type) {
        case MsgType::AuthProof: {
          count(&Counters::auth_proofs);
          AuthProof ap;
          if (codeActive || !decode(p, n, ap)) break;  // a pairing is pending: only PairCode counts
          bool ok = false;
          {
            std::lock_guard<std::mutex> lk(amx);
            if (const auto* pc = findPairing(h.client_id)) {
              const Key32 want = authMac(pc->key, phoneId, h.client_id, ack.nonce);
              ok = constantTimeEqual(want.data(), ap.mac.data(), want.size());
            }
          }
          if (ok) {
            authResult(AuthStatus::Ok, 0, 0);
            count(&Counters::auth_ok);
            vlog("client '%s' authenticated (AuthProof)", h.client_name.c_str());
            return true;
          }
          authResult(AuthStatus::NotPaired, 0, 0);
          count(&Counters::auth_not_paired);
          log("AuthProof from '%s' refused (not paired)", h.client_name.c_str());
          deadline = now + int64_t(cfg.auth_timeout_s) * 1000000;
          break;
        }
        case MsgType::PairBegin: {
          count(&Counters::pair_begins);
          if (codeActive) break;
          int64_t lockUntil;
          {
            std::lock_guard<std::mutex> lk(amx);
            lockUntil = lockUntilUs;
          }
          if (lockUntil > now) {
            const uint16_t left = static_cast<uint16_t>((lockUntil - now + 999999) / 1000000);
            authResult(AuthStatus::Locked, 0, left);
            count(&Counters::pair_locked);
            log("PairBegin during lockout (%u s left)", static_cast<unsigned>(left));
            return false;
          }
          if (cfg.pair_code.empty()) {
            char b[16];
            snprintf(b, sizeof(b), "%06u", static_cast<unsigned>(secureRng() % 1000000u));
            code = b;
          } else {
            code = cfg.pair_code;
          }
          {
            std::lock_guard<std::mutex> lk(amx);
            lastCode = code;
          }
          {
            std::lock_guard<std::mutex> lk(amx);
            if (wrongCodes && now - lastWrongUs > 600LL * 1000000) wrongCodes = 0;  // forgotten after 10 min
            attempts = std::max(1, int(cfg.pair_attempts ? cfg.pair_attempts : 1) - wrongCodes);
          }
          codeExpiry = now + int64_t(cfg.pair_code_lifetime_s) * 1000000;
          codeActive = true;
          log("PAIRING CODE for '%s': %s (valid %u s)", h.client_name.c_str(), code.c_str(),
              static_cast<unsigned>(cfg.pair_code_lifetime_s));
          authResult(AuthStatus::CodeShown, static_cast<uint8_t>(attempts), cfg.pair_code_lifetime_s);
          break;
        }
        case MsgType::PairCode: {
          if (!codeActive) break;
          count(&Counters::pair_codes);
          PairCode pcm;
          if (!decode(p, n, pcm)) break;
          if (pcm.code.size() == code.size() &&
              constantTimeEqual(reinterpret_cast<const uint8_t*>(pcm.code.data()),
                                reinterpret_cast<const uint8_t*>(code.data()), code.size())) {
            FakePhoneConfig::PairedClient pc;
            pc.client_id = h.client_id;
            randomBytes(pc.key.data(), pc.key.size());
            pc.name = h.client_name;
            {
              std::lock_guard<std::mutex> lk(amx);
              pairings.erase(std::remove_if(pairings.begin(), pairings.end(),
                                            [&](const FakePhoneConfig::PairedClient& e) {
                                              return e.client_id == h.client_id;
                                            }),
                             pairings.end());
              pairings.push_back(pc);
              savePairings();
              wrongCodes = 0;
            }
            authResult(AuthStatus::Ok, 0, 0, pc.key);
            count(&Counters::pair_ok);
            log("paired with '%s'", h.client_name.c_str());
            return true;
          }
          count(&Counters::pair_wrong);
          {
            std::lock_guard<std::mutex> lk(amx);
            ++wrongCodes;
            lastWrongUs = now;
          }
          if (--attempts <= 0) {
            {
              std::lock_guard<std::mutex> lk(amx);
              lockUntilUs = now + int64_t(cfg.pair_lockout_s) * 1000000;
              wrongCodes = 0;
            }
            authResult(AuthStatus::Locked, 0, cfg.pair_lockout_s);
            count(&Counters::pair_locked);
            log("wrong pairing code: locked for %u s", static_cast<unsigned>(cfg.pair_lockout_s));
            return false;
          }
          authResult(AuthStatus::WrongCode, static_cast<uint8_t>(attempts), 0);
          log("wrong pairing code (%d attempts left)", attempts);
          break;
        }
        case MsgType::Bye:
          return false;
        default:
          count(&Counters::unauth_msgs);  // ignored before authentication
          break;
      }
    }
    return false;
  }

  bool handle(const TcpFrameParser::Message& m) {
    const uint8_t* p = m.payload.data();
    const size_t n = m.payload.size();
    switch (m.type) {
      case MsgType::Ping: {
        Ping pg;
        if (!decode(p, n, pg)) break;
        Pong po;
        po.t0_us = pg.t0_us;
        po.phone_us = static_cast<uint64_t>(phoneUs());
        sendMsg(MsgType::Pong, encode(po));
        std::lock_guard<std::mutex> lk(cmx);
        ++c.pings;
        break;
      }
      case MsgType::SetParam: {
        SetParam sp;
        if (!decode(p, n, sp)) break;
        ParamState ps = applyParam(sp);
        vlog("SetParam %s -> status %d", toString(sp.id), static_cast<int>(ps.status));
        sendMsg(MsgType::ParamState, encode(ps));
        break;
      }
      case MsgType::Command: {
        Command cmd;
        if (!decode(p, n, cmd)) break;
        switch (cmd.cmd) {
          case CommandId::StartStream: startStream(); break;
          case CommandId::StopStream: stopStream(); break;
          case CommandId::RequestKeyframe: {
            kfRequest = true;
            std::lock_guard<std::mutex> lk(cmx);
            ++c.keyframe_requests;
            break;
          }
          case CommandId::ResetParams:
            resetParams();
            reconfigure = true;
            sendMsg(MsgType::Capabilities, encode(caps()));
            break;
          case CommandId::SetDeviceName: {
            std::lock_guard<std::mutex> lk(pmx);
            name = cmd.text;
            break;
          }
          case CommandId::FocusAt: vlog("focus at %.2f,%.2f", cmd.x, cmd.y); break;
        }
        break;
      }
      case MsgType::ReceiverReport: {
        ReceiverReport rr;
        if (!decode(p, n, rr)) break;
        std::lock_guard<std::mutex> lk(cmx);
        ++c.receiver_reports;
        c.last_rr_jitter_ms = rr.jitter_ms;
        c.last_rr_delay_gradient_ms = rr.delay_gradient_ms;
        c.last_rr_decode_ms = rr.decode_ms;
        c.last_rr_frames_lost = rr.frames_lost;
        break;
      }
      case MsgType::Bye:
        return false;
      default:
        break;
    }
    return true;
  }

  void startStream() {
    if (streaming.exchange(true)) return;
    if (streamTh.joinable()) streamTh.join();
    streamTh = std::thread([this] {
      if (cfg.replay_file.empty()) streamMain();
      else replayMain();
    });
  }
  void stopStream() {
    streaming = false;
    if (streamTh.joinable() && streamTh.get_id() != std::this_thread::get_id()) streamTh.join();
  }

  void udpMain() {
    uint8_t buf[2048];
    while (!stopFlag) {
      if (!readable(usock, 100)) continue;
      sockaddr_in from{};
      int fl = sizeof(from);
      int n = recvfrom(usock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                       reinterpret_cast<sockaddr*>(&from), &fl);
      if (n <= 0) continue;
      UdpHeader h;
      if (!readUdpHeader(buf, static_cast<size_t>(n), h) || h.type != UdpType::Punch) continue;
      if (!sessionActive || h.session_id != sessionId) continue;
      {
        std::lock_guard<std::mutex> lk(udpMx);
        if (!haveUdpPeer) vlog("UDP punch from port %u", ntohs(from.sin_port));
        udpPeer = from;
        haveUdpPeer = true;
      }
      UdpHeader a;
      a.type = UdpType::PunchAck;
      a.session_id = h.session_id;
      uint8_t out[kUdpHeaderSize];
      writeUdpHeader(a, out);
      sendto(usock, reinterpret_cast<const char*>(out), static_cast<int>(kUdpHeaderSize), 0,
             reinterpret_cast<sockaddr*>(&from), sizeof(from));
    }
  }

  void announceMain() {
    const size_t colon = cfg.announce_target.rfind(':');
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(colon == std::string::npos
                            ? kDefaultAnnouncePort
                            : static_cast<uint16_t>(atoi(cfg.announce_target.c_str() + colon + 1)));
    if (inet_pton(AF_INET, cfg.announce_target.substr(0, colon).c_str(), &to.sin_addr) != 1) {
      log("bad --announce target %s", cfg.announce_target.c_str());
      return;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    int64_t next = 0;
    while (!stopFlag) {
      if (pcUs() >= next && !sessionActive) {
        DiscoveryReply r;
        r.announce = true;
        r.control_port = cport;
        r.udp_port = uport;
        {
          std::lock_guard<std::mutex> lk(pmx);
          r.device_name = name;
        }
        r.model = cfg.model;
        const std::vector<uint8_t> b = encode(r);
        sendto(s, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), 0,
               reinterpret_cast<sockaddr*>(&to), sizeof(to));
        next = pcUs() + static_cast<int64_t>(cfg.announce_interval_ms) * 1000;
      }
      Sleep(20);
    }
    closesocket(s);
  }

  void discMain() {
    uint8_t buf[512];
    while (!stopFlag) {
      if (!readable(dsock, 100)) continue;
      sockaddr_in from{};
      int fl = sizeof(from);
      int n = recvfrom(dsock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                       reinterpret_cast<sockaddr*>(&from), &fl);
      if (n <= 0 || !isDiscoveryRequest(buf, static_cast<size_t>(n))) continue;
      DiscoveryReply r;
      r.control_port = cport;
      r.udp_port = uport;
      {
        std::lock_guard<std::mutex> lk(pmx);
        r.device_name = name;
      }
      r.model = cfg.model;
      r.busy = sessionActive;
      std::vector<uint8_t> b = encode(r);
      sendto(dsock, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), 0,
             reinterpret_cast<sockaddr*>(&from), sizeof(from));
    }
  }

  // ---------------------------------------------------------------- stream
  void sendVideo(uint32_t ep, uint32_t seq, uint64_t ts, uint8_t flags, const uint8_t* au,
                 size_t n, int fecPct) {
    if (transport == static_cast<int>(VideoTransport::Tcp)) {
      VideoFrameHeader vh;
      vh.epoch = ep;
      vh.frame_seq = seq;
      vh.capture_ts_us = ts;
      vh.flags = flags;
      std::vector<uint8_t> payload;
      encodeVideoFrame(vh, au, n, payload);
      sendMsg(MsgType::VideoFrame, payload);
      return;
    }
    sockaddr_in to{};
    {
      std::lock_guard<std::mutex> lk(udpMx);
      if (!haveUdpPeer) return;
      to = udpPeer;
    }
    UdpHeader base;
    base.type = UdpType::Video;
    base.session_id = sessionId;
    base.frame_seq = seq;
    base.capture_ts_us = ts;
    base.epoch = static_cast<uint16_t>(ep & 0xFFFF);
    base.flags = flags;
    std::uniform_real_distribution<double> u(0.0, 100.0);
    uint64_t sent = 0, dropped = 0, fecN = 0;
    auto sink = [&](const uint8_t* d, size_t len) {
      if (cfg.loss_percent > 0 && u(lossRng) < cfg.loss_percent) {
        ++dropped;
        return;
      }
      sendto(usock, reinterpret_cast<const char*>(d), static_cast<int>(len), 0,
             reinterpret_cast<const sockaddr*>(&to), sizeof(to));
      ++sent;
    };
    const size_t frags = packetize(base, au, n, sink);
    if (fecPct != 0 && frags > 0) {
      const int pct = fecPct < 0 ? 20 : fecPct;
      const uint8_t m = fecParityCount(static_cast<uint16_t>(frags), pct);
      if (m) fecN = packetizeFec(base, au, n, m, sink);
    }
    std::lock_guard<std::mutex> lk(cmx);
    c.datagrams_sent += sent;
    c.datagrams_dropped += dropped;
    c.fec_datagrams += fecN;
  }

  // ---------------------------------------------------------------- replay
  struct ReplayAu { size_t begin, end; bool key, config; };

  // (start of start code, end of NAL) for every NAL unit.
  static void nalSpans(const std::vector<uint8_t>& b, std::vector<std::pair<size_t, size_t>>& out) {
    out.clear();
    const size_t n = b.size();
    std::vector<size_t> sc;
    for (size_t i = 0; i + 3 <= n;) {
      if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) { sc.push_back(i); i += 3; }
      else if (i + 4 <= n && b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 1) {
        sc.push_back(i); i += 4;
      } else { ++i; }
    }
    for (size_t k = 0; k < sc.size(); ++k) out.emplace_back(sc[k], k + 1 < sc.size() ? sc[k + 1] : n);
  }

  // Splits an Annex-B H.264 stream into access units: a new AU starts at a
  // SPS/PPS/AUD/SEI or at a slice with first_mb_in_slice == 0, once the
  // current AU already has a slice. Leading parameter sets -> csd.
  bool loadReplay(std::vector<uint8_t>& file, std::vector<ReplayAu>& aus, std::vector<uint8_t>& csd,
                  uint32_t& w, uint32_t& h, std::string& err) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, cfg.replay_file.c_str(), "rb") != 0 || !fp) {
      err = "cannot open " + cfg.replay_file;
      return false;
    }
    fseek(fp, 0, SEEK_END);
    const long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    file.resize(sz > 0 ? static_cast<size_t>(sz) : 0);
    const size_t got = file.empty() ? 0 : fread(file.data(), 1, file.size(), fp);
    fclose(fp);
    if (got != file.size() || file.empty()) { err = "cannot read " + cfg.replay_file; return false; }
    std::vector<std::pair<size_t, size_t>> nals;
    nalSpans(file, nals);
    size_t cur = SIZE_MAX;
    bool slice = false, key = false, config = false, inCsd = true;
    w = h = 0;
    for (auto& nl : nals) {
      size_t p = nl.first;
      while (p < nl.second && file[p] == 0) ++p;
      if (p + 1 >= nl.second) continue;
      ++p;  // skip the 0x01 of the start code
      const int t = file[p] & 0x1F;
      const bool vcl = t == 1 || t == 5;
      if (inCsd) {
        if (t == 7 || t == 8) {
          csd.insert(csd.end(), file.begin() + static_cast<std::ptrdiff_t>(nl.first),
                     file.begin() + static_cast<std::ptrdiff_t>(nl.second));
          if (t == 7 && w == 0) {
            SpsInfo si;
            if (parseH264Sps(file.data() + p, nl.second - p, si)) { w = si.width; h = si.height; }
          }
          continue;
        }
        inCsd = false;
      }
      const bool firstMb0 = vcl && p + 1 < nl.second && (file[p + 1] & 0x80) != 0;
      const bool boundary = slice && ((t == 7 || t == 8 || t == 9 || t == 6) || firstMb0);
      if (cur == SIZE_MAX) {
        cur = nl.first;
      } else if (boundary) {
        aus.push_back(ReplayAu{cur, nl.first, key, config});
        cur = nl.first;
        slice = key = config = false;
      }
      if (vcl) slice = true;
      if (t == 5) key = true;
      if (t == 7 || t == 8) config = true;
    }
    if (cur != SIZE_MAX && slice) aus.push_back(ReplayAu{cur, file.size(), key, config});
    if (aus.empty() || !aus[0].key || csd.empty() || w == 0) {
      err = "replay file must start with SPS/PPS and an IDR";
      return false;
    }
    return true;
  }

  void replayMain() {
    std::vector<uint8_t> file, csd;
    std::vector<ReplayAu> aus;
    uint32_t w = 0, h = 0;
    std::string err;
    if (!loadReplay(file, aus, csd, w, h, err)) {
      log("replay: %s", err.c_str());
      streaming = false;
      return;
    }
    const uint32_t f = cfg.replay_fps ? cfg.replay_fps : 30;
    const int64_t interval = 1000000 / f;
    const uint32_t ep = ++epoch;
    VideoFormat vf;
    vf.epoch = ep;
    vf.codec = Codec::H264;
    vf.width = static_cast<uint16_t>(w);
    vf.height = static_cast<uint16_t>(h);
    vf.fps = static_cast<uint16_t>(f);
    vf.rotation_deg = cfg.rotation_deg;
    vf.csd = csd;
    sendMsg(MsgType::VideoFormat, encode(vf));
    vlog("replay %s: %zu AUs %ux%u@%u", cfg.replay_file.c_str(), aus.size(), w, h, f);
    uint32_t seq = 0;
    size_t idx = 0;
    int64_t next = pcUs();
    while (streaming && !stopFlag) {
      const int64_t now = pcUs();
      if (next > now) {
        const int64_t waitMs = (next - now) / 1000;
        if (waitMs > 1) Sleep(static_cast<DWORD>(waitMs - 1));
        while (pcUs() < next) std::this_thread::yield();
      }
      next += interval;
      if (kfRequest.exchange(false)) idx = 0;  // the first AU is an IDR
      const ReplayAu& a = aus[idx];
      idx = (idx + 1) % aus.size();
      const uint8_t flags =
          static_cast<uint8_t>((a.key ? kFrameKey : 0) | (a.config ? kFrameConfig : 0));
      int fecPct;
      {
        std::lock_guard<std::mutex> lk(pmx);
        fecPct = fec;
      }
      sendVideo(ep, seq++, static_cast<uint64_t>(phoneUs()), flags, file.data() + a.begin,
                a.end - a.begin, fecPct);
      std::lock_guard<std::mutex> lk(cmx);
      ++c.frames_encoded;
      ++c.frames_sent;
      if (a.key) ++c.keyframes;
    }
  }

  void streamMain() {
    MfScope mf;
    H264Encoder enc;
    std::vector<uint8_t> frame, csd, ps, rest;
    std::map<int64_t, uint64_t> ptsToCapture;
    uint32_t seq = 0, counter = 0;
    bool open = false, csdSent = false;
    uint32_t w = 0, h = 0, f = 30;
    int64_t interval = 33333, next = pcUs();
    int64_t lastIdr = -1000000;
    uint32_t sinceKey = 0, gopFrames = 0;
    while (streaming && !stopFlag) {
      if (!open || reconfigure.exchange(false)) {
        EncoderConfig ec;
        {
          std::lock_guard<std::mutex> lk(pmx);
          ec.width = width;
          ec.height = height;
          ec.fps = fps;
          ec.bitrate = bitrate;
          ec.bframes = bframes;
          ec.profile = bframes && profile == 0 ? 1 : profile;
          ec.gop_frames = kfMs ? std::max<uint32_t>(1, kfMs * fps / 1000) : 0;
        }
        std::string err;
        if (!enc.open(ec, err)) {
          log("encoder open failed: %s", err.c_str());
          LogMsg lm;
          lm.level = LogLevel::Error;
          lm.text = "encoder: " + err;
          sendMsg(MsgType::Log, encode(lm));
          streaming = false;
          break;
        }
        open = true;
        ++epoch;
        csdSent = false;
        w = ec.width;
        h = ec.height;
        f = ec.fps;
        gopFrames = ec.gop_frames;
        interval = 1000000 / f;
        frame.assign(static_cast<size_t>(w) * h * 3 / 2, 0);
        ptsToCapture.clear();
        sinceKey = 0;
        vlog("encoder %ux%u@%u %u bps bframes %u epoch %u", w, h, f, ec.bitrate, ec.bframes, epoch);
      }
      if (bitrateDirty.exchange(false)) {
        std::lock_guard<std::mutex> lk(pmx);
        enc.setBitrate(bitrate);
      }
      // pace
      const int64_t now = pcUs();
      if (next > now) {
        const int64_t waitMs = (next - now) / 1000;
        if (waitMs > 1) Sleep(static_cast<DWORD>(waitMs - 1));
        while (pcUs() < next) std::this_thread::yield();
      } else if (now - next > 4 * interval) {
        next = now;  // fell behind: don't burst
      }
      next += interval;
      drawTestFrame(frame.data(), w, h, counter);
      const uint64_t capture = static_cast<uint64_t>(phoneUs());
      const int64_t pts = static_cast<int64_t>(counter) * 10000000LL / f;
      ptsToCapture[pts] = capture;
      bool forceKey = !csdSent && sinceKey == 0;
      if (kfRequest.exchange(false)) {
        // like the phone: ignore requests within 150 ms of the last IDR
        if (pcUs() - lastIdr >= 150000) forceKey = true;
      }
      if (gopFrames && sinceKey >= gopFrames) forceKey = true;
      int fecPct;
      {
        std::lock_guard<std::mutex> lk(pmx);
        fecPct = fec;
      }
      const uint32_t ep = epoch;
      bool ok = enc.encode(frame.data(), pts, 10000000LL / f, forceKey,
                           [&](const uint8_t* au, size_t n, int64_t opts, bool key) {
        splitParamSets(au, n, ps, rest);
        if (!ps.empty()) csd = ps;
        const bool inlinePs = cfg.inline_param_sets && !ps.empty();
        uint64_t ts = capture;
        auto it = ptsToCapture.find(opts);
        if (it != ptsToCapture.end()) {
          ts = it->second;
          ptsToCapture.erase(it);
        }
        if (!csdSent) {
          if (csd.empty() || !key) return;  // wait for the first IDR with SPS/PPS
          VideoFormat vf;
          vf.epoch = ep;
          {
            std::lock_guard<std::mutex> lk(pmx);
            vf.codec = static_cast<Codec>(codec);  // hevc_mode 1: deliberately mislabelled
          }
          vf.width = static_cast<uint16_t>(w);
          vf.height = static_cast<uint16_t>(h);
          vf.fps = static_cast<uint16_t>(f);
          vf.rotation_deg = cfg.rotation_deg;
          vf.csd = csd;
          sendMsg(MsgType::VideoFormat, encode(vf));
          csdSent = true;
        }
        if (key) {
          lastIdr = pcUs();
          sinceKey = 0;
        }
        if (inlinePs) {  // Android-like: IDR with SPS/PPS prepended, Key|Config
          ps.insert(ps.end(), rest.begin(), rest.end());
          sendVideo(ep, seq++, ts, static_cast<uint8_t>((key ? kFrameKey : 0) | kFrameConfig),
                    ps.data(), ps.size(), fecPct);
        } else {
          sendVideo(ep, seq++, ts, key ? kFrameKey : 0, rest.data(), rest.size(), fecPct);
        }
        std::lock_guard<std::mutex> lk(cmx);
        ++c.frames_sent;
        if (key) ++c.keyframes;
      });
      while (ptsToCapture.size() > 64) ptsToCapture.erase(ptsToCapture.begin());
      ++sinceKey;
      ++counter;
      {
        std::lock_guard<std::mutex> lk(cmx);
        ++c.frames_encoded;
      }
      if (!ok) {
        log("encode failed");
        open = false;
      }
    }
    enc.close();
  }
};

FakePhone::FakePhone() : impl_(new Impl) {}
FakePhone::~FakePhone() { stop(); }

bool FakePhone::start(const FakePhoneConfig& cfg, std::string& err) {
  stop();
  if (!wsInit()) { err = "WSAStartup failed"; return false; }
  Impl& m = *impl_;
  m.cfg = cfg;
  m.stopFlag = false;
  m.name = cfg.device_name;
  m.resetParams();
  {
    std::lock_guard<std::mutex> lk(m.amx);
    m.phoneId = cfg.phone_id;
    if (std::all_of(m.phoneId.begin(), m.phoneId.end(), [](uint8_t b) { return b == 0; })) {
      const std::string seed = "owc-fakephone:" + cfg.device_name;
      const Key32 hsh = sha256(reinterpret_cast<const uint8_t*>(seed.data()), seed.size());
      std::memcpy(m.phoneId.data(), hsh.data(), m.phoneId.size());
    }
    m.pairings = cfg.paired_clients;
    m.loadPairings();
    m.lastCode.clear();
    m.lockUntilUs = 0;
    m.wrongCodes = 0;
  }
  m.lsock = bindSock(SOCK_STREAM, cfg.bind_address, cfg.control_port, err);
  if (m.lsock == INVALID_SOCKET) return false;
  listen(m.lsock, 4);
  m.usock = bindSock(SOCK_DGRAM, cfg.bind_address, cfg.udp_port, err);
  if (m.usock == INVALID_SOCKET) { stop(); return false; }
  int sb = 1 << 20;
  setsockopt(m.usock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sb), sizeof(sb));
  if (cfg.enable_discovery) {
    m.dsock = bindSock(SOCK_DGRAM, cfg.bind_address, cfg.discovery_port, err);
    if (m.dsock == INVALID_SOCKET) { stop(); return false; }
    m.dport = localPort(m.dsock);
  }
  m.cport = localPort(m.lsock);
  m.uport = localPort(m.usock);
  m.acceptTh = std::thread([&m] { m.acceptMain(); });
  m.udpTh = std::thread([&m] { m.udpMain(); });
  if (m.dsock != INVALID_SOCKET) m.discTh = std::thread([&m] { m.discMain(); });
  if (!cfg.announce_target.empty()) m.annTh = std::thread([&m] { m.announceMain(); });
  return true;
}

void FakePhone::stop() {
  Impl& m = *impl_;
  m.stopFlag = true;
  m.streaming = false;
  if (m.acceptTh.joinable()) m.acceptTh.join();
  if (m.sessionTh.joinable()) m.sessionTh.join();
  if (m.streamTh.joinable()) m.streamTh.join();
  if (m.udpTh.joinable()) m.udpTh.join();
  if (m.discTh.joinable()) m.discTh.join();
  if (m.annTh.joinable()) m.annTh.join();
  if (m.lsock != INVALID_SOCKET) { closesocket(m.lsock); m.lsock = INVALID_SOCKET; }
  if (m.usock != INVALID_SOCKET) { closesocket(m.usock); m.usock = INVALID_SOCKET; }
  if (m.dsock != INVALID_SOCKET) { closesocket(m.dsock); m.dsock = INVALID_SOCKET; }
}

uint16_t FakePhone::controlPort() const { return impl_->cport; }
uint16_t FakePhone::udpPort() const { return impl_->uport; }
uint16_t FakePhone::discoveryPort() const { return impl_->dport; }
bool FakePhone::streaming() const { return impl_->streaming; }

FakePhone::Counters FakePhone::counters() const {
  std::lock_guard<std::mutex> lk(impl_->cmx);
  return impl_->c;
}

Id16 FakePhone::phoneId() const {
  std::lock_guard<std::mutex> lk(impl_->amx);
  return impl_->phoneId;
}

std::string FakePhone::lastPairCode() const {
  std::lock_guard<std::mutex> lk(impl_->amx);
  return impl_->lastCode;
}

std::vector<FakePhoneConfig::PairedClient> FakePhone::pairedClients() const {
  std::lock_guard<std::mutex> lk(impl_->amx);
  return impl_->pairings;
}

void FakePhone::setPairedClients(const std::vector<FakePhoneConfig::PairedClient>& v) {
  std::lock_guard<std::mutex> lk(impl_->amx);
  impl_->pairings = v;
  impl_->savePairings();
  if (impl_->sessionAuthed && !impl_->findPairing(impl_->authClient)) impl_->unpairKick = true;
}

void FakePhone::rejectPairing() { impl_->rejectReq = true; }

void FakePhone::kickClient() { impl_->kick = true; }

}  // namespace fake
}  // namespace owc
