// Colour grading helpers shared by the client and the UI: built-in looks,
// .cube LUT parsing and export of the current grade as a .cube file.
// The per-frame grading itself is internal to owc::Client (ClientOptions::color).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <owc/client.h>

namespace owc {

// A built-in look, selected with ColorGrade::lut = "builtin:" + key.
struct BuiltinLook {
  const char* key;          // stable id, e.g. "teal_orange"
  const char* name;         // UI name (pt-BR), e.g. "Cinema (teal & orange)"
  const char* description;  // one line (pt-BR)
};
const std::vector<BuiltinLook>& builtinLooks();

// A parsed .cube LUT. 3D: size^3 RGB triplets, red changing fastest (the
// .cube order). 1D: size RGB triplets. Values are kept as written (they are
// mapped through domain_min/max on lookup).
struct CubeLut {
  std::string title;
  bool is3d = true;
  int size = 0;
  float domain_min[3] = {0, 0, 0};
  float domain_max[3] = {1, 1, 1};
  std::vector<float> rgb;  // 3 * (is3d ? size^3 : size)
};

// Parses a .cube file (UTF-8 path). Accepts TITLE, LUT_3D_SIZE (2..256),
// LUT_1D_SIZE (2..65536), DOMAIN_MIN/MAX, LUT_3D_INPUT_RANGE /
// LUT_1D_INPUT_RANGE, comments (#) and blank lines. On failure returns false
// and sets `err` (pt-BR, with the line number when relevant).
bool loadCubeFile(const std::string& path, CubeLut& out, std::string& err);
bool parseCube(const std::string& text, CubeLut& out, std::string& err);

// Writes the whole grade (adjustments + LUT at its strength) as a 3D .cube of
// `size` points per axis, in full-range RGB, so it can be used in OBS,
// DaVinci Resolve, Premiere, etc. Ignores `enabled` and `compare_split`.
bool exportCube(const ColorGrade& g, const std::string& path, int size, std::string& err);

// True when the grade changes nothing (disabled, or neutral values and no LUT
// or strength 0).
bool isIdentity(const ColorGrade& g);

// ---- colour wheels
// Puck position on the wheel's unit disk (x right, y up; |p| <= 1) <-> r/g/b
// deltas summing to 0. As in Resolve, the puck direction is the vectorscope
// direction (BT.709 Cb right, Cr up) the colour is pushed towards, so the
// wheels line up with vectorscopeTargets(): R ~103 deg (upper left), Yl ~175,
// G ~230 (lower left), Cy ~283, B ~355 (right), Mg ~50 (counter-clockwise
// from +x). Magnitude: max(|r|,|g|,|b|) = |p|, i.e. radius 1 is a full +-1 on
// the strongest channel in every direction. rgbToWheel is the exact inverse
// for zero-sum input (other input is projected first: its mean is removed).
void wheelToRgb(float px, float py, float& r, float& g, float& b);
void rgbToWheel(float r, float g, float b, float& px, float& py);

// ---- curves (the exact functions the grade uses, for drawing them in the UI)
// All three accept unsorted input: points are clamped to 0..1, sorted by x,
// and points closer than 1e-4 in x are merged (mean); non-finite ones dropped.
// Monotone cubic Hermite with Fritsch-Carlson tangents: C1, no overshoot (the
// curve never leaves the range of its points' y). Output always in 0..1.
// Custom curve: identity if empty or all points on the diagonal. Ends: if
// there is no point at x = 0 (resp. 1) the curve implicitly starts at (0,0)
// (resp. ends at (1,1)), so a single point bends the identity through it.
float evalCurve(const std::vector<CurvePoint>& pts, float x);
// Hue curve: periodic over x in 0..1 (x = 1 is x = 0; the segment from the
// last point wraps to the first), returns y (0.5 if empty; a single point =
// constant). Hue x: R 0, Yl 1/6, G 1/3, Cy 1/2, B 2/3, Mg 5/6 (HSV-like, so
// an HSV rainbow can be drawn under it), piecewise linear in the BT.709 Cb/Cr
// angle between those anchors.
float evalHueCurve(const std::vector<CurvePoint>& pts, float x);
// Luma vs sat: non-periodic, flat before the first / after the last point,
// returns y (0.5 if empty).
float evalLumaSatCurve(const std::vector<CurvePoint>& pts, float x);

// ---- previews for the LUT gallery
// Box-downscales a tightly packed or strided NV12 frame to dstW x dstH (even;
// odd sizes are rounded down) into tightly packed NV12 (cleared on bad input).
void downscaleNV12(const uint8_t* y, size_t yStride, const uint8_t* uv, size_t uvStride,
                   uint32_t w, uint32_t h, uint32_t dstW, uint32_t dstH, std::vector<uint8_t>& dst);
// Renders a small NV12 frame (tightly packed) graded with `g` into RGBA8
// (w*h*4), evaluating the RGB pipeline per pixel (no table: a few ms for
// 192x108). `matrix` = OWC_COLOR_BT601/BT709 (0 = 709 if h >= 720 else 601:
// pass the stream's own matrix for a thumbnail of an HD stream, a 108-line
// image would otherwise be read as 601). Output = the graded RGB clamped to
// 0..255, opaque, no further conversion (same YUV->RGB as the live grade).
// Ignores `enabled` and `compare_split` (the gallery always shows the look).
// The LUT-less part of the last grade is cached, so a gallery of one grade
// with many LUTs prepares it once (~4 ms worst case with every control on).
// .cube files go through an internal cache keyed by path + mtime. Thread-safe.
// Returns false (and sets err) when the LUT cannot be loaded; rgba then holds
// the grade without the LUT.
bool renderGradedRgba(const ColorGrade& g, const uint8_t* nv12, uint32_t w, uint32_t h,
                      uint32_t matrix, bool fullRange, std::vector<uint8_t>& rgba,
                      std::string* err = nullptr);

}  // namespace owc
