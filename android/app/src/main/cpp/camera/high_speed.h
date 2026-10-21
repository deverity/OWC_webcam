// Constrained high-speed capture (ParamId::HighSpeedMode, experimental).
//
// The NDK has no createConstrainedHighSpeedCaptureSession, so the session is
// driven through Java Camera2 by jni_bridge.cpp (JniHighSpeed) and the helper
// class com.owc.webcam.hs.HighSpeedCamera. The core only sees this interface;
// it is null when the helper class is missing (or in the self-test binary).
//
// Threading: every call comes from the worker thread (which the JNI
// implementation attaches to the JVM on first use; detachThread() is called
// before the worker exits).
//
// Constrained high-speed requests only honour a subset of controls (AE/AF/AWB
// modes and locks, AE compensation, AF/AE regions, crop region, flash mode);
// ParamStore flags the others unsupported while the mode is active.
#pragma once

#include <cstdint>
#include <string>

#include "camera/camera_controls.h"
#include "camera/camera_info.h"

struct ANativeWindow;

namespace owc {

class HighSpeedBackend {
 public:
  virtual ~HighSpeedBackend() = default;
  // Opens the camera, creates the high-speed session with `target` (encoder
  // input surface) as its only output and starts the repeating burst with
  // AE target range [fpsMin, fpsMax] (one of the advertised high-speed
  // ranges). Blocking (<= ~6 s).
  // previewUsage selects the gralloc usage of the dummy preview stream
  // (0 GPU_SAMPLED_IMAGE, 1 VIDEO_ENCODE, 2 COMPOSER_OVERLAY, 3 GPU_COLOR_OUTPUT).
  virtual bool open(const CameraDesc& d, ANativeWindow* target, int width, int height, int fpsMin,
                    int fpsMax, const CameraControls& c, int previewUsage, std::string& err) = 0;
  virtual void close() = 0;
  virtual bool apply(const CameraDesc& d, const CameraControls& c) = 0;
  // AF/AE region in active-array coordinates (x0, y0, x1, y1).
  virtual bool focusAt(const int32_t region[4]) = 0;
  virtual uint32_t takeFrames() = 0;
  virtual float ispLatencyMs() = 0;
  virtual float exposureMs() = 0;
  // True (and `why` set) once the device/session failed after open().
  virtual bool failed(std::string& why) = 0;
  virtual void detachThread() = 0;
};

}  // namespace owc
