#include "encoder/encoder_options.h"

#include <dlfcn.h>
#include <media/NdkMediaCodec.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "util/log.h"

namespace owc {

// ------------------------------------------------------------------ caps
bool EncoderCaps::hasVendorKeyPrefix(const char* prefix) const {
  for (const auto& k : vendorKeys)
    if (k.rfind(prefix, 0) == 0) return true;
  return false;
}

bool EncoderCaps::supportsProfile(int32_t p) const {
  if (profileLevels.empty()) return true;  // unknown -> let configure decide
  for (const auto& pl : profileLevels)
    if (pl.first == p) return true;
  return false;
}

static bool startsWithAny(const std::string& s, std::initializer_list<const char*> prefixes) {
  for (const char* p : prefixes)
    if (s.rfind(p, 0) == 0) return true;
  return false;
}

bool EncoderCaps::isQcom() const { return startsWithAny(name, {"OMX.qcom.", "c2.qti."}); }
bool EncoderCaps::isExynos() const {
  return startsWithAny(name, {"OMX.Exynos.", "c2.exynos.", "OMX.SEC."});
}

bool EncoderCaps::qpSupported(int apiLevel) const {
  if (!available) return false;
  if (apiLevel >= 31) return true;  // video-qp-min/max (MediaFormat KEY_VIDEO_QP_*)
  return hasVendorKeyPrefix("vendor.qti-ext-enc-qp-range.");
}

uint32_t EncoderCapsSet::codecMask() const {
  uint32_t m = 0;
  if (avc.available) m |= codecBit(Codec::H264);
  if (hevc.available) m |= codecBit(Codec::Hevc);
  return m;
}

int32_t mediaCodecProfile(Codec c, int p) {
  if (c == Codec::Hevc) return p == 2 ? mc::kHevcMain10 : mc::kHevcMain;
  switch (p) {
    case 1: return mc::kAvcMain;
    case 2: return mc::kAvcHigh;
    default: return mc::kAvcBaseline;
  }
}

// ------------------------------------------------------------------ levels
namespace {
struct AvcLevel {
  int32_t level;
  int64_t maxMbps, maxFs, maxBrKbps;
};
// H.264 Table A-1 (MaxMBPS, MaxFS, MaxBR for Baseline/Main; High = x1.25).
constexpr AvcLevel kAvcLevels[] = {
    {0x1, 1485, 99, 64},           {0x4, 3000, 396, 192},
    {0x8, 6000, 396, 384},         {0x10, 11880, 396, 768},
    {0x20, 11880, 396, 2000},      {0x40, 19800, 792, 4000},
    {0x80, 20250, 1620, 4000},     {0x100, 40500, 1620, 10000},
    {0x200, 108000, 3600, 14000},  {0x400, 216000, 5120, 20000},
    {0x800, 245760, 8192, 20000},  {0x1000, 245760, 8192, 50000},
    {0x2000, 522240, 8704, 50000}, {0x4000, 589824, 22080, 135000},
    {0x8000, 983040, 36864, 240000}, {0x10000, 2073600, 36864, 240000},
    {0x20000, 4177920, 139264, 240000}, {0x40000, 8355840, 139264, 480000},
    {0x80000, 16711680, 139264, 800000},
};
struct HevcLevel {
  int32_t level;  // Main tier constant
  int64_t maxLumaPs, maxLumaSr, maxBrKbps;
};
// H.265 Table A.8 (Main tier).
constexpr HevcLevel kHevcLevels[] = {
    {0x1, 36864, 552960, 128},
    {0x4, 122880, 3686400, 1500},
    {0x10, 245760, 7372800, 3000},
    {0x40, 552960, 16588800, 6000},
    {0x100, 983040, 33177600, 10000},
    {0x400, 2228224, 66846720, 12000},
    {0x1000, 2228224, 133693440, 20000},
    {0x4000, 8912896, 267386880, 25000},
    {0x10000, 8912896, 534773760, 40000},
    {0x40000, 8912896, 1069547520, 60000},
    {0x100000, 35651584, 1069547520, 60000},
    {0x400000, 35651584, 2139095040, 120000},
    {0x1000000, 35651584, 4278190080LL, 240000},
};
}  // namespace

int32_t pickLevel(Codec c, int32_t profile, int w, int h, int fps, int bitrate) {
  const int64_t kbps = (static_cast<int64_t>(bitrate) + 999) / 1000;
  if (c == Codec::Hevc) {
    const int64_t ps = static_cast<int64_t>(w) * h;
    const int64_t sr = ps * fps;
    for (const auto& l : kHevcLevels)
      if (ps <= l.maxLumaPs && sr <= l.maxLumaSr && kbps <= l.maxBrKbps) return l.level;
    return kHevcLevels[std::size(kHevcLevels) - 1].level;
  }
  const int64_t mbs = static_cast<int64_t>((w + 15) / 16) * ((h + 15) / 16);
  const int64_t mbps = mbs * fps;
  const int64_t brFactor = profile == mc::kAvcHigh ? 125 : 100;
  for (const auto& l : kAvcLevels)
    if (mbs <= l.maxFs && mbps <= l.maxMbps && kbps * 100 <= l.maxBrKbps * brFactor) return l.level;
  return kAvcLevels[std::size(kAvcLevels) - 1].level;
}

// ------------------------------------------------------------------ table
namespace {

enum class Kind { I32, F32 };

using ValueFn = bool (*)(const EncoderSettings&, const EncoderCaps&, int api, double& out);

struct FormatKey {
  const char* key;
  KeyTier tier;
  Kind kind;
  ValueFn value;  // returns false => key not set
  // Vendor rows only: codec name must match (isQcom/isExynos/...).
  bool (*vendorMatch)(const EncoderCaps&);
};

bool qcom(const EncoderCaps& c) { return c.isQcom(); }
bool exynos(const EncoderCaps& c) { return c.isExynos(); }
bool qpOn(const EncoderSettings& s) { return s.qpMin > 0 || s.qpMax > 0; }

// THE table. Order matters only for readability. Values documented inline;
// see docs/LOW_LATENCY_RESEARCH.md §2.1 for sources.
const FormatKey kFormatKeys[] = {
    // ---- required
    {"width", KeyTier::Required, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) { v = s.width; return true; }, nullptr},
    {"height", KeyTier::Required, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) { v = s.height; return true; }, nullptr},
    {"color-format", KeyTier::Required, Kind::I32,
     [](const EncoderSettings&, const EncoderCaps&, int, double& v) { v = mc::kColorFormatSurface; return true; }, nullptr},
    {"bitrate", KeyTier::Required, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) { v = s.bitrate; return true; }, nullptr},
    // Real camera fps (fixed AE target range), not scrcpy's 60.
    {"frame-rate", KeyTier::Required, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) { v = s.fps; return true; }, nullptr},
    // Seconds (float accepted since API 25). 0 ms => "only on request": 1 h.
    {"i-frame-interval", KeyTier::Required, Kind::F32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) {
       v = s.keyframeMs > 0 ? s.keyframeMs / 1000.0 : 3600.0;
       return true;
     }, nullptr},
    // High-speed decimation (e.g. 120 fps sensor -> 60 fps encoder): the
    // framework GraphicBufferSource drops excess frames before the encoder.
    {"max-fps-to-encoder", KeyTier::Required, Kind::F32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) {
       v = s.maxFpsToEncoder;
       return s.maxFpsToEncoder > 0;
     }, nullptr},
    // ---- degradable (stage 3 removes)
    {"bitrate-mode", KeyTier::Degradable, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) {
       v = s.bitrateMode == 1 ? mc::kBitrateModeVBR
                              : (s.bitrateMode == 2 ? mc::kBitrateModeCQ : mc::kBitrateModeCBR);
       return true;
     }, nullptr},
    {"profile", KeyTier::Degradable, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) {
       v = mediaCodecProfile(s.codec, s.profile);
       return true;
     }, nullptr},
    // ACodec (OMX) rejects "profile" without "level"; we pick the lowest level
    // that fits size x fps x bitrate-ceiling.
    {"level", KeyTier::Degradable, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) {
       v = pickLevel(s.codec, mediaCodecProfile(s.codec, s.profile), s.width, s.height, s.fps,
                     std::max(s.bitrate, s.bitrateCeiling));
       return true;
     }, nullptr},
    // ---- optional standard keys (stage 2 removes)
    {"priority", KeyTier::Optional, Kind::I32,  // 0 = realtime
     [](const EncoderSettings&, const EncoderCaps&, int, double& v) { v = 0; return true; }, nullptr},
    // Output as soon as one frame is in (string key works from API 26; public const API 30).
    // Meaningless with B-frames, so only set when bframes == 0.
    {"latency", KeyTier::Optional, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) { v = 1; return s.bframes == 0; }, nullptr},
    // Clock the encoder for the real rate (not Short.MAX: may fail on encoders).
    {"operating-rate", KeyTier::Optional, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) { v = s.fps; return true; }, nullptr},
    {"max-bframes", KeyTier::Optional, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int api, double& v) {
       v = (s.codec == Codec::H264 && s.profile == 0) ? 0 : s.bframes;
       return api >= 29 || s.bframes == 0;
     }, nullptr},
    // SPS/PPS(/VPS) in front of every IDR: UDP resync without the CSD.
    {"prepend-sps-pps-to-idr-frames", KeyTier::Optional, Kind::I32,
     [](const EncoderSettings&, const EncoderCaps&, int, double& v) { v = 1; return true; }, nullptr},
    {"intra-refresh-period", KeyTier::Optional, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) {
       v = s.intraRefreshFrames;
       return s.intraRefreshFrames > 0;
     }, nullptr},
    {"video-qp-min", KeyTier::Optional, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int api, double& v) {
       v = s.qpMin;
       return api >= 31 && s.qpMin > 0;
     }, nullptr},
    {"video-qp-max", KeyTier::Optional, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int api, double& v) {
       v = s.qpMax;
       return api >= 31 && s.qpMax > 0;
     }, nullptr},
    // NOT set on purpose:
    //  repeat-previous-frame-after: camera always delivers frames; repeats only add bitrate.
    //  max-fps-to-encoder (normal mode): the GraphicBufferSource frame dropper (2 ms
    //                      jitter tolerance) can drop real camera frames with normal
    //                      timestamp jitter; only used for high-speed decimation.
    //  color-range/standard: let the camera dataspace flow through.
    // ---- vendor keys (stage 1 removes). Guarded by codec name; when the probe
    // listed the component's vendor keys, also by presence in that list.
    {"vendor.qti-ext-enc-low-latency.enable", KeyTier::Vendor, Kind::I32,
     [](const EncoderSettings&, const EncoderCaps&, int, double& v) { v = 1; return true; }, qcom},
    {"vendor.qti-ext-enc-qp-range.qp-i-min", KeyTier::Vendor, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) { v = s.qpMin > 0 ? s.qpMin : 1; return qpOn(s); }, qcom},
    {"vendor.qti-ext-enc-qp-range.qp-i-max", KeyTier::Vendor, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) { v = s.qpMax > 0 ? s.qpMax : 51; return qpOn(s); }, qcom},
    {"vendor.qti-ext-enc-qp-range.qp-p-min", KeyTier::Vendor, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) { v = s.qpMin > 0 ? s.qpMin : 1; return qpOn(s); }, qcom},
    {"vendor.qti-ext-enc-qp-range.qp-p-max", KeyTier::Vendor, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) { v = s.qpMax > 0 ? s.qpMax : 51; return qpOn(s); }, qcom},
    {"vendor.qti-ext-enc-qp-range.qp-b-min", KeyTier::Vendor, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) { v = s.qpMin > 0 ? s.qpMin : 1; return qpOn(s) && s.bframes > 0; }, qcom},
    {"vendor.qti-ext-enc-qp-range.qp-b-max", KeyTier::Vendor, Kind::I32,
     [](const EncoderSettings& s, const EncoderCaps&, int, double& v) { v = s.qpMax > 0 ? s.qpMax : 51; return qpOn(s) && s.bframes > 0; }, qcom},
    // [unverified] Exynos analogue of vendor.rtc-ext-dec-low-latency.enable.
    {"vendor.rtc-ext-enc-low-latency.enable", KeyTier::Vendor, Kind::I32,
     [](const EncoderSettings&, const EncoderCaps&, int, double& v) { v = 1; return true; }, exynos},
};

bool hasKey(const std::vector<std::string>& keys, const char* k) {
  return std::find(keys.begin(), keys.end(), k) != keys.end();
}

}  // namespace

AMediaFormat* buildEncoderFormat(const EncoderSettings& s, const EncoderCaps& caps, int apiLevel,
                                 int stage, std::vector<AppliedKey>* applied) {
  AMediaFormat* f = AMediaFormat_new();
  const char* mime = s.codec == Codec::Hevc ? "video/hevc" : "video/avc";
  AMediaFormat_setString(f, "mime", mime);
  if (applied) applied->push_back({"mime", mime, KeyTier::Required});
  for (const auto& k : kFormatKeys) {
    if (stage >= 1 && k.tier == KeyTier::Vendor) continue;
    if (stage >= 2 && k.tier == KeyTier::Optional) continue;
    if (stage >= 3 && k.tier == KeyTier::Degradable) continue;
    if (k.vendorMatch) {
      if (!k.vendorMatch(caps)) continue;
      if (!caps.vendorKeys.empty() && !hasKey(caps.vendorKeys, k.key)) continue;
    }
    double v = 0;
    if (!k.value(s, caps, apiLevel, v)) continue;
    char buf[32];
    if (k.kind == Kind::I32) {
      AMediaFormat_setInt32(f, k.key, static_cast<int32_t>(v));
      snprintf(buf, sizeof(buf), "%d", static_cast<int32_t>(v));
    } else {
      AMediaFormat_setFloat(f, k.key, static_cast<float>(v));
      snprintf(buf, sizeof(buf), "%g", v);
    }
    if (applied) applied->push_back({k.key, buf, k.tier});
  }
  return f;
}

// ------------------------------------------------------------------ probing
std::string codecName(void* codec) {
  using GetName = media_status_t (*)(AMediaCodec*, char**);
  using ReleaseName = void (*)(AMediaCodec*, char*);
  static GetName getName = reinterpret_cast<GetName>(dlsym(RTLD_DEFAULT, "AMediaCodec_getName"));
  static ReleaseName releaseName =
      reinterpret_cast<ReleaseName>(dlsym(RTLD_DEFAULT, "AMediaCodec_releaseName"));
  if (!getName || !codec) return {};
  char* n = nullptr;
  std::string out;
  if (getName(static_cast<AMediaCodec*>(codec), &n) == AMEDIA_OK && n) {
    out = n;
    if (releaseName) releaseName(static_cast<AMediaCodec*>(codec), n);
  }
  return out;
}

static bool isSoftwareName(const std::string& n) {
  return n.rfind("OMX.google.", 0) == 0 || n.rfind("c2.android.", 0) == 0;
}

// Extracts "vendor.xxx" keys from AMediaFormat_toString() output, which looks
// like "mime: string(video/avc), vendor.qti-ext-enc-low-latency.enable: int32(0), ...".
static std::vector<std::string> parseVendorKeys(const char* s) {
  std::vector<std::string> out;
  if (!s) return out;
  const char* p = s;
  while ((p = strstr(p, "vendor.")) != nullptr) {
    const char* e = p;
    while (*e && *e != ':' && *e != ',' && *e != ' ') ++e;
    out.emplace_back(p, static_cast<size_t>(e - p));
    p = e;
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// One configure attempt on a fresh codec instance; returns the input+output
// format strings (vendor.* keys the component accepted show up there).
static bool tryConfigure(const EncoderCaps& caps, const EncoderSettings& s, int api, int stage,
                         std::string* formats) {
  AMediaCodec* c = AMediaCodec_createCodecByName(caps.name.c_str());
  if (!c) return false;
  AMediaFormat* f = buildEncoderFormat(s, caps, api, stage, nullptr);
  const bool ok = AMediaCodec_configure(c, f, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK;
  AMediaFormat_delete(f);
  if (ok && formats) {
    formats->clear();
    if (AMediaFormat* out = AMediaCodec_getOutputFormat(c)) {
      const char* str = AMediaFormat_toString(out);
      *formats += str ? str : "";
      AMediaFormat_delete(out);
    }
    // API 28+: the input format reflects the parameters the component accepted.
    using GetInputFormat = AMediaFormat* (*)(AMediaCodec*);
    static GetInputFormat gif =
        reinterpret_cast<GetInputFormat>(dlsym(RTLD_DEFAULT, "AMediaCodec_getInputFormat"));
    if (gif) {
      if (AMediaFormat* in = gif(c)) {
        const char* str = AMediaFormat_toString(in);
        *formats += " | ";
        *formats += str ? str : "";
        AMediaFormat_delete(in);
      }
    }
  }
  AMediaCodec_stop(c);
  AMediaCodec_delete(c);
  return ok;
}

static void probeOne(EncoderCaps& caps, Codec codec, int api) {
  caps.mime = codec == Codec::Hevc ? "video/hevc" : "video/avc";
  AMediaCodec* c = nullptr;
  if (!caps.name.empty()) c = AMediaCodec_createCodecByName(caps.name.c_str());
  if (!c) c = AMediaCodec_createEncoderByType(caps.mime.c_str());
  if (!c) {
    caps.available = false;
    LOGI("probe: no %s encoder", caps.mime.c_str());
    return;
  }
  std::string n = codecName(c);
  AMediaCodec_delete(c);
  if (!n.empty()) caps.name = n;
  if (caps.name.empty()) {
    caps.available = false;
    LOGW("probe: %s encoder has no name (API < 28?); disabled", caps.mime.c_str());
    return;
  }
  caps.hardware = !isSoftwareName(caps.name);
  // A software HEVC encoder is far too slow for live video; AVC SW is a last resort.
  caps.available = caps.hardware || codec == Codec::H264;

  EncoderSettings s;
  s.codec = codec;
  s.width = 1280;
  s.height = 720;
  s.fps = 30;
  s.bitrate = s.bitrateCeiling = 4000000;
  // 1) baseline config, full table: which stage works, which vendor keys stick.
  caps.vendorKeys.clear();
  std::string fmts;
  int okStage = -1;
  for (int stage = 0; stage < kLadderStages && okStage < 0; ++stage)
    if (tryConfigure(caps, s, api, stage, &fmts)) okStage = stage;
  if (okStage < 0) {
    LOGW("encoder %s could not be configured at 1280x720; disabled", caps.name.c_str());
    caps.available = false;
    return;
  }
  caps.vendorKeys = parseVendorKeys(fmts.c_str());
  LOGI("probe: %s stage %d formats: %s", caps.name.c_str(), okStage, fmts.c_str());

  // 2) QP range through vendor keys (API < 31): accepted keys are echoed back.
  if (api < 31 && caps.isQcom()) {
    EncoderSettings q = s;
    q.qpMin = 10;
    q.qpMax = 45;
    EncoderCaps tmp = caps;
    tmp.vendorKeys.clear();  // do not filter: we are testing them
    if (tryConfigure(tmp, q, api, 0, &fmts)) {
      for (const auto& k : parseVendorKeys(fmts.c_str()))
        if (k.rfind("vendor.qti-ext-enc-qp-range.", 0) == 0) caps.vendorKeys.push_back(k);
    }
  }

  // 3) maximum B-frames (Main/High for AVC, Main for HEVC); API 29+ key.
  caps.maxBFrames = 0;
  if (api >= 29) {
    EncoderSettings b = s;
    b.profile = codec == Codec::Hevc ? 0 : 2;
    for (int nb = 4; nb >= 1; --nb) {
      b.bframes = nb;
      if (tryConfigure(caps, b, api, 1, nullptr)) {
        caps.maxBFrames = nb;
        break;
      }
    }
  }
  std::sort(caps.vendorKeys.begin(), caps.vendorKeys.end());
  caps.vendorKeys.erase(std::unique(caps.vendorKeys.begin(), caps.vendorKeys.end()), caps.vendorKeys.end());
  std::string keys;
  for (const auto& k : caps.vendorKeys) keys += k + " ";
  LOGI("probe: %s -> %s (%s), max B-frames %d, vendor keys: %s", caps.mime.c_str(), caps.name.c_str(),
       caps.available ? (caps.hardware ? "hw" : "sw") : "unavailable", caps.maxBFrames, keys.c_str());
}

void probeEncoders(EncoderCapsSet& set, int apiLevel) {
  set.apiLevel = apiLevel;
  probeOne(set.avc, Codec::H264, apiLevel);
  probeOne(set.hevc, Codec::Hevc, apiLevel);
}

}  // namespace owc
