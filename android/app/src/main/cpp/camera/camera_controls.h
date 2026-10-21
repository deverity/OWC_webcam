// Live camera controls (ParamId 20..35) and their translation into capture
// request entries.
#pragma once

#include <camera/NdkCaptureRequest.h>

#include <cstdint>

#include "camera/camera_info.h"

namespace owc {

// Values use the protocol's encoding (see ParamId comments in protocol.h).
struct CameraControls {
  int afMode = 1;           // 0=off(manual) 1=continuous-video 2=continuous-picture 3=auto
  float focusDistance = 0;  // diopters (afMode 0)
  int aeMode = 1;           // 0=off(manual ISO/exposure) 1=on
  bool aeLock = false;
  int aeCompensation = 0;
  int iso = 100;
  int64_t exposureNs = 10000000;
  int awbMode = 1;          // ACAMERA_CONTROL_AWB_MODE_* (1 = AUTO)
  bool awbLock = false;
  float zoom = 1.0f;
  bool torch = false;
  int antibanding = 3;      // 0 off, 1 50Hz, 2 60Hz, 3 auto
  bool videoStabilization = false;
  bool opticalStabilization = false;
  int noiseReduction = 1;   // ACAMERA_NOISE_REDUCTION_MODE_*
  int edgeMode = 1;         // ACAMERA_EDGE_MODE_*
};

// Defaults for a camera, latency-first: continuous-video AF, AE on, AWB auto,
// EIS OFF (adds a frame of delay + crop), OIS ON when present, NR/edge FAST.
CameraControls defaultControls(const CameraDesc& d);

// Maps protocol AfMode (0..3) <-> ACAMERA_CONTROL_AF_MODE_*.
uint8_t afModeToCamera(int protoMode);
bool cameraSupportsAfMode(const CameraDesc& d, int protoMode);

// Writes every control into `req` (TEMPLATE_RECORD base). fps selects the AE
// target range and, with AE off, the frame duration.
void applyControls(ACaptureRequest* req, const CameraDesc& d, const CameraControls& c, int fps);

// Crop region for the zoom factor: (x, y, w, h) in active-array coordinates.
void cropForZoom(const CameraDesc& d, float zoom, int32_t out[4]);
// Tap-to-focus region around normalized (x, y) inside the current crop:
// (x0, y0, x1, y1) in active-array coordinates (10 % of the crop each side).
void focusRegion(const CameraDesc& d, float zoom, float x, float y, int32_t out[4]);

}  // namespace owc
