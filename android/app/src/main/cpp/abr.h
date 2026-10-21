// Adaptive bitrate: GCC-inspired loss/delay controller with AIMD
// (docs/LOW_LATENCY_RESEARCH.md §3.5). Worker thread only.
//
// Inputs: ReceiverReport deltas (≈4 Hz from the PC), the phone's send queue
// (frames queued vs. the allowance for the current mode, overflow events,
// TCP backlog) and the measured ENCODER OUTPUT rate. Bitrate param = ceiling.
//
// Rules (v2, fixes the zero-loss downward spiral seen with a VBR encoder on a
// static scene + Wi-Fi power-save jitter):
//   app-limited      encoder output < 70 % of the target: the receive rate says
//                    nothing about the path, so every decrease is relative to
//                    the TARGET, never to recv_bitrate.
//   loss             loss EWMA > 10 %, or post-FEC frame loss with loss EWMA > 1 %
//                    (or >= 2 frames lost in one report):
//                    target = base x (1 - 0.5 x min(loss, 0.3)), floor 500 kbps.
//                    base = target if app-limited, else min(target, recv).
//   delay            delay_gradient > +6 ms in >= 2 consecutive reports AND
//                    corroborated by send-queue growth or loss EWMA > 0.5 %:
//                    target = 0.85 x base, floor 35 % of the ceiling.
//   queue only       queue above the allowance in >= 2 consecutive reports:
//                    target = 0.9 x target, floor 35 % of the ceiling.
//   local overflow   (no report needed) queue overflow drops / TCP backlog:
//                    x0.8, floor 500 kbps. IDR storm: x0.7, floor 500 kbps.
//   pacing           at most one decrease per 500 ms.
//   increase         clean (loss < 2 %, queue within allowance, no corroborated
//                    overuse), >= 1 s after a decrease, every 500 ms:
//                    x1.25 when far from the last congestion point (or it is
//                    older than 10 s, or app-limited), else +3 % of the ceiling.
//                    35 % -> 100 % takes ~3.5 s.
//   no reports       (e.g. TCP client without ReceiverReports, > 1.5 s): tick()
//                    increases on a clean local queue the same way.
//   FEC auto         pct = clamp(10 + 150 x loss, 10, 50); 10 after 10 s of loss < 0.2 %.
#pragma once

#include <cstdint>

#include "owc/protocol.h"

namespace owc {

class Abr {
 public:
  static constexpr int kMinBps = 500000;
  static constexpr double kDelayFloorFraction = 0.35;

  struct Local {
    uint32_t queued = 0;          // frames waiting in the sender
    uint32_t queueAllowance = 1;  // more than this = queue growing (bursty modes allow more)
    uint32_t encoderBps = 0;      // measured encoder output (EWMA), 0 = unknown
  };

  void reset(int ceilingBps, int64_t nowMs);
  void setCeiling(int ceilingBps);
  // Each returns true if the target changed.
  bool onReceiverReport(const ReceiverReport& rr, const Local& local, int64_t nowMs);
  bool tick(const Local& local, int64_t nowMs);  // ~1 Hz
  bool onLocalCongestion(int64_t nowMs);  // send-queue overflow / TCP backlog
  bool onKeyframeStorm(int64_t nowMs);
  int target() const { return cur_; }
  int autoFecPercent() const { return fecPct_; }

 private:
  bool decrease(double t, double floor, int64_t now);
  bool maybeIncrease(bool appLimited, int64_t now);
  bool appLimited(const Local& l) const;

  int ceiling_ = 8000000;
  int cur_ = 5600000;
  double lossEwma_ = 0;
  int overuseStreak_ = 0;
  int queueStreak_ = 0;
  int64_t lastDec_ = 0, lastInc_ = 0, lowLossSince_ = 0, lastReport_ = 0;
  double congestedAt_ = 0;  // target when congestion last happened (0 = none / forgotten)
  bool havePrev_ = false;
  ReceiverReport prev_{};
  int fecPct_ = 10;
};

}  // namespace owc
