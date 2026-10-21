#include <owc/presets.h>

namespace owc {

void Preset::applyTo(ClientOptions& o) const {
  o.jitter_buffer_ms = jitter_buffer_ms;
  o.low_latency_decode = low_latency_decode;
  o.keyframe_request_min_interval_ms = keyframe_request_min_interval_ms;
  o.udp_max_pending_frames = udp_max_pending_frames;
}

const std::vector<Preset>& builtinPresets() {
  static const std::vector<Preset> presets = [] {
    std::vector<Preset> v(3);
    Preset& u = v[0];
    u.id = PresetId::UltraLowLatency;
    u.key = "ultra";
    u.name = u8"Ultra baixa latência";
    u.description =
        u8"Menor atraso possível: sem buffer, Baseline sem B-frames, fila de 1 quadro no "
        u8"celular e descarte até o próximo keyframe. Pode engasgar em Wi-Fi ruim.";
    u.jitter_buffer_ms = 0;
    u.low_latency_decode = true;
    u.keyframe_request_min_interval_ms = 250;
    u.udp_max_pending_frames = 3;
    u.phone = {
        {ParamId::EncoderProfile, ParamValue::EnumV(0)},     // Baseline
        {ParamId::BFrames, ParamValue::Int(0)},
        {ParamId::MaxSendQueueFrames, ParamValue::Int(1)},
        {ParamId::LatencyDropPolicy, ParamValue::EnumV(0)},  // drop to keyframe
        {ParamId::BitrateMode, ParamValue::EnumV(0)},        // CBR
        {ParamId::KeyframeIntervalMs, ParamValue::Int(2000)},
        {ParamId::IntraRefreshFrames, ParamValue::Int(0)},
        {ParamId::AdaptiveBitrate, ParamValue::Bool(true)},
        {ParamId::FecPercent, ParamValue::Int(-1)},          // auto (UDP only)
    };

    Preset& b = v[1];
    b.id = PresetId::Balanced;
    b.key = "balanced";
    b.name = u8"Equilibrado";
    b.description =
        u8"Bom compromisso: buffer de 40 ms para suavizar o Wi-Fi, perfil Main, fila de "
        u8"3 quadros, bitrate adaptativo e intra-refresh para recuperar perdas sem picos.";
    b.jitter_buffer_ms = 40;
    b.low_latency_decode = true;
    b.keyframe_request_min_interval_ms = 300;
    b.udp_max_pending_frames = 4;
    b.phone = {
        {ParamId::EncoderProfile, ParamValue::EnumV(1)},     // Main
        {ParamId::BFrames, ParamValue::Int(0)},
        {ParamId::MaxSendQueueFrames, ParamValue::Int(3)},
        {ParamId::LatencyDropPolicy, ParamValue::EnumV(0)},
        {ParamId::BitrateMode, ParamValue::EnumV(0)},
        {ParamId::KeyframeIntervalMs, ParamValue::Int(2000)},
        {ParamId::IntraRefreshFrames, ParamValue::Int(30)},
        {ParamId::AdaptiveBitrate, ParamValue::Bool(true)},
        {ParamId::FecPercent, ParamValue::Int(-1)},
    };

    Preset& q = v[2];
    q.id = PresetId::MaxQuality;
    q.key = "quality";
    q.name = u8"Qualidade máxima";
    q.description =
        u8"Melhor imagem e movimento mais suave: perfil High com 2 B-frames, VBR, buffer "
        u8"de 150 ms e fila maior sem descarte. Acrescenta ~200-300 ms de atraso.";
    q.jitter_buffer_ms = 150;
    q.low_latency_decode = true;  // OFF only adds DPB buffering (~500 ms), no quality gain
    q.keyframe_request_min_interval_ms = 500;
    q.udp_max_pending_frames = 8;
    q.phone = {
        {ParamId::EncoderProfile, ParamValue::EnumV(2)},     // High
        {ParamId::BFrames, ParamValue::Int(2)},
        {ParamId::MaxSendQueueFrames, ParamValue::Int(10)},
        {ParamId::LatencyDropPolicy, ParamValue::EnumV(1)},  // block/queue
        {ParamId::BitrateMode, ParamValue::EnumV(1)},        // VBR
        {ParamId::KeyframeIntervalMs, ParamValue::Int(2000)},
        {ParamId::IntraRefreshFrames, ParamValue::Int(0)},
        {ParamId::AdaptiveBitrate, ParamValue::Bool(false)},
        {ParamId::FecPercent, ParamValue::Int(20)},
    };
    return v;
  }();
  return presets;
}

const Preset& builtinPreset(PresetId id) {
  const auto& v = builtinPresets();
  const size_t i = static_cast<size_t>(id);
  return v[i < v.size() ? i : 0];
}

int applyPreset(Client& client, const Preset& preset) {
  ClientOptions o = client.options();
  preset.applyTo(o);
  client.setOptions(o);
  Capabilities caps;
  const bool haveCaps = client.capabilities(caps);
  int sent = 0;
  for (const auto& kv : preset.phone) {
    if (haveCaps) {
      bool supported = false;
      for (const ParamDesc& d : caps.params)
        if (d.id == kv.first && (d.flags & kParamSupported) && !(d.flags & kParamReadOnly))
          supported = true;
      if (!supported) continue;
    }
    if (client.setParam(kv.first, kv.second)) ++sent;
  }
  return sent;
}

}  // namespace owc
