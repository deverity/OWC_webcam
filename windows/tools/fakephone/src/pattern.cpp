#include "pattern.h"

#include <cstring>

namespace owc {
namespace fake {

void drawCounterStrip(uint8_t* y, size_t ys, uint8_t* uv, size_t uvs, uint32_t w, uint32_t h,
                      uint32_t value) {
  if (w < kStripMinWidth || h < 32) return;
  const uint32_t y0 = h - 32;
  for (uint32_t i = 0; i < kStripBits; ++i) {
    const uint8_t v = ((value >> i) & 1u) ? 235 : 16;
    const uint32_t x0 = kStripX + i * kStripBlock;
    for (uint32_t r = 0; r < kStripBlock; ++r) memset(y + (y0 + r) * ys + x0, v, kStripBlock);
  }
  for (uint32_t r = y0 / 2; r < (y0 + kStripBlock) / 2; ++r)
    memset(uv + r * uvs + kStripX, 128, kStripBits * kStripBlock);
}

bool readCounterStrip(const uint8_t* y, size_t ys, uint32_t w, uint32_t h, uint32_t& value) {
  if (w < kStripMinWidth || h < 32) return false;
  const uint32_t y0 = h - 32;
  uint32_t v = 0;
  for (uint32_t i = 0; i < kStripBits; ++i) {
    const uint32_t x0 = kStripX + i * kStripBlock + 4;
    uint32_t sum = 0;
    for (uint32_t r = 4; r < 12; ++r)
      for (uint32_t c = 0; c < 8; ++c) sum += y[(y0 + r) * ys + x0 + c];
    const uint32_t avg = sum / 64;
    if (avg > 170) v |= 1u << i;
    else if (avg >= 80) return false;
  }
  value = v;
  return true;
}

void drawTestFrame(uint8_t* dst, uint32_t w, uint32_t h, uint32_t counter) {
  uint8_t* Y = dst;
  uint8_t* UV = dst + static_cast<size_t>(w) * h;
  const uint32_t t = counter;
  for (uint32_t r = 0; r < h; ++r) {
    uint8_t* row = Y + static_cast<size_t>(r) * w;
    const uint32_t base = r + t * 4;
    for (uint32_t c = 0; c < w; ++c) row[c] = static_cast<uint8_t>(40 + ((c + base) & 127));
  }
  // moving box
  const uint32_t bs = h / 6;
  const uint32_t span = w > bs ? w - bs : 1;
  uint32_t bx = (t * 8) % (2 * span);
  if (bx >= span) bx = 2 * span - bx;
  const uint32_t by = h / 3;
  for (uint32_t r = by; r < by + bs && r < h; ++r)
    memset(Y + static_cast<size_t>(r) * w + bx, 220, bs);
  for (uint32_t r = 0; r < h / 2; ++r) {
    uint8_t* row = UV + static_cast<size_t>(r) * w;
    for (uint32_t c = 0; c < w / 2; ++c) {
      row[2 * c] = static_cast<uint8_t>(96 + ((c + t) & 63));
      row[2 * c + 1] = static_cast<uint8_t>(96 + ((r + 2 * t) & 63));
    }
  }
  drawCounterStrip(Y, w, UV, w, w, h, counter);
}

}  // namespace fake
}  // namespace owc
