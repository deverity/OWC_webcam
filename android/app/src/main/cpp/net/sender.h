// Single sender thread per client: multiplexes control messages (always first)
// and video (TCP VideoFrame or paced UDP fragments).
//
// Locking: mu_ guards every queue and setting below it. The sending itself
// happens WITHOUT mu_ (items are popped first). Never log while holding mu_
// (the log sink pushes into this object).
//
// Latency policy (LatencyDropPolicy):
//   0 drop  : video queue holds <= maxQueue frames. On overflow every queued
//             frame and the incoming one are dropped, then all non-key frames
//             until the next keyframe; pushVideo() returns true so the caller
//             requests a sync frame. dropped_frames counts them.
//   1 queue : pushVideo() blocks the encoder output thread while the queue is
//             full (backpressure reaches the camera's BufferQueue, which drops
//             at the source). Nothing encoded is ever dropped.
// TCP backpressure: video is only written while the socket's unsent bytes
// (SIOCOUTQ) are <= max(1.5 x average frame, 16 KiB); otherwise frames wait in
// our queue, where the policy above applies (docs/LOW_LATENCY_RESEARCH.md §4).
#pragma once

#include <netinet/in.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "encoder/video_encoder.h"
#include "net/socket_util.h"
#include "owc/protocol.h"

namespace owc::net {

enum class VideoMode { Tcp, Udp, Pending /* UDP requested, no punch yet: video discarded */ };

class Sender {
 public:
  struct Stats {
    uint64_t framesSent = 0;
    uint64_t bytesSent = 0;       // video payload bytes put on the wire
    uint32_t dropped = 0;         // cumulative latency-policy drops
    uint32_t overflows = 0;       // cumulative overflow events
    uint32_t queued = 0;          // frames currently queued
    uint32_t tcpOutq = 0;         // unsent bytes in the TCP socket
    uint32_t avgFrameBytes = 0;
    int64_t sentLatencyUs = 0;    // EWMA capture -> frame fully handed to the socket
  };

  Sender(std::shared_ptr<Socket> tcp, int udpFd);
  ~Sender();
  Sender(const Sender&) = delete;
  Sender& operator=(const Sender&) = delete;

  void start();
  void stop();  // wakes and joins; does not shut the socket down

  void pushControl(MsgType type, std::vector<uint8_t> payload);
  void pushLog(LogLevel level, const std::string& text);
  // phone_us is stamped at send time as the midpoint of receive and send.
  void pushPong(uint64_t t0, int64_t rxMonoUs);
  // Returns true when a sync frame should be requested (overflow).
  bool pushVideo(FramePtr frame);
  // Drops queued video; with enable=false also rejects future frames and
  // releases a producer blocked in pushVideo().
  void flushVideo(bool enable);
  void setQueuePolicy(int maxFrames, int dropPolicy);
  void setVideoMode(VideoMode m);
  VideoMode videoMode();
  void setUdpTarget(const sockaddr_in& a, uint32_t sessionId);
  void setPacingBitrate(int bps);
  void setFecPercent(int pct);  // effective parity % for UDP (0 = off); never auto here
  // Sends what is queued in the control queue, then shuts the socket down.
  void closeAfterFlush();
  bool dead() const { return dead_.load(); }
  Stats stats();

 private:
  struct Ctrl {
    MsgType type;
    std::vector<uint8_t> payload;
    bool pong = false;
    uint64_t t0 = 0;
    int64_t rxUs = 0;
  };

  void loop();
  bool sendCtrl(Ctrl& c);
  bool drainControl();  // sends all queued control; false on socket failure
  bool sendVideoTcp(const EncodedFrame& f, uint32_t seq);
  bool sendVideoUdp(const EncodedFrame& f, uint32_t seq);
  bool flushBatch();
  void sendFec(const UdpHeader& base, const uint8_t* au, size_t len);
  uint32_t outq() const;
  void fail(const char* why);

  std::shared_ptr<Socket> tcp_;
  int udpFd_;
  std::thread thread_;

  std::mutex mu_;
  std::condition_variable cv_;       // sender thread wakeups
  std::condition_variable spaceCv_;  // producer waits (queue policy)
  std::deque<Ctrl> ctrl_;
  std::deque<FramePtr> video_;
  bool stop_ = false;
  bool closeReq_ = false;
  bool videoEnabled_ = true;
  bool dropUntilKey_ = false;
  int maxQueue_ = 2;
  int dropPolicy_ = 0;
  VideoMode mode_ = VideoMode::Tcp;
  sockaddr_in udpTarget_{};
  bool hasUdpTarget_ = false;
  uint32_t sessionId_ = 0;
  int pacingBps_ = 20000000;
  int fecPct_ = 0;
  Stats stats_;

  std::atomic<bool> dead_{false};
  std::atomic<bool> abort_{false};

  // Sender-thread-only state.
  uint32_t seq_ = 0;
  std::vector<uint8_t> tcpBuf_;
  double tokens_ = 0;
  int64_t tokensAtUs_ = 0;
  int batchCount_ = 0;
  sockaddr_in batchTarget_{};
  int batchRateBps_ = 0;
};

}  // namespace owc::net
