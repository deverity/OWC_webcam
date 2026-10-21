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
//
// Encryption (protocol v3, owc/crypto.h): ALL sealing happens on this sender
// thread, never on the encoder output thread. The worker hands the phone->PC
// keys over with setSessionKeys() before it queues AuthResult{Ok} with
// startSealing = true; the sender thread adopts them under mu_ together with
// the next item it pops, so:
//   - TCP: everything queued before the Ok and the Ok itself go out plain,
//     every later message (control, Pong, Log, Stats, VideoFrame) is one
//     sealed record (VideoFrame: TcpSealer::seal2 over header + AU, which
//     encrypts while copying into sealBuf_: one pass, no extra copy).
//   - UDP: every datagram sent after the adoption (PunchAck, video, FEC) is
//     sealed in place in the sendmmsg batch buffer right before it is sent;
//     sealed sessions fragment with kUdpMaxPayloadSealed.
// Cipher::None: no keys are handed over and nothing changes.
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
#include "owc/crypto.h"
#include "owc/protocol.h"

namespace owc::net {

enum class VideoMode { Tcp, Udp, Pending /* UDP requested, no punch yet: video discarded */ };

// Phone -> PC session crypto (p2c keys). Initialised by the worker, then owned
// by the sender thread. Aead destructors wipe the key schedules.
struct TxCrypto {
  TcpSealer tcp;
  UdpSealer udp;
};

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
    // AEAD cost on this thread, per video frame (all its records/datagrams,
    // FEC included). Cumulative for this connection; only sealed frames count.
    uint64_t sealFrames = 0;
    uint64_t sealNsTotal = 0;
    uint32_t sealNsMax = 0;
  };

  Sender(std::shared_ptr<Socket> tcp, int udpFd);
  ~Sender();
  Sender(const Sender&) = delete;
  Sender& operator=(const Sender&) = delete;

  void start();
  void stop();  // wakes and joins; does not shut the socket down

  // startSealing: this is AuthResult{Ok}; it goes out plain and every TCP
  // message after it is sealed (if setSessionKeys() was called).
  void pushControl(MsgType type, std::vector<uint8_t> payload, bool startSealing = false);
  // Worker, before the Ok is queued and before the session id is published
  // (so every PunchAck is sealed). Not for Cipher::None. At most ONCE per
  // Sender (false = already set, keys discarded): a second init would restart
  // the counters = nonce reuse.
  bool setSessionKeys(std::unique_ptr<TxCrypto> keys);
  // Net thread: answer a validated punch (sealed when the session is).
  void pushPunchAck(const sockaddr_in& to, uint32_t sessionId);
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
    bool startSealing = false;
    bool punchAck = false;
    uint64_t t0 = 0;
    int64_t rxUs = 0;
    sockaddr_in to{};
    uint32_t sid = 0;
  };

  void loop();
  void adoptKeysLocked();  // sender thread, under mu_
  bool sendCtrl(Ctrl& c);
  void sendPunchAck(const Ctrl& c);
  bool sendTcpRecord(MsgType type, const uint8_t* a, size_t an, const uint8_t* b, size_t bn);
  bool drainControl();  // sends all queued control; false on socket failure
  bool sendVideoTcp(const EncodedFrame& f, uint32_t seq);
  bool sendVideoUdp(const EncodedFrame& f, uint32_t seq);
  bool flushBatch();
  void sendFec(const UdpHeader& base, const uint8_t* au, size_t len, size_t maxPayload);
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
  bool okPending_ = false;                // an Ok with startSealing is queued
  std::unique_ptr<TxCrypto> pendingTx_;   // keys not yet adopted by the sender thread
  bool keysSet_ = false;                  // setSessionKeys() happened (never again)
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
  std::unique_ptr<TxCrypto> tx_;  // adopted keys (null: plain session)
  bool tcpSealed_ = false;        // the Ok went out: TCP records are sealed
  std::vector<uint8_t> sealBuf_;  // one sealed TCP record (capacity reused)
  uint64_t frameSealNs_ = 0;      // AEAD time spent on the current video frame
};

}  // namespace owc::net
