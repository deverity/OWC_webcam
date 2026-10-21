// NV12 -> {NV12, YUY2, I420, RGB24, RGB32} conversion with aspect-preserving
// bilinear scaling (letterbox / pillarbox with black) and the "no signal"
// placeholder. Pure CPU code (SSE2), no Windows/COM dependencies, so the probe
// tool can benchmark exactly what the filter runs.
//
// Allocation policy: a Converter owns its scratch buffers. They are (re)sized
// only when the source or output geometry changes, never per frame.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace owc {
namespace vcam {

enum class PixFmt : int { NV12 = 0, YUY2, I420, RGB32, RGB24, Count };

const char* pixFmtName(PixFmt f);
bool pixFmtFromName(const char* name, PixFmt* out);  // case-insensitive
int pixFmtBitCount(PixFmt f);                         // biBitCount
bool pixFmtIsRgb(PixFmt f);

// Default (DIB-aligned) stride in bytes of the first plane for `width` pixels.
int defaultStride(PixFmt f, int width);

// YUV -> RGB matrix for RGB outputs (input is limited range). YUV outputs
// pass samples through unchanged.
enum class ColorMatrix : int { Bt601 = 0, Bt709 };

// Describes a destination buffer as negotiated with DirectShow.
struct OutputDesc {
  PixFmt fmt = PixFmt::NV12;
  int width = 0;    // visible pixels
  int height = 0;   // visible rows
  int stride = 0;   // bytes per row of the first plane (I420 chroma rows = stride/2)
  bool bottomUp = false;  // RGB only: positive biHeight = bottom-up DIB
  ColorMatrix matrix = ColorMatrix::Bt601;
};

// Bytes required by `out` (all planes).
size_t outputBytes(const OutputDesc& out);

struct Nv12View {
  const uint8_t* y = nullptr;
  const uint8_t* uv = nullptr;
  int width = 0, height = 0;  // even
  int yStride = 0, uvStride = 0;
};

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

// Largest even-aligned rect with the source aspect ratio centered in out.
Rect fitRect(int srcW, int srcH, int outW, int outH);

// Bilinear plane scaler, exposed for tests. channels = 1 (Y) or 2 (UV).
struct ScaleScratch {
  int tabSrc = 0, tabDst = 0, tabCh = 0;  // key of the cached x tables
  std::vector<int16_t> rowA, rowB;
  std::vector<uint8_t> row8;  // vertical-first path (vertical downscale)
  std::vector<int32_t> xIdx;
  std::vector<int16_t> xFrac;
};
void scalePlane(const uint8_t* src, int srcW, int srcH, int srcStride, uint8_t* dst, int dstW,
                int dstH, int dstStride, int channels, ScaleScratch& scratch);

class Converter {
 public:
  // Converts `src` into `dst` (size dstBytes) according to `out`.
  // `srcId` identifies the source frame contents (frame number, or a
  // placeholder id): when it and the geometry are unchanged the scaled
  // intermediate is reused (repeat frames cost only the format conversion).
  // Returns false if the destination is too small or arguments are invalid.
  bool convert(const Nv12View& src, uint64_t srcId, uint8_t* dst, size_t dstBytes,
               const OutputDesc& out);

  // Releases scratch memory.
  void reset();

 private:
  void ensureScaled(const Nv12View& src, uint64_t srcId, int dw, int dh);

  std::vector<uint8_t> scaled_;  // NV12 at the letterbox rect size
  int scaledW_ = 0, scaledH_ = 0;
  int scaledSrcW_ = 0, scaledSrcH_ = 0;
  uint64_t scaledId_ = ~0ull;
  bool scaledValid_ = false;
  ScaleScratch scratchY_, scratchUV_;
  std::vector<uint8_t> rgbRow_;       // RGB24 staging row (RGB32)
};

// Renders the "no signal" placeholder as tightly packed NV12 (stride = w)
// into `nv12`, which must hold w*h*3/2 bytes. w, h even.
void drawPlaceholder(uint8_t* nv12, int w, int h);


}  // namespace vcam
}  // namespace owc
