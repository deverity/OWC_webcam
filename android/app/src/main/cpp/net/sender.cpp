#include "net/sender.h"

#include <errno.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>

#include <algorithm>
#include <cstring>

#include "util/clock.h"
#include "util/log.h"
#include "util/thread.h"

namespace owc::net {
namespace {
constexpr int kStallMs = 10000;            // TCP: no progress for this long => dead link
constexpr size_t kMaxCtrlQueue = 512;      // Log messages are dropped beyond this
constexpr int kBatchMax = 40;              // datagrams per sendmmsg (~55 KB)
constexpr double kBurstBytes = 64 * 1024;  // token bucket depth
constexpr int kMinPacingBps = 20000000;    // pacing rate = max(20 Mbps, 4 x bitrate)
constexpr uint32_t kMinOutqThreshold = 16 * 1024;

}  // namespace

// Per-sender batch storage (one sender thread uses it).
struct BatchStore {
  uint8_t data[kBatchMax][kUdpMaxDatagram];
  size_t len[kBatchMax];
};

Sender::Sender(std::shared_ptr<Socket> tcp, int udpFd) : tcp_(std::move(tcp)), udpFd_(udpFd) {}

Sender::~Sender() { stop(); }

void Sender::start() { thread_ = std::thread(&Sender::loop, this); }

void Sender::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
    videoEnabled_ = false;
  }
  abort_ = true;
  cv_.notify_all();
  spaceCv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Sender::pushControl(MsgType type, std::vector<uint8_t> payload) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (stop_ || dead_) return;
    if (ctrl_.size() >= kMaxCtrlQueue && type == MsgType::Log) return;
    ctrl_.push_back(Ctrl{type, std::move(payload)});
  }
  cv_.notify_one();
}

void Sender::pushLog(LogLevel level, const std::string& text) {
  LogMsg m;
  m.level = level;
  m.text = text;
  pushControl(MsgType::Log, encode(m));
}

void Sender::pushPong(uint64_t t0, int64_t rxMonoUs) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (stop_ || dead_) return;
    Ctrl c{MsgType::Pong, {}};
    c.pong = true;
    c.t0 = t0;
    c.rxUs = rxMonoUs;
    // Pongs jump ahead of other control messages (RTT accuracy).
    ctrl_.push_front(std::move(c));
  }
  cv_.notify_one();
}

bool Sender::pushVideo(FramePtr f) {
  bool needSync = false;
  {
    std::unique_lock<std::mutex> lk(mu_);
    if (!videoEnabled_ || stop_ || dead_) return false;
    if (mode_ == VideoMode::Pending) {
      // UDP requested but not punched yet: nothing to send to. Start with a
      // keyframe once the path exists.
      dropUntilKey_ = true;
      return false;
    }
    if (dropUntilKey_) {
      if (!f->key()) {
        ++stats_.dropped;
        return false;
      }
      dropUntilKey_ = false;
    }
    if (static_cast<int>(video_.size()) >= maxQueue_) {
      if (dropPolicy_ == 1) {
        spaceCv_.wait(lk, [&] {
          return static_cast<int>(video_.size()) < maxQueue_ || !videoEnabled_ || stop_ ||
                 dead_.load() || dropPolicy_ != 1;
        });
        if (!videoEnabled_ || stop_ || dead_) return false;
      }
      if (static_cast<int>(video_.size()) >= maxQueue_) {
        // Overflow: drop everything queued; keep the incoming frame only if it
        // is a keyframe, otherwise wait for one.
        ++stats_.overflows;
        stats_.dropped += static_cast<uint32_t>(video_.size());
        video_.clear();
        if (!f->key()) {
          ++stats_.dropped;
          dropUntilKey_ = true;
          needSync = true;
          lk.unlock();
          return needSync;
        }
      }
    }
    video_.push_back(std::move(f));
    stats_.queued = static_cast<uint32_t>(video_.size());
  }
  cv_.notify_one();
  return needSync;
}

void Sender::flushVideo(bool enable) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    video_.clear();
    stats_.queued = 0;
    videoEnabled_ = enable;
    dropUntilKey_ = false;
  }
  spaceCv_.notify_all();
}

void Sender::setQueuePolicy(int maxFrames, int dropPolicy) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    maxQueue_ = std::clamp(maxFrames, 1, 60);
    dropPolicy_ = dropPolicy == 1 ? 1 : 0;
  }
  spaceCv_.notify_all();
}

void Sender::setVideoMode(VideoMode m) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (m == mode_) return;
    mode_ = m;
    // Switching path: the receiver needs a fresh keyframe.
    video_.clear();
    dropUntilKey_ = true;
  }
  spaceCv_.notify_all();
}

VideoMode Sender::videoMode() {
  std::lock_guard<std::mutex> lk(mu_);
  return mode_;
}

void Sender::setUdpTarget(const sockaddr_in& a, uint32_t sessionId) {
  std::lock_guard<std::mutex> lk(mu_);
  udpTarget_ = a;
  hasUdpTarget_ = true;
  sessionId_ = sessionId;
}

void Sender::setPacingBitrate(int bps) {
  std::lock_guard<std::mutex> lk(mu_);
  pacingBps_ = std::max(kMinPacingBps, bps > 0 ? bps * 4 : 0);
}

void Sender::setFecPercent(int pct) {
  std::lock_guard<std::mutex> lk(mu_);
  fecPct_ = std::clamp(pct, 0, 100);
}

void Sender::closeAfterFlush() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    closeReq_ = true;
    video_.clear();
    videoEnabled_ = false;
  }
  cv_.notify_one();
  spaceCv_.notify_all();
}

Sender::Stats Sender::stats() {
  std::lock_guard<std::mutex> lk(mu_);
  Stats s = stats_;
  s.queued = static_cast<uint32_t>(video_.size());
  s.tcpOutq = outq();
  return s;
}

uint32_t Sender::outq() const {
  int n = 0;
  if (tcp_ && ioctl(tcp_->fd(), SIOCOUTQ, &n) == 0 && n > 0) return static_cast<uint32_t>(n);
  return 0;
}

void Sender::fail(const char* why) {
  if (!dead_.exchange(true)) {
    LOGW_LOCAL("sender: %s; closing connection", why);
    if (tcp_) tcp_->shutdownBoth();
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    video_.clear();
    ctrl_.clear();
  }
  spaceCv_.notify_all();
}

bool Sender::sendCtrl(Ctrl& c) {
  if (c.pong) {
    Pong p;
    p.t0_us = c.t0;
    const int64_t now = monoUs();
    p.phone_us = static_cast<uint64_t>((c.rxUs + now) / 2);
    c.payload = encode(p);
  }
  uint8_t hdr[kTcpHeaderSize];
  writeTcpHeader(hdr, c.type, static_cast<uint32_t>(c.payload.size()));
  iovec iov[2] = {{hdr, kTcpHeaderSize}, {c.payload.data(), c.payload.size()}};
  SendResult r = sendAll(tcp_->fd(), iov, 2, kStallMs, &abort_);
  return r == SendResult::Ok;
}

bool Sender::drainControl() {
  for (;;) {
    Ctrl c;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (ctrl_.empty() || stop_) return true;
      c = std::move(ctrl_.front());
      ctrl_.pop_front();
    }
    if (!sendCtrl(c)) return false;
  }
}

bool Sender::sendVideoTcp(const EncodedFrame& f, uint32_t seq) {
  VideoFrameHeader h;
  h.epoch = f.epoch;
  h.frame_seq = seq;
  h.capture_ts_us = static_cast<uint64_t>(f.captureUs);
  h.flags = f.flags;
  // Frame header only (au_len 0) through the protocol encoder, then the AU in
  // place: no per-frame copy of the access unit.
  encodeVideoFrame(h, nullptr, 0, tcpBuf_);
  uint8_t hdr[kTcpHeaderSize];
  writeTcpHeader(hdr, MsgType::VideoFrame, static_cast<uint32_t>(tcpBuf_.size() + f.au.size()));
  iovec iov[3] = {{hdr, kTcpHeaderSize},
                  {tcpBuf_.data(), tcpBuf_.size()},
                  {const_cast<uint8_t*>(f.au.data()), f.au.size()}};
  return sendAll(tcp_->fd(), iov, 3, kStallMs, &abort_) == SendResult::Ok;
}

namespace {
thread_local BatchStore* t_batch = nullptr;
}

bool Sender::flushBatch() {
  if (batchCount_ == 0) return true;
  BatchStore& b = *t_batch;
  // Token bucket pacing.
  size_t total = 0;
  for (int i = 0; i < batchCount_; ++i) total += b.len[i];
  const double rateBytesPerUs = batchRateBps_ / 8.0 / 1e6;
  int64_t now = monoUs();
  tokens_ = std::min(kBurstBytes, tokens_ + (now - tokensAtUs_) * rateBytesPerUs);
  tokensAtUs_ = now;
  if (tokens_ < static_cast<double>(total)) {
    const int64_t waitUs = static_cast<int64_t>((total - tokens_) / rateBytesPerUs);
    if (waitUs > 0) {
      timespec ts{static_cast<time_t>(waitUs / 1000000), static_cast<long>((waitUs % 1000000) * 1000)};
      nanosleep(&ts, nullptr);
    }
    now = monoUs();
    tokens_ = std::min(kBurstBytes, tokens_ + (now - tokensAtUs_) * rateBytesPerUs);
    tokensAtUs_ = now;
  }
  tokens_ -= static_cast<double>(total);

  mmsghdr msgs[kBatchMax];
  iovec iovs[kBatchMax];
  std::memset(msgs, 0, sizeof(msgs));
  for (int i = 0; i < batchCount_; ++i) {
    iovs[i].iov_base = b.data[i];
    iovs[i].iov_len = b.len[i];
    msgs[i].msg_hdr.msg_name = &batchTarget_;
    msgs[i].msg_hdr.msg_namelen = sizeof(batchTarget_);
    msgs[i].msg_hdr.msg_iov = &iovs[i];
    msgs[i].msg_hdr.msg_iovlen = 1;
  }
  int sent = 0;
  while (sent < batchCount_) {
    int r = sendmmsg(udpFd_, msgs + sent, static_cast<unsigned>(batchCount_ - sent), 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      // ENOBUFS/ENETUNREACH etc.: UDP is lossy anyway; drop the rest of the batch.
      break;
    }
    sent += r;
  }
  batchCount_ = 0;
  // Keep control latency low while pacing a big frame.
  return drainControl();
}

bool Sender::sendVideoUdp(const EncodedFrame& f, uint32_t seq) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!hasUdpTarget_) return true;
    batchTarget_ = udpTarget_;
    batchRateBps_ = pacingBps_;
  }
  UdpHeader base;
  base.type = UdpType::Video;
  base.session_id = sessionId_;
  base.frame_seq = seq;
  base.capture_ts_us = static_cast<uint64_t>(f.captureUs);
  base.epoch = static_cast<uint16_t>(f.epoch & 0xFFFFu);
  base.flags = f.flags;
  bool ok = true;
  packetize(base, f.au.data(), f.au.size(), [&](const uint8_t* d, size_t n) {
    if (!ok) return;
    BatchStore& b = *t_batch;
    std::memcpy(b.data[batchCount_], d, n);
    b.len[batchCount_] = n;
    if (++batchCount_ == kBatchMax) ok = flushBatch();
  });
  if (ok) ok = flushBatch();
  if (ok) sendFec(base, f.au.data(), f.au.size());
  return ok;  // false only when a control message could not be written (TCP dead)
}

void Sender::sendFec(const UdpHeader& base, const uint8_t* au, size_t len) {
  // Reed-Solomon parity datagrams (UdpType::Fec) AFTER the data fragments of
  // the frame, through the same batch/pacing path.
  int pct;
  {
    std::lock_guard<std::mutex> lk(mu_);
    pct = fecPct_;
  }
  if (pct <= 0 || len == 0) return;
  const size_t frags = (len + kUdpMaxPayload - 1) / kUdpMaxPayload;
  if (frags > 0xFFFFu) return;
  const uint8_t m = fecParityCount(static_cast<uint16_t>(frags), pct);
  if (m == 0) return;
  bool ok = true;
  packetizeFec(base, au, len, m, [&](const uint8_t* d, size_t n) {
    if (!ok) return;
    BatchStore& b = *t_batch;
    std::memcpy(b.data[batchCount_], d, n);
    b.len[batchCount_] = n;
    if (++batchCount_ == kBatchMax) ok = flushBatch();
  });
  if (ok) flushBatch();
}

void Sender::loop() {
  setThreadName("owc-send");
  raiseThreadPriority(-10);
  BatchStore batch;  // ~56 KB on this thread's stack
  t_batch = &batch;
  tokensAtUs_ = monoUs();
  tokens_ = kBurstBytes;
  uint64_t avgFrame = 0;

  for (;;) {
    FramePtr frame;
    VideoMode mode;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return stop_ || closeReq_ || !ctrl_.empty() || !video_.empty(); });
      if (stop_) break;
      if (!ctrl_.empty()) {
        lk.unlock();
        if (!drainControl()) {
          fail("control send failed");
          break;
        }
        continue;
      }
      if (closeReq_) {
        lk.unlock();
        if (tcp_) tcp_->shutdownBoth();
        dead_ = true;
        break;
      }
      mode = mode_;
      if (mode == VideoMode::Tcp) {
        // Keep in-kernel backlog to ~1.5 frames; the rest waits in our queue.
        const uint32_t thr = std::max<uint32_t>(kMinOutqThreshold,
                                                static_cast<uint32_t>(avgFrame * 3 / 2));
        if (outq() > thr) {
          cv_.wait_for(lk, std::chrono::milliseconds(2));
          continue;
        }
      }
      frame = std::move(video_.front());
      video_.pop_front();
      stats_.queued = static_cast<uint32_t>(video_.size());
    }
    spaceCv_.notify_all();

    const uint32_t seq = ++seq_;
    bool ok = mode == VideoMode::Udp ? sendVideoUdp(*frame, seq) : sendVideoTcp(*frame, seq);
    if (!ok) {
      if (!abort_) fail("video send failed");
      break;
    }
    const uint64_t sz = frame->au.size();
    const int64_t lat = monoUs() - frame->captureUs;
    avgFrame = avgFrame == 0 ? sz : (avgFrame * 7 + sz) / 8;
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.framesSent;
    stats_.bytesSent += sz;
    stats_.avgFrameBytes = static_cast<uint32_t>(avgFrame);
    if (lat >= 0 && lat < 5000000)
      stats_.sentLatencyUs = stats_.sentLatencyUs == 0 ? lat : stats_.sentLatencyUs + (lat - stats_.sentLatencyUs) / 16;
  }
  t_batch = nullptr;
  dead_ = true;
  spaceCv_.notify_all();
}

}  // namespace owc::net
