// Camera -> encoder pipeline for one stream configuration. Worker thread only
// (except the stats getters). The camera is opened only while streaming.
#pragma once

#include <camera/NdkCameraManager.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "camera/camera_stream.h"
#include "camera/high_speed.h"
#include "encoder/video_encoder.h"
#include "params.h"

struct ANativeWindow;

namespace owc {

class Pipeline : public CameraStream::Listener {
 public:
  using LostFn = std::function<void(uint64_t cameraGen, const std::string& what)>;

  Pipeline(ACameraManager* mgr, const EncoderCapsSet* enc, HighSpeedBackend* hs, LostFn onLost);
  ~Pipeline() override;

  // Configures the encoder (fallback ladder: option stages, then smaller
  // sizes), then opens the camera on its input surface. On a size fallback the
  // ParamStore is updated (sizeChanged=true).
  bool start(ParamStore& ps, int bitrate, ANativeWindow* preview, VideoEncoder::Sink* sink,
             bool& sizeChanged, std::string& err);
  void stop();
  bool running() const { return running_; }

  void applyControls(const ParamStore& ps);
  void focusAt(float x, float y, const ParamStore& ps);
  void setBitrate(int bps);
  void requestKeyframe(bool force);
  bool setPreview(ANativeWindow* w, const ParamStore& ps);
  uint64_t cameraGen() const { return camera_.generation(); }

  // Stats
  uint32_t takeCameraFrames() {
    return camera_.takeFrameCount() + synthFrames_.exchange(0) + (hsActive_ ? hs_->takeFrames() : 0);
  }
  // Sensor timestamp (start of exposure) -> capture result, EWMA.
  float ispLatencyMs() const { return hsActive_ ? hs_->ispLatencyMs() : camera_.ispLatencyMs(); }
  bool highSpeedActive() const { return hsActive_; }
  // High-speed session failed after start (polled by the worker).
  bool pollHighSpeedFailure(std::string& why) { return hsActive_ && hs_->failed(why); }
  uint64_t encodedFrames() const { return encoder_.framesOut(); }
  uint64_t encodedBytes() const { return encoder_.bytesOut(); }
  float encodeLatencyMs() const { return encoder_.latencyMs(); }
  const std::string& encoderName() const { return encoder_.name(); }
  bool bframesSeen() const { return encoder_.bframesSeen(); }
  uint32_t epoch() const { return encoder_.epoch(); }
  bool synthetic() const { return synthThread_.joinable(); }

  // TEST ONLY (owc_selftest): when the camera cannot be opened (e.g. a shell
  // process without CAMERA permission) feed a CPU-drawn RGBA pattern into the
  // encoder surface instead, so the encoder/network path can be exercised.
  void setSyntheticFallback(bool on) { syntheticFallback_ = on; }

  // CameraStream::Listener (camera looper thread)
  void onCameraLost(uint64_t gen, int error, const std::string& what) override;

 private:
  const EncoderCapsSet* enc_;
  LostFn onLost_;
  CameraStream camera_;
  VideoEncoder encoder_;
  bool running_ = false;
  HighSpeedBackend* hs_ = nullptr;
  bool hsActive_ = false;
  void startSynthetic(int w, int h, int fps);
  void stopSynthetic();
  bool syntheticFallback_ = false;
  std::thread synthThread_;
  std::atomic<bool> synthStop_{false};
  std::atomic<uint32_t> synthFrames_{0};
  std::atomic<uint32_t> epochCounter_{0};  // never decreases (PC drops older epochs)
};

}  // namespace owc
