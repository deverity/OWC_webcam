#include "convert.h"

#include <emmintrin.h>  // SSE2: baseline on x64 and on every x86 CPU that runs Windows 8+
#include <string.h>

#include <algorithm>

namespace owc {
namespace vcam {

namespace {

const char* const kFmtNames[] = {"NV12", "YUY2", "I420", "RGB32", "RGB24"};

inline uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// Limited range YUV -> full range RGB coefficients, 6-bit fixed point.
struct Coeffs {
  int y, rv, gu, gv, bu;
};
const Coeffs kBt601 = {74, 102, 25, 52, 129};  // 1.164, 1.596, 0.391, 0.813, 2.018
const Coeffs kBt709 = {74, 115, 14, 34, 135};  // 1.164, 1.793, 0.213, 0.533, 2.112

inline void yuvToBgr(int y, int u, int v, uint8_t* bgr, const Coeffs& k) {
  const int c = (y - 16) * k.y;
  const int d = u - 128, e = v - 128;
  bgr[0] = clamp255((c + k.bu * d + 32) >> 6);
  bgr[1] = clamp255((c - k.gu * d - k.gv * e + 32) >> 6);
  bgr[2] = clamp255((c + k.rv * e + 32) >> 6);
}

// ---------------------------------------------------------------- row kernels

// One row of NV12 (y: w luma, uv: w/2 pairs) -> BGRA (w*4 bytes). w even.
void rowNv12ToBgra(const uint8_t* y, const uint8_t* uv, uint8_t* out, int w, const Coeffs& k) {
  const __m128i zero = _mm_setzero_si128();
  const __m128i k16 = _mm_set1_epi16(16), k128 = _mm_set1_epi16(128), kRound = _mm_set1_epi16(32);
  const __m128i cY = _mm_set1_epi16((short)k.y), cRV = _mm_set1_epi16((short)k.rv),
                cGU = _mm_set1_epi16((short)k.gu), cGV = _mm_set1_epi16((short)k.gv),
                cBU = _mm_set1_epi16((short)k.bu);
  const __m128i lowMask = _mm_set1_epi16(0x00FF), alpha = _mm_set1_epi8(-1);
  int x = 0;
  for (; x + 16 <= w; x += 16) {
    const __m128i yv = _mm_loadu_si128((const __m128i*)(y + x));
    const __m128i uvv = _mm_loadu_si128((const __m128i*)(uv + x));
    const __m128i ylo = _mm_mullo_epi16(_mm_sub_epi16(_mm_unpacklo_epi8(yv, zero), k16), cY);
    const __m128i yhi = _mm_mullo_epi16(_mm_sub_epi16(_mm_unpackhi_epi8(yv, zero), k16), cY);
    const __m128i u = _mm_sub_epi16(_mm_and_si128(uvv, lowMask), k128);
    const __m128i v = _mm_sub_epi16(_mm_srli_epi16(uvv, 8), k128);
    const __m128i rv = _mm_add_epi16(_mm_mullo_epi16(v, cRV), kRound);
    const __m128i gc = _mm_sub_epi16(kRound, _mm_add_epi16(_mm_mullo_epi16(u, cGU), _mm_mullo_epi16(v, cGV)));
    const __m128i bu = _mm_add_epi16(_mm_mullo_epi16(u, cBU), kRound);
    // duplicate each chroma term for its two pixels
    const __m128i rlo = _mm_srai_epi16(_mm_adds_epi16(ylo, _mm_unpacklo_epi16(rv, rv)), 6);
    const __m128i rhi = _mm_srai_epi16(_mm_adds_epi16(yhi, _mm_unpackhi_epi16(rv, rv)), 6);
    const __m128i glo = _mm_srai_epi16(_mm_adds_epi16(ylo, _mm_unpacklo_epi16(gc, gc)), 6);
    const __m128i ghi = _mm_srai_epi16(_mm_adds_epi16(yhi, _mm_unpackhi_epi16(gc, gc)), 6);
    const __m128i blo = _mm_srai_epi16(_mm_adds_epi16(ylo, _mm_unpacklo_epi16(bu, bu)), 6);
    const __m128i bhi = _mm_srai_epi16(_mm_adds_epi16(yhi, _mm_unpackhi_epi16(bu, bu)), 6);
    const __m128i r8 = _mm_packus_epi16(rlo, rhi);
    const __m128i g8 = _mm_packus_epi16(glo, ghi);
    const __m128i b8 = _mm_packus_epi16(blo, bhi);
    const __m128i bgLo = _mm_unpacklo_epi8(b8, g8), bgHi = _mm_unpackhi_epi8(b8, g8);
    const __m128i raLo = _mm_unpacklo_epi8(r8, alpha), raHi = _mm_unpackhi_epi8(r8, alpha);
    __m128i* o = (__m128i*)(out + x * 4);
    _mm_storeu_si128(o + 0, _mm_unpacklo_epi16(bgLo, raLo));
    _mm_storeu_si128(o + 1, _mm_unpackhi_epi16(bgLo, raLo));
    _mm_storeu_si128(o + 2, _mm_unpacklo_epi16(bgHi, raHi));
    _mm_storeu_si128(o + 3, _mm_unpackhi_epi16(bgHi, raHi));
  }
  for (; x < w; x += 2) {
    const int u = uv[x], v = uv[x + 1];
    yuvToBgr(y[x], u, v, out + x * 4, k);
    out[x * 4 + 3] = 0xFF;
    yuvToBgr(y[x + 1], u, v, out + x * 4 + 4, k);
    out[x * 4 + 7] = 0xFF;
  }
}

void rowNv12ToYuy2(const uint8_t* y, const uint8_t* uv, uint8_t* out, int w) {
  int x = 0;
  for (; x + 16 <= w; x += 16) {
    const __m128i yv = _mm_loadu_si128((const __m128i*)(y + x));
    const __m128i uvv = _mm_loadu_si128((const __m128i*)(uv + x));
    _mm_storeu_si128((__m128i*)(out + x * 2), _mm_unpacklo_epi8(yv, uvv));
    _mm_storeu_si128((__m128i*)(out + x * 2 + 16), _mm_unpackhi_epi8(yv, uvv));
  }
  for (; x < w; x += 2) {
    out[x * 2 + 0] = y[x];
    out[x * 2 + 1] = uv[x];
    out[x * 2 + 2] = y[x + 1];
    out[x * 2 + 3] = uv[x + 1];
  }
}

// uv: n pairs -> u[n], v[n]
void rowDeinterleave(const uint8_t* uv, uint8_t* u, uint8_t* v, int n) {
  const __m128i mask = _mm_set1_epi16(0x00FF);
  int i = 0;
  for (; i + 16 <= n; i += 16) {
    const __m128i a = _mm_loadu_si128((const __m128i*)(uv + i * 2));
    const __m128i b = _mm_loadu_si128((const __m128i*)(uv + i * 2 + 16));
    _mm_storeu_si128((__m128i*)(u + i), _mm_packus_epi16(_mm_and_si128(a, mask), _mm_and_si128(b, mask)));
    _mm_storeu_si128((__m128i*)(v + i), _mm_packus_epi16(_mm_srli_epi16(a, 8), _mm_srli_epi16(b, 8)));
  }
  for (; i < n; ++i) {
    u[i] = uv[i * 2];
    v[i] = uv[i * 2 + 1];
  }
}

void rowBgraToBgr(const uint8_t* in, uint8_t* out, int w) {
  int x = 0;
  for (; x + 4 <= w; x += 4) {  // 4 px: 16 bytes in, 12 out
    uint32_t p0, p1, p2, p3;
    memcpy(&p0, in + x * 4, 4);
    memcpy(&p1, in + x * 4 + 4, 4);
    memcpy(&p2, in + x * 4 + 8, 4);
    memcpy(&p3, in + x * 4 + 12, 4);
    const uint32_t o0 = (p0 & 0xFFFFFFu) | (p1 << 24);
    const uint32_t o1 = ((p1 >> 8) & 0xFFFFu) | (p2 << 16);
    const uint32_t o2 = ((p2 >> 16) & 0xFFu) | (p3 << 8);
    memcpy(out + x * 3, &o0, 4);
    memcpy(out + x * 3 + 4, &o1, 4);
    memcpy(out + x * 3 + 8, &o2, 4);
  }
  for (; x < w; ++x) {
    out[x * 3 + 0] = in[x * 4 + 0];
    out[x * 3 + 1] = in[x * 4 + 1];
    out[x * 3 + 2] = in[x * 4 + 2];
  }
}

void fillPattern32(uint8_t* p, int count, uint32_t pattern) {
  for (int i = 0; i < count; ++i) memcpy(p + (size_t)i * 4, &pattern, 4);
}

// ---------------------------------------------------------------- scaler

void buildTable(int srcLen, int dstLen, int channels, std::vector<int32_t>& idx,
                std::vector<int16_t>& frac) {
  idx.resize((size_t)dstLen);
  frac.resize((size_t)dstLen);
  const int64_t step = ((int64_t)srcLen << 16) / dstLen;
  int64_t pos = step / 2 - 32768;  // pixel centers aligned
  for (int x = 0; x < dstLen; ++x, pos += step) {
    const int64_t p = pos < 0 ? 0 : pos;
    int i = (int)(p >> 16);
    int f = (int)(((p & 0xFFFF) + 256) >> 9);  // 7-bit weight 0..128
    if (i >= srcLen - 1) {
      i = srcLen - 2;
      f = 128;
    }
    idx[(size_t)x] = i * channels;
    frac[(size_t)x] = (int16_t)f;
  }
}

void vertPos(int y, int srcLen, int dstLen, int* i, int* f) {
  const int64_t step = ((int64_t)srcLen << 16) / dstLen;
  int64_t p = step / 2 - 32768 + step * y;
  if (p < 0) p = 0;
  int ii = (int)(p >> 16);
  int ff = (int)(((p & 0xFFFF) + 256) >> 9);
  if (ii >= srcLen - 1) {
    ii = srcLen - 2;
    ff = 128;
  }
  *i = ii;
  *f = ff;
}

void hscaleRow(const uint8_t* s, int16_t* d, int dstW, int srcW, int ch, const int32_t* idx,
               const int16_t* frac) {
  if (dstW == srcW) {
    const int n = dstW * ch;
    for (int i = 0; i < n; ++i) d[i] = (int16_t)(s[i] << 7);
    return;
  }
  if (ch == 1) {
    for (int x = 0; x < dstW; ++x) {
      const int i = idx[x], f = frac[x];
      d[x] = (int16_t)(s[i] * (128 - f) + s[i + 1] * f);
    }
  } else {
    for (int x = 0; x < dstW; ++x) {
      const int i = idx[x], f = frac[x], g = 128 - f;
      d[2 * x] = (int16_t)(s[i] * g + s[i + 2] * f);
      d[2 * x + 1] = (int16_t)(s[i + 1] * g + s[i + 3] * f);
    }
  }
}

void vblend(const int16_t* a, const int16_t* b, int f, uint8_t* d, int n) {
  int x = 0;
  if (f == 0 || f == 128) {
    const int16_t* s = f == 0 ? a : b;
    const __m128i r = _mm_set1_epi16(64);
    for (; x + 16 <= n; x += 16) {
      const __m128i lo = _mm_srai_epi16(_mm_add_epi16(_mm_loadu_si128((const __m128i*)(s + x)), r), 7);
      const __m128i hi = _mm_srai_epi16(_mm_add_epi16(_mm_loadu_si128((const __m128i*)(s + x + 8)), r), 7);
      _mm_storeu_si128((__m128i*)(d + x), _mm_packus_epi16(lo, hi));
    }
    for (; x < n; ++x) d[x] = (uint8_t)((s[x] + 64) >> 7);
    return;
  }
  const __m128i w = _mm_set1_epi32((f << 16) | (128 - f));
  const __m128i r = _mm_set1_epi32(8192);
  for (; x + 8 <= n; x += 8) {
    const __m128i va = _mm_loadu_si128((const __m128i*)(a + x));
    const __m128i vb = _mm_loadu_si128((const __m128i*)(b + x));
    const __m128i lo = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(va, vb), w), r), 14);
    const __m128i hi = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(_mm_unpackhi_epi16(va, vb), w), r), 14);
    const __m128i p = _mm_packs_epi32(lo, hi);
    _mm_storel_epi64((__m128i*)(d + x), _mm_packus_epi16(p, p));
  }
  for (; x < n; ++x) d[x] = (uint8_t)((a[x] * (128 - f) + b[x] * f + 8192) >> 14);
}

// 8-bit two-row lerp (vertical-first path).
void vblend8(const uint8_t* a, const uint8_t* b, int f, uint8_t* d, int n) {
  if (f == 0 || f == 128) {
    memcpy(d, f == 0 ? a : b, (size_t)n);
    return;
  }
  const __m128i zero = _mm_setzero_si128();
  const __m128i wa = _mm_set1_epi16((short)(128 - f)), wb = _mm_set1_epi16((short)f), r = _mm_set1_epi16(64);
  int x = 0;
  for (; x + 16 <= n; x += 16) {
    const __m128i va = _mm_loadu_si128((const __m128i*)(a + x));
    const __m128i vb = _mm_loadu_si128((const __m128i*)(b + x));
    const __m128i lo = _mm_srli_epi16(
        _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(va, zero), wa),
                                    _mm_mullo_epi16(_mm_unpacklo_epi8(vb, zero), wb)), r), 7);
    const __m128i hi = _mm_srli_epi16(
        _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(va, zero), wa),
                                    _mm_mullo_epi16(_mm_unpackhi_epi8(vb, zero), wb)), r), 7);
    _mm_storeu_si128((__m128i*)(d + x), _mm_packus_epi16(lo, hi));
  }
  for (; x < n; ++x) d[x] = (uint8_t)((a[x] * (128 - f) + b[x] * f + 64) >> 7);
}

void hscaleRow8(const uint8_t* s, uint8_t* d, int dstW, int ch, const int32_t* idx, const int16_t* frac) {
  if (ch == 1) {
    for (int x = 0; x < dstW; ++x) {
      const int i = idx[x], f = frac[x];
      d[x] = (uint8_t)((s[i] * (128 - f) + s[i + 1] * f + 64) >> 7);
    }
  } else {
    for (int x = 0; x < dstW; ++x) {
      const int i = idx[x], f = frac[x], g = 128 - f;
      d[2 * x] = (uint8_t)((s[i] * g + s[i + 2] * f + 64) >> 7);
      d[2 * x + 1] = (uint8_t)((s[i + 1] * g + s[i + 3] * f + 64) >> 7);
    }
  }
}

}  // namespace

// ---------------------------------------------------------------- public helpers

const char* pixFmtName(PixFmt f) {
  const int i = (int)f;
  return (i >= 0 && i < (int)PixFmt::Count) ? kFmtNames[i] : "?";
}

bool pixFmtFromName(const char* name, PixFmt* out) {
  if (!name) return false;
  for (int i = 0; i < (int)PixFmt::Count; ++i) {
    if (_stricmp(name, kFmtNames[i]) == 0) {
      *out = (PixFmt)i;
      return true;
    }
  }
  return false;
}

int pixFmtBitCount(PixFmt f) {
  switch (f) {
    case PixFmt::NV12:
    case PixFmt::I420: return 12;
    case PixFmt::YUY2: return 16;
    case PixFmt::RGB24: return 24;
    case PixFmt::RGB32: return 32;
    default: return 0;
  }
}

bool pixFmtIsRgb(PixFmt f) { return f == PixFmt::RGB24 || f == PixFmt::RGB32; }

int defaultStride(PixFmt f, int width) {
  switch (f) {
    case PixFmt::NV12:
    case PixFmt::I420: return width;
    case PixFmt::YUY2: return width * 2;
    case PixFmt::RGB24: return (width * 3 + 3) & ~3;
    case PixFmt::RGB32: return width * 4;
    default: return 0;
  }
}

size_t outputBytes(const OutputDesc& o) {
  const size_t s = (size_t)o.stride, h = (size_t)o.height;
  switch (o.fmt) {
    case PixFmt::NV12: return s * h + s * (h / 2);
    case PixFmt::I420: return s * h + 2 * (s / 2) * (h / 2);
    default: return s * h;
  }
}

Rect fitRect(int srcW, int srcH, int outW, int outH) {
  Rect r{0, 0, outW, outH};
  if (srcW <= 0 || srcH <= 0) return r;
  const int64_t a = (int64_t)srcW * outH, b = (int64_t)srcH * outW;
  if (a == b) return r;
  if (a > b) {  // source wider: letterbox (bars top/bottom)
    int h = (int)(((int64_t)outW * srcH + srcW / 2) / srcW);
    h = std::min(outH, std::max(2, (h + 1) & ~1));
    r.h = h;
    r.y = ((outH - h) / 2) & ~1;
  } else {  // source taller: pillarbox
    int w = (int)(((int64_t)outH * srcW + srcH / 2) / srcH);
    w = std::min(outW, std::max(2, (w + 1) & ~1));
    r.w = w;
    r.x = ((outW - w) / 2) & ~1;
  }
  return r;
}

void scalePlane(const uint8_t* src, int srcW, int srcH, int srcStride, uint8_t* dst, int dstW,
                int dstH, int dstStride, int channels, ScaleScratch& sc) {
  if (srcW < 2 || srcH < 2 || dstW < 1 || dstH < 1) return;
  if (sc.tabSrc != srcW || sc.tabDst != dstW || sc.tabCh != channels) {
    buildTable(srcW, dstW, channels, sc.xIdx, sc.xFrac);
    sc.tabSrc = srcW;
    sc.tabDst = dstW;
    sc.tabCh = channels;
  }
  const size_t n = (size_t)dstW * channels;
  if (dstH < srcH && dstW != srcW) {
    // Vertical downscale: blend the two source rows first (SIMD, at source
    // width) and scale horizontally once per output row -> fewer gathers.
    const size_t sn = (size_t)srcW * channels;
    if (sc.row8.size() < sn + 16) sc.row8.resize(sn + 16);
    for (int y = 0; y < dstH; ++y) {
      int i, f;
      vertPos(y, srcH, dstH, &i, &f);
      const uint8_t* a = src + (size_t)i * srcStride;
      vblend8(a, a + srcStride, f, sc.row8.data(), (int)sn);
      hscaleRow8(sc.row8.data(), dst + (size_t)y * dstStride, dstW, channels, sc.xIdx.data(), sc.xFrac.data());
    }
    return;
  }
  if (sc.rowA.size() < n + 16) sc.rowA.resize(n + 16);
  if (sc.rowB.size() < n + 16) sc.rowB.resize(n + 16);
  int16_t* ra = sc.rowA.data();
  int16_t* rb = sc.rowB.data();
  int ia = -1, ib = -1;
  for (int y = 0; y < dstH; ++y) {
    int i, f;
    if (srcH == dstH) {
      i = std::min(y, srcH - 2);
      f = y > srcH - 2 ? 128 : 0;
    } else {
      vertPos(y, srcH, dstH, &i, &f);
    }
    if (ia != i) {
      if (ib == i) {
        std::swap(ra, rb);
        std::swap(ia, ib);
      } else {
        hscaleRow(src + (size_t)i * srcStride, ra, dstW, srcW, channels, sc.xIdx.data(), sc.xFrac.data());
        ia = i;
      }
    }
    if (f != 0 && ib != i + 1) {
      hscaleRow(src + (size_t)(i + 1) * srcStride, rb, dstW, srcW, channels, sc.xIdx.data(),
                sc.xFrac.data());
      ib = i + 1;
    }
    vblend(ra, rb, f, dst + (size_t)y * dstStride, (int)n);
  }
}

// ---------------------------------------------------------------- Converter

void Converter::reset() {
  std::vector<uint8_t>().swap(scaled_);
  scratchY_ = ScaleScratch();
  scratchUV_ = ScaleScratch();
  std::vector<uint8_t>().swap(rgbRow_);
  scaledValid_ = false;
}

void Converter::ensureScaled(const Nv12View& src, uint64_t srcId, int dw, int dh) {
  if (scaledValid_ && scaledId_ == srcId && scaledW_ == dw && scaledH_ == dh &&
      scaledSrcW_ == src.width && scaledSrcH_ == src.height)
    return;
  const size_t need = (size_t)dw * dh * 3 / 2;
  if (scaled_.size() < need) scaled_.resize(need);  // geometry change only
  uint8_t* y = scaled_.data();
  uint8_t* uv = y + (size_t)dw * dh;
  scalePlane(src.y, src.width, src.height, src.yStride, y, dw, dh, dw, 1, scratchY_);
  scalePlane(src.uv, src.width / 2, src.height / 2, src.uvStride, uv, dw / 2, dh / 2, dw, 2, scratchUV_);
  scaledW_ = dw;
  scaledH_ = dh;
  scaledSrcW_ = src.width;
  scaledSrcH_ = src.height;
  scaledId_ = srcId;
  scaledValid_ = true;
}

bool Converter::convert(const Nv12View& src, uint64_t srcId, uint8_t* dst, size_t dstBytes,
                        const OutputDesc& out) {
  if (!dst || !src.y || !src.uv || src.width < 2 || src.height < 2 || (src.width & 1) ||
      (src.height & 1) || out.width < 2 || out.height < 2 || (out.width & 1) || (out.height & 1))
    return false;
  const int minRow = out.width * (out.fmt == PixFmt::YUY2    ? 2
                                  : out.fmt == PixFmt::RGB24 ? 3
                                  : out.fmt == PixFmt::RGB32 ? 4
                                                             : 1);
  if (out.stride < minRow || (out.fmt == PixFmt::I420 && (out.stride & 1))) return false;
  if (outputBytes(out) > dstBytes) return false;

  const Rect r = fitRect(src.width, src.height, out.width, out.height);
  Nv12View s = src;
  if (r.w != src.width || r.h != src.height) {
    ensureScaled(src, srcId, r.w, r.h);
    s.y = scaled_.data();
    s.uv = s.y + (size_t)r.w * r.h;
    s.width = r.w;
    s.height = r.h;
    s.yStride = r.w;
    s.uvStride = r.w;
  }
  const bool full = r.x == 0 && r.y == 0 && r.w == out.width && r.h == out.height;
  const size_t stride = (size_t)out.stride;
  const int W = out.width, H = out.height;

  switch (out.fmt) {
    case PixFmt::NV12:
    case PixFmt::I420: {
      uint8_t* dy = dst;
      uint8_t* dc = dst + stride * H;
      const size_t cstride = out.fmt == PixFmt::NV12 ? stride : stride / 2;
      uint8_t* du = dc;
      uint8_t* dv = dc + cstride * (H / 2);
      if (!full) {
        for (int y = 0; y < H; ++y) {
          uint8_t* row = dy + y * stride;
          if (y < r.y || y >= r.y + r.h) {
            memset(row, 16, (size_t)W);
          } else {
            memset(row, 16, (size_t)r.x);
            memset(row + r.x + r.w, 16, (size_t)(W - r.x - r.w));
          }
        }
        for (int y = 0; y < H / 2; ++y) {
          const bool outside = y < r.y / 2 || y >= (r.y + r.h) / 2;
          if (out.fmt == PixFmt::NV12) {
            uint8_t* row = dc + y * cstride;
            if (outside) {
              memset(row, 128, (size_t)W);
            } else {
              memset(row, 128, (size_t)r.x);
              memset(row + r.x + r.w, 128, (size_t)(W - r.x - r.w));
            }
          } else {
            uint8_t* ru = du + y * cstride;
            uint8_t* rv = dv + y * cstride;
            if (outside) {
              memset(ru, 128, (size_t)W / 2);
              memset(rv, 128, (size_t)W / 2);
            } else {
              memset(ru, 128, (size_t)r.x / 2);
              memset(rv, 128, (size_t)r.x / 2);
              memset(ru + (r.x + r.w) / 2, 128, (size_t)(W - r.x - r.w) / 2);
              memset(rv + (r.x + r.w) / 2, 128, (size_t)(W - r.x - r.w) / 2);
            }
          }
        }
      }
      for (int y = 0; y < r.h; ++y)
        memcpy(dy + (size_t)(r.y + y) * stride + r.x, s.y + (size_t)y * s.yStride, (size_t)r.w);
      for (int y = 0; y < r.h / 2; ++y) {
        const uint8_t* suv = s.uv + (size_t)y * s.uvStride;
        const size_t row = (size_t)(r.y / 2 + y);
        if (out.fmt == PixFmt::NV12)
          memcpy(dc + row * cstride + r.x, suv, (size_t)r.w);
        else
          rowDeinterleave(suv, du + row * cstride + r.x / 2, dv + row * cstride + r.x / 2, r.w / 2);
      }
      return true;
    }
    case PixFmt::YUY2: {
      const uint32_t black = 0x80108010u;  // Y=16 U=128 Y=16 V=128
      if (!full) {
        for (int y = 0; y < H; ++y) {
          uint8_t* row = dst + y * stride;
          if (y < r.y || y >= r.y + r.h) {
            fillPattern32(row, W / 2, black);
          } else {
            fillPattern32(row, r.x / 2, black);
            fillPattern32(row + (size_t)(r.x + r.w) * 2, (W - r.x - r.w) / 2, black);
          }
        }
      }
      for (int y = 0; y < r.h; ++y)
        rowNv12ToYuy2(s.y + (size_t)y * s.yStride, s.uv + (size_t)(y / 2) * s.uvStride,
                      dst + (size_t)(r.y + y) * stride + (size_t)r.x * 2, r.w);
      return true;
    }
    case PixFmt::RGB32:
    case PixFmt::RGB24: {
      const bool rgb32 = out.fmt == PixFmt::RGB32;
      const Coeffs& k = out.matrix == ColorMatrix::Bt709 ? kBt709 : kBt601;
      const int bpp = rgb32 ? 4 : 3;
      auto rowPtr = [&](int y) -> uint8_t* {
        return dst + (size_t)(out.bottomUp ? (H - 1 - y) : y) * stride;
      };
      if (!full) {
        for (int y = 0; y < H; ++y) {
          uint8_t* row = rowPtr(y);
          const bool outside = y < r.y || y >= r.y + r.h;
          if (rgb32) {
            if (outside) {
              fillPattern32(row, W, 0xFF000000u);
            } else {
              fillPattern32(row, r.x, 0xFF000000u);
              fillPattern32(row + (size_t)(r.x + r.w) * 4, W - r.x - r.w, 0xFF000000u);
            }
          } else {
            if (outside) {
              memset(row, 0, (size_t)W * 3);
            } else {
              memset(row, 0, (size_t)r.x * 3);
              memset(row + (size_t)(r.x + r.w) * 3, 0, (size_t)(W - r.x - r.w) * 3);
            }
          }
        }
      }
      if (!rgb32 && rgbRow_.size() < (size_t)r.w * 4 + 64) rgbRow_.resize((size_t)r.w * 4 + 64);
      for (int y = 0; y < r.h; ++y) {
        uint8_t* o = rowPtr(r.y + y) + (size_t)r.x * bpp;
        const uint8_t* sy = s.y + (size_t)y * s.yStride;
        const uint8_t* suv = s.uv + (size_t)(y / 2) * s.uvStride;
        if (rgb32) {
          rowNv12ToBgra(sy, suv, o, r.w, k);
        } else {
          rowNv12ToBgra(sy, suv, rgbRow_.data(), r.w, k);
          rowBgraToBgr(rgbRow_.data(), o, r.w);
        }
      }
      return true;
    }
    default: return false;
  }
}

// ---------------------------------------------------------------- placeholder

namespace {

struct Glyph {
  char c;
  uint8_t rows[7];  // 5 bits per row, MSB (bit 4) = leftmost
};

const Glyph kFont[] = {
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {' ', {0, 0, 0, 0, 0, 0, 0}},
};

const Glyph* findGlyph(char c) {
  for (const Glyph& g : kFont)
    if (g.c == c) return &g;
  return nullptr;
}

void fillY(uint8_t* p, int w, int h, int x, int y, int rw, int rh, uint8_t val) {
  const int x0 = std::max(0, x), y0 = std::max(0, y);
  const int x1 = std::min(w, x + rw), y1 = std::min(h, y + rh);
  for (int yy = y0; yy < y1; ++yy)
    if (x1 > x0) memset(p + (size_t)yy * w + x0, val, (size_t)(x1 - x0));
}

void fillUV(uint8_t* p, int w, int h, int x, int y, int rw, int rh, uint8_t u, uint8_t v) {
  uint8_t* uv = p + (size_t)w * h;
  const int x0 = std::max(0, x) / 2, y0 = std::max(0, y) / 2;
  const int x1 = std::min(w, x + rw) / 2, y1 = std::min(h, y + rh) / 2;
  for (int yy = y0; yy < y1; ++yy)
    for (int xx = x0; xx < x1; ++xx) {
      uv[(size_t)yy * w + xx * 2] = u;
      uv[(size_t)yy * w + xx * 2 + 1] = v;
    }
}

// Draws text with glyph cells of `s` pixels; returns the width in pixels.
int textWidth(const char* t, int s) {
  const int n = (int)strlen(t);
  return n > 0 ? (n * 6 - 1) * s : 0;
}

void drawText(uint8_t* p, int w, int h, const char* t, int x, int y, int s, uint8_t val) {
  for (; *t; ++t, x += 6 * s) {
    const Glyph* g = findGlyph(*t);
    if (!g) continue;
    for (int r = 0; r < 7; ++r)
      for (int c = 0; c < 5; ++c)
        if (g->rows[r] & (0x10 >> c)) fillY(p, w, h, x + c * s, y + r * s, s, s, val);
  }
}

}  // namespace

void drawPlaceholder(uint8_t* p, int w, int h) {
  if (!p || w < 2 || h < 2) return;
  memset(p, 22, (size_t)w * h);                 // dark gray
  memset(p + (size_t)w * h, 128, (size_t)w * h / 2);  // neutral chroma
  // grid pattern
  const int cell = std::max(8, (h / 9) & ~1);
  const int lw = std::max(1, h / 540);
  for (int y = (h / 2) % cell; y < h; y += cell) fillY(p, w, h, 0, y, w, lw, 34);
  for (int x = (w / 2) % cell; x < w; x += cell) fillY(p, w, h, x, 0, lw, h, 34);
  // "OWC"
  const int s = std::max(1, h / 40);
  const int tw = textWidth("OWC", s);
  const int panelW = tw + 4 * s, panelH = 7 * s + 4 * s + 4 * s + 7 * std::max(1, s / 3) + 2 * s;
  const int px = (w - panelW) / 2, py = (h - panelH) / 2;
  fillY(p, w, h, px, py, panelW, panelH, 14);  // darker panel behind the text
  const int ty = py + 2 * s;
  drawText(p, w, h, "OWC", (w - tw) / 2, ty, s, 235);
  // accent bar (teal)
  const int barY = (ty + 8 * s) & ~1, barH = std::max(2, (s / 2) & ~1);
  fillY(p, w, h, (w - tw) / 2, barY, tw, barH, 120);
  fillUV(p, w, h, ((w - tw) / 2) & ~1, barY, tw, barH, 150, 90);
  // "NO SIGNAL"
  const int s2 = std::max(1, s / 3);
  const int tw2 = textWidth("NO SIGNAL", s2);
  drawText(p, w, h, "NO SIGNAL", (w - tw2) / 2, barY + barH + 2 * s, s2, 160);
}

}  // namespace vcam
}  // namespace owc
