#include "abr.h"

#include <algorithm>
#include <cmath>

namespace owc {
namespace {
constexpr double kLossLow = 0.02, kLossHigh = 0.10, kLossCorroborate = 0.005, kLossFrames = 0.01;
constexpr float kGradOveruse = 6.0f;
constexpr int64_t kMinDecreaseGapMs = 500;
constexpr int64_t kHoldAfterDecreaseMs = 1000;
constexpr int64_t kIncreasePeriodMs = 500;
constexpr int64_t kForgetCongestionMs = 10000;
constexpr int64_t kReportTimeoutMs = 1500;
constexpr double kAppLimitedRatio = 0.7;
}  // namespace

void Abr::reset(int ceilingBps, int64_t nowMs) {
  ceiling_ = std::max(kMinBps, ceilingBps);
  cur_ = std::max(kMinBps, static_cast<int>(ceiling_ * 0.7));
  lossEwma_ = 0;
  overuseStreak_ = 0;
  queueStreak_ = 0;
  lastDec_ = 0;
  lastInc_ = nowMs;
  lowLossSince_ = nowMs;
  lastReport_ = 0;
  congestedAt_ = 0;
  havePrev_ = false;
  fecPct_ = 10;
}

void Abr::setCeiling(int c) {
  ceiling_ = std::max(kMinBps, c);
  cur_ = std::min(cur_, ceiling_);
}

bool Abr::appLimited(const Local& l) const {
  return l.encoderBps > 0 && l.encoderBps < kAppLimitedRatio * cur_;
}

bool Abr::decrease(double t, double floor, int64_t now) {
  if (now - lastDec_ < kMinDecreaseGapMs) return false;
  const int lo = std::max(kMinBps, static_cast<int>(floor));
  int n = std::clamp(static_cast<int>(t), std::min(lo, cur_), ceiling_);
  if (n >= cur_) return false;
  congestedAt_ = cur_;
  lastDec_ = now;
  cur_ = n;
  return true;
}

bool Abr::maybeIncrease(bool appLim, int64_t now) {
  if (cur_ >= ceiling_ || now - lastDec_ < kHoldAfterDecreaseMs || now - lastInc_ < kIncreasePeriodMs)
    return false;
  if (congestedAt_ > 0 && now - lastDec_ > kForgetCongestionMs) congestedAt_ = 0;
  const bool far = appLim || congestedAt_ <= 0 || cur_ < 0.9 * congestedAt_;
  const double step = far ? cur_ * 0.25 : std::max(50000.0, 0.03 * ceiling_);
  lastInc_ = now;
  const int n = std::min(ceiling_, static_cast<int>(cur_ + step));
  if (n == cur_) return false;
  cur_ = n;
  return true;
}

bool Abr::onLocalCongestion(int64_t now) { return decrease(cur_ * 0.8, kMinBps, now); }

bool Abr::onKeyframeStorm(int64_t now) {
  lastDec_ = 0;  // bypass the gap: this is a strong signal
  return decrease(cur_ * 0.7, kMinBps, now);
}

bool Abr::tick(const Local& local, int64_t now) {
  // Only drives increases when the PC sends no ReceiverReports.
  if (lastReport_ != 0 && now - lastReport_ < kReportTimeoutMs) return false;
  if (local.queued > local.queueAllowance) return false;
  return maybeIncrease(appLimited(local), now);
}

bool Abr::onReceiverReport(const ReceiverReport& rr, const Local& local, int64_t now) {
  lastReport_ = now;
  if (!havePrev_) {
    prev_ = rr;
    havePrev_ = true;
    return false;
  }
  const double dfl = static_cast<double>(rr.fragments_lost - prev_.fragments_lost);
  const double dfr = static_cast<double>(rr.fragments_received - prev_.fragments_received);
  uint32_t framesLost = rr.frames_lost - prev_.frames_lost;
  prev_ = rr;
  double loss = (dfl + dfr) > 0 ? dfl / std::max(1.0, dfl + dfr) : 0.0;
  if (loss < 0 || loss > 1) loss = 0;          // counters reset on the PC
  if (framesLost > 100000) framesLost = 0;     // idem
  lossEwma_ = lossEwma_ * 0.6 + loss * 0.4;

  // FEC auto
  fecPct_ = std::clamp(static_cast<int>(std::lround(10 + 150 * lossEwma_)), 10, 50);
  if (lossEwma_ >= 0.002) lowLossSince_ = now;
  if (now - lowLossSince_ > 10000) fecPct_ = 10;

  const bool appLim = appLimited(local);
  const double recv = rr.recv_bitrate_bps;
  const double base = (appLim || recv <= 0) ? cur_ : std::min<double>(cur_, recv);
  const bool queueGrowing = local.queued > local.queueAllowance;
  queueStreak_ = queueGrowing ? queueStreak_ + 1 : 0;
  overuseStreak_ = rr.delay_gradient_ms > kGradOveruse ? overuseStreak_ + 1 : 0;
  const double delayFloor = kDelayFloorFraction * ceiling_;

  const bool realLoss = lossEwma_ > kLossHigh || framesLost >= 2 || (framesLost > 0 && lossEwma_ > kLossFrames);
  if (realLoss) return decrease(base * (1 - 0.5 * std::min(lossEwma_, 0.3)), kMinBps, now);

  const bool corroborated = queueGrowing || lossEwma_ > kLossCorroborate;
  if (overuseStreak_ >= 2 && corroborated) return decrease(0.85 * base, delayFloor, now);
  if (queueStreak_ >= 2) return decrease(0.9 * cur_, delayFloor, now);

  if (lossEwma_ < kLossLow && !queueGrowing && overuseStreak_ == 0) return maybeIncrease(appLim, now);
  return false;
}

}  // namespace owc
