#include <owc/adb.h>

#include "util.h"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <thread>

namespace owc {

using namespace detail;

// ================================================================ process
namespace {

std::wstring quoteArg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring r = L"\"";
  for (size_t i = 0;; ++i) {
    size_t bs = 0;
    while (i < a.size() && a[i] == L'\\') { ++i; ++bs; }
    if (i == a.size()) { r.append(bs * 2, L'\\'); break; }
    if (a[i] == L'"') { r.append(bs * 2 + 1, L'\\'); r.push_back(L'"'); }
    else { r.append(bs, L'\\'); r.push_back(a[i]); }
  }
  r.push_back(L'"');
  return r;
}

void readAll(HANDLE h, std::string* out) {
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out->append(buf, n);
}

}  // namespace

ProcessResult runProcess(const std::wstring& exe, const std::vector<std::string>& args,
                         uint32_t timeout_ms) {
  ProcessResult r;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr, inR = nullptr;
  if (!CreatePipe(&outR, &outW, &sa, 0)) return r;
  if (!CreatePipe(&errR, &errW, &sa, 0)) { CloseHandle(outR); CloseHandle(outW); return r; }
  SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
  inR = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                    0, nullptr);

  std::wstring cmd = quoteArg(exe);
  for (const auto& a : args) cmd += L" " + quoteArg(utf8ToWide(a));

  // Only the three std handles are inherited (adb may spawn a long-lived
  // server; it must not keep our other handles open).
  SIZE_T attrSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
  std::vector<uint8_t> attrBuf(attrSize);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
  HANDLE inherit[3] = {outW, errW, inR};
  const size_t nInherit = inR != INVALID_HANDLE_VALUE ? 3 : 2;
  bool useList = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                 UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                           sizeof(HANDLE) * nInherit, nullptr, nullptr);

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.StartupInfo.wShowWindow = SW_HIDE;
  si.StartupInfo.hStdInput = inR != INVALID_HANDLE_VALUE ? inR : nullptr;
  si.StartupInfo.hStdOutput = outW;
  si.StartupInfo.hStdError = errW;
  si.lpAttributeList = useList ? attrs : nullptr;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
  cmdBuf.push_back(L'\0');
  BOOL ok = CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | (useList ? EXTENDED_STARTUPINFO_PRESENT : 0),
                           nullptr, nullptr, &si.StartupInfo, &pi);
  if (useList) DeleteProcThreadAttributeList(attrs);
  CloseHandle(outW);
  CloseHandle(errW);
  if (inR != INVALID_HANDLE_VALUE) CloseHandle(inR);
  if (!ok) {
    CloseHandle(outR);
    CloseHandle(errR);
    return r;
  }
  r.started = true;
  std::thread tOut(readAll, outR, &r.out);
  std::thread tErr(readAll, errR, &r.err);
  DWORD w = WaitForSingleObject(pi.hProcess, timeout_ms);
  if (w != WAIT_OBJECT_0) {
    r.timed_out = true;
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 2000);
  }
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  r.exit_code = static_cast<int>(code);
  // A grandchild (adb server) may still hold the pipe: give readers a short
  // grace period, then cancel their blocking ReadFile.
  auto joinWithGrace = [](std::thread& t) {
    HANDLE th = t.native_handle();
    if (WaitForSingleObject(th, 300) != WAIT_OBJECT_0) {
      CancelSynchronousIo(th);
    }
    t.join();
  };
  joinWithGrace(tOut);
  joinWithGrace(tErr);
  CloseHandle(outR);
  CloseHandle(errR);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return r;
}

// ================================================================ parsing
namespace {
std::vector<std::string> splitWs(const std::string& line) {
  std::vector<std::string> v;
  std::istringstream ss(line);
  std::string t;
  while (ss >> t) v.push_back(t);
  return v;
}
std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> v;
  std::string cur;
  for (char c : text) {
    if (c == '\n') { v.push_back(cur); cur.clear(); }
    else if (c != '\r') cur.push_back(c);
  }
  if (!cur.empty()) v.push_back(cur);
  return v;
}
uint16_t tcpPortOf(const std::string& s) {
  if (s.compare(0, 4, "tcp:") != 0) return 0;
  char* end = nullptr;
  unsigned long v = strtoul(s.c_str() + 4, &end, 10);
  if (end == s.c_str() + 4 || *end != '\0' || v == 0 || v > 65535) return 0;
  return static_cast<uint16_t>(v);
}
}  // namespace

uint16_t AdbForward::localTcpPort() const { return tcpPortOf(local); }
uint16_t AdbForward::remoteTcpPort() const { return tcpPortOf(remote); }

std::vector<AdbDevice> Adb::parseDevices(const std::string& text) {
  std::vector<AdbDevice> out;
  for (const std::string& l : lines(text)) {
    if (l.empty() || l[0] == '*') continue;                  // "* daemon started ..."
    if (l.compare(0, 4, "List") == 0) continue;               // "List of devices attached"
    if (l.compare(0, 4, "adb ") == 0) continue;               // "adb server version ..."
    // serial is separated by whitespace (tab without -l, spaces with -l).
    std::vector<std::string> tok = splitWs(l);
    if (tok.size() < 2) continue;
    AdbDevice d;
    d.serial = tok[0];
    size_t i = 1;
    // state may be two words: "no permissions (...)" -> keep words until key:value tokens
    d.state = tok[i++];
    while (i < tok.size() && tok[i].find(':') == std::string::npos) {
      d.state += " " + tok[i++];
    }
    for (; i < tok.size(); ++i) {
      const std::string& t = tok[i];
      size_t c = t.find(':');
      if (c == std::string::npos) continue;
      std::string k = t.substr(0, c), v = t.substr(c + 1);
      if (k == "product") d.product = v;
      else if (k == "model") d.model = v;
      else if (k == "device") d.device = v;
      else if (k == "transport_id") d.transport_id = atoi(v.c_str());
    }
    out.push_back(d);
  }
  return out;
}

std::vector<AdbForward> Adb::parseForwardList(const std::string& text) {
  std::vector<AdbForward> out;
  for (const std::string& l : lines(text)) {
    std::vector<std::string> tok = splitWs(l);
    if (tok.size() != 3) continue;
    if (tok[1].find(':') == std::string::npos || tok[2].find(':') == std::string::npos) continue;
    out.push_back(AdbForward{tok[0], tok[1], tok[2]});
  }
  return out;
}

uint16_t Adb::pickFreeTcpPort() {
  if (!winsockInit()) return 0;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return 0;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  uint16_t port = 0;
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
    int len = sizeof(a);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) port = ntohs(a.sin_port);
  }
  closesocket(s);
  return port;
}

// ================================================================ Adb
struct Adb::Impl {
  mutable std::mutex mx;
  std::string explicitPath;
  mutable std::string resolved;
  mutable bool searched = false;

  static bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
  }

  std::string resolve() const {
    std::lock_guard<std::mutex> lk(mx);
    if (searched) return resolved;
    searched = true;
    resolved.clear();
    if (!explicitPath.empty()) {
      if (fileExists(utf8ToWide(explicitPath))) resolved = explicitPath;
      return resolved;  // explicit path wins; don't silently use another adb
    }
    wchar_t buf[MAX_PATH * 2];
    // PATH
    DWORD n = SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH * 2, buf, nullptr);
    if (n > 0 && n < MAX_PATH * 2) { resolved = wideToUtf8(buf); return resolved; }
    // %LOCALAPPDATA%\Android\Sdk\platform-tools
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) {
      std::wstring p = std::wstring(buf) + L"\\Android\\Sdk\\platform-tools\\adb.exe";
      if (fileExists(p)) { resolved = wideToUtf8(p); return resolved; }
    }
    // next to the running exe
    n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) {
      std::wstring p(buf, n);
      size_t s = p.find_last_of(L"\\/");
      if (s != std::wstring::npos) {
        p = p.substr(0, s + 1) + L"adb.exe";
        if (fileExists(p)) resolved = wideToUtf8(p);
      }
    }
    return resolved;
  }
};

Adb::Adb() : impl_(new Impl) {}
Adb::~Adb() = default;

void Adb::setAdbPath(const std::string& path) {
  std::lock_guard<std::mutex> lk(impl_->mx);
  impl_->explicitPath = path;
  impl_->searched = false;
}

std::string Adb::adbPath() const { return impl_->resolve(); }

ProcessResult Adb::run(const std::vector<std::string>& args, uint32_t timeout_ms) {
  std::string p = adbPath();
  if (p.empty()) {
    ProcessResult r;
    r.err = "adb.exe not found";
    return r;
  }
  return runProcess(utf8ToWide(p), args, timeout_ms);
}

ProcessResult Adb::runFor(const std::string& serial, const std::vector<std::string>& args,
                          uint32_t timeout_ms) {
  std::vector<std::string> a;
  if (!serial.empty()) { a.push_back("-s"); a.push_back(serial); }
  a.insert(a.end(), args.begin(), args.end());
  return run(a, timeout_ms);
}

namespace {
bool fail(std::string* err, const ProcessResult& r, const char* what) {
  if (err) {
    if (!r.started) *err = r.err.empty() ? std::string("cannot run adb") : r.err;
    else if (r.timed_out) *err = std::string(what) + ": adb timed out";
    else {
      std::string t = !r.err.empty() ? r.err : r.out;
      while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
      *err = std::string(what) + ": " + t;
    }
  }
  return false;
}
}  // namespace

std::string Adb::version() {
  ProcessResult r = run({"version"}, 5000);
  if (!r.ok()) return std::string();
  std::vector<std::string> l = lines(r.out);
  return l.empty() ? std::string() : l[0];
}

bool Adb::listDevices(std::vector<AdbDevice>& out, std::string* err) {
  ProcessResult r = run({"devices", "-l"}, 10000);
  if (!r.ok()) return fail(err, r, "adb devices");
  out = parseDevices(r.out);
  return true;
}

bool Adb::listForwards(std::vector<AdbForward>& out, std::string* err) {
  ProcessResult r = run({"forward", "--list"}, 10000);
  if (!r.ok()) return fail(err, r, "adb forward --list");
  out = parseForwardList(r.out);
  return true;
}

uint16_t Adb::forward(const std::string& serial, uint16_t remotePort, std::string* err) {
  std::vector<AdbForward> fw;
  if (listForwards(fw, nullptr)) {
    const std::string remote = "tcp:" + std::to_string(remotePort);
    for (const AdbForward& f : fw)
      if (f.serial == serial && f.remote == remote && f.localTcpPort()) return f.localTcpPort();
  }
  for (int attempt = 0; attempt < 3; ++attempt) {
    uint16_t port = pickFreeTcpPort();
    if (!port) { if (err) *err = "no free local TCP port"; return 0; }
    ProcessResult r = runFor(serial, {"forward", "tcp:" + std::to_string(port),
                                      "tcp:" + std::to_string(remotePort)}, 10000);
    if (r.ok()) return port;
    if (attempt == 2) { fail(err, r, "adb forward"); return 0; }
  }
  return 0;
}

bool Adb::removeForward(const std::string& serial, uint16_t localPort, std::string* err) {
  ProcessResult r = runFor(serial, {"forward", "--remove", "tcp:" + std::to_string(localPort)});
  if (!r.ok()) return fail(err, r, "adb forward --remove");
  return true;
}

bool Adb::isAppInstalled(const std::string& serial, std::string* err) {
  ProcessResult r = runFor(serial, {"shell", "pm", "path", kPhonePackage});
  if (!r.started || r.timed_out) return fail(err, r, "pm path");
  // pm prints "package:/data/app/.../base.apk"; exit code 1 when absent.
  if (r.out.find("package:") != std::string::npos) return true;
  if (err) err->clear();
  return false;
}

bool Adb::launchApp(const std::string& serial, bool autostart, std::string* err) {
  std::vector<std::string> a = {"shell", "am", "start", "-n", kPhoneActivity};
  if (autostart) { a.push_back("--ez"); a.push_back("autostart"); a.push_back("true"); }
  ProcessResult r = runFor(serial, a);
  if (!r.ok()) return fail(err, r, "am start");
  // am prints "Error: ..." with exit code 0 on some versions.
  if (r.out.find("Error") != std::string::npos || r.err.find("Error") != std::string::npos) {
    if (err) *err = "am start: " + (r.err.empty() ? r.out : r.err);
    return false;
  }
  return true;
}

bool Adb::installApk(const std::string& serial, const std::string& apkPath, std::string* err) {
  ProcessResult r = runFor(serial, {"install", "-r", apkPath}, 180000);
  if (!r.ok() || r.out.find("Success") == std::string::npos) return fail(err, r, "adb install");
  return true;
}

}  // namespace owc
