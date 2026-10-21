#include "transform.h"

#include <algorithm>
#include <cstring>

namespace owc {
namespace detail {

int normalizeRotation(int deg) {
  int d = deg % 360;
  if (d < 0) d += 360;
  int q = (d + 45) / 90;  // nearest quarter
  return (q % 4) * 90;
}

namespace {

template <class T>
inline const T* px(const uint8_t* base, size_t stride, int x, int y) {
  return reinterpret_cast<const T*>(base + static_cast<size_t>(y) * stride) + x;
}

template <class T>
void reverseRow(const T* s, T* d, int n) {
  for (int i = 0; i < n; ++i) d[i] = s[n - 1 - i];
}

// Plane of sw x sh elements of type T (uint8 for Y, uint16 for an interleaved UV pair).
template <class T>
void xformPlane(const uint8_t* src, size_t sstride, int sw, int sh, uint8_t* dst,
                size_t dstride, int rot, bool mirror) {
  if (rot == 0 || rot == 180) {
    for (int dy = 0; dy < sh; ++dy) {
      const int sy = rot == 0 ? dy : sh - 1 - dy;
      const T* s = px<T>(src, sstride, 0, sy);
      T* d = reinterpret_cast<T*>(dst + static_cast<size_t>(dy) * dstride);
      // rot 0: copy (mirror -> reverse). rot 180: reverse (mirror -> copy).
      const bool reverse = (rot == 180) != mirror;
      if (reverse)
        reverseRow(s, d, sw);
      else
        memcpy(d, s, static_cast<size_t>(sw) * sizeof(T));
    }
    return;
  }
  // 90 / 270: output is sh wide, sw tall. Tiled for cache locality.
  const int dw = sh, dh = sw;
  constexpr int kTile = 32;
  for (int ty = 0; ty < dh; ty += kTile) {
    const int ty1 = std::min(ty + kTile, dh);
    for (int tx = 0; tx < dw; tx += kTile) {
      const int tx1 = std::min(tx + kTile, dw);
      for (int dy = ty; dy < ty1; ++dy) {
        T* d = reinterpret_cast<T*>(dst + static_cast<size_t>(dy) * dstride);
        if (rot == 90) {
          // dst(dx,dy) = src(dy, sh-1-rx), rx = mirror ? dw-1-dx : dx
          const int sx = dy;
          for (int dx = tx; dx < tx1; ++dx) {
            const int rx = mirror ? dw - 1 - dx : dx;
            d[dx] = *px<T>(src, sstride, sx, sh - 1 - rx);
          }
        } else {
          // dst(dx,dy) = src(sw-1-dy, rx)
          const int sx = sw - 1 - dy;
          for (int dx = tx; dx < tx1; ++dx) {
            const int rx = mirror ? dw - 1 - dx : dx;
            d[dx] = *px<T>(src, sstride, sx, rx);
          }
        }
      }
    }
  }
}

}  // namespace

void transformNV12(const uint8_t* srcY, size_t srcYStride, const uint8_t* srcUV,
                   size_t srcUVStride, uint32_t w, uint32_t h, int rot, bool mirror,
                   uint8_t* dstY, size_t dstYStride, uint8_t* dstUV, size_t dstUVStride) {
  const int sw = static_cast<int>(w), sh = static_cast<int>(h);
  xformPlane<uint8_t>(srcY, srcYStride, sw, sh, dstY, dstYStride, rot, mirror);
  xformPlane<uint16_t>(srcUV, srcUVStride, sw / 2, sh / 2, dstUV, dstUVStride, rot, mirror);
}

}  // namespace detail
}  // namespace owc
