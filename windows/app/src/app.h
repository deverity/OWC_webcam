// OWC application model: owns the owc::Client, discovery, adb polling, the USB
// one-click flow, per-device parameter memory and the preview frame hand-off.
#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <owc/core.h>
#include <owc/presets.h>

#include "settings.h"

namespace owcapp {

struct PreviewFrame {
  std::vector<uint8_t> y, uv;  // tightly packed
  uint32_t w = 0, h = 0;
  int rotation = 0;
  bool mirrored = false;
  uint32_t color_matrix = 0;
  bool full_range = false;
  int64_t capture_qpc = 0;
  uint64_t seq = 0;
};

struct Toast {
  std::string text;
  bool error = false;
  ULONGLONG until = 0;
};

struct LogLine {
  uint64_t id = 0;
  std::string time;
  owc::LogSource source = owc::LogSource::Client;
  owc::LogLevel level = owc::LogLevel::Info;
  std::string text;
};

struct GraphPoint {
  double t = 0;       // seconds (steady)
  float e2e = 0;      // capture -> publish (ms)
  float shown = 0;    // capture -> on screen (ms)
};

enum class UsbPhase { Idle, Running, AwaitInstall, Done, Failed };

struct PendingParam {
  owc::ParamValue value;
  ULONGLONG sent_ms = 0;
};

class App : public owc::ClientEvents {
 public:
  explicit App(bool persist);
  ~App() override;
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  void startServices();

  // ---- settings (UI thread; shared bits protected by mu_)
  bool persist;
  AppSettings settings;
  std::vector<UserPreset> user_presets;
  void markDirty() { dirty_ = true; }
  void tick();  // call regularly from the UI thread: debounced save, pending timeouts
  void saveNow();

  // ---- core objects
  owc::Client client;
  owc::Discovery discovery;
  owc::Adb adb;

  // ---- per-UI-frame snapshot (refreshed by beginFrame on the UI thread)
  void beginFrame();
  owc::ClientStats stats;
  owc::Capabilities caps;
  bool caps_valid = false;
  owc::DeviceInfo dev;
  bool dev_valid = false;
  std::vector<owc::DiscoveredDevice> wifi_devices;  // all links, best link per phone, USB tether first
  std::vector<owc::Discovery::UsbTetherAdapter> tether_adapters;

  // ---- preview hand-off
  HANDLE frameEvent() const { return frame_event_; }
  bool takePreview(PreviewFrame& out);
  std::atomic<uint64_t> frames_in{0};  // frames delivered by onFrame
  // call after uploading a frame to the GPU
  void noteDisplayed(const PreviewFrame& f);
  uint64_t frames_shown = 0;
  double shown_fps = 0;
  double shown_lat_avg = 0, shown_lat_p95 = 0;
  int color_matrix = 0;
  bool full_range = false;
  int last_rotation = 0;
  bool last_mirrored = false;
  std::deque<GraphPoint> graph;

  // ---- connection
  void connectNet(const std::string& host, int port, owc::VideoTransport t, const std::string& label,
                  bool fromUsbFlow = false);
  void connectUsb(const std::string& serial, const std::string& label);
  void disconnect();
  bool sessionUp() const;  // Connected or Streaming
  bool live() const;       // keep running in tray (Connecting..Streaming/Reconnecting)

  // ---- USB flow
  UsbPhase usbPhase();
  std::vector<std::string> usbLines();
  std::string usbApkPath();
  void usbAnswerInstall(bool yes);
  void usbCancel();
  std::vector<owc::AdbDevice> adbDevices();
  std::string adbError();
  bool adbAvailable() { return adb.available(); }

  // ---- parameters
  // Sends a value to the phone and remembers it for this device. `final` =
  // false for intermediate slider drag values (rate-limited).
  void setParam(owc::ParamId id, const owc::ParamValue& v, bool final = true);
  bool pending(owc::ParamId id, owc::ParamValue& out);
  void setDragging(owc::ParamId id, bool dragging);
  void setOrientation(int mode, bool mirror);  // mode -1 auto, 0/90/180/270; remembered per device
  void currentOrientation(int& mode, bool& mirror);
  std::string memKey(owc::ParamId id);
  void resetParams();  // phone camera defaults + forget saved camera controls
  void applyPresetBuiltin(const owc::Preset& p);
  void applyUserPreset(const UserPreset& p);
  void saveUserPreset(const std::string& name);
  void deleteUserPreset(size_t idx);
  void applyOptions(const owc::ClientOptions& o);  // setOptions + persist
  std::string deviceKeyNow();

  // ---- toasts / logs
  void toast(const std::string& text, bool error = false, unsigned ms = 4000);
  std::vector<Toast> toasts();
  void pollLogs();
  std::vector<LogLine> log_lines;
  void clearLogs();

  // ---- outputs (virtual camera / OBS plugin scripts)
  void runScript(const std::string& what, bool install);  // "vcam" | "obs"
  bool scriptBusy();
  std::string scriptOutput();
  // cached registry probes (refreshed every 2 s)
  bool vcam_registered = false, vcam_per_user = false, obs_installed = false;
  std::wstring findScript(const wchar_t* name);

  // Windows Firewall rule for phone announcements (owc::Firewall)
  int firewallState() const { return fw_state_.load(); }  // -1 unknown, 0 missing, 1 present
  bool firewallBusy() const { return fw_busy_.load(); }
  std::string firewallCommand() const;
  void refreshFirewall();
  void addFirewallRule();  // UAC prompt

  std::string stateMessage();
  std::string preset_active;  // name of last applied preset

  // ---- ClientEvents
  void onStateChanged(owc::ClientState, owc::DisconnectReason, const std::string&) override;
  void onConnected(const owc::DeviceInfo&) override;
  void onCapabilities(const owc::Capabilities&) override;
  void onParamState(const owc::ParamState&) override;
  void onFrame(const owc::DecodedFrame&) override;

 private:
  void rememberParam(owc::ParamId id, const owc::ParamValue& v);
  void usbRun(std::string serial, std::string label);
  void usbLog(const std::string& s);
  bool usbWaitState(uint32_t timeoutMs);
  void adbLoop();

  std::mutex mu_;
  std::atomic<bool> dirty_{false};
  ULONGLONG dirty_since_ = 0;
  HANDLE frame_event_ = nullptr;

  // preview
  std::mutex frame_mu_;
  PreviewFrame pending_frame_, reading_frame_;
  bool frame_new_ = false;
  uint64_t frame_seq_ = 0;
  std::vector<double> lat_ring_;
  size_t lat_pos_ = 0;
  ULONGLONG fps_t0_ = 0;
  uint32_t fps_count_ = 0;
  double last_graph_t_ = 0;

  // device identity / memory
  std::string dev_key_;
  bool applied_ = false;
  int applied_cam_ = -1;
  int cur_cam_ = 0;
  int queued_preset_ = -1;

  // params
  std::map<uint16_t, PendingParam> pending_;
  std::map<uint16_t, ULONGLONG> last_sent_;
  std::atomic<int> drag_id_{-1};

  // toasts
  std::vector<Toast> toasts_;
  std::string state_msg_;
  uint64_t last_log_id_ = 0;

  // USB flow
  std::thread usb_th_;
  UsbPhase usb_phase_ = UsbPhase::Idle;
  std::vector<std::string> usb_lines_;
  std::string usb_apk_;
  int usb_answer_ = 0;  // 0 waiting, 1 yes, 2 no
  std::atomic<bool> usb_cancel_{false};
  std::condition_variable usb_cv_;

  // adb polling
  std::thread adb_th_;
  std::atomic<bool> stop_{false};
  std::vector<owc::AdbDevice> adb_devs_;
  std::string adb_err_;
  std::condition_variable adb_cv_;

  // firewall
  std::thread fw_th_;
  std::atomic<int> fw_state_{-1};
  std::atomic<bool> fw_busy_{false};

  // scripts
  std::thread script_th_;
  bool script_busy_ = false;
  std::string script_out_;
  ULONGLONG probe_t_ = 0;
};

const char* reasonText(owc::DisconnectReason r);
const char* stateText(owc::ClientState s);

}  // namespace owcapp
