// Video scopes for the colour page (DaVinci Resolve style): waveform, RGB
// parade, vectorscope and histogram, computed on the CPU from an NV12 frame
// and rendered into RGBA8 images ready to upload as textures. Graticules and
// labels are drawn by the UI on top; vectorscopeTargets() gives the geometry.
// Pure functions, no state: call from any thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace owc {

struct ScopeOptions {
  uint32_t width = 512;    // output width of waveform / parade / histogram images
  uint32_t height = 256;   // output height of waveform / parade / histogram images
  uint32_t vector_size = 256;  // vectorscope image is vector_size x vector_size
  uint32_t step = 2;       // sample every `step` pixels in x and y (speed vs detail)
  float gain = 1.0f;       // trace brightness
  uint32_t matrix = 0;     // OWC_COLOR_* (0 = 709 if h >= 720 else 601)
  bool full_range = false;
};

// RGBA8, straight alpha, background transparent (alpha 0) so the UI's panel
// colour shows through. Waveform: luma traces (white/green-ish like Resolve).
// Parade: R, G, B side by side, each width/3 wide, in their own colours.
// Vectorscope: Cb right, Cr up, 75%/100% colour targets at the positions
// from vectorscopeTargets(); chroma +-0.5 spans +-0.45 of the image (a circle
// of radius 0.45 around the centre = full Cb/Cr scale). Histogram: R, G, B
// (additive) + luma outline; x = 0..1 value, y = normalised count (sqrt-scaled).
// The y axis of waveform/parade is value 0 (bottom) .. 1 (top) in full-range
// RGB/luma (limited-range input is expanded first): row = (h-1) * (1 - value).
// Densities are log tone-mapped; `gain` scales trace brightness. Counting is
// per 8-bit code value and every scope is rendered by box resampling (an
// output pixel integrates the codes whose value interval overlaps it), so
// traces stay continuous at any image size, also for expanded limited-range
// input: waveform limited luma value = (code - 16) / 219, vectorscope chroma
// (code - 128) / 224 (limited) or / 255 (full) before the +-0.45 scaling.
// Histogram: luma spans codes 16..235 (limited) or 0..255 over the full
// width; R/G/B bins are lightly smoothed for limited-range input.
struct ScopeImages {
  uint32_t w = 0, h = 0, vs = 0;
  std::vector<uint8_t> waveform, parade, histogram;  // w*h*4 each
  std::vector<uint8_t> vectorscope;                  // vs*vs*4
};

// Reuse `out` between calls (its buffers keep their capacity). ~4-6 ms for
// 1080p at step 2 on one core; keeps ~1.5 MB of per-thread scratch.
void computeScopes(const uint8_t* y, size_t yStride, const uint8_t* uv, size_t uvStride,
                   uint32_t w, uint32_t h, const ScopeOptions& opt, ScopeImages& out);

// Vectorscope graticule geometry in normalised image coordinates (0..1, origin
// top-left, centre 0.5,0.5): the 100% and 75% R/Mg/B/Cy/G/Yl targets for the
// given matrix, and the skin tone line angle (radians, counter-clockwise from
// +x, about 123 deg).
struct VectorTarget {
  const char* label;  // "R", "Mg", "B", "Cy", "G", "Yl"
  float x100, y100, x75, y75;
};
std::vector<VectorTarget> vectorscopeTargets(uint32_t matrix);
float vectorscopeSkinLineAngle();

}  // namespace owc
