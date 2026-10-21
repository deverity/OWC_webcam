// Minimal Annex-B walker (H.264 / HEVC) used by the client and fakephone.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <owc/protocol.h>

namespace owc {
namespace detail {

// Calls f(nal_ptr, nal_len) for each NAL unit (pointer after the start code).
template <class F>
void forEachNal(const uint8_t* p, size_t n, F&& f) {
  size_t i = 0;
  // find first start code
  auto findStart = [&](size_t from, size_t& scLen) -> size_t {
    for (size_t k = from; k + 3 <= n; ++k) {
      if (p[k] == 0 && p[k + 1] == 0) {
        if (p[k + 2] == 1) { scLen = 3; return k; }
        if (k + 4 <= n && p[k + 2] == 0 && p[k + 3] == 1) { scLen = 4; return k; }
      }
    }
    scLen = 0;
    return n;
  };
  size_t sc = 0;
  i = findStart(0, sc);
  while (i < n) {
    const size_t start = i + sc;
    size_t sc2 = 0;
    size_t next = findStart(start, sc2);
    size_t end = next;
    // trim trailing zero bytes (belong to the next 4-byte start code / padding)
    while (end > start && p[end - 1] == 0) --end;  // trailing_zero_8bits / 4-byte start code
    if (end > start) f(p + start, end - start);
    i = next;
    sc = sc2;
  }
}

inline int nalType(Codec c, uint8_t b0) {
  return c == Codec::H264 ? (b0 & 0x1F) : ((b0 >> 1) & 0x3F);
}
inline bool isParamSetNal(Codec c, int t) {
  return c == Codec::H264 ? (t == 7 || t == 8) : (t >= 32 && t <= 34);
}
// Coded picture data (slice NALs).
inline bool isVclNal(Codec c, int t) {
  return c == Codec::H264 ? (t >= 1 && t <= 5) : (t >= 0 && t <= 31);
}
inline bool isSpsNal(Codec c, int t) { return c == Codec::H264 ? t == 7 : t == 33; }
inline bool isIdrNal(Codec c, int t) {
  return c == Codec::H264 ? t == 5 : (t >= 16 && t <= 21);
}

// True if the AU contains an SPS.
inline bool hasSps(Codec c, const uint8_t* p, size_t n) {
  bool found = false;
  forEachNal(p, n, [&](const uint8_t* nal, size_t len) {
    if (len && isSpsNal(c, nalType(c, nal[0]))) found = true;
  });
  return found;
}

inline void appendNal(std::vector<uint8_t>& out, const uint8_t* nal, size_t len) {
  static const uint8_t sc[4] = {0, 0, 0, 1};
  out.insert(out.end(), sc, sc + 4);
  out.insert(out.end(), nal, nal + len);
}

}  // namespace detail
}  // namespace owc
