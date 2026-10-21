// Internal helpers for owc_core (not part of the public API).
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <cstdint>
#include <string>

namespace owc {
namespace detail {

// ---- time
inline int64_t qpcFreq() {
  static const int64_t f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v.QuadPart;
  }();
  return f;
}
inline int64_t qpcNowRaw() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}
// QPC -> microseconds without overflow.
inline int64_t qpcToUs(int64_t qpc) {
  const int64_t f = qpcFreq();
  return (qpc / f) * 1000000 + ((qpc % f) * 1000000) / f;
}
inline int64_t usToQpc(int64_t us) {
  const int64_t f = qpcFreq();
  return (us / 1000000) * f + ((us % 1000000) * f) / 1000000;
}
// PC monotonic clock in µs (QPC based). Used for Ping t0 and arrival times.
inline int64_t pcNowUs() { return qpcToUs(qpcNowRaw()); }
inline int64_t steadyMs() { return static_cast<int64_t>(GetTickCount64()); }
int64_t wallClockMs();

// ---- strings
std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);
std::string hresultToString(long hr);
void setThreadName(const wchar_t* name);

// ---- winsock
// Ref-counted WSAStartup; safe to call from anywhere, any number of times.
bool winsockInit();
// Resolves an IPv4 host (literal or name). false on failure.
bool resolveIPv4(const std::string& host, uint16_t port, sockaddr_in& out);
std::string addrToString(const sockaddr_in& a, bool withPort = true);
bool setNonBlocking(SOCKET s, bool on);
inline void closeSocket(SOCKET& s) {
  if (s != INVALID_SOCKET) {
    closesocket(s);
    s = INVALID_SOCKET;
  }
}

}  // namespace detail
}  // namespace owc
