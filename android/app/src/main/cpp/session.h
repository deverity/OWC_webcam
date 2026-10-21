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
#include "params.h"
#include "pipeline.h"
#include "util/thread.h"

namespace owc {

struct Event {
  enum class Kind { Connected, Message, Disconnected, Punch, CameraLost, EncoderError, Preview, Shutdown };
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
         ANativeWindow* preview);
  ~Worker() override;

  bool start(std::string& err);  // binds sockets, starts threads
  void stop();                   // joins everything
  void setPreview(ANativeWindow* w);

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
  void rateControlCheck(uint32_t measuredBps, int64_t now);

  void emit(int type, int i, const std::string& s);

  StartOptions opts_;
  EventSink* sink_;
  std::shared_ptr<SharedInfo> info_;
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
  bool helloDone_ = false;
  bool closing_ = false;
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
  double rcMeasuredBps_ = 0;
  bool rcNoticeDone_ = false;

  // Encoder output thread -> current sender (see app_core.h).
  net::Sender* frameSender_ = nullptr;
};

}  // namespace owc
