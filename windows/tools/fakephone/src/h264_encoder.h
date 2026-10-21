// Media Foundation H.264 encoder (Microsoft software MFT) for owc_fakephone.
// Single-threaded: use on one thread that called MfScope.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace owc {
namespace fake {

class MfScope {
 public:
  MfScope();
  ~MfScope();
  bool ok() const { return ok_; }

 private:
  bool com_ = false, ok_ = false;
};

struct EncoderConfig {
  uint32_t width = 1280, height = 720, fps = 30;
  uint32_t bitrate = 4000000;
  uint32_t bframes = 0;       // >0 requires profile >= 1 (Main); disables low-latency mode
  int profile = 0;            // 0 Baseline, 1 Main, 2 High
  uint32_t gop_frames = 60;   // keyframe period in frames (0 = only forced)
};

class H264Encoder {
 public:
  // au: Annex-B access unit; pts: 100 ns units as given to encode(); key: IDR.
  using Sink = std::function<void(const uint8_t* au, size_t n, int64_t pts, bool key)>;
  H264Encoder();
  ~H264Encoder();
  bool open(const EncoderConfig& cfg, std::string& err);
  void close();
  // nv12: tightly packed width*height*3/2.
  bool encode(const uint8_t* nv12, int64_t pts, int64_t duration, bool forceKey, const Sink& sink);
  void setBitrate(uint32_t bps);
  const EncoderConfig& config() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fake
}  // namespace owc
