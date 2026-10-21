#include "util.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace owc {
namespace detail {

int64_t wallClockMs() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  // 100 ns ticks since 1601 -> ms since 1970
  return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

std::string wideToUtf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                              nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr,
                      nullptr);
  return s;
}

std::string hresultToString(long hr) {
  char buf[32];
  snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
  return buf;
}

void setThreadName(const wchar_t* name) {
  using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  static Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
  if (fn) fn(GetCurrentThread(), name);
}

bool winsockInit() {
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    WSADATA d;
    ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    // Intentionally never WSACleanup(): sockets may be used until process exit.
  });
  return ok;
}

bool resolveIPv4(const std::string& host, uint16_t port, sockaddr_in& out) {
  if (!winsockInit()) return false;
  memset(&out, 0, sizeof(out));
  out.sin_family = AF_INET;
  out.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
  out.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
  freeaddrinfo(res);
  return true;
}

std::string addrToString(const sockaddr_in& a, bool withPort) {
  char ip[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
  std::string s(ip);
  if (withPort) s += ":" + std::to_string(ntohs(a.sin_port));
  return s;
}

bool setNonBlocking(SOCKET s, bool on) {
  u_long v = on ? 1 : 0;
  return ioctlsocket(s, FIONBIO, &v) == 0;
}

}  // namespace detail
}  // namespace owc
