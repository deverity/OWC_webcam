// NV12 rotate (clockwise 0/90/180/270) + horizontal mirror on the CPU.
#pragma once

#include <cstddef>
#include <cstdint>

namespace owc {
namespace detail {

// Normalizes any angle to 0/90/180/270 (nearest multiple of 90).
int normalizeRotation(int deg);

// Output size of a transform (swaps for 90/270).
inline void transformedSize(uint32_t w, uint32_t h, int rot, uint32_t& ow, uint32_t& oh) {
  if (rot == 90 || rot == 270) { ow = h; oh = w; } else { ow = w; oh = h; }
}

// final = mirror_h(rotate_cw(src, rot)). w/h must be even. dst planes are
// written with the given strides (>= output width). rot must be normalized.
// Source and destination must not overlap.
void transformNV12(const uint8_t* srcY, size_t srcYStride, const uint8_t* srcUV,
                   size_t srcUVStride, uint32_t w, uint32_t h, int rot, bool mirror,
                   uint8_t* dstY, size_t dstYStride, uint8_t* dstUV, size_t dstUVStride);

}  // namespace detail
}  // namespace owc
