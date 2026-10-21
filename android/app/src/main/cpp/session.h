// Worker thread: owns the session state machine and the stream pipeline.
// See app_core.h for the threading/locking overview.
#pragma once

#include <camera/NdkCameraManager.h>

#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "abr.h"
#include "camera/high_speed.h"
#include "app_core.h"
#include "camera/camera_info.h"
#include "net/net_server.h"
#include "pairing_store.h"
#include "params.h"
#include "pipeline.h"
#include "util/thread.h"

namespace owc {

struct Event {
  enum class Kind {
    Connected, Message, Disconnected, Punch, CameraLost, EncoderError, Preview, ScreenMode,
    PairingChanged, CancelPairing, Shutdown
  };
  Kind kind = Kind::Shutdown;
  uint64_t gen = 0;
  std::shared_ptr<net::ClientConn> conn;
  MsgType type = MsgType::Bye;
  std::vector<uint8_t> payload;
  std::string text;
  ANativeWindow* window = nullptr;
  std::shared_ptr<std::promise<void>> done;
};

class Worker : public net::NetServer::Callbacks, public VideoEncoder::Sink {
 public:
  Worker(const StartOptions& opts, EventSink* sink, std::shared_ptr<SharedInfo> info,
         std::shared_ptr<PairingStore> pairing, ANativeWindow* preview);
  ~Worker() override;

  bool start(std::string& err);  // binds sockets, starts threads
  void stop();                   // joins everything
  void setPreview(ANativeWindow* w);
  // Phone side left black-screen mode (any thread): resets the param and tells the PC.
  void screenModeChanged(int mode);
  // Any thread. A PC was removed from the store: drop the session if its PC
  // is no longer paired.
  void pairingChanged();
  // Any thread. The user cancelled the code on the phone.
  void cancelPairing();

  // NetServer::Callbacks (net thread)
  void onClientConnected(std::shared_ptr<net::ClientConn> c) override;
  void onClientMessage(uint64_t gen, MsgType type, std::vector<uint8_t>&& payload) override;
  void onClientDisconnected(uint64_t gen) override;
  void onPunch(uint64_t gen, const sockaddr_in& from) override;
  DiscoveryReply discoveryInfo() override;
  HelloAck busyAck() override;

  // VideoEncoder::Sink (encoder output thread)
  void onVideoFormat(const VideoFormat& fmt) override;
  bool onEncodedFrame(FramePtr frame) override;
  void onEncoderError(const std::string& msg) override;

 private:
  using Clock = std::chrono::steady_clock;

  void run();
  void init();
  void handle(Event& ev);
  void tick();

  // session
  void onConnected(std::shared_ptr<net::ClientConn> c);
  void onDisconnected();
  void onMessage(MsgType type, const std::vector<uint8_t>& payload);
  void onHello(const std::vector<uint8_t>& payload);
  // authentication (protocol.h "PAIRING"); only between Hello and AuthResult{Ok}
  void onAuthMessage(MsgType type, const std::vector<uint8_t>& payload);
  void onAuthProof(const std::vector<uint8_t>& payload);
  void onPairBegin();
  void onPairCode(const std::vector<uint8_t>& payload);
  void sendAuthResult(AuthStatus st, uint8_t attemptsLeft = 0, uint16_t seconds = 0, const Key32* key = nullptr);
  void authenticated(const Key32* newKey);  // session starts
  void endPairing(int outcome);             // clears the pending code, tells the app
  bool codePending() const { return !code_.empty(); }
  void onSetParam(const std::vector<uint8_t>& payload);
  void onCommand(const std::vector<uint8_t>& payload);
  void onReceiverReport(const std::vector<uint8_t>& payload);
  void closeClient(const char* why);
  void sendControl(MsgType t, const std::vector<uint8_t>& payload);
  void sendParamState(ParamId id, ParamStatus st, const ParamValue& v);
  void sendCapabilities();
  void sendStats(int64_t nowMs);
  HelloAck makeAck(HelloStatus st);

  // stream
  void startStream(bool notify);
  void stopStream(bool notify);
  void scheduleRestart(const std::string& why);
  void applyBitrate(bool force);
  int effectiveFecPercent() const;
  void updateStatus();
  void refreshCameras();
  void attachHsConfigs();
  void fallbackToNormal(const std::string& why);
  void logLatencyBreakdown(const net::Sender::Stats& ss);
  void applyQueuePolicy();
  Abr::Local abrLocal();
  void rateControlCheck(uint32_t measuredBps, int64_t now);

  void emit(int type, int i, const std::string& s);

  StartOptions opts_;
  EventSink* sink_;
  std::shared_ptr<SharedInfo> info_;
  std::shared_ptr<PairingStore> pairing_;
  EventQueue<Event> q_;
  std::thread thread_;
  net::NetServer net_;
  std::promise<void> initDone_;

  // ---- worker-thread-only state
  ACameraManager* camMgr_ = nullptr;
  std::vector<CameraDesc> cams_;
  EncoderCapsSet caps_;
  ParamStore params_;
  std::unique_ptr<Pipeline> pipeline_;
  Abr abr_;
  ANativeWindow* preview_ = nullptr;

  std::shared_ptr<net::ClientConn> conn_;
  bool helloSeen_ = false;  // valid Hello answered with HelloAck{AuthRequired}
  bool helloDone_ = false;  // authenticated: the session is up (Capabilities sent)
  // ---- authentication of the current connection
  Id16 clientId_{};
  std::string clientName_;  // sanitized Hello::client_name
  Id16 nonce_{};
  Id16 phoneId_{};           // as sent in HelloAck (AuthProof is checked against it)
  uint32_t pendingSid_ = 0;  // published in conn_->sessionId only after AuthResult{Ok}
  int64_t authDeadlineMs_ = 0;
  std::string code_;         // pending 6-digit code ("" = none). Never logged.
  int64_t codeExpiryMs_ = 0;
  int attemptsLeft_ = 0;
  // ---- pairing rate limiting (global, survives connections)
  int64_t lockoutUntilMs_ = 0;
  int pairFailures_ = 0;      // wrong codes since the last lockout/success (see onPairBegin)
  int64_t lastPairFailureMs_ = 0;
  bool closing_ = false;
  bool sessionTeardown_ = false;  // inside onDisconnected(): stop is not a PC decision
  bool byeReceived_ = false;  // PC sent Bye: the disconnect event is "graceful", not "lost"
  int64_t helloDeadlineMs_ = 0, closeDeadlineMs_ = 0, punchDeadlineMs_ = 0;
  VideoTransport requested_ = VideoTransport::Tcp;
  bool wantStream_ = false;
  bool streaming_ = false;
  int64_t reconfigAtMs_ = 0;
  int64_t restartAtMs_ = 0;
  int restartAttempts_ = 0;
  int64_t streamStartMs_ = 0;
  int currentBitrate_ = 0;     // wire target (ABR) or param
  int appliedVideoBitrate_ = 0;
  int appliedFec_ = -1;
  int64_t lastStatsMs_ = 0;
  uint64_t lastEncFrames_ = 0, lastEncBytes_ = 0, lastSentFrames_ = 0;
  uint32_t lastOverflows_ = 0;
  std::deque<int64_t> keyReqs_;
  bool bframeNoticeDone_ = false;
  float encodeFps_ = 0;
  bool appStreaming_ = false;  // STREAMING_STARTED sent without STOPPED
  int64_t lastBreakdownMs_ = 0;
  // Closed-loop correction for encoders that overshoot their target (seen in
  // high-speed mode on OMX.qcom): encoder bitrate = target x rcFactor_.
  double rcFactor_ = 1.0;
  double encOutBpsEwma_ = 0;      // measured encoder output (ABR app-limited detection)
  uint32_t queueAllowance_ = 1;   // frames the send queue may hold before it counts as growing
  double rcMeasuredBps_ = 0;
  bool rcNoticeDone_ = false;
  bool hfrIdrNoticeDone_ = false;

  // Encoder output thread -> current sender (see app_core.h).
  net::Sender* frameSender_ = nullptr;
};

}  // namespace owc
