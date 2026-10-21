// Persistent app state: %APPDATA%\OWC\settings.json and presets.json.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <owc/client.h>

namespace owcapp {

using ParamList = std::vector<std::pair<owc::ParamId, owc::ParamValue>>;

struct UserPreset {
  std::string name;
  // PC side
  uint32_t jitter_buffer_ms = 0;
  bool low_latency_decode = true;
  uint32_t keyframe_request_min_interval_ms = 250;
  uint32_t udp_max_pending_frames = 4;
  // phone side
  ParamList phone;
};

// A phone this PC is paired with (protocol v2 pairing). The key is kept in
// clear only in memory; settings.json stores it protected with DPAPI (current
// Windows user), so a copied settings file is useless elsewhere.
struct PairedPhoneEntry {
  owc::Key32 key{};
  std::string label;       // device name (model) at pairing time, for the UI
  long long paired_at = 0; // seconds since the Unix epoch
};

struct AppSettings {
  // last connection
  std::string last_host;
  int last_port = 47800;
  int last_transport = 0;  // 0 tcp, 1 udp
  std::string last_device_label;
  std::string manual_addr = "192.168.0.10:47800";
  std::string last_preset;
  int window_w = 1500, window_h = 920;
  bool hud = true;
  int log_level = 1;
  owc::ClientOptions opts;
  // recording choices
  int rec_mode = 0;           // 0 real time, 1 slow motion
  int rec_priority = 0;       // 0 quality, 1 latency
  int rec_slowmo_h = 720;     // 720 (1280x720) or 1080 (1920x1080)
  int rec_slowmo_out_fps = 30;// 30 (8x) or 60 (4x)
  bool rec_keep_current = true;  // real time: keep current resolution/fps
  int rec_rt_w = 0, rec_rt_h = 0, rec_rt_fps = 0;
  std::string rec_dir;        // "" = %USERPROFILE%\Videos\OWC
  // colour page ("Cor / LUT"): LUT gallery sources and UI layout
  std::vector<std::string> color_recent_luts;  // .cube files loaded by the user (newest first, max 24)
  std::vector<std::string> color_lut_folders;  // folders scanned (non-recursive) for .cube files
  int color_palette = 0;       // 0 wheels, 1 curves, 2 hue/sat, 3 adjustments, 4 LUT gallery
  int color_scope_a = 0;       // 0 waveform, 1 parade, 2 vectorscope, 3 histogram
  int color_scope_b = 2;
  bool color_scope_two = false;
  float color_split = 0.48f;   // viewer row height / page height
  // per-device parameter memory, key = "model|name"
  std::map<std::string, ParamList> device_params;
  // per-device orientation: mode -1 = auto (phone hint), else 0/90/180/270; mirror
  std::map<std::string, std::pair<int, bool>> device_orient;
  // pairing (protocol v2): this PC's identity, generated once (owc::newClientId,
  // BCryptGenRandom) and persisted; all zeros until loaded/generated.
  owc::ClientId client_id{};
  // phones paired with this PC, key = phone_id (lower-case hex)
  std::map<std::string, PairedPhoneEntry> paired_phones;
};

std::wstring settingsPath();
std::wstring presetsPath();

// loadSettings drops paired phones whose key cannot be unprotected (another
// Windows user / machine, corrupted entry). It never generates client_id.
bool loadSettings(AppSettings& s);
bool saveSettings(const AppSettings& s);

// The ConnectParams::paired list for owc::Client.
std::vector<owc::PairedPhone> pairedPhoneList(const AppSettings& s);

bool loadUserPresets(std::vector<UserPreset>& out);
bool saveUserPresets(const std::vector<UserPreset>& in);

// Stable ASCII key for a device.
std::string deviceKey(const std::string& model, const std::string& name);

}  // namespace owcapp
