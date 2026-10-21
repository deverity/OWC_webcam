// Parameter model: current values of every ParamId, their descriptors (ranges
// and options from real camera metadata / encoder capabilities) and SetParam
// validation. Worker thread only (no locking).
//
// Validation is generic: SetParam is checked against the very ParamDesc that
// is advertised in Capabilities, so ranges shown by the PC UI and ranges
// enforced here can never diverge.
#pragma once

#include <cstdint>
#include <vector>

#include "camera/camera_controls.h"
#include "camera/camera_info.h"
#include "encoder/encoder_options.h"
#include "owc/protocol.h"

namespace owc {

struct StreamConfig {
  int cameraIndex = 0;
  int width = 1920, height = 1080, fps = 30;
  Codec codec = Codec::H264;
  int bitrate = 8000000;      // ceiling when ABR is on
  int keyframeMs = 2000;
  int bitrateMode = 0;        // 0 CBR 1 VBR 2 CQ
  int intraRefresh = 0;
  int fecPercent = 0;         // -1 = auto
  bool abr = true;
  int profile = 0;            // 0 Baseline 1 Main 2 High (HEVC: 0/1 Main, 2 Main10)
  int maxQueue = 2;
  int bframes = 0;
  int qpMin = 0, qpMax = 0;
  int dropPolicy = 0;         // 0 drop-to-keyframe 1 queue
  int highSpeed = 0;          // 1 = constrained high-speed session (Fps = encoder output fps)
};

// FEC (Reed-Solomon parity via protocol packetizeFec) is produced on UDP only.
constexpr bool kFecImplemented = true;

class ParamStore {
 public:
  enum Effect : uint32_t {
    kNone = 0,
    kReconfigure = 1u << 0,   // stream restart (epoch++)
    kBitrate = 1u << 1,       // live video-bitrate
    kControls = 1u << 2,      // camera repeating request
    kQueue = 1u << 3,         // sender queue policy
    kAbr = 1u << 4,           // ABR on/off
    kScreen = 1u << 5,        // screen mode event to the app
    kCaps = 1u << 6,          // descriptors changed: resend Capabilities
    kFec = 1u << 7,
  };
  struct Result {
    ParamStatus status = ParamStatus::Ok;
    ParamValue value;               // value now in effect
    uint32_t effects = kNone;
    std::vector<ParamId> alsoChanged;  // other params changed as a side effect
  };

  void init(const std::vector<CameraDesc>* cams, const EncoderCapsSet* enc, bool hsBackend);
  bool highSpeedPossible() const;  // backend present + camera has configs
  int cameraFps() const;           // sensor fps (high-speed: fixed HS rate)
  bool ready() const { return cams_ && !cams_->empty(); }

  const CameraDesc& camera() const { return (*cams_)[static_cast<size_t>(stream.cameraIndex)]; }
  ParamDesc describe(ParamId id) const;
  std::vector<ParamDesc> describeAll() const;
  ParamValue current(ParamId id) const;
  Result set(ParamId id, const ParamValue& v);
  void resetCameraControls();
  // Encoder fell back to another size: record it.
  void setActualSize(int w, int h);
  // Effective encoder settings for the current config.
  EncoderSettings encoderSettings(int bitrateNow) const;
  Capabilities capabilities() const;

  StreamConfig stream;
  CameraControls controls;
  int screenMode = 0;

 private:
  ParamDesc describeRaw(ParamId id) const;
  // Sizes valid for the current mode (high-speed sizes when highSpeed=1).
  std::vector<VideoSize> activeSizes() const;
  std::pair<int, int> fpsRange() const;
  void applyCameraChange(int index, Result& r);
  void clampFps(Result& r);
  bool sizeAllowed(int w, int h) const;

  const std::vector<CameraDesc>* cams_ = nullptr;
  const EncoderCapsSet* enc_ = nullptr;
  bool hsBackend_ = false;
};

extern const ParamId kAllParams[];
extern const size_t kAllParamsCount;

}  // namespace owc
