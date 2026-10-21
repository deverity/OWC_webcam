#include "camera/camera_info.h"

#include <camera/NdkCameraMetadata.h>
#include <media/NdkImage.h>

#include <algorithm>
#include <climits>
#include <cmath>

#include "util/log.h"

namespace owc {

bool CameraDesc::hasMode(const std::vector<uint8_t>& v, uint8_t m) const {
  return std::find(v.begin(), v.end(), m) != v.end();
}

int CameraDesc::maxFpsFor(int w, int h) const {
  for (const auto& s : info.sizes)
    if (s.width == w && s.height == h) return s.max_fps;
  return maxAeFps;
}

int CameraDesc::hsCameraFps(int w, int h) const {
  int fixed = 0, any = 0;
  for (const auto& c : hsConfigs) {
    if (c.width != w || c.height != h) continue;
    any = std::max(any, c.fpsMax);
    if (c.fpsMin == c.fpsMax) fixed = std::max(fixed, c.fpsMax);
  }
  return fixed ? fixed : any;
}

std::pair<int, int> CameraDesc::hsRange(int w, int h) const {
  const int f = hsCameraFps(w, h);
  std::pair<int, int> best{f, f};
  int bestMin = -1;
  for (const auto& c : hsConfigs) {
    if (c.width != w || c.height != h || c.fpsMax != f) continue;
    if (c.fpsMin > bestMin) {  // the narrowest range ending at f (fixed if available)
      bestMin = c.fpsMin;
      best = {c.fpsMin, c.fpsMax};
    }
  }
  return best;
}

std::vector<VideoSize> CameraDesc::hsSizes() const {
  std::vector<VideoSize> out;
  for (const auto& c : hsConfigs) {
    bool dup = false;
    for (const auto& s : out) dup |= (s.width == c.width && s.height == c.height);
    if (dup) continue;
    VideoSize v;
    v.width = static_cast<uint16_t>(c.width);
    v.height = static_cast<uint16_t>(c.height);
    v.max_fps = static_cast<uint16_t>(hsCameraFps(c.width, c.height));
    out.push_back(v);
  }
  std::sort(out.begin(), out.end(), [](const VideoSize& a, const VideoSize& b) {
    return static_cast<int64_t>(a.width) * a.height > static_cast<int64_t>(b.width) * b.height;
  });
  return out;
}

bool CameraDesc::supportsSize(int w, int h) const {
  for (const auto& s : info.sizes)
    if (s.width == w && s.height == h) return true;
  return false;
}

std::pair<int32_t, int32_t> CameraDesc::pickFpsRange(int fps) const {
  std::pair<int32_t, int32_t> best{fps, fps};
  bool found = false;
  int bestScore = INT_MAX;
  for (const auto& r : info.fps_ranges) {
    int lo = r.first, hi = r.second;
    int score;
    if (lo == fps && hi == fps) score = 0;
    else if (hi == fps) score = 1000 + (fps - lo);       // [x,fps], prefer highest x
    else if (lo <= fps && fps <= hi) score = 100000 + (hi - lo);
    else score = 1000000 + std::abs(hi - fps) * 100 + (hi - lo);
    if (score < bestScore) {
      bestScore = score;
      best = {lo, hi};
      found = true;
    }
  }
  if (!found) return {fps, fps};
  return best;
}

namespace {

std::vector<uint8_t> u8List(const ACameraMetadata* m, uint32_t tag) {
  ACameraMetadata_const_entry e{};
  std::vector<uint8_t> out;
  if (ACameraMetadata_getConstEntry(m, tag, &e) == ACAMERA_OK)
    out.assign(e.data.u8, e.data.u8 + e.count);
  return out;
}

bool getU8(const ACameraMetadata* m, uint32_t tag, uint8_t& v) {
  ACameraMetadata_const_entry e{};
  if (ACameraMetadata_getConstEntry(m, tag, &e) != ACAMERA_OK || e.count < 1) return false;
  v = e.data.u8[0];
  return true;
}

bool getI32(const ACameraMetadata* m, uint32_t tag, int32_t* v, uint32_t n) {
  ACameraMetadata_const_entry e{};
  if (ACameraMetadata_getConstEntry(m, tag, &e) != ACAMERA_OK || e.count < n) return false;
  for (uint32_t i = 0; i < n; ++i) v[i] = e.data.i32[i];
  return true;
}

bool getI64(const ACameraMetadata* m, uint32_t tag, int64_t* v, uint32_t n) {
  ACameraMetadata_const_entry e{};
  if (ACameraMetadata_getConstEntry(m, tag, &e) != ACAMERA_OK || e.count < n) return false;
  for (uint32_t i = 0; i < n; ++i) v[i] = e.data.i64[i];
  return true;
}

bool getF(const ACameraMetadata* m, uint32_t tag, float& v) {
  ACameraMetadata_const_entry e{};
  if (ACameraMetadata_getConstEntry(m, tag, &e) != ACAMERA_OK || e.count < 1) return false;
  v = e.data.f[0];
  return true;
}

CameraDesc describe(const char* id, const ACameraMetadata* m) {
  CameraDesc d;
  d.info.id = id;
  uint8_t u = 0;
  if (getU8(m, ACAMERA_LENS_FACING, u))
    d.info.facing = u == ACAMERA_LENS_FACING_FRONT ? Facing::Front
                    : u == ACAMERA_LENS_FACING_BACK ? Facing::Back
                                                     : Facing::External;
  if (getU8(m, ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, u)) d.info.hw_level = u;
  int32_t i4[4];
  if (getI32(m, ACAMERA_SENSOR_ORIENTATION, i4, 1)) d.info.sensor_orientation = i4[0];
  float f = 1;
  if (getF(m, ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, f)) d.info.max_zoom = std::max(1.0f, f);
  if (getU8(m, ACAMERA_FLASH_INFO_AVAILABLE, u)) d.info.has_flash = u != 0;
  if (getI32(m, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, i4, 4)) {
    // NDK: (xmin, ymin, width, height)
    d.activeX = i4[0];
    d.activeY = i4[1];
    d.activeW = i4[2];
    d.activeH = i4[3];
  }

  // AE target fps ranges
  ACameraMetadata_const_entry e{};
  if (ACameraMetadata_getConstEntry(m, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &e) ==
      ACAMERA_OK) {
    int hi = 0, lo = INT_MAX;
    for (uint32_t i = 0; i + 1 < e.count; i += 2) {
      d.info.fps_ranges.emplace_back(static_cast<uint16_t>(e.data.i32[i]),
                                     static_cast<uint16_t>(e.data.i32[i + 1]));
      hi = std::max(hi, e.data.i32[i + 1]);
      lo = std::min(lo, e.data.i32[i]);
    }
    if (hi > 0) d.maxAeFps = hi;
    if (lo != INT_MAX) d.minAeFps = std::max(1, lo);
  }

  // PRIVATE output sizes + min frame durations.
  std::vector<std::pair<int, int>> sizes;
  if (ACameraMetadata_getConstEntry(m, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) ==
      ACAMERA_OK) {
    for (uint32_t i = 0; i + 3 < e.count; i += 4) {
      int32_t fmt = e.data.i32[i], w = e.data.i32[i + 1], h = e.data.i32[i + 2],
              input = e.data.i32[i + 3];
      if (fmt != AIMAGE_FORMAT_PRIVATE || input) continue;
      if (w <= 0 || h <= 0 || (w & 1) || (h & 1) || w > 4096 || h > 4096 ||
          static_cast<int64_t>(w) * h > 4096LL * 2304)
        continue;
      sizes.emplace_back(w, h);
    }
  }
  std::vector<int64_t> durations;  // parallel to sizes
  ACameraMetadata_const_entry de{};
  bool haveDur = ACameraMetadata_getConstEntry(m, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                                               &de) == ACAMERA_OK;
  for (auto& s : sizes) {
    int64_t dur = 0;
    if (haveDur) {
      for (uint32_t i = 0; i + 3 < de.count; i += 4) {
        if (de.data.i64[i] == AIMAGE_FORMAT_PRIVATE && de.data.i64[i + 1] == s.first &&
            de.data.i64[i + 2] == s.second) {
          dur = de.data.i64[i + 3];
          break;
        }
      }
    }
    int fps = dur > 0 ? static_cast<int>(1e9 / static_cast<double>(dur) + 0.5) : d.maxAeFps;
    // Normal capture sessions are bounded by the AE target ranges.
    fps = std::min(fps, d.maxAeFps);
    VideoSize vs;
    vs.width = static_cast<uint16_t>(s.first);
    vs.height = static_cast<uint16_t>(s.second);
    vs.max_fps = static_cast<uint16_t>(std::max(1, fps));
    d.info.sizes.push_back(vs);
  }
  std::sort(d.info.sizes.begin(), d.info.sizes.end(), [](const VideoSize& a, const VideoSize& b) {
    int64_t aa = static_cast<int64_t>(a.width) * a.height, bb = static_cast<int64_t>(b.width) * b.height;
    return aa != bb ? aa > bb : a.width > b.width;
  });
  d.info.sizes.erase(std::unique(d.info.sizes.begin(), d.info.sizes.end(),
                                 [](const VideoSize& a, const VideoSize& b) {
                                   return a.width == b.width && a.height == b.height;
                                 }),
                     d.info.sizes.end());

  d.afModes = u8List(m, ACAMERA_CONTROL_AF_AVAILABLE_MODES);
  d.aeModes = u8List(m, ACAMERA_CONTROL_AE_AVAILABLE_MODES);
  d.awbModes = u8List(m, ACAMERA_CONTROL_AWB_AVAILABLE_MODES);
  d.antibandingModes = u8List(m, ACAMERA_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES);
  d.vstabModes = u8List(m, ACAMERA_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES);
  d.oisModes = u8List(m, ACAMERA_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION);
  d.nrModes = u8List(m, ACAMERA_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES);
  d.edgeModes = u8List(m, ACAMERA_EDGE_AVAILABLE_EDGE_MODES);
  getF(m, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, d.minFocusDistance);
  if (getU8(m, ACAMERA_CONTROL_AE_LOCK_AVAILABLE, u)) d.aeLockAvailable = u != 0;
  if (getU8(m, ACAMERA_CONTROL_AWB_LOCK_AVAILABLE, u)) d.awbLockAvailable = u != 0;
  int32_t r2[2];
  if (getI32(m, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, r2, 2)) {
    d.aeCompMin = r2[0];
    d.aeCompMax = r2[1];
  }
  if (ACameraMetadata_getConstEntry(m, ACAMERA_CONTROL_AE_COMPENSATION_STEP, &e) == ACAMERA_OK &&
      e.count >= 1 && e.data.r[0].denominator != 0)
    d.aeCompStep = static_cast<float>(e.data.r[0].numerator) / e.data.r[0].denominator;
  if (getI32(m, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, r2, 2)) {
    d.isoMin = r2[0];
    d.isoMax = r2[1];
  }
  int64_t l2[2];
  if (getI64(m, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE, l2, 2)) {
    d.expMin = l2[0];
    d.expMax = l2[1];
  }
  int32_t r3[3];
  if (getI32(m, ACAMERA_CONTROL_MAX_REGIONS, r3, 3)) {
    d.maxRegionsAe = r3[0];
    d.maxRegionsAf = r3[2];
  }
  for (uint8_t cap : u8List(m, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES))
    if (cap == ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR) d.manualSensor = true;
  getU8(m, ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE, d.timestampSource);
  return d;
}

}  // namespace

std::vector<CameraDesc> enumerateCameras(ACameraManager* mgr, bool verbose) {
  std::vector<CameraDesc> out;
  ACameraIdList* ids = nullptr;
  camera_status_t st = ACameraManager_getCameraIdList(mgr, &ids);
  if (st != ACAMERA_OK || !ids) {
    LOGE("ACameraManager_getCameraIdList failed: %d", st);
    return out;
  }
  for (int i = 0; i < ids->numCameras; ++i) {
    const char* id = ids->cameraIds[i];
    ACameraMetadata* m = nullptr;
    if (ACameraManager_getCameraCharacteristics(mgr, id, &m) != ACAMERA_OK || !m) {
      LOGW("camera %s: no characteristics", id);
      continue;
    }
    CameraDesc d = describe(id, m);
    ACameraMetadata_free(m);
    if (d.info.sizes.empty()) {
      LOGW("camera %s: no PRIVATE output sizes; skipped", id);
      continue;
    }
    if (verbose) LOGI("camera %s: facing=%d level=%d orient=%d zoom=%.1f flash=%d sizes=%zu (max %ux%u) "
         "fps<=%d tsSource=%s manual=%d afModes=%zu",
         id, static_cast<int>(d.info.facing), d.info.hw_level, d.info.sensor_orientation,
         d.info.max_zoom, d.info.has_flash, d.info.sizes.size(), d.info.sizes[0].width,
         d.info.sizes[0].height, d.maxAeFps, d.timestampSource ? "REALTIME" : "UNKNOWN",
         d.manualSensor, d.afModes.size());
    out.push_back(std::move(d));
  }
  ACameraManager_deleteCameraIdList(ids);
  return out;
}

VideoSize pickDefaultSize(const CameraDesc& d) {
  for (const auto& s : d.info.sizes)
    if (s.width == 1920 && s.height == 1080) return s;
  const VideoSize* best = nullptr;
  auto area = [](const VideoSize& s) { return static_cast<int64_t>(s.width) * s.height; };
  for (const auto& s : d.info.sizes) {
    if (s.width > 1920 || s.height > 1080) continue;
    bool wide = s.width * 9 == s.height * 16;
    if (!best) { best = &s; continue; }
    bool bestWide = best->width * 9 == best->height * 16;
    if ((wide && !bestWide) || (wide == bestWide && area(s) > area(*best))) best = &s;
  }
  if (best) return *best;
  return d.info.sizes.back();  // everything is bigger than 1080p: take the smallest
}

int defaultCameraIndex(const std::vector<CameraDesc>& cams) {
  for (size_t i = 0; i < cams.size(); ++i)
    if (cams[i].info.facing == Facing::Back) return static_cast<int>(i);
  return 0;
}

}  // namespace owc
