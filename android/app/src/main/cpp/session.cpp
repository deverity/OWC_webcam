#include "session.h"

#include <android/native_window.h>
#include <stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "util/clock.h"
#include "util/debug_props.h"
#include "util/log.h"

namespace owc {
namespace {
constexpr int64_t kHelloTimeoutMs = 5000;
constexpr int64_t kRxTimeoutMs = 10000;     // PC pings at 2 Hz (only after authentication)
constexpr int64_t kAuthTimeoutMs = kAuthTimeoutS * 1000LL;
// Wrong pairing codes are remembered across connections for this long (see
// onPairBegin): reconnecting must not buy fresh attempts.
constexpr int64_t kPairFailureMemoryMs = 10 * 60 * 1000;
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

// Names used in the status JSON (ARCHITECTURE.md "Contrato JNI").
const char* cipherName(Cipher c) {
  switch (c) {
    case Cipher::Aes128Gcm: return "aes-128-gcm";
    case Cipher::ChaCha20Poly1305: return "chacha20-poly1305";
    case Cipher::None: break;
  }
  return "none";
}
}  // namespace

Worker::Worker(const StartOptions& opts, EventSink* sink, std::shared_ptr<SharedInfo> info,
               std::shared_ptr<PairingStore> pairing, ANativeWindow* preview)
    : opts_(opts), sink_(sink), info_(std::move(info)), pairing_(std::move(pairing)), preview_(preview) {}

Worker::~Worker() {
  stop();
  if (preview_) {
    ANativeWindow_release(preview_);
    preview_ = nullptr;
  }
}

bool Worker::start(std::string& err) {
  if (!net_.start(opts_.controlPort, opts_.udpPort, opts_.discoveryPort, opts_.announcePort, this, err))
    return false;
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

void Worker::screenModeChanged(int mode) {
  Event e;
  e.kind = Event::Kind::ScreenMode;
  e.gen = static_cast<uint64_t>(mode);  // reused as payload; no connection involved
  q_.push(std::move(e));
}

void Worker::pairingChanged() {
  Event e;
  e.kind = Event::Kind::PairingChanged;
  q_.push(std::move(e));
}

void Worker::cancelPairing() {
  Event e;
  e.kind = Event::Kind::CancelPairing;
  q_.push(std::move(e));
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
    case Event::Kind::ScreenMode: {
      // The phone user left black mode locally: keep the store (and the PC UI) in sync.
      // No emit(): Kotlin already acted on it.
      params_.screenMode = static_cast<int>(ev.gen);
      if (conn_ && helloDone_ && !closing_ && params_.ready())
        sendParamState(ParamId::ScreenMode, ParamStatus::Ok, params_.current(ParamId::ScreenMode));
      break;
    }
    case Event::Kind::PairingChanged:
      // Re-checked here (not by id) so a removal racing with a new pairing of
      // the same PC resolves to the store's final state.
      if (conn_ && helloDone_ && !closing_ && !pairing_->contains(clientId_)) {
        LOGI("PC '%s' was removed on the phone: ending its session", clientName_.c_str());
        conn_->sessionId = 0;  // no more UDP punches
        wantStream_ = false;
        restartAtMs_ = 0;
        sessionTeardown_ = true;
        stopStream(true);
        sessionTeardown_ = false;
        sendControl(MsgType::Bye, {});
        byeReceived_ = true;  // deliberate end (the app treats it like a Bye)
        closeClient("PC unpaired");
      }
      break;
    case Event::Kind::CancelPairing:
      if (conn_ && codePending()) {
        if (!closing_) sendAuthResult(AuthStatus::Rejected);
        endPairing(kPairingRejected);
        closeClient("pairing cancelled on the phone");
      }
      break;
    case Event::Kind::Shutdown:
      break;
  }
}

void Worker::tick() {
  const int64_t now = monoMs();
  if (conn_) {
    if (!helloSeen_ && !closing_ && now > helloDeadlineMs_) closeClient("no Hello within 5 s");
    // Before authentication there is no silence timeout (the user may take a
    // while to type the code); the auth deadline / code lifetime bound it.
    if (helloSeen_ && !helloDone_ && !closing_) {
      if (codePending()) {
        if (now >= codeExpiryMs_) {
          sendAuthResult(AuthStatus::Expired);
          endPairing(kPairingExpired);
          closeClient("pairing code expired");
        }
      } else if (now > authDeadlineMs_) {
        closeClient("not authenticated in time");
      }
    }
    if (helloDone_ && now - conn_->lastRxMs.load() > kRxTimeoutMs && !closing_) {
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
  byeReceived_ = false;
  if (conn_) {  // cannot happen (net thread allows one client) but be safe
    c->sock->shutdownBoth();
    return;
  }
  conn_ = std::move(c);
  helloSeen_ = false;
  helloDone_ = false;
  closing_ = false;
  clientId_ = Id16{};
  clientName_.clear();
  pendingSid_ = 0;
  code_.clear();
  wipeHandshake();
  helloDeadlineMs_ = monoMs() + kHelloTimeoutMs;
  punchDeadlineMs_ = 0;
  wantStream_ = false;
  restartAtMs_ = 0;
  restartAttempts_ = 0;
  keyReqs_.clear();
  lastOverflows_ = 0;  // per-sender counters start at zero
  lastSentFrames_ = 0;
  lastSealFrames_ = lastSealNs_ = 0;
  appliedFec_ = -1;
  applyQueuePolicy();
  // No PC log sink, Capabilities or app CLIENT_CONNECTED event until the
  // connection authenticates (authenticated()).
  updateStatus();
}

void Worker::onDisconnected() {
  if (codePending()) endPairing(kPairingDisconnected);
  log::setPcSink(nullptr);
  const bool wasSession = helloDone_;
  sessionTeardown_ = true;  // kEvStreamingStopped arg 1 = stop caused by the session ending
  stopStream(true);
  sessionTeardown_ = false;
  wantStream_ = false;
  restartAtMs_ = 0;
  reconfigAtMs_ = 0;
  conn_->sessionId = 0;
  conn_->sender->stop();
  conn_->sock->shutdownBoth();
  LOGI("session with %s ended", conn_->peerStr.c_str());
  conn_.reset();
  helloSeen_ = false;
  helloDone_ = false;
  closing_ = false;
  pendingSid_ = 0;
  // Session keys: the openers die with the ClientConn (net thread drops its
  // reference too), the sealers with the Sender; Aead destructors wipe them.
  wipeHandshake();
  // arg: 1 = PC said Bye (explicit), 0 = connection lost/timeout (PC will auto-reconnect).
  // Only for sessions that were up (CLIENT_CONNECTED is sent after authentication).
  if (wasSession) emit(kEvClientDisconnected, byeReceived_ ? 1 : 0, "");
  byeReceived_ = false;
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
  if (!helloSeen_) {
    if (type == MsgType::Hello) onHello(payload);
    else LOGD("message %s before Hello ignored", toString(type));
    return;
  }
  if (!helloDone_) {
    onAuthMessage(type, payload);
    return;
  }
  switch (type) {
    case MsgType::SetParam: onSetParam(payload); break;
    case MsgType::Command: onCommand(payload); break;
    case MsgType::ReceiverReport: onReceiverReport(payload); break;
    case MsgType::Bye:
      byeReceived_ = true;
      closeClient("Bye from client");
      break;
    case MsgType::Hello: LOGW_LOCAL("duplicate Hello ignored"); break;
    case MsgType::PairBegin:
    case MsgType::PairCode:
    case MsgType::AuthProof: LOGD("%s after authentication ignored", toString(type)); break;
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
  Cipher cipher;
  if (!chooseCipher(h.cipher_mask, localCipherMask(), cipher)) {
    sendControl(MsgType::HelloAck, encode(makeAck(HelloStatus::VersionMismatch)));
    LOGW_LOCAL("no common cipher with %s (PC mask 0x%x, ours 0x%x)", conn_->peerStr.c_str(), h.cipher_mask,
               localCipherMask());
    closeClient("no common cipher");
    return;
  }
  clientId_ = h.client_id;
  clientName_ = sanitizePeerName(h.client_name);
  pendingSid_ = 0;
  while (pendingSid_ == 0) pendingSid_ = arc4random();
  requested_ = h.transport;
  // Fresh ephemeral X25519 key per connection; th binds the exact Hello bytes
  // received and the exact HelloAck bytes sent.
  wipeHandshake();
  cipher_ = cipher;
  arc4random_buf(ephSecret_.data(), ephSecret_.size());
  peerEph_ = h.eph_pub;
  helloBytes_ = payload;
  hs_ = std::make_unique<Handshake>(Handshake::Role::Phone, ephSecret_);
  HelloAck ack = makeAck(HelloStatus::AuthRequired);
  ack.session_id = pendingSid_;
  ack.phone_id = pairing_->phoneId();
  ack.eph_pub = hs_->ephPublic();
  ack.cipher = static_cast<uint8_t>(cipher_);
  ack.paired = pairing_->contains(clientId_);
  ackBytes_ = encode(ack);
  if (!hs_->begin(helloBytes_.data(), helloBytes_.size(), ackBytes_.data(), ackBytes_.size(), peerEph_, cipher_)) {
    wipeHandshake();
    closeClient("invalid ephemeral key in Hello");
    return;
  }
  sendControl(MsgType::HelloAck, ackBytes_);
  helloSeen_ = true;
  authDeadlineMs_ = monoMs() + kAuthTimeoutMs;
  LOGI("Hello from '%s' (%s): %s, cipher %s", clientName_.c_str(), conn_->peerStr.c_str(),
       ack.paired ? "paired, waiting for AuthProof" : "not paired", cipherName(cipher_));
  updateStatus();
}

std::unique_ptr<Handshake> Worker::freshHandshake() {
  auto hs = std::make_unique<Handshake>(Handshake::Role::Phone, ephSecret_);
  if (!hs->begin(helloBytes_.data(), helloBytes_.size(), ackBytes_.data(), ackBytes_.size(), peerEph_, cipher_))
    return nullptr;
  return hs;
}

bool Worker::newCpaceShare() {
  hs_ = freshHandshake();
  if (!hs_) return false;
  Key32 rnd;
  arc4random_buf(rnd.data(), rnd.size());
  yb_ = hs_->cpaceStart(code_, rnd);
  secureWipe(rnd.data(), rnd.size());
  return true;
}

void Worker::wipeHandshake() {
  hs_.reset();  // the Handshake destructor wipes its secrets
  secureWipe(ephSecret_.data(), ephSecret_.size());
  secureWipe(yb_.data(), yb_.size());
  peerEph_ = Key32{};
  helloBytes_.clear();
  ackBytes_.clear();
  cipher_ = Cipher::None;
}

// ------------------------------------------------------------------ authentication
// Nothing in here may log the code or a key (LOGI/LOGW_LOCAL are logcat only,
// and the PC log sink is not installed before authenticated() anyway).
void Worker::onAuthMessage(MsgType type, const std::vector<uint8_t>& payload) {
  switch (type) {
    case MsgType::AuthProof: onAuthProof(payload); break;
    case MsgType::PairBegin: onPairBegin(); break;
    case MsgType::PairCode: onPairCode(payload); break;
    case MsgType::Bye:
      byeReceived_ = true;
      if (codePending()) endPairing(kPairingDisconnected);
      closeClient("Bye from client (not authenticated)");
      break;
    case MsgType::Hello: LOGD("duplicate Hello ignored"); break;
    default: LOGD("message %s before authentication ignored", toString(type)); break;
  }
}

void Worker::sendAuthResult(AuthStatus st, uint8_t attemptsLeft, uint16_t seconds, const Key32* cpacePub,
                            const Key32* confirm) {
  AuthResult r;
  r.status = st;
  r.attempts_left = attemptsLeft;
  r.seconds = seconds;
  if (cpacePub) r.cpace_pub = *cpacePub;
  if (confirm) r.confirm = *confirm;
  // AuthResult{Ok} is the last plain TCP message: the sender seals everything after it.
  if (conn_) conn_->sender->pushControl(MsgType::AuthResult, encode(r), st == AuthStatus::Ok);
  secureWipe(r.confirm.data(), r.confirm.size());
}

void Worker::onAuthProof(const std::vector<uint8_t>& payload) {
  if (codePending()) {
    LOGD("AuthProof while a pairing code is shown: ignored");
    return;
  }
  AuthProof p;
  Key32 key{};
  bool ok = decode(payload.data(), payload.size(), p) && pairing_->keyFor(clientId_, key);
  if (ok) {
    // Fresh Handshake per attempt: a wrong proof leaves nothing behind for a
    // PairBegin on the same connection.
    hs_ = freshHandshake();
    ok = hs_ != nullptr;
    if (ok) {
      hs_->usePairingKey(key);
      Key32 expect = hs_->pcConfirm();
      ok = constantTimeEqual(expect.data(), p.confirm.data(), kKeySize);
      secureWipe(expect.data(), expect.size());
    }
  }
  secureWipe(key.data(), key.size());
  if (!ok) {
    hs_.reset();
    // The connection stays open: the PC drops its key and may PairBegin.
    LOGI("AuthProof from '%s' (%s) rejected: unknown PC or wrong proof", clientName_.c_str(),
         conn_->peerStr.c_str());
    sendAuthResult(AuthStatus::NotPaired);
    return;
  }
  authenticated();
}

void Worker::onPairBegin() {
  const int64_t now = monoMs();
  if (clientId_ == Id16{}) {
    LOGW_LOCAL("PairBegin from %s without a client_id: rejected", conn_->peerStr.c_str());
    sendAuthResult(AuthStatus::Rejected);
    closeClient("PairBegin without client_id");
    return;
  }
  if (now < lockoutUntilMs_) {
    const auto left = static_cast<uint16_t>((lockoutUntilMs_ - now + 999) / 1000);
    LOGI("PairBegin from '%s' during the pairing lockout (%u s left)", clientName_.c_str(),
         static_cast<unsigned>(left));
    sendAuthResult(AuthStatus::Locked, 0, left);
    closeClient("pairing locked out");
    return;
  }
  if (codePending()) {  // repeated PairBegin: same code and CPace share, no fresh attempts
    const auto left = static_cast<uint16_t>(std::max<int64_t>(1, (codeExpiryMs_ - now + 999) / 1000));
    sendAuthResult(AuthStatus::CodeShown, static_cast<uint8_t>(attemptsLeft_), left, &yb_);
    return;
  }
  // Wrong codes count globally until a lockout or a success (forgotten after
  // kPairFailureMemoryMs): with a strict per-code budget a client could get 2
  // guesses per reconnect forever without ever being locked out. The fresh
  // code's attempts_left tells the PC what is left (always >= 1 here).
  if (pairFailures_ > 0 && now - lastPairFailureMs_ > kPairFailureMemoryMs) pairFailures_ = 0;
  attemptsLeft_ = std::max(1, static_cast<int>(kPairCodeAttempts) - pairFailures_);
  uint32_t mod = 1;
  for (int i = 0; i < kPairCodeDigits; ++i) mod *= 10;
  char buf[16];
  snprintf(buf, sizeof(buf), "%0*u", static_cast<int>(kPairCodeDigits), arc4random_uniform(mod));
  code_ = buf;
  secureWipe(buf, sizeof(buf));
  // CPace: the code is an input on both sides and never travels; the PC gets
  // only our share Yb.
  if (!newCpaceShare()) {
    code_.clear();
    closeClient("handshake failure");
    return;
  }
  codeExpiryMs_ = now + kPairCodeLifetimeS * 1000LL;
  updateStatus();  // "pairing" is visible before the PC / app hear about the code
  sendAuthResult(AuthStatus::CodeShown, static_cast<uint8_t>(attemptsLeft_), kPairCodeLifetimeS, &yb_);
  emit(kEvPairingCode, kPairCodeLifetimeS, code_ + "\n" + clientName_);
  LOGI("pairing requested by '%s' (%s): code shown on the phone (%d attempt(s))", clientName_.c_str(),
       conn_->peerStr.c_str(), attemptsLeft_);
}

void Worker::onPairCode(const std::vector<uint8_t>& payload) {
  if (!codePending()) {
    LOGD("PairCode without a pending code: ignored");
    return;
  }
  const int64_t now = monoMs();
  if (now >= codeExpiryMs_) {
    sendAuthResult(AuthStatus::Expired);
    endPairing(kPairingExpired);
    closeClient("pairing code expired");
    return;
  }
  // The PC typed a code, ran CPace with our Yb and proved the result:
  // a wrong code (or an invalid share) shows up as a confirmation mismatch.
  PairCode pc;
  bool match = false;
  if (hs_ && decode(payload.data(), payload.size(), pc) && hs_->cpaceFinish(pc.cpace_pub)) {
    Key32 expect = hs_->pcConfirm();
    match = constantTimeEqual(expect.data(), pc.confirm.data(), kKeySize);
    secureWipe(expect.data(), expect.size());
  }
  if (match) {
    Key32 key = hs_->newPairingKey();
    if (!pairing_->add(clientId_, clientName_, key, static_cast<int64_t>(time(nullptr))))
      LOGW_LOCAL("pairing with '%s' could not be saved: it lasts until the app restarts", clientName_.c_str());
    secureWipe(key.data(), key.size());
    pairFailures_ = 0;
    LOGI("PC '%s' (%s) paired", clientName_.c_str(), conn_->peerStr.c_str());
    endPairing(kPairingPaired);
    authenticated();
    return;
  }
  ++pairFailures_;
  lastPairFailureMs_ = now;
  --attemptsLeft_;
  if (attemptsLeft_ > 0) {
    LOGI("wrong pairing code from '%s' (%d attempt(s) left)", clientName_.c_str(), attemptsLeft_);
    // Same code, new CPace share: every try is a fresh exchange.
    if (!newCpaceShare()) {
      endPairing(kPairingFailed);
      closeClient("handshake failure");
      return;
    }
    sendAuthResult(AuthStatus::WrongCode, static_cast<uint8_t>(attemptsLeft_), 0, &yb_);
    return;
  }
  lockoutUntilMs_ = now + kPairLockoutS * 1000LL;
  pairFailures_ = 0;
  LOGW_LOCAL("too many wrong pairing codes from '%s' (%s): pairing locked for %u s", clientName_.c_str(),
             conn_->peerStr.c_str(), static_cast<unsigned>(kPairLockoutS));
  sendAuthResult(AuthStatus::Locked, 0, kPairLockoutS);
  endPairing(kPairingFailed);
  closeClient("too many wrong pairing codes");
}

void Worker::endPairing(int outcome) {
  if (!codePending()) return;
  secureWipe(&code_[0], code_.size());
  code_.clear();
  // A finished pairing leaves hs_ ready for authenticated(); any other
  // outcome drops the CPace state.
  if (outcome != kPairingPaired) hs_.reset();
  secureWipe(yb_.data(), yb_.size());
  codeExpiryMs_ = 0;
  attemptsLeft_ = 0;
  updateStatus();
  emit(kEvPairingEnded, outcome, clientName_);
}

void Worker::authenticated() {
  // Handoff order (all before the Ok can reach the PC, which seals from its
  // first message after reading it):
  //   1. p2c sealers -> Sender (once per connection): adopted with the next
  //      item it pops, so every PunchAck is sealed;
  //   2. c2p openers -> ClientConn, rxSealed: the net thread opens every TCP
  //      message parsed from now on (the PC sends nothing between its proof
  //      and our answer) and every punch;
  //   3. sessionId: published before the Ok goes out (the PC may punch the
  //      instant it reads it); its release orders 1 before the net thread's
  //      acquire;
  //   4. AuthResult{Ok, phoneConfirm} queued with startSealing: plain itself,
  //      everything the sender writes after it is sealed.
  SessionKeys keys = hs_->keys();
  // Keys are installed exactly once per connection (helloDone_ gates every
  // auth message); refuse rather than re-init a sealer (counter reset = nonce reuse).
  if (cipher_ != Cipher::None && (conn_->rx || conn_->rxSealed.load())) {
    secureWipe(&keys, sizeof(keys));
    hs_.reset();
    closeClient("session keys already installed");
    return;
  }
  if (cipher_ != Cipher::None) {
    auto rx = std::make_unique<net::RxCrypto>();
    auto tx = std::make_unique<net::TxCrypto>();
    const bool ok = rx->tcp.init(keys.c2p_tcp) && rx->udp.init(keys.c2p_udp) && tx->tcp.init(keys.p2c_tcp) &&
                    tx->udp.init(keys.p2c_udp);
    if (!ok) {
      secureWipe(&keys, sizeof(keys));
      hs_.reset();
      LOGE_LOCAL("cipher %s could not be initialised", cipherName(cipher_));
      closeClient("cipher init failed");
      return;
    }
    if (!conn_->sender->setSessionKeys(std::move(tx))) {
      secureWipe(&keys, sizeof(keys));
      hs_.reset();
      closeClient("session keys already installed");
      return;
    }
    conn_->rx = std::move(rx);
    conn_->rxSealed.store(true, std::memory_order_release);
  }
  secureWipe(&keys, sizeof(keys));
  Key32 confirm = hs_->phoneConfirm();
  hs_.reset();
  secureWipe(ephSecret_.data(), ephSecret_.size());
  conn_->sessionId.store(pendingSid_, std::memory_order_release);  // UDP punches (and Pings) accepted
  sendAuthResult(AuthStatus::Ok, 0, 0, nullptr, &confirm);
  secureWipe(confirm.data(), confirm.size());
  helloDone_ = true;
  refreshCameras();
  // USB (adb forward => 127.0.0.1) cannot carry UDP.
  if (requested_ == VideoTransport::Udp && conn_->peerStr.rfind("127.", 0) == 0) {
    LOGW_LOCAL("UDP requested over a loopback (adb) connection: using TCP");
    requested_ = VideoTransport::Tcp;
  }
  if (requested_ == VideoTransport::Udp) {
    conn_->sender->setVideoMode(net::VideoMode::Pending);
    punchDeadlineMs_ = monoMs() + kPunchTimeoutMs;
  } else {
    conn_->sender->setVideoMode(net::VideoMode::Tcp);
  }
  std::weak_ptr<net::Sender> ws = conn_->sender;
  log::setPcSink(std::make_shared<log::Sink>([ws](log::Level l, const std::string& text) {
    if (auto s = ws.lock()) s->pushLog(toWire(l), text);
  }));
  sendCapabilities();
  lastStatsMs_ = monoMs();
  emit(kEvClientConnected, 0, conn_->peerStr);
  LOGI_PC("session %08x with '%s' (%s), transport %s, cipher %s", pendingSid_, clientName_.c_str(),
          conn_->peerStr.c_str(), requested_ == VideoTransport::Udp ? "udp" : "tcp", cipherName(cipher_));
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
      if (params_.stream.fps > kIdrSafeOperatingRate && !hfrIdrNoticeDone_) {
        hfrIdrNoticeDone_ = true;
        LOGW("encoder at %d fps runs in its high-frame-rate mode, which ignores on-demand IDR "
             "requests; the next periodic IDR (every <= %d ms) recovers the stream",
             params_.stream.fps, std::min(params_.stream.keyframeMs > 0 ? params_.stream.keyframeMs : 1000, 1000));
      }
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
      std::string name = sanitizePeerName(c.text);  // valid modified-UTF-8 for NewStringUTF
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
  if (!streaming_ || !conn_) return;
  // Always fed: the loss estimate also drives FecPercent=-1 (auto) without ABR.
  const bool changed = abr_.onReceiverReport(rr, abrLocal(), monoMs());
  if (changed && params_.stream.abr) applyBitrate(false);
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
    if (now - streamStartMs_ > 1500)
      encOutBpsEwma_ = encOutBpsEwma_ == 0 ? s.bitrate_bps : 0.5 * encOutBpsEwma_ + 0.5 * s.bitrate_bps;
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
    else if (abr_.tick(abrLocal(), now)) applyBitrate(false);
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
  net_.setIdle(false);
  streamStartMs_ = monoMs();
  appliedVideoBitrate_ = currentBitrate_;
  lastEncFrames_ = lastEncBytes_ = 0;
  rcFactor_ = 1.0;
  rcMeasuredBps_ = 0;
  encOutBpsEwma_ = 0;
  rcNoticeDone_ = false;
  hfrIdrNoticeDone_ = false;
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
    net_.setIdle(true);
  }
  // notify=false: internal restart (reconfigure / camera loss), the app keeps
  // seeing "streaming" until the stream is really stopped or given up.
  if (notify && appStreaming_) {
    appStreaming_ = false;
    emit(kEvStreamingStopped, sessionTeardown_ ? 1 : 0, "");
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
  // HS: the encoder receives batch x encFps / sensorFps frames per HAL batch
  // (batch_size_max = 4 on SM-N9600 -> 1 frame per batch at 60 fps, 4 at 240).
  if (streaming_ && pipeline_->highSpeedActive()) {
    const CameraDesc& cd = params_.camera();
    const int sensorFps = std::max(1, params_.cameraFps());
    const int batch = cd.hsBatch(params_.stream.width, params_.stream.height);
    q = std::max(q, (batch * params_.stream.fps + sensorFps - 1) / sensorFps);
  }
  conn_->sender->setQueuePolicy(q, params_.stream.dropPolicy);
  queueAllowance_ = static_cast<uint32_t>(std::max(1, q - 1));
}

Abr::Local Worker::abrLocal() {
  Abr::Local l;
  if (conn_) l.queued = conn_->sender->stats().queued;
  l.queueAllowance = queueAllowance_;
  // Compare like with like: the ABR target is the wire rate (video + FEC parity).
  const int fec = std::max(0, appliedFec_);
  l.encoderBps = static_cast<uint32_t>(encOutBpsEwma_ * (100 + fec) / 100);
  return l;
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
  const float exp = pipeline_->exposureMs();
  // AEAD time per frame on the sender thread over this window (max = session max).
  const uint64_t sf = ss.sealFrames - std::min(ss.sealFrames, lastSealFrames_);
  const uint64_t sn = ss.sealNsTotal - std::min(ss.sealNsTotal, lastSealNs_);
  lastSealFrames_ = ss.sealFrames;
  lastSealNs_ = ss.sealNsTotal;
  char seal[96] = "";
  if (sf) snprintf(seal, sizeof(seal), " | %s seal %.0f us/frame (max %.0f)", cipherName(cipher_), sn / 1000.0 / sf,
                   ss.sealNsMax / 1000.0);
  LOGI_PC("latency breakdown (%s %dx%d@%d): sensor->ISP done %.1f ms | ->encoder out %.1f ms (+%.1f) | "
          "->sent %.1f ms (+%.1f)%s | queue %u, dropped %u | exp %.2f ms (eff ISP %.1f) frameDur %.2f ms "
          "iso %d depth %.1f",
          pipeline_->highSpeedActive() ? "high-speed" : "normal", params_.stream.width,
          params_.stream.height, params_.stream.fps, isp, enc, enc - isp, sent, sent - enc, seal, ss.queued,
          ss.dropped, exp, isp - exp / 2, pipeline_->frameDurationMs(), pipeline_->iso(),
          pipeline_->pipelineDepth());
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
  if (conn_ && helloDone_) {
    st.state = streaming_ ? "streaming" : "connected";
    st.peer = conn_->peerStr;
    st.transport = conn_->sender->videoMode() == net::VideoMode::Udp ? "udp" : "tcp";
    st.cipher = cipherName(cipher_);
    const net::Sender::Stats ss = conn_->sender->stats();
    st.dropped = ss.dropped;
    if (ss.sealFrames) {
      st.sealUsAvg = static_cast<float>(ss.sealNsTotal / 1000.0 / ss.sealFrames);
      st.sealUsMax = static_cast<float>(ss.sealNsMax / 1000.0);
    }
  } else if (conn_ && codePending()) {
    st.state = "pairing";
    st.peer = conn_->peerStr;
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
