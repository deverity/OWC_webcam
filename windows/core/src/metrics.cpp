#include "metrics.h"

#include <algorithm>
#include <cmath>

namespace owc {
namespace detail {

bool ClockSync::addSample(int64_t t0, int64_t phone, int64_t t1) {
  if (t1 < t0) return false;
  Sample s;
  s.rtt = t1 - t0;
  // (t0 + t1) / 2 without overflow
  s.offset = phone - (t0 + (t1 - t0) / 2);
  last_rtt_ = s.rtt;
  samples_.push_back(s);
  while (samples_.size() > window_) samples_.pop_front();
  const Sample* best = &samples_.front();
  for (const Sample& x : samples_)
    if (x.rtt < best->rtt) best = &x;
  best_offset_ = best->offset;
  best_rtt_ = best->rtt;
  return true;
}

void ClockSync::reset() {
  samples_.clear();
  best_offset_ = best_rtt_ = last_rtt_ = 0;
}

void JitterEstimator::add(int64_t send_us, int64_t arrival_us) {
  const int64_t transit = arrival_us - send_us;
  if (have_) {
    const double d = std::fabs(static_cast<double>(transit - last_transit_));
    jitter_ += (d - jitter_) / 16.0;
  }
  have_ = true;
  last_transit_ = transit;
}

void DelayTrend::add(int64_t send_us, int64_t arrival_us) {
  pts_.emplace_back(arrival_us, arrival_us - send_us);
  while (pts_.size() > window_) pts_.pop_front();
}

double DelayTrend::gradientMs() const {
  const size_t n = pts_.size();
  if (n < 3) return 0;
  const double x0 = static_cast<double>(pts_.front().first);
  const double y0 = static_cast<double>(pts_.front().second);
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (const auto& p : pts_) {
    const double x = static_cast<double>(p.first) - x0;
    const double y = static_cast<double>(p.second) - y0;
    sx += x; sy += y; sxx += x * x; sxy += x * y;
  }
  const double dn = static_cast<double>(n);
  const double den = dn * sxx - sx * sx;
  if (den <= 0) return 0;
  const double slope = (dn * sxy - sx * sy) / den;  // us per us
  const double span = static_cast<double>(pts_.back().first - pts_.front().first);
  return slope * span / 1000.0;
}

int64_t KeyframePolicy::rateLimitUs(int64_t min_interval_us, int64_t rtt_us,
                                    int64_t frame_interval_us) const {
  return std::max(min_interval_us, 2 * rtt_us + 2 * frame_interval_us);
}

bool KeyframePolicy::poll(int64_t now, bool needed, int64_t min_interval_us, int64_t rtt_us,
                          int64_t frame_interval_us) {
  if (!needed) {
    episode_ = false;
    return false;
  }
  const int64_t limit = rateLimitUs(min_interval_us, rtt_us, frame_interval_us);
  const int64_t since = now - last_;
  bool send;
  if (!episode_)
    send = !have_ || since >= limit;
  else
    send = since >= std::max<int64_t>(500000, limit);
  if (send) {
    episode_ = true;
    have_ = true;
    last_ = now;
  }
  return send;
}

void RateMeter::add(int64_t now_us, double amount) {
  rate(now_us);
  acc_ += amount;
}

double RateMeter::rate(int64_t now_us) {
  if (win_start_ == 0) {
    win_start_ = now_us;
    return 0;
  }
  const int64_t dt = now_us - win_start_;
  if (dt >= 1000000) {
    // Long gaps without events average down (rate decays towards 0).
    rate_ = acc_ * 1e6 / static_cast<double>(dt);
    acc_ = 0;
    win_start_ = now_us;
  }
  return rate_;
}

void SampleWindow::add(double v) {
  if (cap_ == 0) return;
  if (v_.size() < cap_) {
    v_.push_back(v);
  } else {
    v_[pos_] = v;
    pos_ = (pos_ + 1) % cap_;
  }
}

double SampleWindow::avg() const {
  if (v_.empty()) return 0;
  double s = 0;
  for (double x : v_) s += x;
  return s / static_cast<double>(v_.size());
}

double SampleWindow::percentile(double p) const {
  if (v_.empty()) return 0;
  std::vector<double> c(v_);
  std::sort(c.begin(), c.end());
  double idx = p / 100.0 * static_cast<double>(c.size() - 1);
  size_t i = static_cast<size_t>(idx + 0.5);
  if (i >= c.size()) i = c.size() - 1;
  return c[i];
}

double SampleWindow::max() const {
  double m = 0;
  for (double x : v_) m = std::max(m, x);
  return m;
}

}  // namespace detail
}  // namespace owc
