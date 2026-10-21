// Camera enumeration: protocol CameraInfo + the metadata needed to build
// ParamDescs and capture requests.
#pragma once

#include <camera/NdkCameraManager.h>

#include <cstdint>
#include <string>
#include <vector>

#include "owc/protocol.h"

namespace owc {

// One constrained high-speed configuration (StreamConfigurationMap
// getHighSpeedVideoSizes/FpsRangesFor, queried through Java).
struct HsConfig {
  int width = 0, height = 0, fpsMin = 0, fpsMax = 0;
  int batch = 0;  // batch_size_max (availableHighSpeedVideoConfigurations[4]); 0 = unknown
};

struct CameraDesc {
  CameraInfo info;  // id, facing, hw level, orientation, max zoom, flash, sizes, fps ranges
  // active pixel array (for crop region / AF regions)
  int32_t activeX = 0, activeY = 0, activeW = 0, activeH = 0;
  std::vector<uint8_t> afModes, aeModes, awbModes, antibandingModes, vstabModes, oisModes,
      nrModes, edgeModes;
  float minFocusDistance = 0;  // 0 = fixed focus
  bool aeLockAvailable = false, awbLockAvailable = false;
  int32_t aeCompMin = 0, aeCompMax = 0;
  float aeCompStep = 0;
  int32_t isoMin = 0, isoMax = 0;
  int64_t expMin = 0, expMax = 0;
  int32_t maxRegionsAe = 0, maxRegionsAf = 0;
  bool manualSensor = false;
  uint8_t timestampSource = 0;  // ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_*
  // Upper bound of the AE target fps ranges (normal capture sessions cannot exceed it).
  int maxAeFps = 30, minAeFps = 1;

  bool hasMode(const std::vector<uint8_t>& v, uint8_t m) const;
  // Max fps for a size in normal (non high-speed) sessions.
  int maxFpsFor(int w, int h) const;
  bool supportsSize(int w, int h) const;
  // Best AE target range for `fps`: exact [fps,fps], else [x,fps] with the
  // highest x, else the range containing fps with the narrowest span.
  std::pair<int32_t, int32_t> pickFpsRange(int fps) const;

  // ---- constrained high-speed (empty when unsupported)
  std::vector<HsConfig> hsConfigs;
  // Sensor fps used for a high-speed size: highest fixed range [f,f], else the
  // highest upper bound; 0 if the size has no high-speed configuration.
  int hsCameraFps(int w, int h) const;
  std::vector<VideoSize> hsSizes() const;  // max_fps = hsCameraFps
  // Advertised range used for hsCameraFps (fixed [f,f] preferred).
  std::pair<int, int> hsRange(int w, int h) const;
  // Frames per HAL batch for that size (fpsMax/30 capped by batch_size_max).
  int hsBatch(int w, int h) const;
};

std::vector<CameraDesc> enumerateCameras(ACameraManager* mgr, bool verbose = true);

// Default stream size: 1920x1080 if supported, else the largest <= 1920x1080
// (16:9 preferred), else the smallest size available.
VideoSize pickDefaultSize(const CameraDesc& d);
int defaultCameraIndex(const std::vector<CameraDesc>& cams);  // first Back camera

}  // namespace owc
