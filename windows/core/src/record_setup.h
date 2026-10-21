// Recording setup planner (pure; unit tested). See RecordPreset in client.h.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <owc/client.h>

namespace owc {
namespace detail {

struct RecordPlan {
  std::vector<std::pair<ParamId, ParamValue>> sets;  // in send order, supported params only
  RecordSetupStatus status;                         // active=false (caller sets it)
  bool forceCodec = false;       // pin the codec in codec negotiation while active
  uint32_t maxPresentFps = 0;    // ClientOptions::max_present_fps to apply (0 = unchanged)
  bool requestedHevc = false;    // HEVC chosen (watch whether the phone sustains the fps)
};

// Quality bitrate rule: 20 Mbps x (pixels/1080p)^0.79 x (fps/30)^0.64, rounded
// to 0.5 Mbps (1080p30 = 20, 720p240 ~ 40, 4K30 ~ 60 Mbps).
uint32_t qualityBitrate(uint32_t width, uint32_t height, uint32_t fps);

// caps: the phone's Capabilities (ParamDesc current values + ranges/options).
// phoneCodecMask: HelloAck::codec_mask. pc: this PC's decoders.
RecordPlan planRecording(const RecordPreset& preset, const Capabilities& caps,
                         uint32_t phoneCodecMask, const DecoderCapabilities& pc,
                         VideoTransport transport);

// pt-BR one-line summary used in RecordSetupStatus::summary.
std::string recordSummary(RecordPriority prio, const RecordSetupStatus& st,
                          const std::string& note);

}  // namespace detail
}  // namespace owc
