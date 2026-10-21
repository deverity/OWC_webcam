#include "camera/camera_controls.h"

#include <camera/NdkCameraMetadataTags.h>

#include <algorithm>
#include <cmath>

namespace owc {

uint8_t afModeToCamera(int p) {
  switch (p) {
    case 0: return ACAMERA_CONTROL_AF_MODE_OFF;
    case 2: return ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
    case 3: return ACAMERA_CONTROL_AF_MODE_AUTO;
    default: return ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
  }
}

bool cameraSupportsAfMode(const CameraDesc& d, int p) {
  if (p == 0) return d.minFocusDistance > 0 && d.hasMode(d.afModes, ACAMERA_CONTROL_AF_MODE_OFF);
  return d.hasMode(d.afModes, afModeToCamera(p));
}

CameraControls defaultControls(const CameraDesc& d) {
  CameraControls c;
  if (cameraSupportsAfMode(d, 1)) c.afMode = 1;
  else if (cameraSupportsAfMode(d, 2)) c.afMode = 2;
  else if (cameraSupportsAfMode(d, 3)) c.afMode = 3;
  else c.afMode = 0;
  c.focusDistance = 0;  // infinity
  c.aeMode = 1;
  c.iso = std::clamp(100, d.isoMin > 0 ? d.isoMin : 100, d.isoMax > 0 ? d.isoMax : 100);
  c.exposureNs = std::clamp<int64_t>(10000000, d.expMin > 0 ? d.expMin : 10000000,
                                     d.expMax > 0 ? d.expMax : 10000000);
  c.awbMode = d.hasMode(d.awbModes, ACAMERA_CONTROL_AWB_MODE_AUTO) || d.awbModes.empty()
                  ? ACAMERA_CONTROL_AWB_MODE_AUTO
                  : d.awbModes[0];
  c.antibanding = d.hasMode(d.antibandingModes, ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO) ||
                          d.antibandingModes.empty()
                      ? ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO
                      : d.antibandingModes[0];
  c.videoStabilization = false;
  c.opticalStabilization =
      d.hasMode(d.oisModes, ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON);
  c.noiseReduction = d.hasMode(d.nrModes, ACAMERA_NOISE_REDUCTION_MODE_FAST) || d.nrModes.empty()
                         ? ACAMERA_NOISE_REDUCTION_MODE_FAST
                         : d.nrModes[0];
  c.edgeMode = d.hasMode(d.edgeModes, ACAMERA_EDGE_MODE_FAST) || d.edgeModes.empty()
                   ? ACAMERA_EDGE_MODE_FAST
                   : d.edgeModes[0];
  return c;
}

void cropForZoom(const CameraDesc& d, float zoom, int32_t out[4]) {
  zoom = std::clamp(zoom, 1.0f, std::max(1.0f, d.info.max_zoom));
  int32_t w = static_cast<int32_t>(std::lround(d.activeW / zoom));
  int32_t h = static_cast<int32_t>(std::lround(d.activeH / zoom));
  w &= ~1;
  h &= ~1;
  out[0] = (d.activeW - w) / 2;  // crop region is relative to the active array
  out[1] = (d.activeH - h) / 2;
  out[2] = w;
  out[3] = h;
}

void focusRegion(const CameraDesc& d, float zoom, float x, float y, int32_t out[4]) {
  x = std::clamp(x, 0.0f, 1.0f);
  y = std::clamp(y, 0.0f, 1.0f);
  int32_t crop[4];
  cropForZoom(d, zoom, crop);
  const int32_t cx = crop[0] + static_cast<int32_t>(x * crop[2]);
  const int32_t cy = crop[1] + static_cast<int32_t>(y * crop[3]);
  const int32_t hw = std::max(16, crop[2] / 10), hh = std::max(16, crop[3] / 10);
  out[0] = std::max(0, cx - hw);
  out[1] = std::max(0, cy - hh);
  out[2] = std::min(d.activeW - 1, cx + hw);
  out[3] = std::min(d.activeH - 1, cy + hh);
}

void applyControls(ACaptureRequest* r, const CameraDesc& d, const CameraControls& c, int fps) {
  uint8_t u;
  u = ACAMERA_CONTROL_MODE_AUTO;
  ACaptureRequest_setEntry_u8(r, ACAMERA_CONTROL_MODE, 1, &u);

  // --- AE
  const bool manualAe = c.aeMode == 0 && d.hasMode(d.aeModes, ACAMERA_CONTROL_AE_MODE_OFF);
  u = manualAe ? ACAMERA_CONTROL_AE_MODE_OFF : ACAMERA_CONTROL_AE_MODE_ON;
  ACaptureRequest_setEntry_u8(r, ACAMERA_CONTROL_AE_MODE, 1, &u);
  auto range = d.pickFpsRange(fps);
  int32_t fpsRange[2] = {range.first, range.second};
  ACaptureRequest_setEntry_i32(r, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fpsRange);
  if (manualAe) {
    int32_t iso = std::clamp(c.iso, d.isoMin, std::max(d.isoMin, d.isoMax));
    int64_t frameNs = 1000000000LL / std::max(1, fps);
    int64_t exp = std::clamp(c.exposureNs, d.expMin, std::max(d.expMin, d.expMax));
    ACaptureRequest_setEntry_i32(r, ACAMERA_SENSOR_SENSITIVITY, 1, &iso);
    ACaptureRequest_setEntry_i64(r, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exp);
    // Keep the stream rate unless the exposure itself is longer than a frame.
    int64_t dur = std::max(frameNs, exp);
    ACaptureRequest_setEntry_i64(r, ACAMERA_SENSOR_FRAME_DURATION, 1, &dur);
  } else {
    u = c.aeLock && d.aeLockAvailable ? ACAMERA_CONTROL_AE_LOCK_ON : ACAMERA_CONTROL_AE_LOCK_OFF;
    ACaptureRequest_setEntry_u8(r, ACAMERA_CONTROL_AE_LOCK, 1, &u);
    int32_t comp = std::clamp(c.aeCompensation, d.aeCompMin, d.aeCompMax);
    ACaptureRequest_setEntry_i32(r, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &comp);
    if (!d.antibandingModes.empty()) {
      u = static_cast<uint8_t>(c.antibanding);
      if (d.hasMode(d.antibandingModes, u))
        ACaptureRequest_setEntry_u8(r, ACAMERA_CONTROL_AE_ANTIBANDING_MODE, 1, &u);
    }
  }

  // --- AF
  if (!d.afModes.empty()) {
    int mode = cameraSupportsAfMode(d, c.afMode) ? c.afMode : -1;
    if (mode >= 0) {
      u = afModeToCamera(mode);
      ACaptureRequest_setEntry_u8(r, ACAMERA_CONTROL_AF_MODE, 1, &u);
      if (mode == 0) {
        float fd = std::clamp(c.focusDistance, 0.0f, d.minFocusDistance);
        ACaptureRequest_setEntry_float(r, ACAMERA_LENS_FOCUS_DISTANCE, 1, &fd);
      }
    }
    u = ACAMERA_CONTROL_AF_TRIGGER_IDLE;
    ACaptureRequest_setEntry_u8(r, ACAMERA_CONTROL_AF_TRIGGER, 1, &u);
  }

  // --- AWB
  if (!d.awbModes.empty() && d.hasMode(d.awbModes, static_cast<uint8_t>(c.awbMode))) {
    u = static_cast<uint8_t>(c.awbMode);
    ACaptureRequest_setEntry_u8(r, ACAMERA_CONTROL_AWB_MODE, 1, &u);
  }
  if (d.awbLockAvailable) {
    u = c.awbLock ? ACAMERA_CONTROL_AWB_LOCK_ON : ACAMERA_CONTROL_AWB_LOCK_OFF;
    ACaptureRequest_setEntry_u8(r, ACAMERA_CONTROL_AWB_LOCK, 1, &u);
  }

  // --- zoom (SCALER_CROP_REGION; CONTROL_ZOOM_RATIO intentionally unused: API 30+)
  if (d.activeW > 0 && d.activeH > 0) {
    int32_t crop[4];
    cropForZoom(d, c.zoom, crop);
    ACaptureRequest_setEntry_i32(r, ACAMERA_SCALER_CROP_REGION, 4, crop);
  }

  // --- torch (AE must not be in a flash mode; ON/OFF modes allow torch)
  if (d.info.has_flash) {
    u = c.torch ? ACAMERA_FLASH_MODE_TORCH : ACAMERA_FLASH_MODE_OFF;
    ACaptureRequest_setEntry_u8(r, ACAMERA_FLASH_MODE, 1, &u);
  }

  // --- stabilization
  if (!d.vstabModes.empty()) {
    u = c.videoStabilization && d.hasMode(d.vstabModes, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON)
            ? ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON
            : ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
    ACaptureRequest_setEntry_u8(r, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE, 1, &u);
  }
  if (!d.oisModes.empty()) {
    u = c.opticalStabilization && d.hasMode(d.oisModes, ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON)
            ? ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON
            : ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_OFF;
    ACaptureRequest_setEntry_u8(r, ACAMERA_LENS_OPTICAL_STABILIZATION_MODE, 1, &u);
  }

  // --- processing
  if (d.hasMode(d.nrModes, static_cast<uint8_t>(c.noiseReduction))) {
    u = static_cast<uint8_t>(c.noiseReduction);
    ACaptureRequest_setEntry_u8(r, ACAMERA_NOISE_REDUCTION_MODE, 1, &u);
  }
  if (d.hasMode(d.edgeModes, static_cast<uint8_t>(c.edgeMode))) {
    u = static_cast<uint8_t>(c.edgeMode);
    ACaptureRequest_setEntry_u8(r, ACAMERA_EDGE_MODE, 1, &u);
  }
}

}  // namespace owc
