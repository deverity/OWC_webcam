#include "session.h"

#include <android/native_window.h>
#include <stdlib.h>

#include <algorithm>
#include <cmath>

#include "util/clock.h"
#include "util/log.h"

namespace owc {
namespace {
constexpr int64_t kHelloTimeoutMs = 5000;
constexpr int64_t kRxTimeoutMs = 10000;     // PC pings at 2 Hz
constexpr int64_t kPunchTimeoutMs = 3000;   // UDP requested but no punch -> TCP
constexpr int64_t kCloseGraceMs = 2000;
constexpr int64_t kReconfigDebounceMs = 150;  // coalesce width+height(+fps) changes
constexpr int64_t kStatsPeriodMs = 1000;
constexpr int kMaxRestartAttempts = 6;
constexpr int64_t kKeyStormWindowMs = 10000;
constexpr size_t kKeyStormCount = 5;

LogLevel toWire(log::Level l) {
  switch (l) {
    case log::Level::Debug: return LogLevel::Debug;
    case log::Level::Info: return LogLevel::Info;
    case log::Level::Warn: return LogLevel::Warn;
    case log::Level::Error: return LogLevel::Error;
  }
  return LogLevel::Info;
}
}  // namespace

Worker::Worker(const StartOptions& opts, EventSink* sink, std::shared_ptr<SharedInfo> info,
               ANativeWindow* preview)
    : opts_(opts), sink_(sink), info_(std::move(info)), preview_(preview) {}

Worker::~Worker() {
  stop();
  if (preview_) {
    ANativeWindow_release(preview_);
    preview_ = nullptr;
  }
}

bool Worker::start(std::string& err) {
  if (!net_.start(opts_.controlPort, opts_.udpPort, opts_.discoveryPort, this, err)) return false;
  thread_ = std::thread(&Worker::run, this);
  return true;
}

void Worker::stop() {
  if (thread_.joinable()) {
    Event e;
    e.kind = Event::Kind::Shutdown;
    q_.push(std::move(e));
    thread_.join();
  }
  net_.stop();
  // Leftover events (e.g. a client accepted during shutdown) own connections
  // whose destructors stop their sender threads and close the sockets.
  q_.clear();
}

void Worker::setPreview(ANativeWindow* w) {
  Event e;
  e.kind = Event::Kind::Preview;
  e.window = w;
  e.done = std::make_shared<std::promise<void>>();
  auto fut = e.done->get_future();
  q_.push(std::move(e));
  if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
    LOGW_LOCAL("setPreviewSurface: worker busy for > 2 s");
}

// ------------------------------------------------------------------ callbacks
void Worker::onClientConnected(std::shared_ptr<net::ClientConn> c) {
  Event e;
  e.kind = Event::Kind::Connected;
  e.gen = c->gen;
  e.conn = std::move(c);
  q_.push(std::move(e));
}

void Worker::onClientMessage(uint64_t gen, MsgType type, std::vector<uint8_t>&& payload) {
  Event e;
  e.kind = Event::Kind::Message;
  e.gen = gen;
  e.type = type;
  e.payload = std::move(payload);
  q_.push(std::move(e));
}

void Worker::onClientDisconnected(uint64_t gen) {
  Event e;
  e.kind = Event::Kind::Disconnected;
  e.gen = gen;
  q_.push(std::move(e));
}

void Worker::onPunch(uint64_t gen, const sockaddr_in& from) {
  Event e;
  e.kind = Event::Kind::Punch;
  e.gen = gen;
  e.text = net::addrToString(from);
  q_.push(std::move(e));
}

DiscoveryReply Worker::discoveryInfo() {
  DiscoveryReply r;
  std::lock_guard<std::mutex> lk(info_->infoMu);
  r.device_name = info_->deviceName;
  r.model = info_->model;
  return r;
}

HelloAck Worker::busyAck() { return makeAck(HelloStatus::Busy); }

HelloAck Worker::makeAck(HelloStatus st) {
  HelloAck a;
  a.status = st;
  a.udp_port = opts_.udpPort;
  {
    std::lock_guard<std::mutex> lk(info_->infoMu);
    a.device_name = info_->deviceName;
    a.model = info_->model;
  }
  a.android_api = static_cast<uint16_t>(opts_.apiLevel);
  a.codec_mask = caps_.codecMask();  // written once in init(), before any client is handled
  return a;
}

void Worker::onVideoFormat(const VideoFormat& fmt) {
  if (frameSender_) frameSender_->pushControl(MsgType::VideoFormat, encode(fmt));
  LOGI("VideoFormat epoch %u %ux%u@%u rot %d mirror %d csd %zu bytes", fmt.epoch, fmt.width,
       fmt.height, fmt.fps, fmt.rotation_deg, fmt.mirror, fmt.csd.size());
}

bool Worker::onEncodedFrame(FramePtr frame) {
  return frameSender_ ? frameSender_->pushVideo(std::move(frame)) : false;
}

void Worker::onEncoderError(const std::string& msg) {
  Event e;
  e.kind = Event::Kind::EncoderError;
  e.text = msg;
  q_.push(std::move(e));
}

void Worker::emit(int type, int i, const std::string& s) {
  if (sink_) sink_->onEvent(type, i, s);
}

// ------------------------------------------------------------------ loop
void Worker::init() {
  camMgr_ = ACameraManager_create();
  cams_ = enumerateCameras(camMgr_);
  attachHsConfigs();
  caps_ = opts_.javaCaps;
  probeEncoders(caps_, opts_.apiLevel);
  params_.init(&cams_, &caps_, opts_.hsBackend != nullptr);
  pipeline_ = std::make_unique<Pipeline>(camMgr_, &caps_, opts_.hsBackend, [this](uint64_t gen, const std::string& what) {
    Event e;
    e.kind = Event::Kind::CameraLost;
    e.gen = gen;
    e.text = what;
    q_.push(std::move(e));
  });
  pipeline_->setSyntheticFallback(opts_.testSyntheticSource);
  if (cams_.empty()) {
    LOGE("no usable camera found (permission missing or no camera)");
    emit(kEvError, 0, "no usable camera");
  }
  if (!caps_.avc.available && !caps_.hevc.available) {
    LOGE("no usable video encoder");
    emit(kEvError, 0, "no usable video encoder");
  }
  LOGI("core ready: %zu cameras, codec mask 0x%x (avc %s, hevc %s)", cams_.size(), caps_.codecMask(),
       caps_.avc.name.c_str(), caps_.hevc.name.c_str());
  updateStatus();
}

void Worker::run() {
  setThreadName("owc-worker");
  init();
  for (;;) {
    auto deadline = Clock::now() + std::chrono::milliseconds(100);
    auto ev = q_.popUntil(deadline);
    if (ev) {
      if (ev->kind == Event::Kind::Shutdown) break;
      handle(*ev);
    }
    tick();
  }
  // Shutdown: polite Bye, stop the stream, flush, close.
  if (conn_) {
    stopStream(true);
    sendControl(MsgType::Bye, {});
    conn_->sender->closeAfterFlush();
    for (int i = 0; i < 30 && !conn_->sender->dead(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    onDisconnected();
  }
  pipeline_.reset();
  if (opts_.hsBackend) opts_.hsBackend->detachThread();
  if (camMgr_) {
    ACameraManager_delete(camMgr_);
    camMgr_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(info_->statusMu);
    info_->status = SharedInfo::Status{};
  }
}

void Worker::handle(Event& ev) {
  switch (ev.kind) {
    case Event::Kind::Connected:
      onConnected(std::move(ev.conn));
      break;
    case Event::Kind::Message:
      if (conn_ && ev.gen == conn_->gen) onMessage(ev.type, ev.payload);
      break;
    case Event::Kind::Disconnected:
      if (conn_ && ev.gen == conn_->gen) onDisconnected();
      break;
    case Event::Kind::Punch:
      if (conn_ && ev.gen == conn_->gen && helloDone_ && requested_ == VideoTransport::Udp &&
          conn_->sender->videoMode() != net::VideoMode::Udp) {
        conn_->sender->setVideoMode(net::VideoMode::Udp);
        LOGI_PC("UDP video path established to %s", ev.text.c_str());
        applyBitrate(true);
        if (streaming_) pipeline_->requestKeyframe(true);
        updateStatus();
      }
      break;
    case Event::Kind::CameraLost:
      if (streaming_ && ev.gen == pipeline_->cameraGen()) {
        stopStream(false);
        scheduleRestart("camera lost: " + ev.text);
      }
      break;
    case Event::Kind::EncoderError:
      if (streaming_) {
        stopStream(false);
        scheduleRestart("encoder error: " + ev.text);
      }
      break;
    case Event::Kind::Preview: {
      ANativeWindow* old = preview_;
      preview_ = ev.window;
      if (streaming_) pipeline_->setPreview(preview_, params_);
      if (old) ANativeWindow_release(old);
      if (ev.done) ev.done->set_value();
      break;
    }
    case Event::Kind::Shutdown:
      break;
  }
}

void Worker::tick() {
  const int64_t now = monoMs();
  if (conn_) {
    if (!helloDone_ && !closing_ && now > helloDeadlineMs_) closeClient("no Hello within 5 s");
    if (now - conn_->lastRxMs.load() > kRxTimeoutMs && !closing_) {
      LOGW_LOCAL("client silent for %lld ms; dropping", static_cast<long long>(now - conn_->lastRxMs.load()));
      conn_->sock->shutdownBoth();
      closing_ = true;
      closeDeadlineMs_ = now + kCloseGraceMs;
    }
    if (closing_ && now > closeDeadlineMs_) conn_->sock->shutdownBoth();
    if (helloDone_ && requested_ == VideoTransport::Udp && punchDeadlineMs_ && now > punchDeadlineMs_) {
      punchDeadlineMs_ = 0;
      if (conn_->sender->videoMode() == net::VideoMode::Pending) {
        conn_->sender->setVideoMode(net::VideoMode::Tcp);
        LOGW("no UDP punch within 3 s: sending video over TCP");
        if (streaming_) pipeline_->requestKeyframe(true);
        updateStatus();
      }
    }
  }
  if (streaming_) {
    std::string why;
    if (pipeline_->pollHighSpeedFailure(why)) {
      stopStream(false);
      fallbackToNormal(why);
      startStream(true);
    }
  }
  if (reconfigAtMs_ && now >= reconfigAtMs_) {
    reconfigAtMs_ = 0;
    if (streaming_) {
      stopStream(false);
      startStream(false);
    }
  }
  if (wantStream_ && !streaming_ && restartAtMs_ && now >= restartAtMs_) {
    restartAtMs_ = 0;
    startStream(true);
  }
  if (streaming_ && restartAttempts_ && now - streamStartMs_ > 5000) restartAttempts_ = 0;
  if (helloDone_ && now - lastStatsMs_ >= kStatsPeriodMs) sendStats(now);
}

// ------------------------------------------------------------------ session
void Worker::onConnected(std::shared_ptr<net::ClientConn> c) {
  if (conn_) {  // cannot happen (net thread allows one client) but be safe
    c->sock->shutdownBoth();
    return;
  }
  conn_ = std::move(c);
  refreshCameras();
  helloDone_ = false;
  closing_ = false;
  helloDeadlineMs_ = monoMs() + kHelloTimeoutMs;
  punchDeadlineMs_ = 0;
  wantStream_ = false;
  restartAtMs_ = 0;
  restartAttempts_ = 0;
  keyReqs_.clear();
  lastOverflows_ = 0;  // per-sender counters start at zero
  lastSentFrames_ = 0;
  appliedFec_ = -1;
  applyQueuePolicy();
  std::weak_ptr<net::Sender> ws = conn_->sender;
  log::setPcSink(std::make_shared<log::Sink>([ws](log::Level l, const std::string& text) {
    if (auto s = ws.lock()) s->pushLog(toWire(l), text);
  }));
  emit(kEvClientConnected, 0, conn_->peerStr);
  updateStatus();
}

void Worker::onDisconnected() {
  log::setPcSink(nullptr);
  stopStream(true);
  wantStream_ = false;
  restartAtMs_ = 0;
  reconfigAtMs_ = 0;
  conn_->sessionId = 0;
  conn_->sender->stop();
  conn_->sock->shutdownBoth();
  LOGI("session with %s ended", conn_->peerStr.c_str());
  conn_.reset();
  helloDone_ = false;
  closing_ = false;
  emit(kEvClientDisconnected, 0, "");
  updateStatus();
}

void Worker::closeClient(const char* why) {
  if (!conn_ || closing_) return;
  LOGI("closing client: %s", why);
  closing_ = true;
  closeDeadlineMs_ = monoMs() + kCloseGraceMs;
  conn_->sender->closeAfterFlush();
}

void Worker::sendControl(MsgType t, const std::vector<uint8_t>& payload) {
  if (conn_) conn_->sender->pushControl(t, payload);
}

void Worker::sendParamState(ParamId id, ParamStatus st, const ParamValue& v) {
  ParamState ps;
  ps.id = id;
  ps.status = st;
  ps.value = v;
  sendControl(MsgType::ParamState, encode(ps));
}

void Worker::sendCapabilities() {
  if (!params_.ready()) return;
  sendControl(MsgType::Capabilities, encode(params_.capabilities()));
}

void Worker::onMessage(MsgType type, const std::vector<uint8_t>& payload) {
  if (closing_) return;
  if (!helloDone_) {
    if (type == MsgType::Hello) onHello(payload);
    else LOGW_LOCAL("message %s before Hello ignored", toString(type));
    return;
  }
  switch (type) {
    case MsgType::SetParam: onSetParam(payload); break;
    case MsgType::Command: onCommand(payload); break;
    case MsgType::ReceiverReport: onReceiverReport(payload); break;
    case MsgType::Bye: closeClient("Bye from client"); break;
    case MsgType::Hello: LOGW_LOCAL("duplicate Hello ignored"); break;
    default: LOGD("ignoring message %s", toString(type)); break;
  }
}

void Worker::onHello(const std::vector<uint8_t>& payload) {
  Hello h;
  if (!decode(payload.data(), payload.size(), h)) {
    closeClient("malformed Hello");
    return;
  }
  if (h.magic != kMagic) {
    sendControl(MsgType::HelloAck, encode(makeAck(HelloStatus::BadMagic)));
    closeClient("bad magic");
    return;
  }
  if (h.version != kProtocolVersion) {
    sendControl(MsgType::HelloAck, encode(makeAck(HelloStatus::VersionMismatch)));
    closeClient("protocol version mismatch");
    return;
  }
  uint32_t sid = 0;
  while (sid == 0) sid = arc4random();
  HelloAck ack = makeAck(HelloStatus::Ok);
  ack.session_id = sid;
  conn_->sessionId = sid;
  requested_ = h.transport;
  // USB (adb forward => 127.0.0.1) cannot carry UDP.
  if (requested_ == VideoTransport::Udp && conn_->peerStr.rfind("127.", 0) == 0) {
    LOGW("UDP requested over a loopback (adb) connection: using TCP");
    requested_ = VideoTransport::Tcp;
  }
  if (requested_ == VideoTransport::Udp) {
    conn_->sender->setVideoMode(net::VideoMode::Pending);
    punchDeadlineMs_ = monoMs() + kPunchTimeoutMs;
  } else {
    conn_->sender->setVideoMode(net::VideoMode::Tcp);
  }
  sendControl(MsgType::HelloAck, encode(ack));
  sendCapabilities();
  helloDone_ = true;
  lastStatsMs_ = monoMs();
  LOGI_PC("session %08x with '%s' (%s), transport %s", sid, h.client_name.c_str(),
          conn_->peerStr.c_str(), requested_ == VideoTransport::Udp ? "udp" : "tcp");
  updateStatus();
}

void Worker::onSetParam(const std::vector<uint8_t>& payload) {
  SetParam sp;
  if (!decode(payload.data(), payload.size(), sp)) {
    LOGW("malformed SetParam");
    return;
  }
  if (!params_.ready()) {
    sendParamState(sp.id, ParamStatus::Failed, sp.value);
    return;
  }
  ParamStore::Result r = params_.set(sp.id, sp.value);
  sendParamState(sp.id, r.status, r.value);
  for (ParamId other : r.alsoChanged) sendParamState(other, ParamStatus::Ok, params_.current(other));
  if (r.status != ParamStatus::Ok && r.status != ParamStatus::Unsupported) return;
  const uint32_t fx = r.effects;
  if (fx & ParamStore::kReconfigure && streaming_) reconfigAtMs_ = monoMs() + kReconfigDebounceMs;
  if (fx & ParamStore::kBitrate) {
    abr_.setCeiling(params_.stream.bitrate);
    applyBitrate(true);
  }
  if (fx & ParamStore::kAbr) {
    if (params_.stream.abr) abr_.reset(params_.stream.bitrate, monoMs());
    applyBitrate(true);
  }
  if (fx & ParamStore::kFec) applyBitrate(true);
  if (fx & ParamStore::kControls && streaming_) pipeline_->applyControls(params_);
  if (fx & ParamStore::kQueue && conn_) applyQueuePolicy();
  if (fx & ParamStore::kScreen) emit(kEvScreenMode, params_.screenMode, "");
  if (fx & ParamStore::kCaps) sendCapabilities();
  if (sp.id == ParamId::BFrames && params_.stream.bframes > 0 && params_.stream.codec == Codec::H264 &&
      params_.stream.profile == 0)
    LOGW("B-frames need Main/High profile; ignored while EncoderProfile=Baseline");
  updateStatus();
}

void Worker::onCommand(const std::vector<uint8_t>& payload) {
  Command c;
  if (!decode(payload.data(), payload.size(), c)) {
    LOGW("malformed Command");
    return;
  }
  const int64_t now = monoMs();
  switch (c.cmd) {
    case CommandId::StartStream:
      wantStream_ = true;
      restartAttempts_ = 0;
      if (!streaming_) startStream(true);
      break;
    case CommandId::StopStream:
      wantStream_ = false;
      restartAtMs_ = 0;
      stopStream(true);
      break;
    case CommandId::RequestKeyframe:
      if (!streaming_) break;
      pipeline_->requestKeyframe(false);  // ignored if an IDR left < 150 ms ago
      keyReqs_.push_back(now);
      while (!keyReqs_.empty() && now - keyReqs_.front() > kKeyStormWindowMs) keyReqs_.pop_front();
      if (keyReqs_.size() >= kKeyStormCount && params_.stream.abr) {
        keyReqs_.clear();
        if (abr_.onKeyframeStorm(now)) {
          LOGW("%zu keyframe requests in 10 s: lowering bitrate to %d kbps", kKeyStormCount,
               abr_.target() / 1000);
          applyBitrate(false);
        }
      }
      break;
    case CommandId::FocusAt:
      if (streaming_) pipeline_->focusAt(c.x, c.y, params_);
      break;
    case CommandId::ResetParams:
      params_.resetCameraControls();
      if (streaming_) pipeline_->applyControls(params_);
      sendCapabilities();
      break;
    case CommandId::SetDeviceName: {
      std::string name = c.text.substr(0, 64);
      if (!name.empty()) {
        {
          std::lock_guard<std::mutex> lk(info_->infoMu);
          info_->deviceName = name;
        }
        emit(kEvDeviceName, 0, name);
      }
      LOGI("device name set by PC: %s", name.c_str());
      break;
    }
    default:
      LOGD("unknown command %d", static_cast<int>(c.cmd));
  }
}

void Worker::onReceiverReport(const std::vector<uint8_t>& payload) {
  ReceiverReport rr;
  if (!decode(payload.data(), payload.size(), rr)) return;
  if (!params_.stream.abr || !streaming_ || !conn_) return;
  const auto st = conn_->sender->stats();
  if (abr_.onReceiverReport(rr, st.queued, params_.stream.fps, monoMs())) applyBitrate(false);
  else if (params_.stream.fecPercent == -1 && effectiveFecPercent() != appliedFec_) applyBitrate(true);
}

void Worker::sendStats(int64_t now) {
  const double dt = std::max<int64_t>(1, now - lastStatsMs_) / 1000.0;
  lastStatsMs_ = now;
  Stats s;
  net::Sender::Stats ss;
  if (conn_) ss = conn_->sender->stats();
  if (streaming_) {
    const uint64_t ef = pipeline_->encodedFrames(), eb = pipeline_->encodedBytes();
    s.camera_fps = static_cast<float>(pipeline_->takeCameraFrames() / dt);
    s.encode_fps = static_cast<float>((ef - lastEncFrames_) / dt);
    s.bitrate_bps = static_cast<uint32_t>((eb - lastEncBytes_) * 8 / dt);
    rateControlCheck(s.bitrate_bps, now);
    s.encode_latency_ms = pipeline_->encodeLatencyMs();
    lastEncFrames_ = ef;
    lastEncBytes_ = eb;
    if (!bframeNoticeDone_ && params_.stream.bframes > 0 && ef > 60) {
      bframeNoticeDone_ = true;
      if (pipeline_->bframesSeen()) LOGI_PC("encoder is producing B-frames");
      else LOGW("BFrames=%d requested but the encoder emits no B-frames", params_.stream.bframes);
    }
  }
  s.send_fps = static_cast<float>((ss.framesSent - lastSentFrames_) / dt);
  lastSentFrames_ = ss.framesSent;
  s.dropped_frames = ss.dropped;
  s.send_queue = ss.queued;
  s.battery_pct = static_cast<int8_t>(info_->batteryPct.load());
  s.battery_temp_c = info_->batteryTempC.load();
  s.charging = info_->charging.load();
  s.thermal_status = static_cast<uint8_t>(info_->thermal.load());
  s.target_bitrate_bps = static_cast<uint32_t>(streaming_ ? appliedVideoBitrate_ : currentBitrate_);
  s.isp_latency_ms = streaming_ ? pipeline_->ispLatencyMs() : 0.0f;
  s.fec_percent = static_cast<int8_t>(std::clamp(appliedFec_, 0, 100));
  encodeFps_ = s.encode_fps;
  sendControl(MsgType::Stats, encode(s));
  if (streaming_ && now - lastBreakdownMs_ >= 5000) {
    lastBreakdownMs_ = now;
    logLatencyBreakdown(ss);
  }

  // Local congestion signal for ABR (works without ReceiverReports, e.g. USB):
  // overflow drops, or the TCP socket holding > 3 average frames.
  if (streaming_ && params_.stream.abr) {
    const bool overflow = ss.overflows != lastOverflows_;
    const bool backlog = ss.avgFrameBytes > 0 && ss.tcpOutq > 3 * ss.avgFrameBytes;
    if ((overflow || backlog) && abr_.onLocalCongestion(now)) applyBitrate(false);
  }
  lastOverflows_ = ss.overflows;
  updateStatus();
}

// ------------------------------------------------------------------ stream
int Worker::effectiveFecPercent() const {
  if (!conn_ || conn_->sender->videoMode() != net::VideoMode::Udp) return 0;
  const int p = params_.stream.fecPercent;
  if (p < 0) return std::clamp(abr_.autoFecPercent(), 10, 50);
  return p;
}

void Worker::applyBitrate(bool force) {
  const int fec = effectiveFecPercent();
  int wire = params_.stream.abr ? abr_.target() : params_.stream.bitrate;
  // With ABR the target is the wire rate: leave room for parity. Without ABR
  // the Bitrate param is the encoder bitrate as the user asked.
  int video = params_.stream.abr && fec > 0 ? static_cast<int>(wire * 100LL / (100 + fec)) : wire;
  video = std::max(video, Abr::kMinBps / 2);
  currentBitrate_ = video;
  if (conn_) {
    conn_->sender->setPacingBitrate(params_.stream.abr ? wire : video * (100 + fec) / 100);
    if (fec != appliedFec_) conn_->sender->setFecPercent(fec);
  }
  appliedFec_ = fec;
  if (!streaming_) return;
  if (force || std::abs(video - appliedVideoBitrate_) * 100 > appliedVideoBitrate_) {
    pipeline_->setBitrate(static_cast<int>(video * rcFactor_));
    appliedVideoBitrate_ = video;
    LOGD("bitrate -> %d kbps (fec %d%%)", video / 1000, fec);
  }
}

void Worker::startStream(bool notify) {
  if (!conn_ || !helloDone_ || streaming_) return;
  if (!params_.ready()) {
    LOGE("cannot stream: no camera");
    return;
  }
  if (params_.stream.abr && notify) abr_.reset(params_.stream.bitrate, monoMs());
  applyBitrate(true);  // computes currentBitrate_ (stream not running yet)
  conn_->sender->flushVideo(true);
  frameSender_ = conn_->sender.get();
  bool sizeChanged = false;
  std::string err;
  if (!pipeline_->start(params_, currentBitrate_, preview_, this, sizeChanged, err)) {
    frameSender_ = nullptr;
    if (params_.stream.highSpeed) {
      fallbackToNormal(err);
      startStream(notify);
      return;
    }
    LOGE("stream start failed: %s", err.c_str());
    if (restartAttempts_ == 0) emit(kEvError, 0, err);
    scheduleRestart(err);
    return;
  }
  streaming_ = true;
  streamStartMs_ = monoMs();
  appliedVideoBitrate_ = currentBitrate_;
  lastEncFrames_ = lastEncBytes_ = 0;
  rcFactor_ = 1.0;
  rcMeasuredBps_ = 0;
  rcNoticeDone_ = false;
  applyQueuePolicy();
  bframeNoticeDone_ = false;
  pipeline_->takeCameraFrames();
  if (sizeChanged) {
    sendParamState(ParamId::VideoWidth, ParamStatus::Ok, params_.current(ParamId::VideoWidth));
    sendParamState(ParamId::VideoHeight, ParamStatus::Ok, params_.current(ParamId::VideoHeight));
    sendCapabilities();
  }
  if (!appStreaming_) {
    appStreaming_ = true;
    emit(kEvStreamingStarted, 0, "");
  }
  updateStatus();
}

void Worker::stopStream(bool notify) {
  if (streaming_) {
    if (conn_) conn_->sender->flushVideo(false);  // releases an encoder blocked in pushVideo
    pipeline_->stop();
    frameSender_ = nullptr;
    streaming_ = false;
  }
  // notify=false: internal restart (reconfigure / camera loss), the app keeps
  // seeing "streaming" until the stream is really stopped or given up.
  if (notify && appStreaming_) {
    appStreaming_ = false;
    emit(kEvStreamingStopped, 0, "");
  }
  updateStatus();
}

void Worker::scheduleRestart(const std::string& why) {
  if (!wantStream_ || !conn_) return;
  if (restartAttempts_ >= kMaxRestartAttempts) {
    LOGE("giving up restarting the stream after %d attempts (%s)", restartAttempts_, why.c_str());
    wantStream_ = false;
    emit(kEvError, 0, "stream stopped: " + why);
    if (appStreaming_) {
      appStreaming_ = false;
      emit(kEvStreamingStopped, 0, "");
    }
    return;
  }
  const int64_t backoff = std::min<int64_t>(8000, 500LL << restartAttempts_);
  ++restartAttempts_;
  restartAtMs_ = monoMs() + backoff;
  LOGW("%s; retrying in %lld ms (attempt %d/%d)", why.c_str(), static_cast<long long>(backoff),
       restartAttempts_, kMaxRestartAttempts);
}

void Worker::applyQueuePolicy() {
  if (!conn_) return;
  int q = params_.stream.maxQueue;
  // High-speed sessions deliver frames in bursts of sensorFps/30 (one burst
  // per 33 ms): a 2-frame queue would overflow on every burst.
  if (streaming_ && pipeline_->highSpeedActive()) q = std::max(q, (params_.cameraFps() + 29) / 30);
  conn_->sender->setQueuePolicy(q, params_.stream.dropPolicy);
}

void Worker::rateControlCheck(uint32_t measuredBps, int64_t now) {
  if (now - streamStartMs_ < 3000 || currentBitrate_ <= 0 || measuredBps == 0) return;
  rcMeasuredBps_ = rcMeasuredBps_ == 0 ? measuredBps : 0.6 * rcMeasuredBps_ + 0.4 * measuredBps;
  const double ratio = rcMeasuredBps_ / currentBitrate_;
  double f = rcFactor_;
  if (ratio > 1.25) f = rcFactor_ / std::pow(ratio, 0.7);
  else if (ratio < 0.8 && rcFactor_ < 1.0) f = rcFactor_ / std::pow(ratio, 0.5);
  f = std::clamp(f, 0.1, 1.0);
  if (std::abs(f - rcFactor_) < 0.02) return;
  rcFactor_ = f;
  pipeline_->setBitrate(static_cast<int>(currentBitrate_ * rcFactor_));
  if (!rcNoticeDone_ && f < 0.9) {
    rcNoticeDone_ = true;
    LOGI_PC("encoder overshoots its target (%.1f vs %.1f Mbps): correcting encoder bitrate x%.2f",
            rcMeasuredBps_ / 1e6, currentBitrate_ / 1e6, rcFactor_);
  } else {
    LOGD("rate-control correction x%.2f (measured %.2f Mbps, target %.2f)", rcFactor_,
         rcMeasuredBps_ / 1e6, currentBitrate_ / 1e6);
  }
}

void Worker::attachHsConfigs() {
  for (auto& cd : cams_) {
    cd.hsConfigs.clear();
    for (const auto& c : opts_.hsConfigs)
      if (c.first == cd.info.id) cd.hsConfigs.push_back(c.second);
  }
}

void Worker::fallbackToNormal(const std::string& why) {
  LOGW("high-speed mode failed (%s): falling back to the normal session", why.c_str());
  ParamStore::Result r = params_.set(ParamId::HighSpeedMode, ParamValue::EnumV(0));
  sendParamState(ParamId::HighSpeedMode, ParamStatus::Ok, params_.current(ParamId::HighSpeedMode));
  for (ParamId other : r.alsoChanged) sendParamState(other, ParamStatus::Ok, params_.current(other));
  sendCapabilities();
}

// Every 5 s at Info: where the capture->wire latency goes. All values are
// EWMAs measured from the sensor timestamp (start of exposure):
//   isp    = capture result delivered (exposure + readout + ISP)
//   encode = encoder output (adds buffer hand-off + encode)
//   sent   = frame fully written to the socket (adds queueing + send)
void Worker::logLatencyBreakdown(const net::Sender::Stats& ss) {
  const float isp = pipeline_->ispLatencyMs();
  const float enc = pipeline_->encodeLatencyMs();
  const float sent = ss.sentLatencyUs / 1000.0f;
  LOGI_PC("latency breakdown (%s %dx%d@%d): sensor->ISP done %.1f ms | ->encoder out %.1f ms (+%.1f) | "
          "->sent %.1f ms (+%.1f) | queue %u, dropped %u",
          pipeline_->highSpeedActive() ? "high-speed" : "normal", params_.stream.width,
          params_.stream.height, params_.stream.fps, isp, enc, enc - isp, sent, sent - enc, ss.queued,
          ss.dropped);
}

// Camera characteristics are re-read on every connection (no stream is running
// then): keys such as LENS_INFO_MINIMUM_FOCUS_DISTANCE are hidden from processes
// without the CAMERA permission, which the app may grant after nativeStart.
void Worker::refreshCameras() {
  if (streaming_ || !camMgr_) return;
  std::vector<CameraDesc> fresh = enumerateCameras(camMgr_, false);
  if (fresh.empty()) return;
  bool sameSet = fresh.size() == cams_.size();
  for (size_t i = 0; sameSet && i < fresh.size(); ++i) sameSet = fresh[i].info.id == cams_[i].info.id;
  const StreamConfig keep = params_.stream;
  const CameraControls keepCtl = params_.controls;
  const int screen = params_.screenMode;
  cams_ = std::move(fresh);
  attachHsConfigs();
  if (!sameSet || !params_.ready()) {
    params_.init(&cams_, &caps_, opts_.hsBackend != nullptr);
    return;
  }
  params_.stream = keep;
  params_.controls = keepCtl;
  params_.screenMode = screen;
  if (keep.highSpeed && !params_.highSpeedPossible()) params_.stream.highSpeed = 0;
  if (!params_.stream.highSpeed && !params_.camera().supportsSize(keep.width, keep.height)) {
    VideoSize vs = pickDefaultSize(params_.camera());
    params_.setActualSize(vs.width, vs.height);
  }
}

void Worker::updateStatus() {
  SharedInfo::Status st;
  if (conn_) {
    st.state = streaming_ ? "streaming" : "connected";
    st.peer = conn_->peerStr;
    st.transport = conn_->sender->videoMode() == net::VideoMode::Udp ? "udp" : "tcp";
    st.dropped = conn_->sender->stats().dropped;
  } else {
    st.state = "listening";
  }
  if (params_.ready()) {
    st.cameraId = params_.camera().info.id;
    st.width = params_.stream.width;
    st.height = params_.stream.height;
    st.fps = streaming_ && encodeFps_ > 0 ? encodeFps_ : static_cast<float>(params_.stream.fps);
    st.codec = params_.stream.codec == Codec::Hevc ? "hevc" : "h264";
  }
  st.bitrateKbps = (streaming_ ? appliedVideoBitrate_ : (currentBitrate_ ? currentBitrate_ : params_.stream.bitrate)) / 1000;
  st.encodeLatencyMs = streaming_ ? pipeline_->encodeLatencyMs() : 0.0f;
  std::lock_guard<std::mutex> lk(info_->statusMu);
  info_->status = std::move(st);
}

}  // namespace owc
