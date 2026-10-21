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
  kEvClientDisconnected = 2,
  kEvScreenMode = 3,
  kEvStreamingStarted = 4,
  kEvStreamingStopped = 5,
  kEvError = 6,
  kEvDeviceName = 7,  // str = new name set by the PC (the app persists it)
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
  // Optional MediaCodecList info gathered on the Java side (javaInfo=true).
  EncoderCapsSet javaCaps;
  // TEST ONLY (owc_selftest): synthetic frames when the camera cannot be opened.
  bool testSyntheticSource = false;
  // Constrained high-speed support (from Java Camera2; optional).
  std::vector<std::pair<std::string, HsConfig>> hsConfigs;  // camera id -> config
  class HighSpeedBackend* hsBackend = nullptr;              // not owned; outlives the core
};

class Worker;

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

 private:
  std::mutex lifeMu_;  // serializes start/stop/setPreviewSurface
  std::atomic<bool> running_{false};
  std::unique_ptr<Worker> worker_;
  std::shared_ptr<SharedInfo> info_;
  ANativeWindow* pendingPreview_ = nullptr;  // guarded by lifeMu_ (used before start)
};

}  // namespace owc
