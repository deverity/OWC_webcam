// Clocks.
//
// WIRE CLOCK: every phone timestamp on the wire (VideoFrameHeader/UdpHeader
// capture_ts_us, Pong.phone_us) is CLOCK_MONOTONIC in microseconds, as
// protocol.h states. Camera sensor timestamps arrive in the clock domain given
// by ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE:
//   REALTIME -> CLOCK_BOOTTIME base: converted to MONOTONIC by subtracting the
//               current (BOOTTIME - MONOTONIC) offset (only changes across
//               suspend, which cannot happen while streaming: wakelock held).
//   UNKNOWN  -> in practice CLOCK_MONOTONIC on Qualcomm/Exynos/MTK; used as-is,
//               but sanity-checked against "now" (see VideoEncoder) and replaced
//               by the encoder output time if it is implausible.
// So capture_ts_us and Pong.phone_us always share one clock.
#pragma once

#include <cstdint>
#include <ctime>

namespace owc {

inline int64_t clockUs(clockid_t c) {
  timespec ts{};
  clock_gettime(c, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

inline int64_t monoUs() { return clockUs(CLOCK_MONOTONIC); }
inline int64_t monoMs() { return monoUs() / 1000; }

// BOOTTIME - MONOTONIC, sampled as tightly as possible.
inline int64_t bootMinusMonoUs() {
  int64_t m0 = clockUs(CLOCK_MONOTONIC);
  int64_t b = clockUs(CLOCK_BOOTTIME);
  int64_t m1 = clockUs(CLOCK_MONOTONIC);
  return b - (m0 + m1) / 2;
}

}  // namespace owc
