#include "params.h"

#include <camera/NdkCameraMetadataTags.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "util/debug_props.h"

namespace owc {

const ParamId kAllParams[] = {
    ParamId::CameraIndex, ParamId::VideoWidth, ParamId::VideoHeight, ParamId::Fps,
    ParamId::VideoCodec, ParamId::Bitrate, ParamId::KeyframeIntervalMs, ParamId::BitrateMode,
    ParamId::IntraRefreshFrames, ParamId::FecPercent, ParamId::AdaptiveBitrate,
    ParamId::EncoderProfile, ParamId::MaxSendQueueFrames, ParamId::BFrames, ParamId::QpMin,
    ParamId::QpMax, ParamId::LatencyDropPolicy, ParamId::HighSpeedMode,
    ParamId::AfMode, ParamId::FocusDistance, ParamId::AeMode, ParamId::AeLock,
    ParamId::ExposureCompensation, ParamId::Iso, ParamId::ExposureTimeNs, ParamId::AwbMode,
    ParamId::AwbLock, ParamId::Zoom, ParamId::Torch, ParamId::AntiBanding,
    ParamId::VideoStabilization, ParamId::OpticalStabilization, ParamId::NoiseReduction,
    ParamId::EdgeMode, ParamId::ScreenMode,
};
const size_t kAllParamsCount = sizeof(kAllParams) / sizeof(kAllParams[0]);

namespace {

constexpr int kMaxBitrateFallback = 100000000;  // when MediaCodecList is unavailable
constexpr int kMinBitrate = 500000;

ParamDesc num(ParamId id, ValueType t, double mn, double mx, double step, ParamValue cur,
              bool supported, bool reconf = false) {
  ParamDesc d;
  d.id = id;
  d.type = t;
  d.min = mn;
  d.max = mx;
  d.step = step;
  d.current = cur;
  d.flags = static_cast<uint8_t>((supported ? kParamSupported : 0) | (reconf ? kParamReconfigures : 0));
  return d;
}

ParamDesc boolean(ParamId id, bool cur, bool supported, bool reconf = false) {
  return num(id, ValueType::Bool, 0, 1, 1, ParamValue::Bool(cur), supported, reconf);
}

ParamDesc enumeration(ParamId id, int cur, std::vector<EnumOption> opts, bool supported,
                      bool reconf = false) {
  double mn = 0, mx = 0;
  if (!opts.empty()) {
    mn = mx = opts[0].value;
    for (const auto& o : opts) {
      mn = std::min<double>(mn, o.value);
      mx = std::max<double>(mx, o.value);
    }
  }
  ParamDesc d = num(id, ValueType::Enum, mn, mx, 1, ParamValue::EnumV(cur), supported, reconf);
  d.options = std::move(opts);
  return d;
}

const char* awbLabel(int m) {
  static const char* k[] = {"off", "auto", "incandescent", "fluorescent", "warm-fluorescent",
                            "daylight", "cloudy-daylight", "twilight", "shade"};
  return m >= 0 && m < 9 ? k[m] : "mode";
}
const char* nrLabel(int m) {
  static const char* k[] = {"off", "fast", "high-quality", "minimal", "zero-shutter-lag"};
  return m >= 0 && m < 5 ? k[m] : "mode";
}
const char* edgeLabel(int m) {
  static const char* k[] = {"off", "fast", "high-quality", "zero-shutter-lag"};
  return m >= 0 && m < 4 ? k[m] : "mode";
}
const char* bandLabel(int m) {
  static const char* k[] = {"off", "50Hz", "60Hz", "auto"};
  return m >= 0 && m < 4 ? k[m] : "mode";
}

int64_t asInt(const ParamValue& v) {
  return v.type == ValueType::Float ? static_cast<int64_t>(std::llround(v.f)) : v.i;
}
float asFloat(const ParamValue& v) {
  return v.type == ValueType::Float ? v.f : static_cast<float>(v.i);
}

}  // namespace

void ParamStore::init(const std::vector<CameraDesc>* cams, const EncoderCapsSet* enc, bool hsBackend) {
  cams_ = cams;
  enc_ = enc;
  hsBackend_ = hsBackend;
  stream = StreamConfig{};
  screenMode = 0;
  if (!ready()) return;
  stream.cameraIndex = defaultCameraIndex(*cams_);
  VideoSize s = pickDefaultSize(camera());
  stream.width = s.width;
  stream.height = s.height;
  stream.fps = std::min(30, camera().maxFpsFor(s.width, s.height));
  stream.codec = enc_->avc.available || !enc_->hevc.available ? Codec::H264 : Codec::Hevc;
  stream.bitrate = 8000000;
  stream.bitrateMode = 0;
  // CBR may be unsupported (MediaCodecList said so): fall back to VBR.
  const EncoderCaps& ec = enc_->get(stream.codec);
  if (ec.bitrateModes && !(ec.bitrateModes & (1u << mc::kBitrateModeCBR))) stream.bitrateMode = 1;
  // Profile: High (CABAC, still 0 B-frames) when available, else Main, else
  // Baseline. Measured on the PC: +0.5 ms decode, decoder delay stays 0, and
  // Baseline/CAVLC left ~7 % redundancy. HEVC: Main.
  if (stream.codec == Codec::H264) {
    stream.profile = ec.supportsProfile(mc::kAvcHigh) ? 2 : (ec.supportsProfile(mc::kAvcMain) ? 1 : 0);
  }
  stream.profile = static_cast<int>(debugProp("profile", stream.profile));
  controls = defaultControls(camera());
}

void ParamStore::resetCameraControls() {
  if (ready()) controls = defaultControls(camera());
}

bool ParamStore::sizeAllowed(int w, int h) const {
  const EncoderCaps& ec = enc_->get(stream.codec);
  if (ec.maxWidth > 0 && ec.maxHeight > 0) {
    // Encoders usually accept the rotated size too (e.g. 1080x1920).
    const int a = std::max(ec.maxWidth, ec.maxHeight), b = std::min(ec.maxWidth, ec.maxHeight);
    if (std::max(w, h) > a || std::min(w, h) > b) return false;
  }
  return true;
}

void ParamStore::setActualSize(int w, int h) {
  stream.width = w;
  stream.height = h;
}

ParamValue ParamStore::current(ParamId id) const { return describe(id).current; }

bool ParamStore::highSpeedPossible() const { return hsBackend_ && !camera().hsConfigs.empty(); }

int ParamStore::cameraFps() const {
  if (stream.highSpeed) {
    int f = camera().hsCameraFps(stream.width, stream.height);
    if (f > 0) return f;
  }
  return stream.fps;
}

std::vector<VideoSize> ParamStore::activeSizes() const {
  std::vector<VideoSize> out;
  for (const auto& s : stream.highSpeed ? camera().hsSizes() : camera().info.sizes)
    if (sizeAllowed(s.width, s.height)) out.push_back(s);
  return out;
}

std::pair<int, int> ParamStore::fpsRange() const {
  if (stream.highSpeed) {
    // Fps = encoder output rate; the sensor runs at the high-speed rate and the
    // surplus is dropped before the encoder (max-fps-to-encoder).
    const int mx = std::max(1, camera().hsCameraFps(stream.width, stream.height));
    return {std::min(30, mx), mx};
  }
  const int mx = camera().maxFpsFor(stream.width, stream.height);
  return {std::min(camera().minAeFps, mx), mx};
}

// Controls a constrained high-speed request ignores (CameraConstrainedHighSpeedCaptureSession docs).
static bool blockedInHighSpeed(ParamId id) {
  switch (id) {
    case ParamId::FocusDistance:
    case ParamId::AeMode:
    case ParamId::Iso:
    case ParamId::ExposureTimeNs:
    case ParamId::AntiBanding:
    case ParamId::VideoStabilization:
    case ParamId::OpticalStabilization:
    case ParamId::NoiseReduction:
    case ParamId::EdgeMode:
      return true;
    default:
      return false;
  }
}

ParamDesc ParamStore::describe(ParamId id) const {
  ParamDesc d = describeRaw(id);
  if (stream.highSpeed) {
    if (blockedInHighSpeed(id)) d.flags = static_cast<uint8_t>(d.flags & ~kParamSupported);
    if (id == ParamId::AfMode) {
      d.options.erase(std::remove_if(d.options.begin(), d.options.end(),
                                     [](const EnumOption& o) { return o.value == 0; }),
                      d.options.end());
      if (!d.options.empty()) d.min = d.options.front().value;
    }
  }
  return d;
}

ParamDesc ParamStore::describeRaw(ParamId id) const {
  const CameraDesc& cd = camera();
  const EncoderCaps& ec = enc_->get(stream.codec);
  const int api = enc_->apiLevel;
  switch (id) {
    // ------------------------------------------------------------ stream
    case ParamId::CameraIndex:
      return num(id, ValueType::Int32, 0, static_cast<double>(cams_->size() - 1), 1,
                 ParamValue::Int(stream.cameraIndex), cams_->size() > 1, true);
    case ParamId::VideoWidth:
    case ParamId::VideoHeight: {
      int mn = 1 << 30, mx = 0;
      for (const auto& s : activeSizes()) {
        int v = id == ParamId::VideoWidth ? s.width : s.height;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
      }
      if (mx == 0) mn = 0;
      // Discrete: valid pairs are CameraInfo::sizes (filtered by encoder limits).
      return num(id, ValueType::Int32, mn, mx, 2,
                 ParamValue::Int(id == ParamId::VideoWidth ? stream.width : stream.height), true, true);
    }
    case ParamId::Fps: {
      auto r = fpsRange();
      return num(id, ValueType::Int32, r.first, r.second, 1, ParamValue::Int(stream.fps), true, true);
    }
    case ParamId::HighSpeedMode: {
      std::vector<EnumOption> o{{0, "normal"}};
      if (highSpeedPossible()) {
        int best = 0;
        for (const auto& c : cd.hsConfigs) best = std::max(best, c.fpsMax);
        o.push_back({1, "high-speed (up to " + std::to_string(best) + " fps, experimental)"});
      }
      return enumeration(id, stream.highSpeed, o, highSpeedPossible(), true);
    }
    case ParamId::VideoCodec: {
      std::vector<EnumOption> o;
      if (enc_->avc.available) o.push_back({static_cast<int32_t>(Codec::H264), "H.264"});
      if (enc_->hevc.available) o.push_back({static_cast<int32_t>(Codec::Hevc), "HEVC"});
      return enumeration(id, static_cast<int>(stream.codec), o, o.size() > 1, true);
    }
    case ParamId::Bitrate: {
      int mx = ec.maxBitrate > 0 ? ec.maxBitrate : kMaxBitrateFallback;
      return num(id, ValueType::Int32, kMinBitrate, std::max(mx, kMinBitrate), 50000,
                 ParamValue::Int(stream.bitrate), true, false);
    }
    case ParamId::KeyframeIntervalMs:
      return num(id, ValueType::Int32, 0, 60000, 100, ParamValue::Int(stream.keyframeMs), true, true);
    case ParamId::BitrateMode: {
      std::vector<EnumOption> o;
      const uint32_t m = ec.bitrateModes;
      if (!m || (m & (1u << mc::kBitrateModeCBR))) o.push_back({0, "CBR"});
      if (!m || (m & (1u << mc::kBitrateModeVBR))) o.push_back({1, "VBR"});
      if (!m || (m & (1u << mc::kBitrateModeCQ))) o.push_back({2, "CQ"});
      return enumeration(id, stream.bitrateMode, o, !o.empty(), true);
    }
    case ParamId::IntraRefreshFrames:
      return num(id, ValueType::Int32, 0, 600, 1, ParamValue::Int(stream.intraRefresh),
                 ec.intraRefresh || !ec.javaInfo, true);
    case ParamId::FecPercent:
      return num(id, ValueType::Int32, -1, 100, 1, ParamValue::Int(stream.fecPercent),
                 kFecImplemented, false);
    case ParamId::AdaptiveBitrate:
      return boolean(id, stream.abr, true);
    case ParamId::EncoderProfile: {
      std::vector<EnumOption> o;
      if (stream.codec == Codec::Hevc) {
        o.push_back({0, "Main"});
        // Main10 only when MediaCodecList confirms it (camera surfaces are 8-bit).
        if (ec.javaInfo && ec.supportsProfile(mc::kHevcMain10)) o.push_back({2, "Main10"});
      } else {
        if (ec.supportsProfile(mc::kAvcBaseline)) o.push_back({0, "Baseline"});
        if (ec.supportsProfile(mc::kAvcMain)) o.push_back({1, "Main"});
        if (ec.supportsProfile(mc::kAvcHigh)) o.push_back({2, "High"});
      }
      return enumeration(id, stream.profile, o, !o.empty(), true);
    }
    case ParamId::MaxSendQueueFrames:
      return num(id, ValueType::Int32, 1, 60, 1, ParamValue::Int(stream.maxQueue), true, false);
    case ParamId::BFrames: {
      // max-bframes is API 29+; the maximum the component accepts is probed at
      // startup (OMX.qcom AVC on SDM845: 1). Whether it really emits B-frames
      // is checked at runtime (PTS reordering, logged to the PC).
      const int mx = ec.maxBFrames >= 0 ? ec.maxBFrames : 4;
      bool ok = api >= 29 && mx > 0 &&
                (stream.codec == Codec::Hevc || ec.supportsProfile(mc::kAvcMain) ||
                 ec.supportsProfile(mc::kAvcHigh));
      return num(id, ValueType::Int32, 0, std::max(mx, 0), 1, ParamValue::Int(stream.bframes), ok, true);
    }
    case ParamId::QpMin:
      return num(id, ValueType::Int32, 0, 51, 1, ParamValue::Int(stream.qpMin), ec.qpSupported(api), true);
    case ParamId::QpMax:
      return num(id, ValueType::Int32, 0, 51, 1, ParamValue::Int(stream.qpMax), ec.qpSupported(api), true);
    case ParamId::LatencyDropPolicy:
      return enumeration(id, stream.dropPolicy, {{0, "drop-to-keyframe"}, {1, "queue"}}, true);

    // ------------------------------------------------------------ camera
    case ParamId::AfMode: {
      std::vector<EnumOption> o;
      static const char* labels[] = {"off (manual)", "continuous-video", "continuous-picture", "auto"};
      for (int m = 0; m < 4; ++m)
        if (cameraSupportsAfMode(cd, m)) o.push_back({m, labels[m]});
      return enumeration(id, controls.afMode, o, o.size() > 1);
    }
    case ParamId::FocusDistance:
      return num(id, ValueType::Float, 0, cd.minFocusDistance, 0.01,
                 ParamValue::Real(controls.focusDistance), cameraSupportsAfMode(cd, 0));
    case ParamId::AeMode: {
      bool manual = cd.hasMode(cd.aeModes, ACAMERA_CONTROL_AE_MODE_OFF) && cd.isoMax > 0 && cd.expMax > 0;
      std::vector<EnumOption> o;
      if (manual) o.push_back({0, "off (manual)"});
      o.push_back({1, "on"});
      return enumeration(id, controls.aeMode, o, manual);
    }
    case ParamId::AeLock:
      return boolean(id, controls.aeLock, cd.aeLockAvailable);
    case ParamId::ExposureCompensation: {
      ParamDesc d = num(id, ValueType::Int32, cd.aeCompMin, cd.aeCompMax, 1,
                        ParamValue::Int(controls.aeCompensation), cd.aeCompMax > cd.aeCompMin);
      return d;
    }
    case ParamId::Iso: {
      bool manual = cd.hasMode(cd.aeModes, ACAMERA_CONTROL_AE_MODE_OFF) && cd.isoMax > 0;
      return num(id, ValueType::Int32, cd.isoMin, cd.isoMax, 1, ParamValue::Int(controls.iso), manual);
    }
    case ParamId::ExposureTimeNs: {
      bool manual = cd.hasMode(cd.aeModes, ACAMERA_CONTROL_AE_MODE_OFF) && cd.expMax > 0;
      return num(id, ValueType::Int64, static_cast<double>(cd.expMin), static_cast<double>(cd.expMax),
                 1000, ParamValue::Long(controls.exposureNs), manual);
    }
    case ParamId::AwbMode: {
      std::vector<EnumOption> o;
      for (uint8_t m : cd.awbModes) o.push_back({m, awbLabel(m)});
      return enumeration(id, controls.awbMode, o, o.size() > 1);
    }
    case ParamId::AwbLock:
      return boolean(id, controls.awbLock, cd.awbLockAvailable);
    case ParamId::Zoom:
      return num(id, ValueType::Float, 1.0, cd.info.max_zoom, 0.1, ParamValue::Real(controls.zoom),
                 cd.info.max_zoom > 1.0f && cd.activeW > 0);
    case ParamId::Torch:
      return boolean(id, controls.torch, cd.info.has_flash);
    case ParamId::AntiBanding: {
      std::vector<EnumOption> o;
      for (uint8_t m : cd.antibandingModes) o.push_back({m, bandLabel(m)});
      return enumeration(id, controls.antibanding, o, o.size() > 1);
    }
    case ParamId::VideoStabilization:
      return boolean(id, controls.videoStabilization,
                     cd.hasMode(cd.vstabModes, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON));
    case ParamId::OpticalStabilization:
      return boolean(id, controls.opticalStabilization,
                     cd.hasMode(cd.oisModes, ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON));
    case ParamId::NoiseReduction: {
      std::vector<EnumOption> o;
      for (uint8_t m : cd.nrModes) o.push_back({m, nrLabel(m)});
      return enumeration(id, controls.noiseReduction, o, o.size() > 1);
    }
    case ParamId::EdgeMode: {
      std::vector<EnumOption> o;
      for (uint8_t m : cd.edgeModes) o.push_back({m, edgeLabel(m)});
      return enumeration(id, controls.edgeMode, o, o.size() > 1);
    }
    case ParamId::ScreenMode:
      return enumeration(id, screenMode, {{0, "normal"}, {1, "black"}}, true);
  }
  ParamDesc unknown;
  unknown.id = id;
  return unknown;
}

std::vector<ParamDesc> ParamStore::describeAll() const {
  std::vector<ParamDesc> v;
  v.reserve(kAllParamsCount);
  for (size_t i = 0; i < kAllParamsCount; ++i) v.push_back(describe(kAllParams[i]));
  return v;
}

Capabilities ParamStore::capabilities() const {
  Capabilities c;
  c.active_camera = static_cast<uint16_t>(stream.cameraIndex);
  for (const auto& cd : *cams_) {
    CameraInfo ci = cd.info;
    // High-speed mode: the active camera advertises its high-speed sizes
    // (max_fps = sensor rate) so the PC's size list matches what is valid.
    if (stream.highSpeed && &cd == &camera()) ci.sizes = cd.hsSizes();
    ci.sizes.erase(std::remove_if(ci.sizes.begin(), ci.sizes.end(),
                                  [&](const VideoSize& s) { return !sizeAllowed(s.width, s.height); }),
                   ci.sizes.end());
    c.cameras.push_back(std::move(ci));
  }
  c.params = describeAll();
  return c;
}

void ParamStore::clampFps(Result& r) {
  auto range = fpsRange();
  int f = std::clamp(stream.fps, range.first, range.second);
  if (f != stream.fps) {
    stream.fps = f;
    r.alsoChanged.push_back(ParamId::Fps);
  }
}

void ParamStore::applyCameraChange(int index, Result& r) {
  stream.cameraIndex = index;
  const CameraDesc& cd = camera();
  if (stream.highSpeed && !highSpeedPossible()) {
    stream.highSpeed = 0;
    r.alsoChanged.push_back(ParamId::HighSpeedMode);
  }
  if (stream.highSpeed && cd.hsCameraFps(stream.width, stream.height) == 0) {
    VideoSize s = cd.hsSizes().front();
    stream.width = s.width;
    stream.height = s.height;
    r.alsoChanged.push_back(ParamId::VideoWidth);
    r.alsoChanged.push_back(ParamId::VideoHeight);
  } else if (!stream.highSpeed &&
             (!cd.supportsSize(stream.width, stream.height) || !sizeAllowed(stream.width, stream.height))) {
    VideoSize s = pickDefaultSize(cd);
    stream.width = s.width;
    stream.height = s.height;
    r.alsoChanged.push_back(ParamId::VideoWidth);
    r.alsoChanged.push_back(ParamId::VideoHeight);
  }
  clampFps(r);
  controls = defaultControls(cd);
  r.effects |= kCaps;  // camera params differ per camera
}

ParamStore::Result ParamStore::set(ParamId id, const ParamValue& v) {
  Result r;
  const ParamDesc d = describe(id);
  if (!(d.flags & kParamSupported) && id != ParamId::FecPercent) {
    r.status = ParamStatus::Unsupported;
    r.value = d.current;
    return r;
  }
  // Generic range / option validation against the advertised descriptor.
  int64_t iv = asInt(v);
  float fv = asFloat(v);
  bool inRange = true;
  switch (d.type) {
    case ValueType::Bool: iv = iv ? 1 : 0; break;
    case ValueType::Enum: {
      inRange = std::any_of(d.options.begin(), d.options.end(),
                            [&](const EnumOption& o) { return o.value == iv; });
      // HEVC accepts 1 as "Main" too (protocol: 0/1 = Main).
      if (!inRange && id == ParamId::EncoderProfile && stream.codec == Codec::Hevc && iv == 1) {
        iv = 0;
        inRange = true;
      }
      break;
    }
    case ValueType::Float:
      inRange = fv >= d.min - 1e-6 && fv <= d.max + 1e-6;
      break;
    default:
      inRange = static_cast<double>(iv) >= d.min && static_cast<double>(iv) <= d.max;
  }
  if (!inRange) {
    r.status = ParamStatus::OutOfRange;
    r.value = d.current;
    return r;
  }
  const int i32 = static_cast<int>(iv);
  switch (id) {
    case ParamId::CameraIndex:
      if (i32 != stream.cameraIndex) {
        applyCameraChange(i32, r);
        r.effects |= kReconfigure;
      }
      break;
    case ParamId::VideoWidth:
    case ParamId::VideoHeight: {
      // Pick the supported size with this width (height) whose other side is
      // closest to the current one; the PC typically sends both in a row.
      const bool isW = id == ParamId::VideoWidth;
      const VideoSize* best = nullptr;
      int bestDiff = 1 << 30;
      const std::vector<VideoSize> sizes = activeSizes();
      for (const auto& s : sizes) {
        if ((isW ? s.width : s.height) != i32) continue;
        int diff = std::abs((isW ? s.height : s.width) - (isW ? stream.height : stream.width));
        if (diff < bestDiff) {
          bestDiff = diff;
          best = &s;
        }
      }
      if (!best) {
        r.status = ParamStatus::OutOfRange;
        r.value = d.current;
        return r;
      }
      if (best->width != stream.width || best->height != stream.height) {
        if (isW && best->height != stream.height) r.alsoChanged.push_back(ParamId::VideoHeight);
        if (!isW && best->width != stream.width) r.alsoChanged.push_back(ParamId::VideoWidth);
        stream.width = best->width;
        stream.height = best->height;
        clampFps(r);
        r.effects |= kReconfigure | kCaps;  // Fps range depends on the size
      }
      break;
    }
    case ParamId::Fps:
      if (i32 != stream.fps) { stream.fps = i32; r.effects |= kReconfigure; }
      break;
    case ParamId::VideoCodec: {
      Codec c = static_cast<Codec>(i32);
      if (c != stream.codec) {
        stream.codec = c;
        if (c == Codec::Hevc && stream.profile == 1) stream.profile = 0;
        if (c == Codec::Hevc && stream.profile == 2 && !(enc_->hevc.javaInfo && enc_->hevc.supportsProfile(mc::kHevcMain10)))
          stream.profile = 0;
        r.alsoChanged.push_back(ParamId::EncoderProfile);
        r.effects |= kReconfigure | kCaps;
      }
      break;
    }
    case ParamId::Bitrate:
      stream.bitrate = i32;
      r.effects |= kBitrate;
      break;
    case ParamId::KeyframeIntervalMs:
      if (i32 != stream.keyframeMs) { stream.keyframeMs = i32; r.effects |= kReconfigure; }
      break;
    case ParamId::BitrateMode:
      if (i32 != stream.bitrateMode) { stream.bitrateMode = i32; r.effects |= kReconfigure; }
      break;
    case ParamId::IntraRefreshFrames:
      if (i32 != stream.intraRefresh) { stream.intraRefresh = i32; r.effects |= kReconfigure; }
      break;
    case ParamId::FecPercent:
      stream.fecPercent = i32;
      r.effects |= kFec;
      if (!kFecImplemented) r.status = ParamStatus::Unsupported;
      break;
    case ParamId::AdaptiveBitrate:
      stream.abr = iv != 0;
      r.effects |= kAbr;
      break;
    case ParamId::EncoderProfile:
      if (i32 != stream.profile) { stream.profile = i32; r.effects |= kReconfigure; }
      break;
    case ParamId::MaxSendQueueFrames:
      stream.maxQueue = i32;
      r.effects |= kQueue;
      break;
    case ParamId::BFrames:
      if (i32 != stream.bframes) { stream.bframes = i32; r.effects |= kReconfigure; }
      break;
    case ParamId::QpMin:
      if (i32 != stream.qpMin) { stream.qpMin = i32; r.effects |= kReconfigure; }
      break;
    case ParamId::QpMax:
      if (i32 != stream.qpMax) { stream.qpMax = i32; r.effects |= kReconfigure; }
      break;
    case ParamId::LatencyDropPolicy:
      stream.dropPolicy = i32;
      r.effects |= kQueue;
      break;
    case ParamId::AfMode: controls.afMode = i32; r.effects |= kControls; break;
    case ParamId::FocusDistance: controls.focusDistance = fv; r.effects |= kControls; break;
    case ParamId::AeMode: controls.aeMode = i32; r.effects |= kControls; break;
    case ParamId::AeLock: controls.aeLock = iv != 0; r.effects |= kControls; break;
    case ParamId::ExposureCompensation: controls.aeCompensation = i32; r.effects |= kControls; break;
    case ParamId::Iso: controls.iso = i32; r.effects |= kControls; break;
    case ParamId::ExposureTimeNs: controls.exposureNs = iv; r.effects |= kControls; break;
    case ParamId::AwbMode: controls.awbMode = i32; r.effects |= kControls; break;
    case ParamId::AwbLock: controls.awbLock = iv != 0; r.effects |= kControls; break;
    case ParamId::Zoom: controls.zoom = fv; r.effects |= kControls; break;
    case ParamId::Torch: controls.torch = iv != 0; r.effects |= kControls; break;
    case ParamId::AntiBanding: controls.antibanding = i32; r.effects |= kControls; break;
    case ParamId::VideoStabilization: controls.videoStabilization = iv != 0; r.effects |= kControls; break;
    case ParamId::OpticalStabilization: controls.opticalStabilization = iv != 0; r.effects |= kControls; break;
    case ParamId::NoiseReduction: controls.noiseReduction = i32; r.effects |= kControls; break;
    case ParamId::EdgeMode: controls.edgeMode = i32; r.effects |= kControls; break;
    case ParamId::ScreenMode: screenMode = i32; r.effects |= kScreen; break;
    case ParamId::HighSpeedMode:
      if (i32 != stream.highSpeed) {
        stream.highSpeed = i32;
        const CameraDesc& cd = camera();
        if (i32) {
          // Keep the size if it has a high-speed config, else prefer 1080p, else the largest.
          if (cd.hsCameraFps(stream.width, stream.height) == 0) {
            VideoSize pick = cd.hsSizes().front();
            for (const auto& s : cd.hsSizes())
              if (s.width == 1920 && s.height == 1080) pick = s;
            stream.width = pick.width;
            stream.height = pick.height;
          }
          stream.fps = cd.hsCameraFps(stream.width, stream.height);  // full sensor rate by default
        } else {
          if (!cd.supportsSize(stream.width, stream.height)) {
            VideoSize s = pickDefaultSize(cd);
            stream.width = s.width;
            stream.height = s.height;
          }
          stream.fps = std::min(30, cd.maxFpsFor(stream.width, stream.height));
        }
        r.alsoChanged.push_back(ParamId::VideoWidth);
        r.alsoChanged.push_back(ParamId::VideoHeight);
        r.alsoChanged.push_back(ParamId::Fps);
        r.effects |= kReconfigure | kCaps;
      }
      break;
  }
  r.value = current(id);
  return r;
}

EncoderSettings ParamStore::encoderSettings(int bitrateNow) const {
  EncoderSettings s;
  s.codec = stream.codec;
  s.width = stream.width;
  s.height = stream.height;
  s.fps = stream.fps;
  s.bitrate = bitrateNow > 0 ? bitrateNow : stream.bitrate;
  s.bitrateCeiling = stream.bitrate;
  s.keyframeMs = stream.keyframeMs;
  s.bitrateMode = stream.bitrateMode;
  s.intraRefreshFrames = stream.intraRefresh;
  s.profile = stream.profile;
  s.bframes = stream.bframes;
  if (stream.codec == Codec::H264 && stream.profile == 0) s.bframes = 0;  // Baseline has no B
  const EncoderCaps& ec = enc_->get(stream.codec);
  if (ec.maxBFrames >= 0) s.bframes = std::min(s.bframes, ec.maxBFrames);
  s.qpMin = stream.qpMin;
  s.qpMax = stream.qpMax;
  if (stream.highSpeed && stream.fps < cameraFps()) s.maxFpsToEncoder = stream.fps;
  // High-frame-rate encodes (measured SM-N9600, 720p@240):
  //  - CABAC (Main/High) at > 20 Mbps cannot sustain 240 fps (217 fps, +105 ms
  //    encoder backlog, camera drops); Baseline/CAVLC does (236 fps, +16 ms).
  //  - request-sync is ignored above 60 fps, so a periodic IDR is the only
  //    recovery: clamp the interval to <= 1 s.
  if (s.codec == Codec::H264 && s.fps > 120 && stream.bitrate > 20000000) {
    s.profile = 0;
    s.bframes = 0;
  }
  if (s.fps > kIdrSafeOperatingRate && (s.keyframeMs == 0 || s.keyframeMs > 1000)) s.keyframeMs = 1000;
  return s;
}

}  // namespace owc
