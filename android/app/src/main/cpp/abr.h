// Adaptive bitrate: GCC-inspired loss/delay controller with AIMD
// (docs/LOW_LATENCY_RESEARCH.md §3.5). Runs on the worker thread only.
//
// Inputs: ReceiverReport deltas (≈4 Hz from the PC) and the phone's own send
// queue (frames queued, overflow events, TCP unsent bytes). Bitrate param is
// the ceiling, kMinBps the floor. Constants (starting points, to calibrate):
//   start            0.7 x ceiling
//   loss low/high    2 % / 10 % (per-fragment, EWMA alpha 0.4)
//   delay gradient   overuse > +6 ms; 2 consecutive overuse reports are required
//                    before acting (Wi-Fi power-save jitter). Underuse is not used.
//   overuse also     phone send queue >= 2 frames, or PC decode_ms > 0.8 frame
//   decrease         loss: x(1 - 0.5*min(loss,0.3)) of min(cur, recv), capped 0.85*recv
//                    overuse: 0.85 x max(recv, 0.5*cur)
//                    local queue overflow: x0.8 (no RR needed)
//                    IDR storm (5 PC keyframe requests / 10 s): x0.7
//   hold             no increase for 1500 ms after a decrease; at most 1 increase/s
//   increase         +8 % while below 90 % of the rate where congestion last
//                    happened (or never congested), else +50 kbps (additive)
//   FEC auto         pct = clamp(10 + 150*loss, 10, 50); 10 after 10 s of loss < 0.2 %
#pragma once

#include <cstdint>

#include "owc/protocol.h"

namespace owc {

class Abr {
 public:
  static constexpr int kMinBps = 500000;

  void reset(int ceilingBps, int64_t nowMs);
  void setCeiling(int ceilingBps);
  // Each returns true if the target changed.
  bool onReceiverReport(const ReceiverReport& rr, uint32_t queuedFrames, int fps, int64_t nowMs);
  bool onLocalCongestion(int64_t nowMs);  // send-queue overflow / TCP backlog
  bool onKeyframeStorm(int64_t nowMs);
  int target() const { return cur_; }
  int autoFecPercent() const { return fecPct_; }

 private:
  bool decrease(double t, int64_t now);
  bool increase(double t, int64_t now);

  int ceiling_ = 8000000;
  int cur_ = 5600000;
  double lossEwma_ = 0;
  int overuseStreak_ = 0;
  int64_t lastDec_ = 0, lastInc_ = 0, lowLossSince_ = 0;
  double congestedAt_ = 0;  // bitrate when congestion last happened (0 = never)
  bool havePrev_ = false;
  ReceiverReport prev_{};
  int fecPct_ = 10;
};

}  // namespace owc
