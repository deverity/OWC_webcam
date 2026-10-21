// adb helper for the USB path: find adb.exe, list devices, manage
// `adb forward tcp:<local> tcp:47800`, check/install/launch the phone app.
//
// USB flow for the UI:
//   Adb adb;                                     // resolves adb.exe lazily
//   std::vector<AdbDevice> devs; adb.listDevices(devs);
//   uint16_t local = adb.forward(serial);        // reuses an existing forward
//   if (!adb.isAppInstalled(serial)) adb.installApk(serial, apkPath);
//   adb.launchApp(serial);                       // am start ... --ez autostart true
//   client.connect({"127.0.0.1", local, VideoTransport::Tcp});
//
// THREADING: every method that runs adb BLOCKS (typically 20-300 ms, up to
// the timeout) — call them from a worker thread, never the UI thread. An Adb
// instance is thread-safe (concurrent calls each spawn their own adb process).
// adb.exe is run hidden (CREATE_NO_WINDOW). Note: different adb versions
// fight over the shared adb server (killing it); prefer the one the user
// already uses (PATH) or set it explicitly with setAdbPath().
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace owc {

constexpr const char* kPhonePackage = "com.owc.webcam";
constexpr const char* kPhoneActivity = "com.owc.webcam/.MainActivity";

struct AdbDevice {
  std::string serial;
  std::string state;         // "device", "unauthorized", "offline", "no permissions", ...
  std::string product, model, device;   // from `-l`; model has '_' for spaces (adb's encoding)
  int transport_id = -1;
  bool online() const { return state == "device"; }
};

struct AdbForward {
  std::string serial;
  std::string local;         // e.g. "tcp:51234"
  std::string remote;        // e.g. "tcp:47800"
  // Port of local if it is "tcp:<n>", else 0.
  uint16_t localTcpPort() const;
  uint16_t remoteTcpPort() const;
};

struct ProcessResult {
  bool started = false;      // false: exe not found / CreateProcess failed
  bool timed_out = false;    // killed after the timeout
  int exit_code = -1;
  std::string out, err;      // captured stdout/stderr (raw bytes, adb prints UTF-8)
  bool ok() const { return started && !timed_out && exit_code == 0; }
};

// Runs an executable hidden, capturing stdout/stderr, killing it after
// timeout_ms. args are quoted per CommandLineToArgvW rules. Blocking.
ProcessResult runProcess(const std::wstring& exe, const std::vector<std::string>& args,
                         uint32_t timeout_ms);

class Adb {
 public:
  Adb();
  ~Adb();
  Adb(const Adb&) = delete;
  Adb& operator=(const Adb&) = delete;

  // Explicit adb.exe path (UTF-8). Empty = automatic search:
  //   explicit path -> PATH -> %LOCALAPPDATA%\Android\Sdk\platform-tools ->
  //   directory of the running exe.
  void setAdbPath(const std::string& path);
  // Resolved full path (UTF-8), or "" if adb.exe was not found.
  std::string adbPath() const;
  bool available() const { return !adbPath().empty(); }
  // `adb version` first line, "" on failure. Blocking.
  std::string version();

  // Raw `adb <args>` (adds nothing). Blocking.
  ProcessResult run(const std::vector<std::string>& args, uint32_t timeout_ms = 10000);
  // `adb -s <serial> <args>`. Blocking.
  ProcessResult runFor(const std::string& serial, const std::vector<std::string>& args,
                       uint32_t timeout_ms = 10000);

  // `adb devices -l`. Returns false if adb failed (error text in *err).
  bool listDevices(std::vector<AdbDevice>& out, std::string* err = nullptr);
  // `adb forward --list` (all serials).
  bool listForwards(std::vector<AdbForward>& out, std::string* err = nullptr);
  // Ensures `forward tcp:<local> tcp:<remotePort>` exists for `serial`,
  // reusing an existing one (via forward --list) or picking a free local
  // port. Returns the local port, 0 on failure.
  uint16_t forward(const std::string& serial, uint16_t remotePort = 47800,
                   std::string* err = nullptr);
  // `forward --remove tcp:<localPort>`.
  bool removeForward(const std::string& serial, uint16_t localPort, std::string* err = nullptr);
  // `shell pm path com.owc.webcam` printed "package:".
  bool isAppInstalled(const std::string& serial, std::string* err = nullptr);
  // `shell am start -n com.owc.webcam/.MainActivity --ez autostart true`.
  bool launchApp(const std::string& serial, bool autostart = true, std::string* err = nullptr);
  // `install -r <apk>` (timeout 180 s). apkPath UTF-8.
  bool installApk(const std::string& serial, const std::string& apkPath,
                  std::string* err = nullptr);

  // ---- pure parsers (exposed for tests)
  static std::vector<AdbDevice> parseDevices(const std::string& text);
  static std::vector<AdbForward> parseForwardList(const std::string& text);

  // Picks a currently free local TCP port (bind 127.0.0.1:0). 0 on failure.
  static uint16_t pickFreeTcpPort();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace owc
