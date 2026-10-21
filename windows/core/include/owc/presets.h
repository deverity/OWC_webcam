// Latency/quality presets: one bundle of PC-side ClientOptions changes plus
// phone ParamId values. The UI shows them as one-click choices and may let
// the user tweak each value afterwards.
//
//   UltraLowLatency ("Ultra baixa latência"): no jitter buffer, Baseline, no
//       B-frames, send queue 1, drop-to-keyframe. Expect ~1 frame of total
//       buffering end to end; stutters/artifacts on bad Wi-Fi.
//   Balanced ("Equilibrado"): 40 ms jitter buffer, Main profile, queue 3,
//       adaptive bitrate, intra-refresh for smoother loss recovery.
//   MaxQuality ("Qualidade máxima"): 150 ms jitter buffer, High profile,
//       2 B-frames (decoder stays in low-latency mode: it still reorders
//       correctly with 1-3 frames delay), queue 10,
//       VBR, phone blocks instead of dropping. +200..300 ms latency.
//
// Phone parameters the phone does not support are answered with
// ParamStatus::Unsupported (onParamState) and otherwise ignored; Client
// sends only params listed as supported in Capabilities when known.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include <owc/client.h>

namespace owc {

enum class PresetId : uint8_t { UltraLowLatency = 0, Balanced = 1, MaxQuality = 2 };

struct Preset {
  PresetId id = PresetId::UltraLowLatency;
  const char* key = "";          // stable ASCII key: "ultra", "balanced", "quality"
  const char* name = "";         // UI label (pt-BR, UTF-8)
  const char* description = "";  // UI tooltip (pt-BR, UTF-8)
  // PC fields the preset sets: jitter_buffer_ms, low_latency_decode,
  // keyframe_request_min_interval_ms, udp_max_pending_frames. All other
  // ClientOptions fields are left as the user had them (see applyTo()).
  uint32_t jitter_buffer_ms = 0;
  bool low_latency_decode = true;
  uint32_t keyframe_request_min_interval_ms = 250;
  uint32_t udp_max_pending_frames = 4;
  // Phone params (sent in this order).
  std::vector<std::pair<ParamId, ParamValue>> phone;

  // Copies the preset's PC fields into `o`, leaving the others untouched.
  void applyTo(ClientOptions& o) const;
};

// The three built-in presets, in PresetId order.
const std::vector<Preset>& builtinPresets();
const Preset& builtinPreset(PresetId id);

// Applies a preset to a live client: setOptions() with the PC fields and
// setParam() for each phone param (skipping params the phone's Capabilities
// mark unsupported, when Capabilities are known). Returns the number of
// phone params sent (0 if not connected — PC options are still applied).
int applyPreset(Client& client, const Preset& preset);

}  // namespace owc
