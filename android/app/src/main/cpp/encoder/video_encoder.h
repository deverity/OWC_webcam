// AMediaCodec surface encoder with a dedicated high-priority output thread.
//
// Threading: configure/start/stop/setBitrate/requestKeyframe are called from
// the worker thread; the output thread only touches the codec through
// dequeue/release and the Sink. setParameters calls are serialized with
// stop() by ctlMu_ (MediaCodec itself is thread-safe for these calls).
#pragma once

#include <media/NdkMediaCodec.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "encoder/encoder_options.h"
#include "owc/protocol.h"

struct ANativeWindow;

namespace owc {

struct EncodedFrame {
  uint32_t epoch = 0;
  int64_t captureUs = 0;   // CLOCK_MONOTONIC
  uint8_t flags = 0;       // FrameFlags
  std::vector<uint8_t> au; // Annex-B access unit
  bool key() const { return (flags & kFrameKey) != 0; }
};
using FramePtr = std::shared_ptr<EncodedFrame>;

class VideoEncoder {
 public:
  class Sink {
   public:
    virtual ~Sink() = default;
    // Called on the output thread when codec config (SPS/PPS/VPS) is known.
    virtual void onVideoFormat(const VideoFormat& fmt) = 0;
    // Called on the output thread. Return true to ask for a sync frame
    // (the send queue overflowed). May block (LatencyDropPolicy=queue).
    virtual bool onEncodedFrame(FramePtr frame) = 0;
    virtual void onEncoderError(const std::string& msg) = 0;
  };

  struct Params {
    EncoderSettings settings;
    int rotationDeg = 0;
    bool mirror = false;
    uint32_t epoch = 0;  // epoch of the first VideoFormat
    // Shared epoch counter; bumped if the encoder emits new parameter sets mid-stream.
    std::atomic<uint32_t>* epochCounter = nullptr;
  };

  VideoEncoder() = default;
  ~VideoEncoder();
  VideoEncoder(const VideoEncoder&) = delete;
  VideoEncoder& operator=(const VideoEncoder&) = delete;

  // One configure attempt (ladder stage 0..3) + createInputSurface.
  bool configure(const Params& p, const EncoderCaps& caps, int apiLevel, int stage,
                 std::string& err);
  ANativeWindow* inputSurface() const { return surface_; }
  bool start(Sink* sink, std::string& err);
  void stop();  // joins the output thread, releases codec + surface
  bool running() const { return running_.load(); }

  void setBitrate(int bps);
  // force=false: ignored if an IDR left the encoder < 150 ms ago.
  // Always ignored while a previous request is still pending (< 500 ms).
  void requestKeyframe(bool force);

  const Params& params() const { return params_; }  // worker thread only
  uint32_t epoch() const { return curEpoch_.load(); }
  const std::string& name() const { return name_; }

  // Stats (any thread).
  uint64_t framesOut() const { return framesOut_.load(); }
  uint64_t bytesOut() const { return bytesOut_.load(); }
  float latencyMs() const { return latencyMsX100_.load() / 100.0f; }
  int64_t lastKeyMs() const { return lastKeyMs_.load(); }
  bool bframesSeen() const { return bframesSeen_.load(); }

 private:
  void outputLoop();
  void handleConfig(const uint8_t* data, size_t len);
  // Builds the frame straight from the codec buffer (the only copy); nullptr = drop.
  FramePtr buildFrame(const uint8_t* data, size_t len, int64_t ptsUs, uint32_t flags);
  int64_t toMonotonic(int64_t ptsUs);
  void release();

  AMediaCodec* codec_ = nullptr;
  ANativeWindow* surface_ = nullptr;
  std::string name_;
  Params params_;
  Sink* sink_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopFlag_{false};
  std::mutex ctlMu_;      // serializes setParameters vs stop
  bool started_ = false;  // guarded by ctlMu_

  std::vector<uint8_t> csd_;  // output thread only
  bool formatSent_ = false;   // output thread only
  int64_t lastPts_ = -1;      // output thread only
  int tsMode_ = -1;           // output thread only: 0 raw mono, 1 boot->mono, 2 wallclock fallback

  std::atomic<uint64_t> framesOut_{0}, bytesOut_{0};
  std::atomic<int32_t> latencyMsX100_{0};
  std::atomic<int64_t> lastKeyMs_{0};
  std::atomic<int64_t> syncRequestedMs_{0};
  std::atomic<bool> bframesSeen_{false};
  std::atomic<uint32_t> curEpoch_{0};
};

}  // namespace owc
