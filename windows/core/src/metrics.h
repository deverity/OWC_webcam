// Measurement building blocks (pure, single-threaded, unit tested).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace owc {
namespace detail {

// Phone<->PC clock offset from Ping/Pong. For each exchange:
//   t0 = PC send time, t1 = PC receive time, p = phone time in the Pong.
//   rtt = t1 - t0, offset = p - (t0 + t1) / 2   (phone = pc + offset)
// Assumes symmetric paths; the sample with the smallest RTT in a sliding
// window has the least queueing asymmetry, so its offset is used.
class ClockSync {
 public:
  explicit ClockSync(size_t window = 16) : window_(window ? window : 1) {}
  // Returns false (sample ignored) if t1 < t0.
  bool addSample(int64_t t0_us, int64_t phone_us, int64_t t1_us);
  bool synced() const { return !samples_.empty(); }
  int64_t offsetUs() const { return best_offset_; }
  int64_t lastRttUs() const { return last_rtt_; }
  int64_t minRttUs() const { return best_rtt_; }
  // phone time -> PC time (us). Requires synced().
  int64_t phoneToPc(int64_t phone_us) const { return phone_us - best_offset_; }
  void reset();

 private:
  struct Sample { int64_t rtt, offset; };
  size_t window_;
  std::deque<Sample> samples_;
  int64_t best_offset_ = 0, best_rtt_ = 0, last_rtt_ = 0;
};

// RFC 3550 interarrival jitter on (capture timestamp, arrival time) pairs:
//   D(i-1,i) = (Ri - Ri-1) - (Si - Si-1);  J += (|D| - J) / 16
class JitterEstimator {
 public:
  void add(int64_t send_us, int64_t arrival_us);
  double jitterUs() const { return jitter_; }
  void reset() { have_ = false; jitter_ = 0; }

 private:
  bool have_ = false;
  int64_t last_transit_ = 0;
  double jitter_ = 0;
};

// Delay gradient (research sec. 3.5): least-squares slope of the one-way delay
// d_i = arrival_i - capture_i (any constant clock offset cancels out) versus
// arrival time over the last `window` frames, expressed as the delay change
// in ms across the fitted window span. >0 = queues building up.
class DelayTrend {
 public:
  explicit DelayTrend(size_t window = 20) : window_(window < 2 ? 2 : window) {}
  void add(int64_t send_us, int64_t arrival_us);
  double gradientMs() const;  // 0 until 3 samples
  void reset() { pts_.clear(); }

 private:
  size_t window_;
  std::deque<std::pair<int64_t, int64_t>> pts_;  // (arrival_us, delay_us)
};

// RequestKeyframe pacing (research sec. 3.4.2):
//  - first request of a loss episode is sent immediately, unless the last
//    request is younger than the rate limit;
//  - rate limit = max(min_interval, 2*RTT + 2 frame intervals);
//  - while still needed, re-send every max(500 ms, rate limit).
class KeyframePolicy {
 public:
  // Returns true if a RequestKeyframe should be sent now (and records it).
  bool poll(int64_t now_us, bool needed, int64_t min_interval_us, int64_t rtt_us,
            int64_t frame_interval_us);
  // Records an externally sent request (e.g. user button).
  void noteSent(int64_t now_us) { last_ = now_us; have_ = true; }
  int64_t rateLimitUs(int64_t min_interval_us, int64_t rtt_us, int64_t frame_interval_us) const;

 private:
  bool have_ = false;
  bool episode_ = false;
  int64_t last_ = 0;
};

// Event-rate meter over a ~1 s window, value cached when the window closes.
class RateMeter {
 public:
  void add(int64_t now_us, double amount = 1.0);
  // Rate (amount per second) over the last closed window; updates on add/poll.
  double rate(int64_t now_us);

 private:
  int64_t win_start_ = 0;
  double acc_ = 0, rate_ = 0;
};

// Keeps the last N samples; avg and percentile.
class SampleWindow {
 public:
  explicit SampleWindow(size_t cap = 120) : cap_(cap) {}
  void add(double v);
  double avg() const;
  double percentile(double p) const;  // p in 0..100
  double max() const;
  size_t size() const { return v_.size(); }
  void reset() { v_.clear(); pos_ = 0; }

 private:
  size_t cap_;
  std::vector<double> v_;
  size_t pos_ = 0;
};

}  // namespace detail
}  // namespace owc
