// Encoder capabilities + the single MediaFormat option table.
//
// Every key the encoder may receive lives in kFormatKeys (encoder_options.cpp),
// tagged with a degradation tier used by the configure fallback ladder
// (docs/LOW_LATENCY_RESEARCH.md §2.2):
//   stage 0: everything
//   stage 1: drop Tier::Vendor            (vendor.* extensions)
//   stage 2: drop Tier::Optional          (latency, priority, operating-rate, max-bframes,
//                                          intra-refresh, prepend-sps-pps, color-range, qp)
//   stage 3: drop Tier::Degradable        (profile+level, bitrate-mode -> codec default)
// Required keys (mime/size/color-format/bitrate/frame-rate/i-frame-interval) are always set.
// To add a vendor key: append one row to kFormatKeys with Tier::Vendor and a
// codec-name prefix filter. Nothing else needs to change.
#pragma once

#include <media/NdkMediaFormat.h>

#include <cstdint>
#include <string>
#include <vector>

#include "owc/protocol.h"

namespace owc {

// What we know about the encoder of one MIME type.
struct EncoderCaps {
  bool available = false;     // a usable (hardware, for HEVC) encoder exists
  bool hardware = true;
  std::string name;           // codec component name ("OMX.qcom.video.encoder.avc", ...)
  std::string mime;           // "video/avc" / "video/hevc"
  // From Java MediaCodecList (JNI, optional). Unknown => *Known=false.
  bool javaInfo = false;
  std::vector<std::pair<int32_t, int32_t>> profileLevels;  // (MediaCodec profile, max level)
  bool intraRefresh = false;  // FEATURE_IntraRefresh
  int32_t maxBitrate = 0;     // VideoCapabilities.getBitrateRange().upper
  int32_t maxWidth = 0, maxHeight = 0;
  uint32_t bitrateModes = 0;  // bit (1<<MediaCodec mode) per supported mode; 0 = unknown
  // From probing a configured codec's output format (NDK).
  std::vector<std::string> vendorKeys;  // "vendor.*" keys the component accepted (probe)
  int maxBFrames = -1;                  // probed max "max-bframes" accepted; -1 unknown
  bool hasVendorKeyPrefix(const char* prefix) const;
  bool supportsProfile(int32_t mcProfile) const;  // true when unknown
  bool isQcom() const;
  bool isExynos() const;
  bool qpSupported(int apiLevel) const;
};

struct EncoderCapsSet {
  EncoderCaps avc, hevc;
  int apiLevel = 26;
  const EncoderCaps& get(Codec c) const { return c == Codec::Hevc ? hevc : avc; }
  uint32_t codecMask() const;
};

// Everything needed to build a MediaFormat.
struct EncoderSettings {
  Codec codec = Codec::H264;
  int width = 1920, height = 1080, fps = 30;
  int bitrate = 8000000;       // initial bps
  int bitrateCeiling = 8000000;  // used for level selection
  int keyframeMs = 2000;       // 0 = only on request
  int bitrateMode = 0;         // protocol: 0=CBR 1=VBR 2=CQ
  int intraRefreshFrames = 0;
  int profile = 0;             // protocol EncoderProfile: 0=Baseline 1=Main 2=High (HEVC 0/1=Main 2=Main10)
  int bframes = 0;
  int qpMin = 0, qpMax = 0;    // 0 = auto
  int maxFpsToEncoder = 0;
  int operatingRateMult = 2;   // operating-rate = fps x this, capped at 60 (Venus clock vote)
  bool vendorLowLatency = true;  // vendor.qti-ext-enc-low-latency.enable     // >0: drop surface frames above this rate (high-speed decimation)
};

// Above this operating-rate / frame-rate OMX.qcom (SDM845) ignores request-sync.
constexpr int kIdrSafeOperatingRate = 60;

// MediaCodec constants (android.media.MediaCodecInfo.CodecProfileLevel).
namespace mc {
constexpr int32_t kColorFormatSurface = 0x7F000789;
constexpr int32_t kBitrateModeCQ = 0, kBitrateModeVBR = 1, kBitrateModeCBR = 2;
constexpr int32_t kAvcBaseline = 0x01, kAvcMain = 0x02, kAvcHigh = 0x08,
                  kAvcConstrainedBaseline = 0x10000;
constexpr int32_t kHevcMain = 0x01, kHevcMain10 = 0x02;
}  // namespace mc

int32_t mediaCodecProfile(Codec c, int protoProfile);
// Lowest level that fits size/fps/bitrate (OMX/MediaCodec level constant).
int32_t pickLevel(Codec c, int32_t mcProfile, int w, int h, int fps, int bitrate);

enum class KeyTier { Required, Degradable, Optional, Vendor };

struct AppliedKey {
  std::string key;
  std::string value;
  KeyTier tier;
};

// Builds the format for a given ladder stage (0..3). `applied` (optional)
// receives the list of keys set, for logging.
AMediaFormat* buildEncoderFormat(const EncoderSettings& s, const EncoderCaps& caps, int apiLevel,
                                 int stage, std::vector<AppliedKey>* applied);

constexpr int kLadderStages = 4;

// Probes AVC/HEVC encoders through the NDK (create, configure 1280x720 with the
// stage-0 table, read the output format to list vendor.* keys). Fills `set`
// in place, keeping any Java-provided info already present. Slow (~100-300 ms).
void probeEncoders(EncoderCapsSet& set, int apiLevel);

// AMediaCodec_getName is API 28; resolved with dlsym so minSdk 26 still loads.
std::string codecName(void* amediacodec);

}  // namespace owc
