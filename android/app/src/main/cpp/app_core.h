// OWC native core lifecycle (no JNI here; jni_bridge.cpp adapts it).
//
// Threads (all joined by stop()):
//   owc-net      NetServer: accept / client reads / UDP punch / discovery
//   owc-worker   session state machine: handshake, params, stream start/stop,
//                camera+encoder lifecycle, stats (1 Hz), ABR, timeouts
//   owc-send     one per client (Sender): control + video multiplexing
//   owc-enc-out  one per running encoder (VideoEncoder output loop)
//   + NDK camera callback looper threads (owned by the camera framework)
//
// Data ownership / locking:
//   - Everything session/stream related (ParamStore, Pipeline, Abr, current
//     client) is owned by the worker thread; other threads talk to it only by
//     posting Events to its queue.
//   - statusMu_ guards the status snapshot read by nativeGetStatus().
//   - infoMu_ guards device name/model (read by the net thread for discovery).
//   - Device status (battery/thermal) are atomics.
//   - PairingStore (pairing_store.h) has its own mutex; it is owned by AppCore
//     and shared with the worker, so the UI can list/remove PCs while the core
//     is stopped. Unpair/cancel requests reach the worker as queued Events.
//   - The encoder output thread uses frameSender_ (raw pointer) which is only
//     changed by the worker while no encoder is running (start/join order gives
//     the happens-before), so it needs no lock.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "camera/camera_info.h"
#include "encoder/encoder_options.h"

struct ANativeWindow;

namespace owc {

// Events delivered to the app (ARCHITECTURE.md "Contrato JNI").
enum AppEvent : int {
  kEvClientConnected = 1,
  kEvClientDisconnected = 2,  // arg: 1 = graceful (Bye), 0 = lost
  kEvScreenMode = 3,
  kEvStreamingStarted = 4,
  kEvStreamingStopped = 5,  // arg: 1 = emitted because the session ended
  kEvError = 6,
  kEvDeviceName = 7,  // str = new name set by the PC (the app persists it)
  // A PC asked to pair: show the code. int = code lifetime (s),
  // str = "<6-digit code>\n<PC name>" (newline-separated).
  kEvPairingCode = 8,
  // The pairing code is no longer valid. int = PairingOutcome, str = PC name.
  kEvPairingEnded = 9,
};

enum PairingOutcome : int {
  kPairingPaired = 0,        // code typed correctly: PC stored
  kPairingFailed = 1,        // too many wrong codes (lockout started)
  kPairingExpired = 2,       // code lifetime elapsed
  kPairingRejected = 3,      // user cancelled on the phone
  kPairingDisconnected = 4,  // PC went away / core stopped
};

class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void onEvent(int type, int intArg, const std::string& strArg) = 0;
};

struct StartOptions {
  std::string deviceName, model;
  int apiLevel = 26;
  uint16_t controlPort = 47800, udpPort = 47801, discoveryPort = 47802;
  uint16_t announcePort = kDefaultAnnouncePort;  // 0 = no announcements
  // Optional MediaCodecList info gathered on the Java side (javaInfo=true).
  EncoderCapsSet javaCaps;
  // TEST ONLY (owc_selftest): synthetic frames when the camera cannot be opened.
  bool testSyntheticSource = false;
  // Constrained high-speed support (from Java Camera2; optional).
  std::vector<std::pair<std::string, HsConfig>> hsConfigs;  // camera id -> config
  class HighSpeedBackend* hsBackend = nullptr;              // not owned; outlives the core
};

class Worker;
class PairingStore;

// State shared between the JNI-facing API and the worker.
struct SharedInfo {
  std::mutex infoMu;  // name/model
  std::string deviceName, model;
  std::atomic<int> batteryPct{-1};
  std::atomic<float> batteryTempC{0.0f};
  std::atomic<bool> charging{false};
  std::atomic<int> thermal{0};

  struct Status {
    std::string state = "idle";  // idle|listening|connected|streaming
    std::string peer, transport = "tcp", cameraId, codec = "h264";
    int width = 0, height = 0;
    float fps = 0;
    int bitrateKbps = 0;
    uint32_t dropped = 0;
    float encodeLatencyMs = 0;
  };
  std::mutex statusMu;
  Status status;
};

class AppCore {
 public:
  AppCore();
  ~AppCore();

  bool start(const StartOptions& opts, EventSink* sink, std::string& err);
  void stop();
  bool running() const { return running_.load(); }

  // Takes ownership of one reference of `w` (may be null). Blocks until the
  // worker no longer uses the previous window (<= 2 s).
  void setPreviewSurface(ANativeWindow* w);
  std::string statusJson();
  void updateDeviceStatus(int batteryPct, float tempC, bool charging, int thermal);
  void setDeviceName(const std::string& name);
  void screenModeChanged(int mode);  // posts to the worker thread

  // Pairing (any thread; work whether or not the core is running).
  // setDataDir must succeed before start() (start fails otherwise).
  bool setDataDir(const std::string& dir, std::string& err);
  std::string pairedClientsJson();
  // idHex = "" removes every PC; a removed PC that is connected is dropped.
  bool unpairClient(const std::string& idHex);
  void cancelPairing();  // user tapped "Cancelar": AuthResult{Rejected} + close

 private:
  std::mutex lifeMu_;  // serializes start/stop/setPreviewSurface
  std::atomic<bool> running_{false};
  std::unique_ptr<Worker> worker_;
  std::shared_ptr<SharedInfo> info_;
  std::shared_ptr<PairingStore> pairing_;
  ANativeWindow* pendingPreview_ = nullptr;  // guarded by lifeMu_ (used before start)
};

}  // namespace owc
