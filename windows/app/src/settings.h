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
  // per-device parameter memory, key = "model|name"
  std::map<std::string, ParamList> device_params;
  // per-device orientation: mode -1 = auto (phone hint), else 0/90/180/270; mirror
  std::map<std::string, std::pair<int, bool>> device_orient;
};

std::wstring settingsPath();
std::wstring presetsPath();

bool loadSettings(AppSettings& s);
bool saveSettings(const AppSettings& s);

bool loadUserPresets(std::vector<UserPreset>& out);
bool saveUserPresets(const std::vector<UserPreset>& in);

// Stable ASCII key for a device.
std::string deviceKey(const std::string& model, const std::string& name);

}  // namespace owcapp
