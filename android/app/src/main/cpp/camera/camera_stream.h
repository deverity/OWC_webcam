// One opened camera feeding the encoder surface (+ optional preview surface).
//
// Threading: every public method runs on the worker thread. NDK camera
// callbacks run on the camera's looper thread; they only bump atomics and call
// Listener (which must be thread-safe: it posts to the worker queue).
// ACameraDevice_close() stops the device's callback looper, so after close()
// returns no callback for that device can run; the generation counter
// additionally discards anything stale.
#pragma once

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>

#include <atomic>
#include <cstdint>
#include <ctime>
#include <string>

#include "camera/camera_controls.h"
#include "camera/camera_info.h"

struct ANativeWindow;

namespace owc {

class CameraStream {
 public:
  class Listener {
   public:
    virtual ~Listener() = default;
    // Device disconnected (another app took it, policy) or fatal error.
    virtual void onCameraLost(uint64_t gen, int error, const std::string& what) = 0;
  };

  CameraStream(ACameraManager* mgr, Listener* l) : mgr_(mgr), listener_(l) {}
  ~CameraStream() { close(); }
  CameraStream(const CameraStream&) = delete;
  CameraStream& operator=(const CameraStream&) = delete;

  bool open(const CameraDesc& d, ANativeWindow* encoder, ANativeWindow* preview,
            const CameraControls& c, int fps, std::string& err);
  void close();
  bool isOpen() const { return device_ != nullptr; }
  uint64_t generation() const { return gen_.load(); }

  // Re-applies all controls on the repeating request.
  bool apply(const CameraControls& c, int fps);
  // Tap-to-focus: (x, y) normalized in the sensor-oriented frame.
  bool focusAt(float x, float y, const CameraControls& c, int fps);
  // Rebuilds the capture session with a new preview window (nullptr = none).
  bool setPreview(ANativeWindow* preview, const CameraControls& c, int fps, std::string& err);

  // Frames started since the last call (camera fps).
  uint32_t takeFrameCount() { return frames_.exchange(0); }
  // EWMA of sensor timestamp (start of exposure) -> capture result arrival.
  float ispLatencyMs() const { return ispUsEwma_.load() / 1000.0f; }

 private:
  bool createSession(std::string& err);
  void destroySession();
  bool buildRequest(const CameraControls& c, int fps, std::string& err);
  bool submitRepeating();

  static void onDisconnected(void* ctx, ACameraDevice* dev);
  static void onError(void* ctx, ACameraDevice* dev, int err);
  static void onSessionClosed(void* ctx, ACameraCaptureSession* s);
  static void onSessionReady(void* ctx, ACameraCaptureSession* s);
  static void onSessionActive(void* ctx, ACameraCaptureSession* s);
  static void onCaptureStarted(void* ctx, ACameraCaptureSession* s, const ACaptureRequest* r,
                               int64_t ts);
  static void onCaptureCompleted(void* ctx, ACameraCaptureSession* s, ACaptureRequest* r,
                                 const ACameraMetadata* result);
  static void onCaptureFailed(void* ctx, ACameraCaptureSession* s, ACaptureRequest* r,
                              ACameraCaptureFailure* f);

  ACameraManager* mgr_;
  Listener* listener_;
  CameraDesc desc_;
  ACameraDevice* device_ = nullptr;
  ACaptureSessionOutputContainer* outputs_ = nullptr;
  ACaptureSessionOutput* encOut_ = nullptr;
  ACaptureSessionOutput* prevOut_ = nullptr;
  ACameraOutputTarget* encTarget_ = nullptr;
  ACameraOutputTarget* prevTarget_ = nullptr;
  ACameraCaptureSession* session_ = nullptr;
  ACaptureRequest* request_ = nullptr;
  ANativeWindow* encWin_ = nullptr;   // acquired
  ANativeWindow* prevWin_ = nullptr;  // acquired

  ACameraDevice_StateCallbacks devCb_{};
  ACameraCaptureSession_stateCallbacks sessCb_{};
  ACameraCaptureSession_captureCallbacks capCb_{};

  std::atomic<uint64_t> gen_{0};
  std::atomic<uint32_t> frames_{0};
  std::atomic<uint32_t> failures_{0};
  std::atomic<int64_t> ispUsEwma_{0};
  clockid_t sensorClock_ = CLOCK_MONOTONIC;  // REALTIME source -> CLOCK_BOOTTIME
};

}  // namespace owc
