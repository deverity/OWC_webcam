#include "camera/camera_stream.h"

#include <android/native_window.h>
#include <camera/NdkCameraMetadataTags.h>

#include <algorithm>

#include "util/log.h"

namespace owc {

void CameraStream::onDisconnected(void* ctx, ACameraDevice* dev) {
  auto* self = static_cast<CameraStream*>(ctx);
  LOGW("camera %s disconnected (taken by another app or policy)", ACameraDevice_getId(dev));
  if (self->listener_) self->listener_->onCameraLost(self->gen_.load(), 0, "disconnected");
}

void CameraStream::onError(void* ctx, ACameraDevice* dev, int err) {
  auto* self = static_cast<CameraStream*>(ctx);
  const char* what = "unknown";
  switch (err) {
    case ERROR_CAMERA_IN_USE: what = "camera in use"; break;
    case ERROR_MAX_CAMERAS_IN_USE: what = "max cameras in use"; break;
    case ERROR_CAMERA_DISABLED: what = "camera disabled by policy"; break;
    case ERROR_CAMERA_DEVICE: what = "fatal device error"; break;
    case ERROR_CAMERA_SERVICE: what = "camera service error"; break;
    default: break;
  }
  LOGE("camera %s error %d (%s)", ACameraDevice_getId(dev), err, what);
  if (self->listener_) self->listener_->onCameraLost(self->gen_.load(), err, what);
}

void CameraStream::onSessionClosed(void*, ACameraCaptureSession*) { LOGD("capture session closed"); }
void CameraStream::onSessionReady(void*, ACameraCaptureSession*) { LOGD("capture session ready"); }
void CameraStream::onSessionActive(void*, ACameraCaptureSession*) { LOGD("capture session active"); }

void CameraStream::onCaptureStarted(void* ctx, ACameraCaptureSession*, const ACaptureRequest*,
                                    int64_t) {
  static_cast<CameraStream*>(ctx)->frames_.fetch_add(1, std::memory_order_relaxed);
}

void CameraStream::onCaptureCompleted(void* ctx, ACameraCaptureSession*, ACaptureRequest*,
                                      const ACameraMetadata* result) {
  auto* self = static_cast<CameraStream*>(ctx);
  ACameraMetadata_const_entry e{};
  if (!result || ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_TIMESTAMP, &e) != ACAMERA_OK ||
      e.count < 1)
    return;
  timespec now{};
  clock_gettime(self->sensorClock_, &now);
  const int64_t us = (static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec - e.data.i64[0]) / 1000;
  if (us < 0 || us > 2000000) return;  // clock mismatch: ignore
  auto ewma = [](std::atomic<int64_t>& a, int64_t v) {
    const int64_t p = a.load(std::memory_order_relaxed);
    a.store(p == 0 ? v : p + (v - p) / 16, std::memory_order_relaxed);
  };
  ewma(self->ispUsEwma_, us);
  if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_EXPOSURE_TIME, &e) == ACAMERA_OK && e.count)
    ewma(self->expUsEwma_, e.data.i64[0] / 1000);
  if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_FRAME_DURATION, &e) == ACAMERA_OK && e.count)
    ewma(self->durUsEwma_, e.data.i64[0] / 1000);
  if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_SENSITIVITY, &e) == ACAMERA_OK && e.count)
    ewma(self->isoEwma_, e.data.i32[0]);
  if (ACameraMetadata_getConstEntry(result, ACAMERA_REQUEST_PIPELINE_DEPTH, &e) == ACAMERA_OK && e.count)
    ewma(self->depthX100_, static_cast<int64_t>(e.data.u8[0]) * 100);
}

void CameraStream::onCaptureFailed(void* ctx, ACameraCaptureSession*, ACaptureRequest*,
                                   ACameraCaptureFailure* f) {
  auto* self = static_cast<CameraStream*>(ctx);
  uint32_t n = self->failures_.fetch_add(1) + 1;
  if (n == 1 || n % 100 == 0)
    LOGW("camera capture failed (reason %d, %u total)", f ? f->reason : -1, n);
}

bool CameraStream::open(const CameraDesc& d, ANativeWindow* encoder, ANativeWindow* preview,
                        const CameraControls& c, int fps, std::string& err) {
  close();
  desc_ = d;
  gen_.fetch_add(1);
  frames_ = 0;
  failures_ = 0;
  ispUsEwma_ = 0;
  expUsEwma_ = 0;
  durUsEwma_ = 0;
  isoEwma_ = 0;
  depthX100_ = 0;
  sensorClock_ = d.timestampSource == ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME ? CLOCK_BOOTTIME
                                                                                 : CLOCK_MONOTONIC;
  encWin_ = encoder;
  ANativeWindow_acquire(encWin_);
  if (preview) {
    prevWin_ = preview;
    ANativeWindow_acquire(prevWin_);
  }
  devCb_.context = this;
  devCb_.onDisconnected = &CameraStream::onDisconnected;
  devCb_.onError = &CameraStream::onError;
  camera_status_t st = ACameraManager_openCamera(mgr_, d.info.id.c_str(), &devCb_, &device_);
  if (st != ACAMERA_OK || !device_) {
    err = "openCamera(" + d.info.id + ") failed: " + std::to_string(st) +
          (st == ACAMERA_ERROR_PERMISSION_DENIED ? " (CAMERA permission missing)" : "");
    device_ = nullptr;
    close();
    return false;
  }
  if (!createSession(err) || !buildRequest(c, fps, err) || !submitRepeating()) {
    if (err.empty()) err = "setRepeatingRequest failed";
    close();
    return false;
  }
  LOGI("camera %s streaming: fps range [%d,%d]%s", d.info.id.c_str(), d.pickFpsRange(fps).first,
       d.pickFpsRange(fps).second, prevWin_ ? " + preview" : "");
  return true;
}

bool CameraStream::createSession(std::string& err) {
  camera_status_t st = ACaptureSessionOutputContainer_create(&outputs_);
  if (st != ACAMERA_OK) {
    err = "output container create failed";
    return false;
  }
  if (ACaptureSessionOutput_create(encWin_, &encOut_) != ACAMERA_OK ||
      ACaptureSessionOutputContainer_add(outputs_, encOut_) != ACAMERA_OK ||
      ACameraOutputTarget_create(encWin_, &encTarget_) != ACAMERA_OK) {
    err = "encoder output setup failed";
    return false;
  }
  if (prevWin_) {
    if (ACaptureSessionOutput_create(prevWin_, &prevOut_) != ACAMERA_OK ||
        ACaptureSessionOutputContainer_add(outputs_, prevOut_) != ACAMERA_OK ||
        ACameraOutputTarget_create(prevWin_, &prevTarget_) != ACAMERA_OK) {
      err = "preview output setup failed";
      return false;
    }
  }
  sessCb_.context = this;
  sessCb_.onClosed = &CameraStream::onSessionClosed;
  sessCb_.onReady = &CameraStream::onSessionReady;
  sessCb_.onActive = &CameraStream::onSessionActive;
  st = ACameraDevice_createCaptureSession(device_, outputs_, &sessCb_, &session_);
  if (st != ACAMERA_OK || !session_) {
    err = "createCaptureSession failed: " + std::to_string(st);
    session_ = nullptr;
    return false;
  }
  return true;
}

bool CameraStream::buildRequest(const CameraControls& c, int fps, std::string& err) {
  if (request_) {
    ACaptureRequest_free(request_);
    request_ = nullptr;
  }
  camera_status_t st = ACameraDevice_createCaptureRequest(device_, TEMPLATE_RECORD, &request_);
  if (st != ACAMERA_OK || !request_) {
    err = "createCaptureRequest failed: " + std::to_string(st);
    request_ = nullptr;
    return false;
  }
  ACaptureRequest_addTarget(request_, encTarget_);
  if (prevTarget_) ACaptureRequest_addTarget(request_, prevTarget_);
  applyControls(request_, desc_, c, fps);
  return true;
}

bool CameraStream::submitRepeating() {
  capCb_ = ACameraCaptureSession_captureCallbacks{};
  capCb_.context = this;
  capCb_.onCaptureStarted = &CameraStream::onCaptureStarted;
  capCb_.onCaptureFailed = &CameraStream::onCaptureFailed;
  capCb_.onCaptureCompleted = &CameraStream::onCaptureCompleted;
  camera_status_t st = ACameraCaptureSession_setRepeatingRequest(session_, &capCb_, 1, &request_,
                                                                 nullptr);
  if (st != ACAMERA_OK) {
    LOGE("setRepeatingRequest failed: %d", st);
    return false;
  }
  return true;
}

void CameraStream::destroySession() {
  if (session_) {
    ACameraCaptureSession_stopRepeating(session_);
    ACameraCaptureSession_close(session_);
    session_ = nullptr;
  }
  if (request_) {
    ACaptureRequest_free(request_);
    request_ = nullptr;
  }
  if (encTarget_) { ACameraOutputTarget_free(encTarget_); encTarget_ = nullptr; }
  if (prevTarget_) { ACameraOutputTarget_free(prevTarget_); prevTarget_ = nullptr; }
  if (outputs_) {
    if (encOut_) ACaptureSessionOutputContainer_remove(outputs_, encOut_);
    if (prevOut_) ACaptureSessionOutputContainer_remove(outputs_, prevOut_);
    ACaptureSessionOutputContainer_free(outputs_);
    outputs_ = nullptr;
  }
  if (encOut_) { ACaptureSessionOutput_free(encOut_); encOut_ = nullptr; }
  if (prevOut_) { ACaptureSessionOutput_free(prevOut_); prevOut_ = nullptr; }
}

void CameraStream::close() {
  destroySession();
  if (device_) {
    ACameraDevice_close(device_);  // stops the callback looper
    device_ = nullptr;
  }
  if (encWin_) { ANativeWindow_release(encWin_); encWin_ = nullptr; }
  if (prevWin_) { ANativeWindow_release(prevWin_); prevWin_ = nullptr; }
}

bool CameraStream::apply(const CameraControls& c, int fps) {
  if (!session_ || !request_) return false;
  applyControls(request_, desc_, c, fps);
  return submitRepeating();
}

bool CameraStream::focusAt(float x, float y, const CameraControls& c, int fps) {
  if (!session_ || !request_) return false;
  if (c.afMode == 0) {
    LOGW("FocusAt ignored: manual focus (AfMode=off)");
    return false;
  }
  x = std::clamp(x, 0.0f, 1.0f);
  y = std::clamp(y, 0.0f, 1.0f);
  int32_t crop[4];
  cropForZoom(desc_, c.zoom, crop);
  const int32_t cx = crop[0] + static_cast<int32_t>(x * crop[2]);
  const int32_t cy = crop[1] + static_cast<int32_t>(y * crop[3]);
  const int32_t hw = std::max(16, crop[2] / 10), hh = std::max(16, crop[3] / 10);
  int32_t region[5] = {std::max(0, cx - hw), std::max(0, cy - hh),
                       std::min(desc_.activeW - 1, cx + hw), std::min(desc_.activeH - 1, cy + hh),
                       1000};
  applyControls(request_, desc_, c, fps);
  if (desc_.maxRegionsAf > 0) ACaptureRequest_setEntry_i32(request_, ACAMERA_CONTROL_AF_REGIONS, 5, region);
  if (desc_.maxRegionsAe > 0 && c.aeMode != 0)
    ACaptureRequest_setEntry_i32(request_, ACAMERA_CONTROL_AE_REGIONS, 5, region);
  // One-shot: cancel any previous lock, then trigger a scan at the new region.
  uint8_t trig = ACAMERA_CONTROL_AF_TRIGGER_CANCEL;
  ACaptureRequest_setEntry_u8(request_, ACAMERA_CONTROL_AF_TRIGGER, 1, &trig);
  ACameraCaptureSession_capture(session_, nullptr, 1, &request_, nullptr);
  trig = ACAMERA_CONTROL_AF_TRIGGER_START;
  ACaptureRequest_setEntry_u8(request_, ACAMERA_CONTROL_AF_TRIGGER, 1, &trig);
  camera_status_t st = ACameraCaptureSession_capture(session_, nullptr, 1, &request_, nullptr);
  // Back to the repeating request (regions kept, trigger idle).
  trig = ACAMERA_CONTROL_AF_TRIGGER_IDLE;
  ACaptureRequest_setEntry_u8(request_, ACAMERA_CONTROL_AF_TRIGGER, 1, &trig);
  bool ok = submitRepeating();
  LOGD("FocusAt(%.2f, %.2f) -> region [%d,%d,%d,%d] st=%d", x, y, region[0], region[1], region[2],
       region[3], st);
  return ok && st == ACAMERA_OK;
}

bool CameraStream::setPreview(ANativeWindow* preview, const CameraControls& c, int fps,
                              std::string& err) {
  if (!device_) return false;
  destroySession();
  if (prevWin_) {
    ANativeWindow_release(prevWin_);
    prevWin_ = nullptr;
  }
  if (preview) {
    prevWin_ = preview;
    ANativeWindow_acquire(prevWin_);
  }
  return createSession(err) && buildRequest(c, fps, err) && submitRepeating();
}

}  // namespace owc
