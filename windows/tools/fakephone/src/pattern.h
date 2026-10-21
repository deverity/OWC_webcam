// Synthetic NV12 test pattern + frame-counter strip.
//
// Counter strip (shared convention with owc_testsrc): 32 blocks of 16x16 px
// starting at x=16, y=height-32; block i = bit i (LSB first) of the uint32
// counter; white Y=235 = 1, black Y=16 = 0; UV = 128 inside the strip area.
// Needs width >= 16 + 32*16 = 528 and height >= 32.
#pragma once

#include <cstddef>
#include <cstdint>

namespace owc {
namespace fake {

constexpr uint32_t kStripX = 16, kStripBlock = 16, kStripBits = 32;
constexpr uint32_t kStripMinWidth = kStripX + kStripBits * kStripBlock;

// Draws the strip into an NV12 image.
void drawCounterStrip(uint8_t* y, size_t yStride, uint8_t* uv, size_t uvStride, uint32_t w,
                      uint32_t h, uint32_t value);

// Reads the strip back (averages the inner 8x8 of each block). Returns false
// if the image is too small or any block is ambiguous (not clearly black or
// white), i.e. the strip is not present / heavily corrupted.
bool readCounterStrip(const uint8_t* y, size_t yStride, uint32_t w, uint32_t h, uint32_t& value);

// Full test frame: moving gradient + moving box + counter strip.
// dst is tightly packed NV12 (w*h*3/2 bytes). w, h even.
void drawTestFrame(uint8_t* dst, uint32_t w, uint32_t h, uint32_t counter);

}  // namespace fake
}  // namespace owc
