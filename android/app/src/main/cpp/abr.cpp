#include "abr.h"

#include <algorithm>
#include <cmath>

namespace owc {
namespace {
constexpr double kLossLow = 0.02, kLossHigh = 0.10;
constexpr float kGradOveruse = 6.0f;  // underuse (-3 ms) is not acted upon yet
constexpr int64_t kHoldAfterDecreaseMs = 1500, kIncreasePeriodMs = 1000;
constexpr int64_t kMinDecreaseGapMs = 250;  // one decrease per report period at most
}  // namespace

void Abr::reset(int ceilingBps, int64_t nowMs) {
  ceiling_ = std::max(kMinBps, ceilingBps);
  cur_ = std::max(kMinBps, static_cast<int>(ceiling_ * 0.7));
  lossEwma_ = 0;
  overuseStreak_ = 0;
  lastDec_ = 0;
  lastInc_ = nowMs;
  lowLossSince_ = nowMs;
  congestedAt_ = 0;
  havePrev_ = false;
  fecPct_ = 10;
}

void Abr::setCeiling(int c) {
  ceiling_ = std::max(kMinBps, c);
  cur_ = std::min(cur_, ceiling_);
}

bool Abr::decrease(double t, int64_t now) {
  if (now - lastDec_ < kMinDecreaseGapMs) return false;
  congestedAt_ = cur_;
  int n = std::clamp(static_cast<int>(t), kMinBps, ceiling_);
  lastDec_ = now;
  if (n >= cur_) return false;
  cur_ = n;
  return true;
}

bool Abr::increase(double t, int64_t now) {
  int n = std::clamp(static_cast<int>(t), kMinBps, ceiling_);
  lastInc_ = now;
  if (n == cur_) return false;
  cur_ = n;
  return true;
}

bool Abr::onLocalCongestion(int64_t now) { return decrease(cur_ * 0.8, now); }

bool Abr::onKeyframeStorm(int64_t now) {
  lastDec_ = 0;  // bypass the gap: this is a strong signal
  return decrease(cur_ * 0.7, now);
}

bool Abr::onReceiverReport(const ReceiverReport& rr, uint32_t queued, int fps, int64_t now) {
  if (!havePrev_) {
    prev_ = rr;
    havePrev_ = true;
    return false;
  }
  const double dfl = static_cast<double>(rr.fragments_lost - prev_.fragments_lost);
  const double dfr = static_cast<double>(rr.fragments_received - prev_.fragments_received);
  const uint32_t framesLost = rr.frames_lost - prev_.frames_lost;
  prev_ = rr;
  double loss = (dfl + dfr) > 0 ? dfl / std::max(1.0, dfl + dfr) : 0.0;
  if (loss < 0 || loss > 1) loss = 0;  // counters reset on the PC
  lossEwma_ = lossEwma_ * 0.6 + loss * 0.4;

  const double frameMs = 1000.0 / std::max(1, fps);
  bool overuse = rr.delay_gradient_ms > kGradOveruse || queued >= 2 ||
                 rr.decode_ms > 0.8 * frameMs;
  overuseStreak_ = overuse ? overuseStreak_ + 1 : 0;
  const double recv = rr.recv_bitrate_bps;

  // FEC auto
  int pct = static_cast<int>(std::lround(10 + 150 * lossEwma_));
  fecPct_ = std::clamp(pct, 10, 50);
  if (lossEwma_ >= 0.002) lowLossSince_ = now;
  if (now - lowLossSince_ > 10000) fecPct_ = 10;

  if ((framesLost > 0 && framesLost < 1000000) || lossEwma_ > kLossHigh) {
    double t = std::min<double>(cur_, recv > 0 ? recv : cur_) * (1 - 0.5 * std::min(lossEwma_, 0.3));
    if (recv > 0) t = std::min(t, 0.85 * recv);
    return decrease(t, now);
  }
  if (overuseStreak_ >= 2) {
    return decrease(0.85 * std::max(recv, 0.5 * cur_), now);
  }
  if (lossEwma_ < kLossLow && !overuse && now - lastDec_ > kHoldAfterDecreaseMs &&
      now - lastInc_ > kIncreasePeriodMs && cur_ < ceiling_) {
    const bool far = congestedAt_ <= 0 || cur_ < 0.9 * congestedAt_;
    double step = far ? cur_ * 0.08 : 50000.0;
    return increase(cur_ + step, now);
  }
  return false;
}

}  // namespace owc
