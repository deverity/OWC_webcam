#include "record_setup.h"

#include "codec_select.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace owc {
namespace detail {

namespace {

const ParamDesc* findParam(const Capabilities& caps, ParamId id) {
  for (const ParamDesc& d : caps.params)
    if (d.id == id && (d.flags & kParamSupported) && !(d.flags & kParamReadOnly)) return &d;
  return nullptr;
}

int64_t currentInt(const Capabilities& caps, ParamId id, int64_t fallback) {
  for (const ParamDesc& d : caps.params)
    if (d.id == id) return d.current.i;
  return fallback;
}

bool hasOption(const ParamDesc& d, int32_t v) {
  if (d.options.empty()) return d.min <= v && v <= d.max;  // range-only enum
  for (const EnumOption& o : d.options)
    if (o.value == v) return true;
  return false;
}

int32_t clampTo(const ParamDesc* d, double v) {
  if (d && d->max > d->min) v = std::min(std::max(v, d->min), d->max);
  return static_cast<int32_t>(v);
}

}  // namespace

uint32_t qualityBitrate(uint32_t width, uint32_t height, uint32_t fps) {
  if (!width || !height) return 20000000u;
  const double px = static_cast<double>(width) * height / (1920.0 * 1080.0);
  const double fr = static_cast<double>(fps ? fps : 30) / 30.0;
  const double bps = 20e6 * std::pow(px, 0.79) * std::pow(fr, 0.64);
  return static_cast<uint32_t>(std::round(bps / 500000.0) * 500000.0);
}

std::string recordSummary(RecordPriority prio, const RecordSetupStatus& st, const std::string& note) {
  char b[256];
  snprintf(b, sizeof(b), "%s: %s %ux%u @ %u fps, %.0f Mbps",
           prio == RecordPriority::Quality ? "Qualidade" : "Latência", codecName(st.codec), st.width,
           st.height, st.fps, st.bitrate_bps / 1e6);
  std::string s = b;
  if (!st.preview_available) s += std::string(", sem preview (") + codecName(st.codec) + " sem decoder)";
  if (!note.empty()) s += " (" + note + ")";
  return s;
}

RecordPlan planRecording(const RecordPreset& p, const Capabilities& caps, uint32_t phoneCodecMask,
                         const DecoderCapabilities& pc, VideoTransport transport) {
  RecordPlan plan;
  RecordSetupStatus& st = plan.status;
  const bool slow = p.mode == RecordMode::SlowMotion;
  const bool quality = p.priority == RecordPriority::Quality;
  auto add = [&](ParamId id, const ParamValue& v) {
    if (findParam(caps, id)) plan.sets.emplace_back(id, v);
  };

  // ---- geometry: 0 = keep current (SlowMotion: 1280x720 @ 240)
  uint32_t w = p.width, h = p.height, fps = p.fps;
  const bool keepGeometry = !slow && !w && !h && !fps;
  if (slow) {
    if (!w || !h) { w = 1280; h = 720; }
    if (!fps) fps = 240;
  }
  if (!w || !h) {
    w = static_cast<uint32_t>(currentInt(caps, ParamId::VideoWidth, 1920));
    h = static_cast<uint32_t>(currentInt(caps, ParamId::VideoHeight, 1080));
  }
  if (!fps) fps = static_cast<uint32_t>(currentInt(caps, ParamId::Fps, 30));

  // ---- codec
  Codec codec = static_cast<Codec>(currentInt(caps, ParamId::VideoCodec, 0));
  if (quality) {
    const bool phoneHevc = (phoneCodecMask & codecBit(Codec::Hevc)) != 0;
    const bool pcHevc = pc.hevc.any();
    codec = phoneHevc && (pcHevc || p.allow_codec_without_pc_decoder) ? Codec::Hevc : Codec::H264;
    plan.forceCodec = true;
  }
  plan.requestedHevc = codec == Codec::Hevc && quality;
  st.codec = codec;
  st.preview_available = pc.get(codec).any();

  // ---- latency priority, RealTime: today's behaviour = change nothing
  if (!quality && !slow) {
    st.width = w;
    st.height = h;
    st.fps = fps;
    st.bitrate_bps = static_cast<uint32_t>(currentInt(caps, ParamId::Bitrate, 0));
    st.summary = recordSummary(p.priority, st, "parâmetros atuais");
    return plan;
  }

  // ---- sends, in order: mode/codec first, then geometry, then rate control
  // Explicit geometry: high-speed session iff fps > 60 (normal sizes like
  // 1080p30 are not available in a constrained high-speed session).
  if (!keepGeometry) add(ParamId::HighSpeedMode, ParamValue::EnumV(fps > 60 ? 1 : 0));
  if (quality) add(ParamId::VideoCodec, ParamValue::EnumV(static_cast<int32_t>(codec)));
  if (!keepGeometry) {
    add(ParamId::VideoWidth, ParamValue::Int(static_cast<int32_t>(w)));
    add(ParamId::VideoHeight, ParamValue::Int(static_cast<int32_t>(h)));
    add(ParamId::Fps, ParamValue::Int(static_cast<int32_t>(fps)));
  }
  const ParamDesc* br = findParam(caps, ParamId::Bitrate);
  double bitrate = quality ? qualityBitrate(w, h, fps)
                           : 40e6 * (static_cast<double>(w) * h) / (1280.0 * 720.0);  // legacy slow-mo
  bitrate = clampTo(br, bitrate);
  add(ParamId::Bitrate, ParamValue::Int(static_cast<int32_t>(bitrate)));
  if (quality) {
    // VBR: high quality with a predictable ceiling; CQ only if VBR is absent.
    if (const ParamDesc* bm = findParam(caps, ParamId::BitrateMode)) {
      if (hasOption(*bm, 1)) add(ParamId::BitrateMode, ParamValue::EnumV(1));
      else if (hasOption(*bm, 2)) add(ParamId::BitrateMode, ParamValue::EnumV(2));
    }
  }
  add(ParamId::AdaptiveBitrate, ParamValue::Bool(false));
  add(ParamId::LatencyDropPolicy, ParamValue::EnumV(1));
  const ParamDesc* q = findParam(caps, ParamId::MaxSendQueueFrames);
  const double queue = quality ? std::max<double>(32, fps) : 32;
  add(ParamId::MaxSendQueueFrames, ParamValue::Int(clampTo(q, queue)));
  if (quality) {
    // <= 60 fps: 2 s GOP; above, the phone caps its own GOP at <= 1 s.
    if (fps <= 60) add(ParamId::KeyframeIntervalMs, ParamValue::Int(2000));
    add(ParamId::BFrames, ParamValue::Int(0));  // the recorder assumes no reordering
  } else {
    // Some high-speed encoders ignore RequestKeyframe: 1 s GOP bounds the wait.
    add(ParamId::KeyframeIntervalMs, ParamValue::Int(1000));
  }
  if (transport == VideoTransport::Tcp) add(ParamId::FecPercent, ParamValue::Int(0));

  if (fps > 60) plan.maxPresentFps = 60;
  st.width = w;
  st.height = h;
  st.fps = fps;
  st.bitrate_bps = static_cast<uint32_t>(bitrate);
  st.summary = recordSummary(p.priority, st, std::string());
  return plan;
}

}  // namespace detail
}  // namespace owc
