#include <owc/client.h>

#include "annexb.h"
#include "codec_select.h"
#include "decoder.h"
#include "metrics.h"
#include "sps_patch.h"
#include "transform.h"
#include "util.h"

#include <owc/shm.h>

#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <type_traits>

namespace owc {

using namespace detail;

// ================================================================ strings
const char* toString(ClientState s) {
  switch (s) {
    case ClientState::Idle: return "Idle";
    case ClientState::Connecting: return "Connecting";
    case ClientState::Handshaking: return "Handshaking";
    case ClientState::Connected: return "Connected";
    case ClientState::Streaming: return "Streaming";
    case ClientState::Reconnecting: return "Reconnecting";
    case ClientState::Failed: return "Failed";
  }
  return "?";
}

const char* toString(DisconnectReason r) {
  switch (r) {
    case DisconnectReason::None: return "None";
    case DisconnectReason::UserRequested: return "UserRequested";
    case DisconnectReason::ConnectFailed: return "ConnectFailed";
    case DisconnectReason::NoResponse: return "NoResponse";
    case DisconnectReason::Busy: return "Busy";
    case DisconnectReason::VersionMismatch: return "VersionMismatch";
    case DisconnectReason::BadMagic: return "BadMagic";
    case DisconnectReason::PeerBye: return "PeerBye";
    case DisconnectReason::PeerClosed: return "PeerClosed";
    case DisconnectReason::Timeout: return "Timeout";
    case DisconnectReason::ProtocolError: return "ProtocolError";
    case DisconnectReason::SocketError: return "SocketError";
  }
  return "?";
}

const char* toString(DecoderMode m) {
  switch (m) {
    case DecoderMode::Auto: return "Auto";
    case DecoderMode::Hardware: return "Hardware";
    case DecoderMode::Software: return "Software";
  }
  return "?";
}

namespace {

constexpr size_t kLogCap = 1000;
constexpr size_t kMaxQueuedDatagrams = 8192;  // ~11 MB worst case
constexpr size_t kMaxQueuedFrames = 240;      // TCP frames queued: memory safety net only (the
                                              // latency policy is decodeReady, age-based)
constexpr size_t kMaxJitterFrames = 64;
constexpr int64_t kKeyGraceUs = 150000;       // don't request IDR right at stream start
constexpr int64_t kMaxBacklogUs = 100000;     // + jitter_buffer_ms: oldest undecoded frame age

// FrameAssembler::Counters::frames_recovered appears when FEC lands in protocol/.
template <class T, class = void>
struct HasRecovered : std::false_type {};
template <class T>
struct HasRecovered<T, std::void_t<decltype(std::declval<T>().frames_recovered)>>
    : std::true_type {};
template <class C>
uint64_t recoveredOf(const C& c) {
  if constexpr (HasRecovered<C>::value)
    return static_cast<uint64_t>(c.frames_recovered);
  else
    return 0;
}

struct WorkItem {
  enum Kind : uint8_t { Datagram, Frame, Format, SessionStart, StreamEnd } kind = Datagram;
  UdpHeader udp;
  VideoFrameHeader vf;
  VideoFormat fmt;
  std::vector<uint8_t> data;
  int64_t arrival_us = 0;
};

struct SessionResult {
  DisconnectReason reason = DisconnectReason::None;
  std::string message;
};

// Estimates lost UDP fragments from frag_count of partially/never seen frames.
class FragLoss {
 public:
  void add(uint32_t seq, uint16_t count) {
    if (have_ && seqNewer(seq, newest_)) {
      uint32_t gap = seq - newest_ - 1u;
      if (gap > 0 && gap < 1000) lost_ += static_cast<uint64_t>(gap * avg_ + 0.5);
    }
    if (!have_ || seqNewer(seq, newest_)) { newest_ = seq; have_ = true; }
    auto& e = m_[seq];
    e.first = count;
    ++e.second;
    for (auto it = m_.begin(); it != m_.end();) {
      if (static_cast<int32_t>(newest_ - it->first) > 8) {
        if (it->second.first > it->second.second)
          lost_ += it->second.first - it->second.second;
        avg_ = avg_ * 0.9 + it->second.first * 0.1;
        it = m_.erase(it);
      } else {
        ++it;
      }
    }
  }
  uint64_t lost() const { return lost_; }
  void reset() { m_.clear(); have_ = false; }

 private:
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> m_;
  bool have_ = false;
  uint32_t newest_ = 0;
  double avg_ = 4;
  uint64_t lost_ = 0;
};

}  // namespace

// ================================================================ capabilities
namespace {

std::mutex g_capsMx;
bool g_capsProbed = false;
DecoderCapabilities g_caps;
bool g_capsOverridden = false;
DecoderCapabilities g_capsOverride;

DecoderCapabilities probeDecoders() {
  DecoderCapabilities caps;
  // Own thread: needs COM MTA + MFStartup regardless of the caller's apartment.
  std::thread([&caps] {
    MfThreadScope mf;
    if (!mf.ok()) return;
    for (Codec c : {Codec::H264, Codec::Hevc, Codec::Av1}) {
      DecoderCapabilities::Entry& e =
          c == Codec::H264 ? caps.h264 : c == Codec::Hevc ? caps.hevc : caps.av1;
      for (DecoderMode m : {DecoderMode::Hardware, DecoderMode::Software}) {
        MfDecoder d;
        DecoderConfig cfg;
        cfg.codec = c;
        cfg.width = 1920;
        cfg.height = 1080;
        cfg.mode = m;
        std::string err;
        if (!d.open(cfg, err)) continue;
        if (m == DecoderMode::Hardware && d.d3dConfigured()) e.hardware = true;
        if (m == DecoderMode::Software) e.software = true;
        if (e.mft_name.empty()) e.mft_name = d.mftName();
        d.close();
      }
    }
    Microsoft::WRL::ComPtr<IDXGIFactory1> f;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) && SUCCEEDED(f->EnumAdapters1(0, &a))) {
      DXGI_ADAPTER_DESC1 d{};
      if (SUCCEEDED(a->GetDesc1(&d))) {
        caps.adapter = wideToUtf8(d.Description);
        caps.dedicated_video_memory = d.DedicatedVideoMemory;
        caps.adapter_discrete = d.DedicatedVideoMemory >= (512ull << 20);
      }
    }
  }).join();
  return caps;
}

}  // namespace

DecoderCapabilities Client::decoderCapabilities() {
  std::lock_guard<std::mutex> lk(g_capsMx);
  if (g_capsOverridden) return g_capsOverride;
  if (!g_capsProbed) {
    g_caps = probeDecoders();
    g_capsProbed = true;
  }
  return g_caps;
}

void Client::overrideDecoderCapabilitiesForTest(const DecoderCapabilities* caps) {
  std::lock_guard<std::mutex> lk(g_capsMx);
  g_capsOverridden = caps != nullptr;
  if (caps) g_capsOverride = *caps;
}

// ================================================================ Impl
struct Client::Impl {
  ClientEvents* ev = nullptr;

  // ---- options
  mutable std::mutex optMx;
  ClientOptions opt;
  std::atomic<bool> decoderReconfig{false}, assemblerReset{false}, udpBufDirty{false};
  ClientOptions getOpt() const {
    std::lock_guard<std::mutex> lk(optMx);
    return opt;
  }

  // ---- control (ctlMx)
  std::mutex ctlMx;
  std::condition_variable ctlCv;
  bool quit = false;
  bool wantConnected = false;
  uint64_t gen = 0;
  ConnectParams params;
  std::deque<std::function<void()>> pending;  // events from other threads
  std::atomic<bool> wantStreaming{false};
  std::thread::id netId;

  // ---- sockets
  std::mutex sendMx;
  SOCKET tcp = INVALID_SOCKET;
  std::atomic<bool> sessionUp{false};
  std::mutex udpMx;
  SOCKET udp = INVALID_SOCKET;
  std::atomic<bool> udpStop{false};
  std::atomic<bool> punched{false};
  std::thread udpThread;

  // ---- cached state (stMx)
  mutable std::mutex stMx;
  ClientState state = ClientState::Idle;
  DisconnectReason reason = DisconnectReason::None;
  bool haveDev = false, haveCaps = false, haveFmt = false, havePhone = false;
  DeviceInfo dev;
  Capabilities caps;
  VideoFormat fmt;
  Stats phone;
  std::string peer;
  VideoTransport transport = VideoTransport::Tcp;
  int64_t sessionStartMs = 0;
  uint32_t reconnects = 0;

  // ---- clock
  mutable std::mutex clockMx;
  ClockSync clock;

  // ---- statistics (statMx)
  mutable std::mutex statMx;
  struct Rx {
    uint64_t frames_received = 0, frames_lost_extra = 0;
    uint64_t asm_frames_lost = 0, fragments_received = 0, fragments_lost = 0, recovered = 0;
    uint64_t datagrams_dropped = 0;
    uint64_t notPresented = 0, backlogDrops = 0;
    RateMeter rxFps, rxBits, decFps, pubFps;
    JitterEstimator jitter;
    DelayTrend trend{20};
    SampleWindow arrToDec, capToPub, capToArr, decodeMs, decDelay, readbackMs;
    uint32_t decDelayMax = 0;
    uint64_t frames_decoded = 0, frames_published = 0;
    bool decoderHw = false, decoderLL = false, bframes = false, spsPatched = false;
    std::string decName, decErr, decSelection, shmError, codecSel;
    Codec requestedCodec = Codec::H264;
    bool shmPublishing = false;
    uint32_t colorMatrix = 0;
    bool fullRange = false;
    uint32_t outW = 0, outH = 0, jbFrames = 0;
    bool haveTcpSeq = false;
    uint32_t lastTcpSeq = 0;
  } rx;

  // ---- logs
  mutable std::mutex logMx;
  std::deque<LogEntry> logs;
  uint64_t nextLogId = 1;

  // ---- keyframe requests
  std::atomic<bool> needKey{false};
  std::atomic<bool> forceKey{false};
  std::mutex kfMx;
  KeyframePolicy kfPolicy;
  std::atomic<uint32_t> kfRequests{0};
  std::atomic<int32_t> bframesParam{0};

  // ---- codec negotiation
  std::atomic<uint32_t> blockedCodecs{0};   // codecBit()s that failed (until connect())
  std::atomic<int> codecFailure{-1};        // set by the decode thread: Codec that failed
  Codec requestedCodec = Codec::H264;       // net thread
  bool capsSeen = false, negotiatePending = false;  // net thread, per session

  // ---- decode queue (qMx)
  std::mutex qMx;
  std::condition_variable qCv;
  std::deque<WorkItem> q;
  size_t qFrames = 0, qDatagrams = 0;
  bool decQuit = false;
  std::atomic<bool> dropToKey{false};

  // ---- latest frame
  mutable std::mutex latestMx;
  FrameCopy latest;
  bool haveLatest = false;
  std::atomic<uint64_t> publishCounter{0};

  std::thread netThread, decThread;

  // ============================================================ helpers
  bool onNet() const { return std::this_thread::get_id() == netId; }

  void postEvent(std::function<void()> fn) {
    if (!ev) return;
    if (onNet()) {
      fn();
      return;
    }
    std::lock_guard<std::mutex> lk(ctlMx);
    pending.push_back(std::move(fn));
    ctlCv.notify_all();
  }

  // Runs pending events on the net thread (caller must NOT hold ctlMx).
  void drainEvents() {
    std::deque<std::function<void()>> v;
    {
      std::lock_guard<std::mutex> lk(ctlMx);
      v.swap(pending);
    }
    for (auto& f : v) f();
  }

  void log(LogLevel lvl, LogSource src, const std::string& text) {
    LogEntry e;
    e.pc_time_ms = wallClockMs();
    e.source = src;
    e.level = lvl;
    e.text = text;
    {
      std::lock_guard<std::mutex> lk(logMx);
      e.id = nextLogId++;
      logs.push_back(e);
      while (logs.size() > kLogCap) logs.pop_front();
    }
    if (ev) postEvent([this, e] { ev->onLog(e); });
  }
  void logf(LogLevel lvl, const char* fmtStr, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmtStr);
    vsnprintf(buf, sizeof(buf), fmtStr, ap);
    va_end(ap);
    log(lvl, LogSource::Client, buf);
  }

  // Net thread only.
  void setState(ClientState s, DisconnectReason r = DisconnectReason::None,
                const std::string& msg = std::string()) {
    {
      std::lock_guard<std::mutex> lk(stMx);
      if (state == s && r == DisconnectReason::None) return;
      state = s;
      if (r != DisconnectReason::None) reason = r;
    }
    if (ev) ev->onStateChanged(s, r, msg);
  }
  ClientState getState() const {
    std::lock_guard<std::mutex> lk(stMx);
    return state;
  }

  bool sendRaw(const std::vector<uint8_t>& buf) {
    std::lock_guard<std::mutex> lk(sendMx);
    if (tcp == INVALID_SOCKET) return false;
    size_t off = 0;
    const int64_t deadline = steadyMs() + 2000;
    while (off < buf.size()) {
      int n = send(tcp, reinterpret_cast<const char*>(buf.data() + off),
                   static_cast<int>(buf.size() - off), 0);
      if (n > 0) {
        off += static_cast<size_t>(n);
        continue;
      }
      if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        if (steadyMs() > deadline) return false;
        fd_set w;
        FD_ZERO(&w);
        FD_SET(tcp, &w);
        timeval tv{0, 20000};
        select(0, nullptr, &w, nullptr, &tv);
        continue;
      }
      return false;
    }
    return true;
  }
  bool sendMsg(MsgType t, const std::vector<uint8_t>& payload, bool requireSession = true) {
    if (requireSession && !sessionUp) return false;
    std::vector<uint8_t> b;
    b.reserve(payload.size() + kTcpHeaderSize);
    appendTcpMessage(b, t, payload);
    return sendRaw(b);
  }
  bool sendCmd(CommandId id, int64_t arg = 0, float x = 0, float y = 0,
               const std::string& text = std::string()) {
    Command c;
    c.cmd = id;
    c.arg = arg;
    c.x = x;
    c.y = y;
    c.text = text;
    return sendMsg(MsgType::Command, encode(c));
  }

  int64_t frameIntervalUs() const {
    std::lock_guard<std::mutex> lk(stMx);
    const uint32_t f = haveFmt && fmt.fps ? fmt.fps : 30;
    return 1000000 / f;
  }
  int64_t rttUs() const {
    std::lock_guard<std::mutex> lk(clockMx);
    return clock.synced() ? clock.minRttUs() : 0;
  }
  bool clockSynced(int64_t& offset) const {
    std::lock_guard<std::mutex> lk(clockMx);
    offset = clock.offsetUs();
    return clock.synced();
  }

  bool requestKeyframeNow(bool user) {
    if (!sessionUp) return false;
    const int64_t now = pcNowUs();
    {
      std::lock_guard<std::mutex> lk(kfMx);
      if (user) {
        // honour the min interval for user requests too
        const ClientOptions o = getOpt();
        if (!kfPolicy.poll(now, true, int64_t(o.keyframe_request_min_interval_ms) * 1000, 0, 0)) {
          kfPolicy.poll(now, false, 0, 0, 0);  // end the pseudo-episode
          return true;                         // rate-limited: treat as sent
        }
        kfPolicy.poll(now, false, 0, 0, 0);
      } else {
        kfPolicy.noteSent(now);
      }
    }
    ++kfRequests;
    return sendCmd(CommandId::RequestKeyframe);
  }

  // ============================================================ decode queue
  void pushWork(WorkItem&& w) {
    std::lock_guard<std::mutex> lk(qMx);
    if (w.kind == WorkItem::Datagram) {
      if (qDatagrams >= kMaxQueuedDatagrams) {
        std::lock_guard<std::mutex> sl(statMx);
        ++rx.datagrams_dropped;
        return;
      }
      ++qDatagrams;
    } else if (w.kind == WorkItem::Frame) {
      if (qFrames >= kMaxQueuedFrames) {
        // Decoder can't keep up: drop the backlog, resume at the next keyframe.
        size_t dropped = 0;
        for (auto it = q.begin(); it != q.end();) {
          if (it->kind == WorkItem::Frame) { it = q.erase(it); ++dropped; }
          else ++it;
        }
        qFrames = 0;
        dropToKey = true;
        std::lock_guard<std::mutex> sl(statMx);
        rx.frames_lost_extra += dropped;
      }
      ++qFrames;
    } else if (w.kind == WorkItem::SessionStart) {
      q.clear();
      qFrames = qDatagrams = 0;
    }
    q.push_back(std::move(w));
    qCv.notify_one();
  }

  // ============================================================ net thread
  void netMain() {
    setThreadName(L"owc-net");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    winsockInit();
    for (;;) {
      drainEvents();
      uint64_t myGen;
      ConnectParams p;
      {
        std::unique_lock<std::mutex> lk(ctlMx);
        if (quit) break;
        if (!wantConnected) {
          ctlCv.wait_for(lk, std::chrono::milliseconds(100));
          lk.unlock();
          const ClientState s = getState();
          if (s != ClientState::Idle && s != ClientState::Failed) {
            bool stillOff;
            {
              std::lock_guard<std::mutex> lk2(ctlMx);
              stillOff = !wantConnected;
            }
            if (stillOff) setState(ClientState::Idle, DisconnectReason::UserRequested);
          }
          continue;
        }
        myGen = gen;
        p = params;
      }
      SessionResult r = runSession(p, myGen);
      bool userStop;
      {
        std::lock_guard<std::mutex> lk(ctlMx);
        if (quit) break;
        userStop = gen != myGen || !wantConnected;
      }
      if (userStop) {
        bool off;
        {
          std::lock_guard<std::mutex> lk(ctlMx);
          off = !wantConnected;
        }
        if (off) setState(ClientState::Idle, DisconnectReason::UserRequested);
        continue;
      }
      const ClientOptions o = getOpt();
      const bool fatal = r.reason == DisconnectReason::VersionMismatch ||
                         r.reason == DisconnectReason::BadMagic;
      logf(LogLevel::Warn, "session ended: %s %s", toString(r.reason), r.message.c_str());
      if (o.auto_reconnect && !fatal) {
        setState(ClientState::Reconnecting, r.reason, r.message);
        {
          std::lock_guard<std::mutex> lk(stMx);
          ++reconnects;
        }
        const int64_t until = steadyMs() + o.reconnect_delay_ms;
        while (steadyMs() < until) {
          drainEvents();
          std::unique_lock<std::mutex> lk(ctlMx);
          if (quit || gen != myGen || !wantConnected) break;
          ctlCv.wait_for(lk, std::chrono::milliseconds(20));
        }
      } else {
        {
          std::lock_guard<std::mutex> lk(ctlMx);
          if (gen == myGen) wantConnected = false;
        }
        setState(ClientState::Failed, r.reason, r.message);
      }
    }
    drainEvents();
  }

  bool aborted(uint64_t myGen) {
    std::lock_guard<std::mutex> lk(ctlMx);
    return quit || gen != myGen || !wantConnected;
  }

  // Non-blocking connect with timeout, abortable. Returns INVALID_SOCKET on failure.
  SOCKET connectTcp(const sockaddr_in& addr, uint32_t timeoutMs, uint64_t myGen,
                    std::string& err) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { err = "socket() failed"; return s; }
    BOOL nd = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
    int rb = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rb), sizeof(rb));
    setNonBlocking(s, true);
    int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) return s;
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
      err = "connect error " + std::to_string(WSAGetLastError());
      closesocket(s);
      return INVALID_SOCKET;
    }
    const int64_t deadline = steadyMs() + timeoutMs;
    while (steadyMs() < deadline) {
      if (aborted(myGen)) { err = "aborted"; closesocket(s); return INVALID_SOCKET; }
      fd_set w, e;
      FD_ZERO(&w);
      FD_ZERO(&e);
      FD_SET(s, &w);
      FD_SET(s, &e);
      timeval tv{0, 50000};
      int n = select(0, nullptr, &w, &e, &tv);
      if (n > 0) {
        if (FD_ISSET(s, &e)) {
          int so = 0, sl = sizeof(so);
          getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &sl);
          err = so == WSAECONNREFUSED ? "connection refused" : "connect error " + std::to_string(so);
          closesocket(s);
          return INVALID_SOCKET;
        }
        if (FD_ISSET(s, &w)) return s;
      }
    }
    err = "connect timeout";
    closesocket(s);
    return INVALID_SOCKET;
  }

  // Reads available bytes into parser; returns -1 closed, -2 error, else bytes read.
  int pump(SOCKET s, TcpFrameParser& parser, int64_t& t_recv_us) {
    uint8_t buf[64 * 1024];
    int total = 0;
    for (;;) {
      int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
      if (n > 0) {
        if (total == 0) t_recv_us = pcNowUs();
        total += n;
        if (!parser.feed(buf, static_cast<size_t>(n))) return -3;
        if (n < static_cast<int>(sizeof(buf))) return total;
        continue;
      }
      if (n == 0) return -1;
      if (WSAGetLastError() == WSAEWOULDBLOCK) return total;
      return -2;
    }
  }

  void resetSessionCaches() {
    std::lock_guard<std::mutex> lk(stMx);
    haveDev = haveCaps = haveFmt = havePhone = false;
    dev = DeviceInfo();
    caps = Capabilities();
    fmt = VideoFormat();
  }

  SessionResult runSession(const ConnectParams& p, uint64_t myGen) {
    SessionResult res;
    const ClientOptions o = getOpt();
    {
      std::lock_guard<std::mutex> lk(stMx);
      transport = p.transport;
      peer = p.host + ":" + std::to_string(p.port);
    }
    setState(ClientState::Connecting);
    sockaddr_in addr{};
    if (!resolveIPv4(p.host, p.port, addr)) {
      res.reason = DisconnectReason::ConnectFailed;
      res.message = "cannot resolve " + p.host;
      return res;
    }
    // ---- connect + handshake with retries (adb forward accepts TCP even if
    // nothing listens on the phone: success = HelloAck, not connect()).
    SOCKET s = INVALID_SOCKET;
    TcpFrameParser parser;
    HelloAck ack;
    bool gotAck = false;
    const uint32_t attempts = std::max<uint32_t>(1, o.handshake_attempts);
    std::string lastErr;
    bool everConnected = false;
    for (uint32_t a = 0; a < attempts && !gotAck; ++a) {
      if (aborted(myGen)) { res.reason = DisconnectReason::UserRequested; return res; }
      std::string err;
      s = connectTcp(addr, o.connect_timeout_ms, myGen, err);
      if (s == INVALID_SOCKET) {
        if (aborted(myGen)) { res.reason = DisconnectReason::UserRequested; return res; }
        res.reason = DisconnectReason::ConnectFailed;
        res.message = err;
        return res;
      }
      everConnected = true;
      setState(ClientState::Handshaking);
      parser = TcpFrameParser();
      Hello h;
      h.transport = p.transport;
      h.client_name = p.client_name;
      {
        std::lock_guard<std::mutex> lk(sendMx);
        tcp = s;
      }
      if (!sendMsg(MsgType::Hello, encode(h), false)) lastErr = "send Hello failed";
      const int64_t deadline = steadyMs() + o.handshake_timeout_ms;
      bool closed = false;
      while (!gotAck && !closed && steadyMs() < deadline) {
        if (aborted(myGen)) break;
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{0, 20000};
        if (select(0, &r, nullptr, nullptr, &tv) <= 0) continue;
        int64_t t = 0;
        int n = pump(s, parser, t);
        if (n < 0) { closed = true; lastErr = n == -1 ? "closed by peer" : "socket error"; break; }
        TcpFrameParser::Message m;
        // Only look for HelloAck; leave other messages queued in the parser.
        if (parser.next(m)) {
          if (m.type == MsgType::HelloAck && decode(m.payload.data(), m.payload.size(), ack)) {
            gotAck = true;
          } else {
            closed = true;
            lastErr = "unexpected first message";
          }
        }
      }
      if (!gotAck) {
        {
          std::lock_guard<std::mutex> lk(sendMx);
          tcp = INVALID_SOCKET;
        }
        closeSocket(s);
        if (aborted(myGen)) { res.reason = DisconnectReason::UserRequested; return res; }
        if (lastErr.empty()) lastErr = "no HelloAck";
        logf(LogLevel::Info, "handshake attempt %u/%u failed: %s", a + 1, attempts,
             lastErr.c_str());
      }
    }
    if (!gotAck) {
      res.reason = DisconnectReason::NoResponse;
      res.message = everConnected
                        ? "TCP connected but the OWC app did not answer (" + lastErr +
                              ") - is the app running and the server started on the phone?"
                        : lastErr;
      return res;
    }
    auto closeTcp = [&] {
      std::lock_guard<std::mutex> lk(sendMx);
      closeSocket(tcp);
      s = INVALID_SOCKET;
    };
    if (ack.magic != kMagic || ack.status == HelloStatus::BadMagic) {
      closeTcp();
      res.reason = DisconnectReason::BadMagic;
      res.message = "peer is not an OWC phone";
      return res;
    }
    if (ack.status == HelloStatus::Busy) {
      closeTcp();
      res.reason = DisconnectReason::Busy;
      res.message = "phone is busy (another PC is connected)";
      return res;
    }
    if (ack.status == HelloStatus::VersionMismatch || ack.version != kProtocolVersion) {
      closeTcp();
      res.reason = DisconnectReason::VersionMismatch;
      res.message = "protocol version mismatch (phone v" + std::to_string(ack.version) +
                    ", PC v" + std::to_string(kProtocolVersion) + ")";
      return res;
    }

    // ---- session up
    resetSessionCaches();
    DeviceInfo di;
    di.session_id = ack.session_id;
    di.device_name = ack.device_name;
    di.model = ack.model;
    di.android_api = ack.android_api;
    di.codec_mask = ack.codec_mask;
    di.udp_port = ack.udp_port;
    {
      std::lock_guard<std::mutex> lk(stMx);
      dev = di;
      haveDev = true;
      sessionStartMs = steadyMs();
    }
    {
      std::lock_guard<std::mutex> lk(clockMx);
      clock.reset();
    }
    {
      std::lock_guard<std::mutex> lk(statMx);
      rx.haveTcpSeq = false;
      rx.jitter.reset();
      rx.trend.reset();
    }
    punched = false;
    capsSeen = negotiatePending = false;
    codecFailure = -1;
    needKey = false;
    WorkItem start;
    start.kind = WorkItem::SessionStart;
    start.udp.session_id = ack.session_id;
    pushWork(std::move(start));
    sessionUp = true;
    logf(LogLevel::Info, "connected to %s (%s, Android API %u) via %s", di.device_name.c_str(),
         di.model.c_str(), di.android_api, p.transport == VideoTransport::Udp ? "UDP" : "TCP");
    setState(ClientState::Connected);
    if (ev) ev->onConnected(di);

    // ---- UDP
    sockaddr_in udpPeer = addr;
    udpPeer.sin_port = htons(ack.udp_port);
    if (p.transport == VideoTransport::Udp) {
      std::string err;
      if (!openUdp(o, ack.session_id, err)) {
        sessionUp = false;
        closeTcp();
        res.reason = DisconnectReason::SocketError;
        res.message = "UDP socket: " + err;
        return res;
      }
    }
    if (o.auto_start_stream) wantStreaming = true;

    // ---- main loop
    const int64_t t0 = steadyMs();
    int64_t nextPing = t0, nextRR = t0 + 250, nextPunch = t0, lastRx = t0;
    int pingsSent = 0;
    bool startSent = false;
    for (;;) {
      if (aborted(myGen)) {
        sendMsg(MsgType::Bye, {});
        res.reason = DisconnectReason::UserRequested;
        break;
      }
      drainEvents();
      const int64_t now = steadyMs();
      const ClientOptions oc = getOpt();
      if (udpBufDirty.exchange(false)) applyUdpBuffer(oc);

      if (p.transport == VideoTransport::Udp && now >= nextPunch) {
        sendPunch(udpPeer, ack.session_id);
        nextPunch = now + (punched ? 1000 : 200);
      }
      {
        const int cf = codecFailure.exchange(-1);
        if (cf >= 0 && cf != static_cast<int>(Codec::H264)) {
          blockedCodecs |= codecBit(static_cast<Codec>(cf));
          logf(LogLevel::Warn, "%s does not work here (decoder failed): falling back to H.264",
               codecName(static_cast<Codec>(cf)));
          negotiatePending = true;
        }
      }
      if (negotiatePending) {
        negotiatePending = false;
        negotiateCodec(ack.codec_mask, oc);
      }
      if (!startSent && wantStreaming && (capsSeen || now - t0 > 500) &&
          (p.transport == VideoTransport::Tcp || punched || now - t0 > 1000)) {
        if (sendCmd(CommandId::StartStream)) startSent = true;
      }
      if (startSent && !wantStreaming) startSent = false;
      if (now >= nextPing) {
        Ping pg;
        pg.t0_us = static_cast<uint64_t>(pcNowUs());
        sendMsg(MsgType::Ping, encode(pg));
        ++pingsSent;
        nextPing = now + (pingsSent < 4 ? 100 : 500);
      }
      if (now >= nextRR) {
        if (getState() == ClientState::Streaming) sendReceiverReport();
        nextRR = now + 250;
      }
      // keyframe policy
      {
        const bool need = needKey.load() || forceKey.exchange(false);
        bool send;
        {
          std::lock_guard<std::mutex> lk(kfMx);
          send = kfPolicy.poll(pcNowUs(), need && getState() == ClientState::Streaming,
                               int64_t(oc.keyframe_request_min_interval_ms) * 1000, rttUs(),
                               frameIntervalUs());
        }
        if (send) {
          ++kfRequests;
          sendCmd(CommandId::RequestKeyframe);
        }
      }
      if (now - lastRx > static_cast<int64_t>(oc.idle_timeout_ms)) {
        res.reason = DisconnectReason::Timeout;
        res.message = "phone silent for " + std::to_string(oc.idle_timeout_ms) + " ms";
        break;
      }
      // Messages already parsed during the handshake are handled first.
      bool stop = false;
      TcpFrameParser::Message m;
      int64_t trecv = pcNowUs();
      while (!stop && parser.next(m)) stop = !handleMsg(m, trecv, res);
      if (stop) break;

      fd_set r;
      FD_ZERO(&r);
      FD_SET(s, &r);
      timeval tv{0, 5000};
      int n = select(0, &r, nullptr, nullptr, &tv);
      if (n < 0) {
        res.reason = DisconnectReason::SocketError;
        res.message = "select failed";
        break;
      }
      if (n == 0) continue;
      int got = pump(s, parser, trecv);
      if (got > 0) lastRx = steadyMs();
      if (got == -1) {
        res.reason = DisconnectReason::PeerClosed;
        res.message = "connection closed by phone";
      } else if (got == -2) {
        res.reason = DisconnectReason::SocketError;
        res.message = "recv error " + std::to_string(WSAGetLastError());
      } else if (got == -3) {
        res.reason = DisconnectReason::ProtocolError;
        res.message = "bad TCP framing";
      }
      while (!stop && parser.next(m)) stop = !handleMsg(m, trecv, res);
      if (stop || got < 0) break;
    }
    // ---- teardown
    sessionUp = false;
    closeTcp();
    closeUdp();
    WorkItem end;
    end.kind = WorkItem::StreamEnd;
    pushWork(std::move(end));
    {
      std::lock_guard<std::mutex> lk(stMx);
      haveFmt = false;
    }
    return res;
  }

  // Net thread: decide the codec and ask the phone for it if needed.
  void negotiateCodec(uint32_t phoneMask, const ClientOptions& o) {
    CodecChoice ch = chooseCodec(o.codec_preference, phoneMask, Client::decoderCapabilities(),
                                 blockedCodecs.load());
    ParamDesc vc;
    bool haveParam = false;
    {
      std::lock_guard<std::mutex> lk(stMx);
      for (const ParamDesc& pd : caps.params)
        if (pd.id == ParamId::VideoCodec && (pd.flags & kParamSupported)) {
          vc = pd;
          haveParam = true;
        }
    }
    if (!haveParam && ch.codec != Codec::H264) {
      ch.reason += " - but the phone has no VideoCodec param: H.264";
      ch.codec = Codec::H264;
    }
    const bool changed = ch.codec != requestedCodec;
    requestedCodec = ch.codec;
    if (haveParam && vc.current.i != static_cast<int64_t>(ch.codec))
      sendMsg(MsgType::SetParam,
              encode(SetParam{ParamId::VideoCodec, ParamValue::EnumV(static_cast<int32_t>(ch.codec))}));
    {
      std::lock_guard<std::mutex> lk(statMx);
      if (rx.codecSel != ch.reason || changed) logf(LogLevel::Info, "codec %s", ch.reason.c_str());
      rx.codecSel = ch.reason;
      rx.requestedCodec = ch.codec;
    }
  }

  // Returns false to end the session (res filled).
  bool handleMsg(const TcpFrameParser::Message& m, int64_t trecv, SessionResult& res) {
    const uint8_t* d = m.payload.data();
    const size_t n = m.payload.size();
    switch (m.type) {
      case MsgType::VideoFrame: {
        VideoFrameHeader h;
        const uint8_t* au = nullptr;
        size_t len = 0;
        if (!decodeVideoFrame(d, n, h, au, len)) break;
        WorkItem w;
        w.kind = WorkItem::Frame;
        w.vf = h;
        w.data.assign(au, au + len);
        w.arrival_us = trecv;
        pushWork(std::move(w));
        break;
      }
      case MsgType::Pong: {
        Pong pg;
        if (!decode(d, n, pg)) break;
        std::lock_guard<std::mutex> lk(clockMx);
        clock.addSample(static_cast<int64_t>(pg.t0_us), static_cast<int64_t>(pg.phone_us), trecv);
        break;
      }
      case MsgType::Capabilities: {
        Capabilities c;
        if (!decode(d, n, c)) { logf(LogLevel::Warn, "bad Capabilities message"); break; }
        {
          std::lock_guard<std::mutex> lk(stMx);
          caps = c;
          haveCaps = true;
        }
        for (const ParamDesc& pd : c.params)
          if (pd.id == ParamId::BFrames) bframesParam = static_cast<int32_t>(pd.current.i);
        capsSeen = true;
        negotiatePending = true;
        if (ev) ev->onCapabilities(c);
        break;
      }
      case MsgType::ParamState: {
        ParamState ps;
        if (!decode(d, n, ps)) break;
        {
          std::lock_guard<std::mutex> lk(stMx);
          for (ParamDesc& pd : caps.params)
            if (pd.id == ps.id) pd.current = ps.value;
        }
        if (ps.id == ParamId::BFrames) bframesParam = static_cast<int32_t>(ps.value.i);
        if (ps.id == ParamId::VideoCodec && ps.status != ParamStatus::Ok &&
            requestedCodec != Codec::H264 && ps.value.i != static_cast<int64_t>(requestedCodec)) {
          blockedCodecs |= codecBit(requestedCodec);
          logf(LogLevel::Warn, "phone rejected %s: falling back to H.264", codecName(requestedCodec));
          negotiatePending = true;
        }
        if (ps.status != ParamStatus::Ok)
          logf(LogLevel::Warn, "phone rejected %s (status %d)", owc::toString(ps.id),
               static_cast<int>(ps.status));
        if (ev) ev->onParamState(ps);
        break;
      }
      case MsgType::VideoFormat: {
        VideoFormat f;
        if (!decode(d, n, f)) { logf(LogLevel::Warn, "bad VideoFormat message"); break; }
        {
          std::lock_guard<std::mutex> lk(stMx);
          fmt = f;
          haveFmt = true;
        }
        logf(LogLevel::Info, "video format: epoch %u %s %ux%u@%u rot %d%s csd %zu B", f.epoch,
             f.codec == Codec::H264 ? "H.264" : "HEVC", f.width, f.height, f.fps,
             f.rotation_deg, f.mirror ? " mirror" : "", f.csd.size());
        WorkItem w;
        w.kind = WorkItem::Format;
        w.fmt = f;
        pushWork(std::move(w));
        setState(ClientState::Streaming);
        if (ev) ev->onVideoFormat(f);
        break;
      }
      case MsgType::Stats: {
        Stats st;
        if (!decode(d, n, st)) break;
        {
          std::lock_guard<std::mutex> lk(stMx);
          phone = st;
          havePhone = true;
        }
        if (ev) ev->onPhoneStats(st);
        break;
      }
      case MsgType::Log: {
        LogMsg lm;
        if (decode(d, n, lm)) log(lm.level, LogSource::Phone, lm.text);
        break;
      }
      case MsgType::Bye:
        res.reason = DisconnectReason::PeerBye;
        res.message = "phone ended the session";
        return false;
      default:
        break;  // unknown/unused types are ignored
    }
    return true;
  }

  void sendReceiverReport() {
    ReceiverReport r;
    {
      std::lock_guard<std::mutex> lk(statMx);
      const int64_t now = pcNowUs();
      r.frames_received = static_cast<uint32_t>(rx.frames_received);
      r.frames_lost = static_cast<uint32_t>(rx.asm_frames_lost + rx.frames_lost_extra);
      r.fragments_received = static_cast<uint32_t>(rx.fragments_received);
      r.fragments_lost = static_cast<uint32_t>(rx.fragments_lost);
      r.jitter_ms = static_cast<float>(rx.jitter.jitterUs() / 1000.0);
      r.delay_gradient_ms = static_cast<float>(rx.trend.gradientMs());
      r.recv_bitrate_bps = static_cast<uint32_t>(rx.rxBits.rate(now));
      r.decode_ms = static_cast<float>(rx.decodeMs.avg());
    }
    sendMsg(MsgType::ReceiverReport, encode(r));
  }

  // ---- UDP
  bool openUdp(const ClientOptions& o, uint32_t sid, std::string& err) {
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u == INVALID_SOCKET) { err = "socket() failed"; return false; }
    // Ignore ICMP port-unreachable resets (WSAECONNRESET on recvfrom).
    BOOL f = FALSE;
    DWORD br = 0;
    WSAIoctl(u, SIO_UDP_CONNRESET, &f, sizeof(f), nullptr, 0, &br, nullptr, nullptr);
    sockaddr_in a{};
    if (!resolveIPv4(o.udp_bind_address.empty() ? "0.0.0.0" : o.udp_bind_address, 0, a) ||
        bind(u, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
      err = "bind failed " + std::to_string(WSAGetLastError());
      closesocket(u);
      return false;
    }
    DWORD tmo = 50;
    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
    {
      std::lock_guard<std::mutex> lk(udpMx);
      udp = u;
    }
    applyUdpBuffer(o);
    udpStop = false;
    udpThread = std::thread([this, u, sid] { udpMain(u, sid); });
    return true;
  }

  void applyUdpBuffer(const ClientOptions& o) {
    std::lock_guard<std::mutex> lk(udpMx);
    if (udp == INVALID_SOCKET) return;
    int sz = static_cast<int>(std::max<uint32_t>(64 * 1024, o.udp_recv_buffer_bytes));
    setsockopt(udp, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&sz), sizeof(sz));
  }

  void closeUdp() {
    udpStop = true;
    if (udpThread.joinable()) udpThread.join();
    std::lock_guard<std::mutex> lk(udpMx);
    closeSocket(udp);
  }

  void sendPunch(const sockaddr_in& to, uint32_t sid) {
    UdpHeader h;
    h.type = UdpType::Punch;
    h.session_id = sid;
    h.frame_seq = 0;
    h.frag_idx = 0;
    h.frag_count = 0;
    h.frame_size = 0;
    h.capture_ts_us = 0;
    h.epoch = 0;
    h.flags = 0;
    uint8_t b[kUdpHeaderSize];
    writeUdpHeader(h, b);
    std::lock_guard<std::mutex> lk(udpMx);
    if (udp != INVALID_SOCKET)
      sendto(udp, reinterpret_cast<const char*>(b), static_cast<int>(kUdpHeaderSize), 0,
             reinterpret_cast<const sockaddr*>(&to), sizeof(to));
  }

  // "owc-udp": only drains the socket into the decode queue (research sec. 3.2).
  void udpMain(SOCKET u, uint32_t sid) {
    setThreadName(L"owc-udp");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    uint8_t buf[2048];
    while (!udpStop) {
      sockaddr_in from{};
      int fl = sizeof(from);
      int n = recvfrom(u, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                       reinterpret_cast<sockaddr*>(&from), &fl);
      if (n <= 0) continue;  // timeout / transient error
      const int64_t arrival = pcNowUs();
      UdpHeader h;
      if (!readUdpHeader(buf, static_cast<size_t>(n), h)) continue;
      if (h.session_id != sid) continue;
      if (h.type == UdpType::PunchAck) {
        if (!punched.exchange(true)) logf(LogLevel::Info, "UDP path established");
        continue;
      }
      if (h.type != UdpType::Video && h.type != UdpType::Fec) continue;
      WorkItem w;
      w.kind = WorkItem::Datagram;
      w.udp = h;
      w.data.assign(buf + kUdpHeaderSize, buf + n);
      w.arrival_us = arrival;
      pushWork(std::move(w));
    }
  }

  // ============================================================ decode thread
  struct InFlight {
    int64_t sample_time;
    uint64_t input_idx;
    uint32_t seq, epoch;
    uint64_t capture_ts;
    int64_t arrival_us;
    int64_t submit_us;
  };
  struct JbFrame {
    std::vector<uint8_t> nv12;
    uint32_t w = 0, h = 0;
    InFlight meta{};
    int64_t present_at = 0;
  };
  struct EarlyFrame {
    FrameAssembler::Frame f;
    int64_t arrival_us;
  };
  struct Dec {
    std::unique_ptr<FrameAssembler> assembler;
    FragLoss fragLoss;
    MfDecoder decoder;
    bool haveFmt = false;
    VideoFormat fmt;
    std::vector<uint8_t> csd;
    bool waitKey = true;
    bool reopen = false;
    bool decoderFailed = false;   // open failed for this format; retry at next keyframe
    std::string lastOpenErr;
    int errors = 0;
    bool bframes = false;
    bool haveLastTs = false;
    uint64_t lastInputTs = 0;
    uint64_t inputs = 0;
    std::deque<InFlight> inflight;
    std::deque<EarlyFrame> early;
    int64_t epochFirstDataUs = 0;
    uint32_t lastStreamChanges = 0;
    int64_t streamChangeWindowStart = 0;
    uint32_t streamChangesInWindow = 0;
    std::vector<uint8_t> auBuf;
    std::vector<uint8_t> xform;
    std::deque<JbFrame> jb;
    std::vector<JbFrame> jbPool;
    std::deque<std::pair<int64_t, int64_t>> transit;  // (time, decoded-capture)
    ShmWriter shm;
    bool shmActive = false;
    int64_t shmRetryAt = 0;
    std::string shmLastErr;
    // colour from the H.264 SPS VUI (0 / -1 = not signalled)
    uint32_t spsMatrix = 0;
    int spsFullRange = -1;
    uint32_t shmW = 0, shmH = 0, shmFps = 0;
    int64_t lastHeartbeat = 0;
    uint32_t sessionId = 0;
    bool udpMode = false;  // datagrams seen this session
    // DecoderMode::Auto benchmark: 0 = measuring HW, 1 = measuring SW, 2 = decided
    int autoPhase = 0;
    DecoderMode autoChoice = DecoderMode::Auto;
    double benchMs[2] = {0, 0};
    uint32_t benchN[2] = {0, 0};
    uint32_t framesSinceOpen = 0;
    uint32_t inputsSinceOpen = 0, outputsSinceOpen = 0;
    bool codecFailReported = false;
    // Latency-first decode backlog (see decodeReady)
    std::deque<EarlyFrame> ready;   // complete frames not decoded yet (oldest first)
    bool presentNext = true;        // present the output of the frame being decoded
    int64_t backlogWindowStart = 0;
    uint32_t backlogDropsInWindow = 0;
    int64_t lastReevalUs = 0;
    int64_t lastBacklogLogUs = 0;
    double frameCostUs = 0;
    int64_t lastPresentUs = 0;     // EMA of decodeFrame() duration (decode + present when presented)
  };

  void decMain() {
    setThreadName(L"owc-decode");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    MfThreadScope mf;
    if (!mf.ok()) logf(LogLevel::Error, "MFStartup failed: decoding unavailable");
    Dec ds;
    ds.assembler.reset(new FrameAssembler(getOpt().udp_max_pending_frames));
    for (;;) {
      std::deque<WorkItem> batch;
      {
        std::unique_lock<std::mutex> lk(qMx);
        int64_t waitUs = 50000;
        if (!ds.jb.empty())
          waitUs = std::min<int64_t>(waitUs, ds.jb.front().present_at - pcNowUs());
        if (q.empty() && !decQuit && waitUs > 0)
          qCv.wait_for(lk, std::chrono::microseconds(waitUs));
        if (decQuit) break;
        batch.swap(q);
        qFrames = qDatagrams = 0;
      }
      for (WorkItem& w : batch) process(ds, w);
      decodeReady(ds);
      presentDue(ds, false);
      // liveness
      const ClientOptions o = getOpt();
      if (!o.publish_to_shm && ds.shm.isOpen()) {
        if (ds.shmActive) ds.shm.setActive(false);
        ds.shm.close();  // releases the producer marker for other apps
        ds.shmActive = false;
        std::lock_guard<std::mutex> lk(statMx);
        rx.shmPublishing = false;
      }
      if (ds.shmActive && steadyMs() - ds.lastHeartbeat >= 50) {
        ds.shm.heartbeat();
        ds.lastHeartbeat = steadyMs();
      }
    }
    if (ds.shmActive) ds.shm.setActive(false);
    ds.shm.close();
    ds.decoder.close();
    {
      std::lock_guard<std::mutex> lk(statMx);
      rx.shmPublishing = false;
    }
  }

  void setNeedKey(Dec& ds) {
    // The assembler only matters for UDP sessions (it starts in "need keyframe").
    const bool asmNeed = ds.udpMode && ds.assembler && ds.assembler->needKeyframe() &&
                         ds.epochFirstDataUs != 0;
    const bool raw = ds.waitKey || asmNeed;
    const bool grace =
        ds.epochFirstDataUs == 0 || pcNowUs() - ds.epochFirstDataUs < kKeyGraceUs;
    needKey = raw && !grace;
  }

  void process(Dec& ds, WorkItem& w) {
    switch (w.kind) {
      case WorkItem::SessionStart:
        ds.assembler.reset(new FrameAssembler(getOpt().udp_max_pending_frames));
        ds.fragLoss.reset();
        ds.haveFmt = false;
        ds.waitKey = true;
        ds.early.clear();
        ds.ready.clear();
        ds.jb.clear();
        ds.inflight.clear();
        ds.haveLastTs = false;
        ds.bframes = false;
        ds.epochFirstDataUs = 0;
        ds.sessionId = w.udp.session_id;
        resetAutoBench(ds);
        ds.udpMode = false;
        if (ds.decoder.isOpen()) ds.decoder.flush();
        break;
      case WorkItem::StreamEnd:
        ds.ready.clear();
        presentDue(ds, true);
        ds.jb.clear();
        if (ds.shmActive) {
          ds.shm.setActive(false);
          ds.shmActive = false;
        }
        ds.waitKey = true;
        ds.epochFirstDataUs = 0;
        needKey = false;
        break;
      case WorkItem::Format:
        decodeReady(ds);  // frames of the previous epoch first
        onFormat(ds, w.fmt);
        break;
      case WorkItem::Frame: {
        FrameAssembler::Frame f;
        f.frame_seq = w.vf.frame_seq;
        f.epoch = static_cast<uint16_t>(w.vf.epoch & 0xFFFF);
        f.flags = w.vf.flags;
        f.capture_ts_us = w.vf.capture_ts_us;
        f.data.swap(w.data);
        {
          std::lock_guard<std::mutex> lk(statMx);
          if (rx.haveTcpSeq && seqNewer(f.frame_seq, rx.lastTcpSeq)) {
            const uint32_t gap = f.frame_seq - rx.lastTcpSeq - 1u;
            if (gap < 100000) rx.frames_lost_extra += gap;
          }
          rx.haveTcpSeq = true;
          rx.lastTcpSeq = f.frame_seq;
        }
        if (dropToKey.exchange(false)) {
          ds.waitKey = true;
          logf(LogLevel::Warn, "decoder backlog: dropping frames until next keyframe");
          // Auto mode still measuring the hardware path: it is too slow for
          // this stream, try software right away.
          if (getOpt().decoder_mode == DecoderMode::Auto && ds.autoPhase == 0 &&
              ds.decoder.isOpen() && ds.decoder.hardware()) {
            ds.benchMs[0] = 1e6;
            ds.benchN[0] = 1;
            ds.autoPhase = 1;
            ds.reopen = true;
            forceKey = true;
            logf(LogLevel::Info, "decoder auto: hardware cannot keep up (backlog), trying software");
            std::lock_guard<std::mutex> lk(statMx);
            rx.decSelection = "auto: hardware cannot keep up, trying software";
          }
        }
        onCompleteFrame(ds, f, w.arrival_us);
        break;
      }
      case WorkItem::Datagram: {
        if (assemblerReset.exchange(false))
          ds.assembler.reset(new FrameAssembler(getOpt().udp_max_pending_frames));
        if (ds.epochFirstDataUs == 0) ds.epochFirstDataUs = w.arrival_us;
        ds.udpMode = true;
        if (w.udp.type == UdpType::Video) ds.fragLoss.add(w.udp.frame_seq, w.udp.frag_count);
        FrameAssembler::Frame f;
        const bool done = ds.assembler->push(w.udp, w.data.data(), w.data.size(), f);
        {
          const auto& c = ds.assembler->counters();
          std::lock_guard<std::mutex> lk(statMx);
          rx.asm_frames_lost = c.frames_lost;
          rx.fragments_received = c.fragments_received;
          rx.fragments_lost = ds.fragLoss.lost();
          rx.recovered = recoveredOf(c);
        }
        if (done) onCompleteFrame(ds, f, w.arrival_us);
        setNeedKey(ds);
        break;
      }
    }
  }

  void onFormat(Dec& ds, const VideoFormat& f) {
    const bool sameGeometry = ds.haveFmt && ds.fmt.codec == f.codec && ds.fmt.width == f.width &&
                              ds.fmt.height == f.height;
    ds.fmt = f;
    ds.haveFmt = true;
    ds.csd = f.csd;
    ds.spsMatrix = 0;
    ds.spsFullRange = -1;
    updateColorFromAu(ds, ds.csd.data(), ds.csd.size());
    ds.waitKey = true;
    ds.decoderFailed = false;
    ds.haveLastTs = false;
    ds.epochFirstDataUs = 0;
    ds.inflight.clear();
    if (!sameGeometry) resetAutoBench(ds);
    if (!sameGeometry || ds.reopen) {
      ds.decoder.close();
    } else if (ds.decoder.isOpen()) {
      ds.decoder.flush();
    }
    // Frames that arrived (UDP) before their VideoFormat.
    std::deque<EarlyFrame> early;
    early.swap(ds.early);
    for (EarlyFrame& e : early)
      if (e.f.epoch == static_cast<uint16_t>(f.epoch & 0xFFFF)) ds.ready.push_back(std::move(e));
  }

  static constexpr uint32_t kBenchSkip = 10, kBenchFrames = 40;

  void resetAutoBench(Dec& ds) {
    ds.autoPhase = 0;
    ds.autoChoice = DecoderMode::Auto;
    ds.benchMs[0] = ds.benchMs[1] = 0;
    ds.benchN[0] = ds.benchN[1] = 0;
  }

  // Auto mode: HW decode is not always faster end-to-end, because the GPU ->
  // CPU read-back can be slow (measured ~11 ms at 720p / ~21 ms at 1080p on an
  // Intel Iris Xe vs 2.5 / 5.5 ms for the software MFT). Measure both on the
  // live stream and keep the faster one.
  void autoBench(Dec& ds, double decodeMs) {
    if (getOpt().decoder_mode != DecoderMode::Auto || ds.autoPhase >= 2) return;
    ++ds.framesSinceOpen;
    if (ds.framesSinceOpen <= kBenchSkip) return;
    const int ph = ds.autoPhase;
    ds.benchMs[ph] += decodeMs;
    ++ds.benchN[ph];
    // Conclude early if this path cannot even keep up with the frame rate
    // (e.g. 240 fps high-speed mode): waiting for 40 samples would mean
    // seconds of backlog drops.
    const double frameMs = 1000.0 / (ds.fmt.fps ? ds.fmt.fps : 30);
    const bool tooSlow = ds.benchN[ph] >= 8 && ds.benchMs[ph] / ds.benchN[ph] > 0.9 * frameMs;
    if (ds.benchN[ph] < kBenchFrames && !tooSlow) return;
    const double avg = ds.benchMs[ph] / ds.benchN[ph];
    char msg[200];
    if (ph == 0) {
      if (!ds.decoder.hardware()) {
        ds.autoPhase = 2;  // no hardware path: nothing to compare
        snprintf(msg, sizeof(msg), "auto: software only (%.1f ms)", avg);
      } else if (avg <= 4.0) {
        ds.autoPhase = 2;
        snprintf(msg, sizeof(msg), "auto: hardware (%.1f ms/frame incl. read-back)", avg);
      } else {
        ds.autoPhase = 1;
        ds.reopen = true;
        forceKey = true;
        snprintf(msg, sizeof(msg), "auto: hardware %.1f ms/frame, trying software", avg);
      }
    } else {
      const double hw = ds.benchMs[0] / std::max<uint32_t>(1, ds.benchN[0]);
      ds.autoPhase = 2;
      if (avg < hw * 0.8) {
        ds.autoChoice = DecoderMode::Software;
        if (hw >= 1e5)
          snprintf(msg, sizeof(msg), "auto: software %.1f ms (hardware could not keep up) -> software", avg);
        else
          snprintf(msg, sizeof(msg), "auto: software %.1f ms vs hardware %.1f ms -> software", avg, hw);
      } else {
        ds.autoChoice = DecoderMode::Hardware;
        ds.reopen = true;
        forceKey = true;
        snprintf(msg, sizeof(msg), "auto: software %.1f ms vs hardware %.1f ms -> hardware", avg, hw);
      }
    }
    logf(LogLevel::Info, "decoder %s", msg);
    std::lock_guard<std::mutex> lk(statMx);
    rx.decSelection = msg;
  }

  void openDecoder(Dec& ds) {
    const ClientOptions o = getOpt();
    DecoderConfig dc;
    dc.codec = ds.fmt.codec;
    dc.width = ds.fmt.width;
    dc.height = ds.fmt.height;
    dc.fps = ds.fmt.fps;
    dc.mode = o.decoder_mode;
    if (o.decoder_mode == DecoderMode::Auto)
      dc.mode = ds.autoPhase == 1 ? DecoderMode::Software
                                  : ds.autoPhase == 2 ? ds.autoChoice : DecoderMode::Auto;
    ds.framesSinceOpen = 0;
    ds.inputsSinceOpen = ds.outputsSinceOpen = 0;
    ds.codecFailReported = false;
    // Measured with the Microsoft H.264 MFT: low-latency OFF buffers a whole
    // DPB (14-19 frames); ON still outputs B-frame streams in display order.
    dc.low_latency = o.low_latency_decode;
    std::string err;
    const bool ok = ds.decoder.open(dc, err);
    ds.reopen = false;
    {
      std::lock_guard<std::mutex> lk(statMx);
      rx.decName = ok ? ds.decoder.name() : std::string();
      rx.decErr = ok ? std::string() : err;
      rx.decoderLL = ok && dc.low_latency;
      rx.decoderHw = false;
    }
    if (ok) {
      ds.decoderFailed = false;
      ds.lastStreamChanges = 0;
      logf(LogLevel::Info, "decoder: %s, low-latency %s", ds.decoder.name().c_str(),
           dc.low_latency ? "on" : "off");
    } else {
      ds.decoderFailed = true;
      if (err != ds.lastOpenErr) logf(LogLevel::Error, "decoder open failed: %s", err.c_str());
      if (ds.fmt.codec != Codec::H264) codecFailure = static_cast<int>(ds.fmt.codec);
    }
    ds.lastOpenErr = ok ? std::string() : err;
  }

  void onCompleteFrame(Dec& ds, FrameAssembler::Frame& f, int64_t arrival) {
    int64_t offset = 0;
    const bool synced = clockSynced(offset);
    {
      std::lock_guard<std::mutex> lk(statMx);
      ++rx.frames_received;
      rx.rxFps.add(arrival);
      rx.rxBits.add(arrival, static_cast<double>(f.data.size()) * 8.0);
      rx.jitter.add(static_cast<int64_t>(f.capture_ts_us), arrival);
      rx.trend.add(static_cast<int64_t>(f.capture_ts_us) - offset, arrival);
      if (synced)
        rx.capToArr.add(static_cast<double>(arrival - (static_cast<int64_t>(f.capture_ts_us) - offset)) / 1000.0);
    }
    if (ds.epochFirstDataUs == 0) ds.epochFirstDataUs = arrival;
    ds.ready.push_back(EarlyFrame{std::move(f), arrival});
  }

  // Latency-first decode of the complete frames gathered from one batch:
  //  * every frame is DECODED (keeps references valid, no IDR needed), but only
  //    the newest is read back / transformed / published / sent to onFrame
  //    (+ all of them when a jitter buffer is configured) -> when the PC
  //    can't keep up, the published fps degrades and latency stays bounded;
  //  * if even decode-only falls behind (oldest frame older than 100 ms +
  //    jitter_buffer_ms), jump to the newest keyframe in the backlog, or drop
  //    it all, wait for a keyframe and request one (counted in frames_lost).
  void decodeReady(Dec& ds) {
    if (ds.ready.empty()) return;
    const ClientOptions o = getOpt();
    const int64_t now = pcNowUs();
    const int64_t maxAge = kMaxBacklogUs + static_cast<int64_t>(o.jitter_buffer_ms) * 1000;
    // Predictive: age of the oldest frame + time to get through the backlog.
    // Soft limit: jump to the newest keyframe already in the backlog (free).
    // Hard limit (1.5x): decode-only can't keep up at all -> drop everything,
    // wait for a keyframe and request one (an IDR costs bandwidth and a round
    // trip, so it is the last resort).
    const int64_t backlogCost = static_cast<int64_t>(ds.frameCostUs * static_cast<double>(ds.ready.size()));
    const int64_t projected = now - ds.ready.front().arrival_us + backlogCost;
    size_t keyIdx = ds.ready.size();
    for (size_t i = ds.ready.size(); i-- > 0;)
      if (ds.ready[i].f.flags & kFrameKey) {
        keyIdx = i;
        break;
      }
    size_t drop = 0;
    if (ds.ready.size() > 1 && projected > maxAge && keyIdx < ds.ready.size()) {
      drop = keyIdx;  // everything before the newest keyframe (never the keyframe itself)
    } else if (ds.ready.size() > 1 && projected > maxAge + maxAge / 2 &&
               keyIdx == ds.ready.size() && !ds.waitKey) {
      // The cost estimate only learns from delta frames, so keyframe/decoder
      // re-open spikes can't trigger an IDR-request loop. While waiting for a
      // keyframe, deltas are skipped anyway.
      drop = ds.ready.size();
      ds.waitKey = true;
      forceKey = true;
    }
    if (drop) {
      for (size_t i = 0; i < drop; ++i) ds.ready.pop_front();
      {
        std::lock_guard<std::mutex> lk(statMx);
        rx.frames_lost_extra += drop;
        ++rx.backlogDrops;
      }
      if (now - ds.lastBacklogLogUs > 1000000) {
        ds.lastBacklogLogUs = now;
        logf(LogLevel::Warn, "decoder can't keep up: dropped %zu backlog frame(s) (%s)", drop,
             ds.ready.empty() ? "requesting keyframe" : "jumped to keyframe");
      }
      noteBacklogDrop(ds, now);
    }
    while (!ds.ready.empty()) {
      EarlyFrame r = std::move(ds.ready.front());
      ds.ready.pop_front();
      const bool newest = ds.ready.empty();
      const bool isKey = (r.f.flags & kFrameKey) != 0;
      const uint64_t inputsBefore = ds.inputs;
      const int64_t t0 = pcNowUs();
      // Present the newest frame of the backlog, plus one at least every 50 ms
      // so the published rate degrades to ~20 fps rather than collapsing.
      // (the extra presentations only while not behind: frame younger than maxAge/2).
      const bool presentIt = newest || o.jitter_buffer_ms > 0 ||
                             (t0 - ds.lastPresentUs >= 50000 && t0 - r.arrival_us < maxAge / 2);
      if (presentIt) ds.lastPresentUs = t0;
      decodeFrame(ds, r.f, r.arrival_us, presentIt);
      if (ds.inputs == inputsBefore) continue;  // not submitted (waiting for keyframe, ...)
      const double cost = static_cast<double>(pcNowUs() - t0);
      // steady-state cost: keyframes (and decoder re-opens on them) are outliers
      if (!isKey) ds.frameCostUs = ds.frameCostUs == 0 ? cost : ds.frameCostUs * 0.9 + cost * 0.1;
      // Auto HW/SW benchmark on the real per-frame cost (decode + read-back +
      // transform + publish when presented): what limits throughput.
      autoBench(ds, cost / 1000.0);
    }
  }

  // Auto mode: repeated backlog drops => the chosen path can't keep up with
  // this stream (e.g. 1080p120): re-run the HW/SW benchmark (at most every 30 s).
  void noteBacklogDrop(Dec& ds, int64_t now) {
    if (now - ds.backlogWindowStart > 5000000) {
      ds.backlogWindowStart = now;
      ds.backlogDropsInWindow = 0;
    }
    ++ds.backlogDropsInWindow;
    if (getOpt().decoder_mode != DecoderMode::Auto || ds.autoPhase < 2) return;
    if (ds.backlogDropsInWindow < 2 || now - ds.lastReevalUs < 10000000) return;
    // Switch to the other path only if it was MEASURED faster than what the
    // current one costs now (never go back to a path known to be slower).
    const bool onSw = ds.autoChoice == DecoderMode::Software;
    const int other = onSw ? 0 : 1;  // benchMs index: 0 = HW, 1 = SW
    if (ds.benchN[other] == 0) return;
    const double otherMs = ds.benchMs[other] / ds.benchN[other];
    const double currentMs = ds.frameCostUs / 1000.0;
    if (!(otherMs < currentMs * 0.8)) return;
    ds.lastReevalUs = now;
    ds.autoChoice = onSw ? DecoderMode::Hardware : DecoderMode::Software;
    ds.reopen = true;
    forceKey = true;
    char msg[160];
    snprintf(msg, sizeof(msg), "auto: %s can't keep up with %u fps (%.1f ms/frame), switching to %s (%.1f ms)",
             onSw ? "software" : "hardware", ds.fmt.fps, currentMs, onSw ? "hardware" : "software",
             otherMs);
    logf(LogLevel::Info, "decoder %s", msg);
    std::lock_guard<std::mutex> lk(statMx);
    rx.decSelection = msg;
  }

  void decodeFrame(Dec& ds, FrameAssembler::Frame& f, int64_t arrival, bool present) {
    if (!ds.haveFmt || f.epoch != static_cast<uint16_t>(ds.fmt.epoch & 0xFFFF)) {
      if (ds.early.size() < 16) ds.early.push_back(EarlyFrame{std::move(f), arrival});
      return;
    }
    // AV1 is not Annex-B (OBUs): no NAL processing (csd prepend / SPS patch / colour).
    const bool annexB = ds.fmt.codec != Codec::Av1;
    if ((f.flags & kFrameConfig) && annexB) {
      // kFrameConfig = "the AU carries codec config". The Android encoder sets
      // it (together with kFrameKey) on IDRs with SPS/PPS prepended, so it is
      // NOT necessarily config-only: refresh the csd from its parameter sets
      // and decode the picture if there is one (real-phone bug: every IDR was
      // swallowed as config, the decoder never opened).
      std::vector<uint8_t> ps;
      bool picture = false;
      forEachNal(f.data.data(), f.data.size(), [&](const uint8_t* nal, size_t len) {
        const int t = nalType(ds.fmt.codec, nal[0]);
        if (isParamSetNal(ds.fmt.codec, t)) appendNal(ps, nal, len);
        else if (isVclNal(ds.fmt.codec, t)) picture = true;
      });
      if (!ps.empty()) {
        ds.csd.swap(ps);
        updateColorFromAu(ds, ds.csd.data(), ds.csd.size());
      }
      if (!picture) return;
    }
    const bool key = (f.flags & kFrameKey) != 0;
    // B-frames: param from the phone or decode-order timestamps going backwards.
    const bool bfParam = bframesParam.load() > 0;
    bool reorder = false;
    if (ds.haveLastTs && f.capture_ts_us < ds.lastInputTs) reorder = true;
    ds.haveLastTs = true;
    ds.lastInputTs = f.capture_ts_us;
    if ((reorder || bfParam) && !ds.bframes) {
      ds.bframes = true;
      logf(LogLevel::Info, "stream has B-frames (reordering): SPS patch disabled");
    }
    if (decoderReconfig.exchange(false)) {
      ds.reopen = true;
      resetAutoBench(ds);
    }
    {
      std::lock_guard<std::mutex> lk(statMx);
      rx.bframes = ds.bframes;
    }
    if (ds.waitKey && !key) {
      setNeedKey(ds);
      return;
    }
    if (key && (ds.reopen || ds.decoderFailed || !ds.decoder.isOpen())) {
      ds.decoder.close();
      openDecoder(ds);
      ds.inflight.clear();
      }
    if (!ds.decoder.isOpen()) {
      ds.waitKey = true;
      setNeedKey(ds);
      return;
    }
    // Build the AU: keyframes get the codec config prepended if they lack an SPS.
    const uint8_t* au = f.data.data();
    size_t auLen = f.data.size();
    const bool patch = getOpt().patch_sps && !ds.bframes;
    if (key && annexB && (!ds.csd.empty() || patch)) {
      ds.auBuf.clear();
      if (!hasSps(ds.fmt.codec, au, auLen)) ds.auBuf.insert(ds.auBuf.end(), ds.csd.begin(), ds.csd.end());
      ds.auBuf.insert(ds.auBuf.end(), au, au + auLen);
      updateColorFromAu(ds, ds.auBuf.data(), ds.auBuf.size());
      bool patched = false;
      if (patch) patched = patchAccessUnitSps(ds.fmt.codec, ds.auBuf);
      {
        std::lock_guard<std::mutex> lk(statMx);
        rx.spsPatched = patched;
      }
      au = ds.auBuf.data();
      auLen = ds.auBuf.size();
    }
    InFlight inf;
    inf.sample_time = static_cast<int64_t>(f.capture_ts_us) * 10;
    inf.input_idx = ds.inputs++;
    ++ds.inputsSinceOpen;
    inf.seq = f.frame_seq;
    inf.epoch = ds.fmt.epoch;
    inf.capture_ts = f.capture_ts_us;
    inf.arrival_us = arrival;
    inf.submit_us = pcNowUs();
    ds.inflight.push_back(inf);
    while (ds.inflight.size() > 64) ds.inflight.pop_front();
    const int64_t dur = 10000000 / std::max<int64_t>(1, ds.fmt.fps ? ds.fmt.fps : 30);
    ds.presentNext = present;
    const bool ok = ds.decoder.decode(au, auLen, inf.sample_time, dur, key,
                                      [&](const DecodedImage& img) { onDecoded(ds, img); },
                                      present);
    if (!ok) {
      ++ds.errors;
      logf(LogLevel::Warn, "decode error on frame %u (%d in a row): %s", f.frame_seq, ds.errors,
           ds.decoder.lastError().c_str());
      ds.decoder.flush();
      ds.inflight.clear();
        ds.waitKey = true;
      if (ds.errors >= 3) ds.reopen = true;
      if (ds.errors >= 3 && ds.fmt.codec != Codec::H264 && !ds.codecFailReported) {
        ds.codecFailReported = true;
        codecFailure = static_cast<int>(ds.fmt.codec);
      }
    } else {
      ds.errors = 0;
      if (key) ds.waitKey = false;
    }
    // HEVC/AV1 accepted by the MFT but nothing comes out: treat as unusable.
    if (ds.fmt.codec != Codec::H264 && !ds.codecFailReported && ds.inputsSinceOpen >= 60 &&
        ds.outputsSinceOpen == 0) {
      ds.codecFailReported = true;
      codecFailure = static_cast<int>(ds.fmt.codec);
      logf(LogLevel::Warn, "%s decoder produced no output after %u frames", codecName(ds.fmt.codec),
           ds.inputsSinceOpen);
    }
    // MF_E_TRANSFORM_STREAM_CHANGE storm -> ask for a clean keyframe.
    const uint32_t sc = ds.decoder.streamChanges();
    if (sc > ds.lastStreamChanges) {
      const int64_t now = pcNowUs();
      if (now - ds.streamChangeWindowStart > 1000000) {
        ds.streamChangeWindowStart = now;
        ds.streamChangesInWindow = 0;
      }
      ds.streamChangesInWindow += sc - ds.lastStreamChanges;
      ds.lastStreamChanges = sc;
      if (ds.streamChangesInWindow >= 3) {
        forceKey = true;
        ds.streamChangesInWindow = 0;
      }
    }
    setNeedKey(ds);
  }

  void onDecoded(Dec& ds, const DecodedImage& img) {
    const int64_t now = pcNowUs();
    InFlight meta{};
    bool found = false;
    // Measured: the MS H.264 MFT in low-latency mode emits B-frame streams in
    // correct DISPLAY order on both paths, but the software path re-stamps the
    // output sample times (decode-order times). So with B-frames, the frame
    // that comes out is the in-flight one with the smallest capture time;
    // without B-frames, match by sample time. Never reorder on the PC.
    auto pick = ds.inflight.end();
    for (auto it = ds.inflight.begin(); it != ds.inflight.end(); ++it) {
      if (ds.bframes) {
        if (pick == ds.inflight.end() || it->capture_ts < pick->capture_ts) pick = it;
      } else if (it->sample_time == img.sample_time) {
        pick = it;
        break;
      }
    }
    if (pick != ds.inflight.end()) {
      meta = *pick;
      found = true;
      ds.inflight.erase(pick);
    }
    if (!found) {
      meta.sample_time = img.sample_time;
      meta.capture_ts = static_cast<uint64_t>(img.sample_time / 10);
      meta.input_idx = ds.inputs ? ds.inputs - 1 : 0;
      meta.arrival_us = now;
      meta.submit_us = now;
      meta.epoch = ds.fmt.epoch;
    }
    ++ds.outputsSinceOpen;
    const uint64_t delayFrames = ds.inputs - 1 - meta.input_idx;
    {
      std::lock_guard<std::mutex> lk(statMx);
      ++rx.frames_decoded;
      rx.decFps.add(now);
      rx.decodeMs.add(static_cast<double>(now - meta.submit_us) / 1000.0);
      rx.readbackMs.add(static_cast<double>(ds.decoder.lastReadbackUs()) / 1000.0);
      rx.arrToDec.add(static_cast<double>(now - meta.arrival_us) / 1000.0);
      rx.decDelay.add(static_cast<double>(delayFrames));
      rx.decDelayMax = std::max<uint32_t>(rx.decDelayMax, static_cast<uint32_t>(delayFrames));
      rx.decoderHw = ds.decoder.hardware();
      if (!ds.presentNext || !img.y) ++rx.notPresented;
    }
    if (!ds.presentNext || !img.y) return;  // decoded only (backlog): not presented
    const uint32_t jbMs = getOpt().jitter_buffer_ms;
    if (jbMs == 0) {
      if (!ds.jb.empty()) presentDue(ds, true);
      present(ds, img.y, img.y_stride, img.uv, img.uv_stride, img.width, img.height, meta);
      return;
    }
    // ---- jitter buffer: hold and pace by capture timestamps
    const int64_t transit = now - static_cast<int64_t>(meta.capture_ts);
    ds.transit.emplace_back(now, transit);
    while (!ds.transit.empty() && now - ds.transit.front().first > 2000000) ds.transit.pop_front();
    int64_t base = transit;
    for (auto& t : ds.transit) base = std::min(base, t.second);
    JbFrame jf;
    if (!ds.jbPool.empty()) {
      jf = std::move(ds.jbPool.back());
      ds.jbPool.pop_back();
    }
    jf.w = img.width;
    jf.h = img.height;
    jf.nv12.resize(static_cast<size_t>(img.width) * img.height * 3 / 2);
    for (uint32_t r = 0; r < img.height; ++r)
      memcpy(jf.nv12.data() + static_cast<size_t>(r) * img.width, img.y + static_cast<size_t>(r) * img.y_stride, img.width);
    uint8_t* uvDst = jf.nv12.data() + static_cast<size_t>(img.width) * img.height;
    for (uint32_t r = 0; r < img.height / 2; ++r)
      memcpy(uvDst + static_cast<size_t>(r) * img.width, img.uv + static_cast<size_t>(r) * img.uv_stride, img.width);
    jf.meta = meta;
    jf.present_at = static_cast<int64_t>(meta.capture_ts) + base + static_cast<int64_t>(jbMs) * 1000;
    // keep ordered by present time (decoder already outputs in display order)
    auto pos = ds.jb.end();
    while (pos != ds.jb.begin() && std::prev(pos)->present_at > jf.present_at) --pos;
    ds.jb.insert(pos, std::move(jf));
    while (ds.jb.size() > kMaxJitterFrames) {
      ds.jbPool.push_back(std::move(ds.jb.front()));
      ds.jb.pop_front();
    }
    {
      std::lock_guard<std::mutex> lk(statMx);
      rx.jbFrames = static_cast<uint32_t>(ds.jb.size());
    }
  }

  // Presents jitter-buffered frames whose time has come (all = flush: newest only).
  void presentDue(Dec& ds, bool all) {
    if (ds.jb.empty()) return;
    if (all) {
      JbFrame& f = ds.jb.back();
      const uint32_t w = f.w, h = f.h;
      present(ds, f.nv12.data(), w, f.nv12.data() + static_cast<size_t>(w) * h, w, w, h, f.meta);
      while (!ds.jb.empty()) {
        ds.jbPool.push_back(std::move(ds.jb.front()));
        ds.jb.pop_front();
      }
    } else {
      const int64_t now = pcNowUs();
      while (!ds.jb.empty() && ds.jb.front().present_at <= now) {
        JbFrame& f = ds.jb.front();
        const uint32_t w = f.w, h = f.h;
        present(ds, f.nv12.data(), w, f.nv12.data() + static_cast<size_t>(w) * h, w, w, h, f.meta);
        ds.jbPool.push_back(std::move(ds.jb.front()));
        ds.jb.pop_front();
      }
    }
    while (ds.jbPool.size() > 8) ds.jbPool.pop_back();
    std::lock_guard<std::mutex> lk(statMx);
    rx.jbFrames = static_cast<uint32_t>(ds.jb.size());
  }

  // Single-producer bus: open() fails while another producer owns it.
  void tryOpenShm(Dec& ds) {
    if (ds.shm.open()) {
      ds.shmActive = false;  // forces setActive with the current size
      ds.shmLastErr.clear();
      logf(LogLevel::Info, "publishing to shared memory %ls", shmName());
      std::lock_guard<std::mutex> lk(statMx);
      rx.shmPublishing = true;
      rx.shmError.clear();
      return;
    }
    ds.shmRetryAt = steadyMs() + 2000;
    std::string why = "cannot create the shared-memory bus";
    ShmReader r;
    if (r.open() && r.header()) {
      char b[160];
      snprintf(b, sizeof(b),
               "another OWC producer (pid %u) owns the shared-memory bus; retrying every 2 s",
               r.header()->producer_pid);
      why = b;
    }
    if (why != ds.shmLastErr) logf(LogLevel::Warn, "%s", why.c_str());
    ds.shmLastErr = why;
    std::lock_guard<std::mutex> lk(statMx);
    rx.shmPublishing = false;
    rx.shmError = why;
  }

  // H.264 VUI colour info from any SPS inside an Annex-B buffer.
  void updateColorFromAu(Dec& ds, const uint8_t* p, size_t n) {
    if (ds.fmt.codec != Codec::H264) return;  // HEVC VUI not parsed: MF type is used
    forEachNal(p, n, [&](const uint8_t* nal, size_t len) {
      if (!len || nalType(Codec::H264, nal[0]) != 7) return;
      SpsInfo si;
      if (!parseH264Sps(nal, len, si)) return;
      ds.spsFullRange = si.video_signal_type_present ? (si.video_full_range ? 1 : 0) : -1;
      ds.spsMatrix = 0;
      if (si.colour_description_present) {
        if (si.matrix_coefficients == 1) ds.spsMatrix = 2;  // BT.709
        else if (si.matrix_coefficients == 5 || si.matrix_coefficients == 6) ds.spsMatrix = 1;  // BT.601
      }
    });
  }

  void present(Dec& ds, const uint8_t* y, uint32_t ys, const uint8_t* uv, uint32_t uvs, uint32_t w,
               uint32_t h, const InFlight& meta) {
    if (w == 0 || h == 0) return;
    const ClientOptions o = getOpt();
    const OutputTransform& t = o.transform;
    int rot = t.rotation_deg + (t.apply_phone_hints ? ds.fmt.rotation_deg : 0);
    rot = normalizeRotation(rot);
    const bool mirror = t.mirror != (t.apply_phone_hints && ds.fmt.mirror);
    uint32_t ow = w, oh = h;
    if (rot != 0 || mirror) {
      transformedSize(w, h, rot, ow, oh);
      ds.xform.resize(static_cast<size_t>(ow) * oh * 3 / 2);
      uint8_t* dy = ds.xform.data();
      uint8_t* duv = dy + static_cast<size_t>(ow) * oh;
      transformNV12(y, ys, uv, uvs, w, h, rot, mirror, dy, ow, duv, ow);
      y = dy;
      uv = duv;
      ys = uvs = ow;
    }
    int64_t offset = 0;
    const bool synced = clockSynced(offset);
    const int64_t captureQpc =
        synced ? usToQpc(static_cast<int64_t>(meta.capture_ts) - offset) : 0;
    int64_t publishQpc = qpcNowRaw();
    // colour: SPS VUI first, then the decoder's output type
    uint32_t mfMatrix = 0;
    int mfFull = -1;
    ds.decoder.outputColor(mfMatrix, mfFull);
    const uint32_t colorMatrix = ds.spsMatrix ? ds.spsMatrix : mfMatrix;
    const bool fullRange = ds.spsFullRange >= 0 ? ds.spsFullRange == 1 : mfFull == 1;
    if (o.publish_to_shm) {
      if (!ds.shm.isOpen() && steadyMs() >= ds.shmRetryAt) tryOpenShm(ds);
      if (ds.shm.isOpen()) {
        const uint32_t fps = ds.fmt.fps ? ds.fmt.fps : 30;
        if (!ds.shmActive || ds.shmW != ow || ds.shmH != oh || ds.shmFps != fps) {
          ds.shm.setActive(true, ow, oh, fps, 1);
          ds.shmActive = true;
          ds.shmW = ow;
          ds.shmH = oh;
          ds.shmFps = fps;
        }
        ds.shm.publishNV12(y, ys, uv, uvs, ow, oh, captureQpc, colorMatrix, fullRange);
        ds.lastHeartbeat = steadyMs();
        publishQpc = qpcNowRaw();
      }
    }
    const uint64_t number = ++publishCounter;
    const int64_t pubUs = qpcToUs(publishQpc);
    {
      std::lock_guard<std::mutex> lk(statMx);
      ++rx.frames_published;
      rx.pubFps.add(pubUs);
      rx.outW = ow;
      rx.outH = oh;
      rx.colorMatrix = colorMatrix;
      rx.fullRange = fullRange;
      if (synced)
        rx.capToPub.add(static_cast<double>(pubUs - (static_cast<int64_t>(meta.capture_ts) - offset)) / 1000.0);
    }
    if (ev) {
      DecodedFrame df;
      df.y = y;
      df.uv = uv;
      df.y_stride = ys;
      df.uv_stride = uvs;
      df.width = ow;
      df.height = oh;
      df.epoch = meta.epoch;
      df.frame_seq = meta.seq;
      df.capture_ts_us = meta.capture_ts;
      df.capture_qpc = captureQpc;
      df.publish_qpc = publishQpc;
      df.rotation_applied = rot;
      df.mirrored = mirror;
      df.color_matrix = colorMatrix;
      df.full_range = fullRange;
      ev->onFrame(df);
    }
    if (o.keep_latest_frame) {
      std::lock_guard<std::mutex> lk(latestMx);
      latest.width = ow;
      latest.height = oh;
      latest.nv12.resize(static_cast<size_t>(ow) * oh * 3 / 2);
      for (uint32_t r = 0; r < oh; ++r)
        memcpy(latest.nv12.data() + static_cast<size_t>(r) * ow, y + static_cast<size_t>(r) * ys, ow);
      uint8_t* d = latest.nv12.data() + static_cast<size_t>(ow) * oh;
      for (uint32_t r = 0; r < oh / 2; ++r)
        memcpy(d + static_cast<size_t>(r) * ow, uv + static_cast<size_t>(r) * uvs, ow);
      latest.frame_number = number;
      latest.capture_ts_us = meta.capture_ts;
      latest.capture_qpc = captureQpc;
      latest.publish_qpc = publishQpc;
      haveLatest = true;
    }
  }
};

// ================================================================ Client
Client::Client(ClientEvents* events, const ClientOptions& options) : impl_(new Impl) {
  impl_->ev = events;
  impl_->opt = options;
  winsockInit();
  impl_->netThread = std::thread([this] { impl_->netMain(); });
  impl_->netId = impl_->netThread.get_id();
  impl_->decThread = std::thread([this] { impl_->decMain(); });
}

Client::~Client() {
  {
    std::lock_guard<std::mutex> lk(impl_->ctlMx);
    impl_->quit = true;
    impl_->wantConnected = false;
    impl_->ctlCv.notify_all();
  }
  if (impl_->netThread.joinable()) impl_->netThread.join();
  {
    std::lock_guard<std::mutex> lk(impl_->qMx);
    impl_->decQuit = true;
    impl_->qCv.notify_all();
  }
  if (impl_->decThread.joinable()) impl_->decThread.join();
}

bool Client::connect(const ConnectParams& p) {
  if (p.host.empty() || p.port == 0) return false;
  std::lock_guard<std::mutex> lk(impl_->ctlMx);
  impl_->params = p;
  impl_->wantConnected = true;
  impl_->blockedCodecs = 0;  // a new connect() may retry HEVC/AV1
  ++impl_->gen;
  impl_->ctlCv.notify_all();
  return true;
}

void Client::disconnect() {
  std::lock_guard<std::mutex> lk(impl_->ctlMx);
  impl_->wantConnected = false;
  ++impl_->gen;
  impl_->ctlCv.notify_all();
}

ClientState Client::state() const { return impl_->getState(); }

DisconnectReason Client::lastDisconnectReason() const {
  std::lock_guard<std::mutex> lk(impl_->stMx);
  return impl_->reason;
}

ConnectParams Client::connectParams() const {
  std::lock_guard<std::mutex> lk(impl_->ctlMx);
  return impl_->params;
}

ClientOptions Client::options() const { return impl_->getOpt(); }

void Client::setOptions(const ClientOptions& o) {
  ClientOptions n = o;
  n.udp_max_pending_frames = std::min<uint32_t>(32, std::max<uint32_t>(1, n.udp_max_pending_frames));
  std::lock_guard<std::mutex> lk(impl_->optMx);
  const ClientOptions& old = impl_->opt;
  if (old.decoder_mode != n.decoder_mode || old.low_latency_decode != n.low_latency_decode ||
      old.patch_sps != n.patch_sps) {
    impl_->decoderReconfig = true;
    impl_->forceKey = true;  // re-create needs a keyframe
  }
  if (old.udp_max_pending_frames != n.udp_max_pending_frames) impl_->assemblerReset = true;
  if (old.udp_recv_buffer_bytes != n.udp_recv_buffer_bytes) impl_->udpBufDirty = true;
  impl_->opt = n;
}

void Client::setTransform(const OutputTransform& t) {
  std::lock_guard<std::mutex> lk(impl_->optMx);
  impl_->opt.transform = t;
}

bool Client::setParam(ParamId id, const ParamValue& value) {
  SetParam sp;
  sp.id = id;
  sp.value = value;
  return impl_->sendMsg(MsgType::SetParam, encode(sp));
}

bool Client::startStream() {
  impl_->wantStreaming = true;
  return impl_->sendCmd(CommandId::StartStream);
}

bool Client::stopStream() {
  impl_->wantStreaming = false;
  const bool ok = impl_->sendCmd(CommandId::StopStream);
  if (ok) {
    WorkItem end;
    end.kind = WorkItem::StreamEnd;
    impl_->pushWork(std::move(end));
    impl_->postEvent([this] {
      if (impl_->getState() == ClientState::Streaming) impl_->setState(ClientState::Connected);
    });
    if (!impl_->ev) {
      std::lock_guard<std::mutex> lk(impl_->stMx);
      if (impl_->state == ClientState::Streaming) impl_->state = ClientState::Connected;
    }
  }
  return ok;
}

bool Client::requestKeyframe() { return impl_->requestKeyframeNow(true); }
bool Client::focusAt(float x, float y) { return impl_->sendCmd(CommandId::FocusAt, 0, x, y); }
bool Client::resetParams() { return impl_->sendCmd(CommandId::ResetParams); }
bool Client::setDeviceName(const std::string& name) {
  return impl_->sendCmd(CommandId::SetDeviceName, 0, 0, 0, name);
}
bool Client::sendCommand(const Command& cmd) {
  return impl_->sendMsg(MsgType::Command, encode(cmd));
}

bool Client::deviceInfo(DeviceInfo& out) const {
  std::lock_guard<std::mutex> lk(impl_->stMx);
  if (!impl_->haveDev) return false;
  out = impl_->dev;
  return true;
}

bool Client::capabilities(Capabilities& out) const {
  std::lock_guard<std::mutex> lk(impl_->stMx);
  if (!impl_->haveCaps) return false;
  out = impl_->caps;
  return true;
}

bool Client::paramValue(ParamId id, ParamValue& out) const {
  std::lock_guard<std::mutex> lk(impl_->stMx);
  for (const ParamDesc& d : impl_->caps.params)
    if (d.id == id) {
      out = d.current;
      return true;
    }
  return false;
}

bool Client::paramDesc(ParamId id, ParamDesc& out) const {
  std::lock_guard<std::mutex> lk(impl_->stMx);
  for (const ParamDesc& d : impl_->caps.params)
    if (d.id == id) {
      out = d;
      return true;
    }
  return false;
}

bool Client::videoFormat(VideoFormat& out) const {
  std::lock_guard<std::mutex> lk(impl_->stMx);
  if (!impl_->haveFmt) return false;
  out = impl_->fmt;
  return true;
}

ClientStats Client::stats() const {
  ClientStats s;
  {
    std::lock_guard<std::mutex> lk(impl_->stMx);
    s.state = impl_->state;
    s.last_reason = impl_->reason;
    s.transport = impl_->transport;
    s.peer = impl_->peer;
    s.reconnect_count = impl_->reconnects;
    if (impl_->sessionUp && impl_->sessionStartMs)
      s.session_uptime_ms = static_cast<uint64_t>(steadyMs() - impl_->sessionStartMs);
    s.has_format = impl_->haveFmt;
    if (impl_->haveFmt) s.format = impl_->fmt;
    s.has_phone_stats = impl_->havePhone;
    s.phone = impl_->phone;
  }
  {
    std::lock_guard<std::mutex> lk(impl_->clockMx);
    s.clock_synced = impl_->clock.synced();
    s.rtt_ms = static_cast<double>(impl_->clock.lastRttUs()) / 1000.0;
    s.rtt_min_ms = static_cast<double>(impl_->clock.minRttUs()) / 1000.0;
    s.clock_offset_us = impl_->clock.offsetUs();
  }
  s.keyframe_requests = impl_->kfRequests;
  s.udp_punched = impl_->punched;
  {
    std::lock_guard<std::mutex> lk(impl_->statMx);
    auto& r = impl_->rx;
    const int64_t now = pcNowUs();
    s.rx_fps = r.rxFps.rate(now);
    s.rx_bitrate_bps = r.rxBits.rate(now);
    s.frames_received = r.frames_received;
    s.frames_lost = r.asm_frames_lost + r.frames_lost_extra;
    s.fragments_received = r.fragments_received;
    s.fragments_lost = r.fragments_lost;
    s.frames_recovered_fec = r.recovered;
    s.datagrams_dropped_queue = r.datagrams_dropped;
    s.jitter_ms = r.jitter.jitterUs() / 1000.0;
    s.delay_gradient_ms = r.trend.gradientMs();
    s.output_width = r.outW;
    s.output_height = r.outH;
    s.decoder_hardware = r.decoderHw;
    s.decoder_low_latency = r.decoderLL;
    s.stream_has_bframes = r.bframes;
    s.sps_patched = r.spsPatched;
    s.decoder_name = r.decName;
    s.decoder_error = r.decErr;
    s.decoder_selection = r.decSelection;
    s.codec_selection = r.codecSel;
    s.requested_codec = r.requestedCodec;
    s.shm_publishing = r.shmPublishing;
    s.shm_error = r.shmError;
    s.color_matrix = r.colorMatrix;
    s.full_range = r.fullRange;
    s.decoded_fps = r.decFps.rate(now);
    s.published_fps = r.pubFps.rate(now);
    s.frames_decoded = r.frames_decoded;
    s.frames_published = r.frames_published;
    s.decode_ms_avg = r.decodeMs.avg();
    s.readback_ms_avg = r.readbackMs.avg();
    s.decoder_delay_frames_avg = r.decDelay.avg();
    s.decoder_delay_frames_max = r.decDelayMax;
    s.arrival_to_decoded_ms_avg = r.arrToDec.avg();
    s.arrival_to_decoded_ms_p95 = r.arrToDec.percentile(95);
    s.capture_to_publish_ms_avg = r.capToPub.avg();
    s.capture_to_publish_ms_p95 = r.capToPub.percentile(95);
    s.capture_to_arrival_ms_avg = r.capToArr.avg();
    s.jitter_buffer_frames = r.jbFrames;
    s.frames_not_presented = r.notPresented;
    s.backlog_drops = r.backlogDrops;
  }
  return s;
}

std::vector<LogEntry> Client::logs(uint64_t sinceId) const {
  std::lock_guard<std::mutex> lk(impl_->logMx);
  std::vector<LogEntry> v;
  for (const LogEntry& e : impl_->logs)
    if (e.id > sinceId) v.push_back(e);
  return v;
}

void Client::clearLogs() {
  std::lock_guard<std::mutex> lk(impl_->logMx);
  impl_->logs.clear();
}

bool Client::copyLatestFrame(FrameCopy& out, uint64_t lastSeen) const {
  std::lock_guard<std::mutex> lk(impl_->latestMx);
  if (!impl_->haveLatest || impl_->latest.frame_number == lastSeen) return false;
  out = impl_->latest;
  return true;
}

int64_t Client::phoneTimeToQpc(uint64_t phone_us) const {
  int64_t off = 0;
  if (!impl_->clockSynced(off)) return 0;
  return usToQpc(static_cast<int64_t>(phone_us) - off);
}

}  // namespace owc
