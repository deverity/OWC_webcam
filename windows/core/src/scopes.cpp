// Video scopes (waveform, RGB parade, vectorscope, histogram) on the CPU.
//
// One pass over the sampled pixels fills counters per 8-bit CODE VALUE (never
// per output pixel): waveform / parade = columns x 256 codes, vectorscope = a
// 256 x 256 (U, V) grid, histogram = 256 bins. Each scope is then rendered by
// box resampling: an output pixel integrates the codes whose value interval
// overlaps it (see buildCsr). Mapping codes straight to pixels instead leaves
// empty rows / a dot lattice as soon as the output has more pixels than code
// values (or a limited-range input is expanded). The (code, weight) tables are
// the same for every column, so they are built once per call.
//
// Densities go through a log curve normalised by the expected count per
// pixel, so a flat field saturates while sparse traces stay visible (the
// Resolve look).
//
// Speed (1080p, step 2 = 518k samples): the waveform / parade counters are
// column-major (one output column = 256 contiguous counters), so consecutive
// samples of an image row hit the same or the next column: cache friendly.
// The histograms are the column sums of those counters (no per-sample
// increments); a sample spanning several output columns is copied after the
// pass. Rendering scatters only the occupied codes into 16-column density
// blocks (row-contiguous pixel writes). The density -> colour mapping is a
// per-call table (steps of 1/8 sample).
#include <owc/scopes.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace owc {
namespace {

// Vectorscope scale: chroma +-0.5 (the Cb/Cr range) spans +-0.45 of the image.
constexpr float kVecScale = 0.9f;
constexpr float kPi = 3.14159265f;

struct Matrix {
  float kr, kb;
};
Matrix matrixOf(uint32_t m, uint32_t h) {
  if (m != 1 && m != 2) m = h >= 720 ? 2 : 1;
  return m == 1 ? Matrix{0.299f, 0.114f} : Matrix{0.2126f, 0.0722f};
}

inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
inline uint8_t u8(float v) { return static_cast<uint8_t>(clamp01(v) * 255.0f + 0.5f); }

struct Rgb {
  float r, g, b;
};
inline Rgb mixRgb(Rgb a, Rgb b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
inline void put(uint8_t* p, Rgb c, float a) {
  p[0] = u8(c.r);
  p[1] = u8(c.g);
  p[2] = u8(c.b);
  p[3] = u8(a);
}

// Density -> intensity t (log curve, `ref` and above = full) and alpha
// (t^0.7: lifts faint traces). Densities are fractional (box-resampled counts),
// tabulated in steps of 1/kRes for densities below kN/kRes.
class ToneMap {
 public:
  ToneMap(double ref, float gain) : k_(gain / std::log1p(std::max(1.0, ref))) {
    for (uint32_t i = 0; i < kN; ++i) {
      t_[i] = calcT(static_cast<float>(i) / kRes);
      a_[i] = std::pow(t_[i], 0.7f);
    }
  }
  // LUT index of a density; 0 = invisible (below 1/(2*kRes) of a sample)
  static uint32_t index(float d) { return d < kN / kRes - 1 ? static_cast<uint32_t>(d * kRes + 0.5f) : kN; }
  float t(float d) const {
    const uint32_t i = index(d);
    return i < kN ? t_[i] : calcT(d);
  }
  float alpha(float d) const {
    const uint32_t i = index(d);
    return i < kN ? a_[i] : std::pow(calcT(d), 0.7f);
  }
  // density from which t is exactly 1 (and alpha 1): saturated pixels skip pow()
  float saturation() const { return static_cast<float>(std::expm1(1.0 / k_)); }
  float tAt(uint32_t i) const { return t_[i]; }
  float alphaAt(uint32_t i) const { return a_[i]; }
  static constexpr uint32_t kN = 1024;
  static constexpr float kRes = 8.0f;

 private:
  float calcT(float c) const { return clamp01(static_cast<float>(std::log1p(static_cast<double>(c)) * k_)); }
  double k_;
  float t_[kN], a_[kN];
};

// Box resampling table: for each output pixel p (covering [p, p+1] in the
// output axis) the list of (code, weight) pairs of the 8-bit codes whose value
// interval [a, b] overlaps it. weight = overlap / (b - a) * scale: a code that
// is stretched over `len` > 1 pixels spreads its count evenly over them with
// the per-pixel density divided by len, and `scale` (= max(1, len)) gives it
// back, so a flat field is as bright at any output size; a code narrower than
// a pixel (downscaling) just adds up with its neighbours.
// Stored twice: pixel-major (start/code/w: the codes of each pixel) and
// code-major (cstart/pix/cw: the pixels of each code, ascending).
struct Csr {
  std::vector<uint32_t> start;  // n + 1
  std::vector<uint16_t> code;
  std::vector<float> w;
  uint32_t cstart[257];
  std::vector<uint32_t> pix;
  std::vector<float> cw;
};
void buildCsr(uint32_t n, const float* a, const float* b, const float* scale, Csr& t) {
  t.start.assign(n + 1, 0);
  auto each = [&](auto&& fn) {
    for (int c = 0; c < 256; ++c) {
      const float len = b[c] - a[c];
      if (!(len > 0)) continue;
      const int p0 = std::max(0, static_cast<int>(std::floor(a[c])));
      const int p1 = std::min(static_cast<int>(n) - 1, static_cast<int>(std::ceil(b[c])) - 1);
      for (int p = p0; p <= p1; ++p) {
        const float ov = std::min(b[c], static_cast<float>(p + 1)) - std::max(a[c], static_cast<float>(p));
        if (ov > 1e-6f) fn(static_cast<uint32_t>(p), c, ov / len * scale[c]);
      }
    }
  };
  std::fill(t.cstart, t.cstart + 257, 0u);
  each([&](uint32_t p, int c, float) {
    ++t.start[p + 1];
    ++t.cstart[c + 1];
  });
  for (uint32_t p = 0; p < n; ++p) t.start[p + 1] += t.start[p];
  for (int c = 0; c < 256; ++c) t.cstart[c + 1] += t.cstart[c];
  t.code.resize(t.start[n]);
  t.w.resize(t.start[n]);
  t.pix.resize(t.start[n]);
  t.cw.resize(t.start[n]);
  std::vector<uint32_t> fill(t.start.begin(), t.start.end() - 1);
  uint32_t k = 0;  // code-major cursor: `each` visits codes ascending, then pixels ascending
  each([&](uint32_t p, int c, float wt) {
    t.code[fill[p]] = static_cast<uint16_t>(c);
    t.w[fill[p]++] = wt;
    t.pix[k] = p;
    t.cw[k++] = wt;
  });
}

// Waveform / parade rows. Code c has value (c - off) / span +- 0.5 / span;
// row r covers values ((H-1-r) +- 0.5) / (H-1) (row = (H-1) * (1 - value) at
// the row centre). Intervals are clipped to the image; codes entirely outside
// (super-black / super-white) are folded onto the edge row.
void buildRowTable(uint32_t H, float off, float span, Csr& t) {
  float a[256], b[256], sc[256];
  const float hm = static_cast<float>(H - 1), fh = static_cast<float>(H);
  const float scale = std::max(1.0f, hm / span);
  for (int c = 0; c < 256; ++c) {
    const float v0 = (static_cast<float>(c) - 0.5f - off) / span, v1 = (static_cast<float>(c) + 0.5f - off) / span;
    float pa = hm * (1 - v1) + 0.5f, pb = hm * (1 - v0) + 0.5f;
    pa = std::min(fh, std::max(0.0f, pa));
    pb = std::min(fh, std::max(0.0f, pb));
    if (pb - pa < 1e-4f) {
      if (v0 + v1 < 1.0f) pa = fh - 1, pb = fh;  // below black: bottom row
      else pa = 0, pb = 1;                        // above white: top row
    }
    a[c] = pa;
    b[c] = pb;
    sc[c] = scale;
  }
  buildCsr(H, a, b, sc, t);
}

// Vectorscope axis: chroma code c -> (c - 128) / range * kVecScale around the
// centre; y runs upwards (Cr up). Codes falling outside the image are dropped.
void buildChromaTable(uint32_t n, float range, bool flip, Csr& t) {
  float a[256], b[256], sc[256];
  const float fn = static_cast<float>(n);
  for (int c = 0; c < 256; ++c) {
    const float c0 = (static_cast<float>(c) - 0.5f - 128) / range * kVecScale;
    const float c1 = (static_cast<float>(c) + 0.5f - 128) / range * kVecScale;
    a[c] = flip ? (0.5f - c1) * fn : (0.5f + c0) * fn;
    b[c] = flip ? (0.5f - c0) * fn : (0.5f + c1) * fn;
    sc[c] = std::max(1.0f, b[c] - a[c]);
  }
  buildCsr(n, a, b, sc, t);
}

// Per-code counters (cols x 256 codes, code contiguous) -> row-major RGBA (row
// stride W pixels, first column at `out`). Colour: `lo` where sparse, towards
// `hi` where dense. Per column only the codes that have counts are scattered
// into a float density block (typically a small subset of the 256); blocks of
// kB columns are interleaved (row-major inside the block) so that the pixel
// writes of a row are one contiguous 4*kB byte run instead of kB cache lines.
void renderColumns(const uint32_t* cnt, uint32_t cols, uint32_t H, uint32_t W, const Csr& rows, const ToneMap& tm,
                   Rgb lo, Rgb hi, uint8_t* out) {
  constexpr uint32_t kB = 16;
  thread_local std::vector<uint32_t> lutBuf;
  thread_local std::vector<float> densBuf;
  lutBuf.resize(ToneMap::kN);
  densBuf.assign(static_cast<size_t>(H) * kB, 0.0f);
  uint32_t* lut = lutBuf.data();
  float* dens = densBuf.data();
  lut[0] = 0;
  for (uint32_t i = 1; i < ToneMap::kN; ++i) {
    const float t = tm.tAt(i);
    uint8_t px[4];
    put(px, mixRgb(lo, hi, t * t), tm.alphaAt(i));
    memcpy(&lut[i], px, 4);
  }
  const float sat = tm.saturation();
  uint32_t satPx;
  {
    uint8_t px[4];
    put(px, hi, 1.0f);  // t = 1: mixRgb(lo, hi, 1)
    memcpy(&satPx, px, 4);
  }
  for (uint32_t x0 = 0; x0 < cols; x0 += kB) {
    const uint32_t nb = std::min(kB, cols - x0);
    uint32_t r0 = H, r1 = 0;  // touched rows [r0, r1]
    for (uint32_t j = 0; j < nb; ++j) {
      const uint32_t* col = cnt + static_cast<size_t>(x0 + j) * 256;
      for (uint32_t c = 0; c < 256; ++c) {
        const uint32_t n = col[c];
        if (!n) continue;
        const uint32_t k0 = rows.cstart[c], k1 = rows.cstart[c + 1];
        if (k0 == k1) continue;
        const float fn = static_cast<float>(n);
        for (uint32_t k = k0; k < k1; ++k) dens[static_cast<size_t>(rows.pix[k]) * kB + j] += fn * rows.cw[k];
        r0 = std::min(r0, rows.pix[k0]);
        r1 = std::max(r1, rows.pix[k1 - 1]);
      }
    }
    for (uint32_t r = r0; r <= r1 && r < H; ++r) {
      float* d = dens + static_cast<size_t>(r) * kB;
      uint8_t* row = out + (static_cast<size_t>(r) * W + x0) * 4;
      for (uint32_t j = 0; j < nb; ++j) {
        const float v = d[j];
        d[j] = 0;
        const uint32_t idx = ToneMap::index(v);
        if (!idx) continue;  // already transparent
        if (idx < ToneMap::kN) {
          memcpy(row + j * 4, &lut[idx], 4);
        } else if (v >= sat) {
          memcpy(row + j * 4, &satPx, 4);
        } else {
          const float t = tm.t(v);
          put(row + j * 4, mixRgb(lo, hi, t * t), tm.alpha(v));
        }
      }
    }
  }
}

// Histogram bins (bin i covers [i, i+1] of nb) -> W output columns by box
// overlap (sums when downscaling, splits when upscaling).
void histColumns(const float* f, uint32_t nb, uint32_t W, float* out) {
  for (uint32_t x = 0; x < W; ++x) {
    const float s0 = static_cast<float>(x) * static_cast<float>(nb) / static_cast<float>(W);
    const float s1 = static_cast<float>(x + 1) * static_cast<float>(nb) / static_cast<float>(W);
    const uint32_t i0 = static_cast<uint32_t>(s0), i1 = std::min(nb - 1, static_cast<uint32_t>(std::ceil(s1)) - 1);
    float s = 0;
    for (uint32_t i = i0; i <= i1; ++i) {
      const float ov = std::min(s1, static_cast<float>(i + 1)) - std::max(s0, static_cast<float>(i));
      if (ov > 0) s += f[i] * ov;
    }
    out[x] = s;
  }
}

}  // namespace

void computeScopes(const uint8_t* y, size_t yStride, const uint8_t* uv, size_t uvStride, uint32_t w,
                   uint32_t h, const ScopeOptions& opt, ScopeImages& out) {
  const uint32_t W = std::max<uint32_t>(opt.width, 3), H = std::max<uint32_t>(opt.height, 2);
  const uint32_t VS = std::max<uint32_t>(opt.vector_size, 2);
  out.w = W;
  out.h = H;
  out.vs = VS;
  const size_t imgBytes = static_cast<size_t>(W) * H * 4;
  for (auto* v : {&out.waveform, &out.parade, &out.histogram}) v->assign(imgBytes, 0);
  out.vectorscope.assign(static_cast<size_t>(VS) * VS * 4, 0);
  if (!y || !uv || w < 2 || h < 2) return;
  const uint32_t step = std::max<uint32_t>(opt.step, 1);
  const float gain = opt.gain > 0 ? opt.gain : 1.0f;

  // ---- conversion tables (fixed point, 8 fractional bits) to full-range 0..255
  const Matrix M = matrixOf(opt.matrix, h);
  const float kg = 1 - M.kr - M.kb;
  const float yOff = opt.full_range ? 0.0f : 16.0f;
  const float yScale = opt.full_range ? 1.0f : 255.0f / 219.0f;
  const float cScale = opt.full_range ? 1.0f : 255.0f / 224.0f;
  const float rCr = 2 * (1 - M.kr), bCb = 2 * (1 - M.kb);
  int yv[256], rv[256], gu[256], gv[256], bu[256];
  for (int i = 0; i < 256; ++i) {
    const float yf = (static_cast<float>(i) - yOff) * yScale;
    const float c = (static_cast<float>(i) - 128) * cScale;
    yv[i] = static_cast<int>(std::lround(yf * 256)) + 128;  // + rounding
    rv[i] = static_cast<int>(std::lround(rCr * c * 256));
    bu[i] = static_cast<int>(std::lround(bCb * c * 256));
    gu[i] = static_cast<int>(std::lround(-M.kb * bCb / kg * c * 256));
    gv[i] = static_cast<int>(std::lround(-M.kr * rCr / kg * c * 256));
  }
  // (v >> 8) spans about -330..590 for any YUV: clamp through a table
  constexpr int kClampOff = 512;
  uint8_t clampTab[1536];
  for (int i = 0; i < 1536; ++i) clampTab[i] = static_cast<uint8_t>(std::min(255, std::max(0, i - kClampOff)));
  const uint32_t P = W / 3;  // parade panel width
  // Output columns of each sampled x: [col, col + n). n > 1 only when the
  // image has fewer samples per row than the scope has columns (no gaps).
  const uint32_t nx = (w + step - 1) / step;
  std::vector<uint32_t> colW(nx), nW(nx), colP(nx), nP(nx);
  uint64_t rowIncW = 0, rowIncP = 0;
  bool spread = false;
  for (uint32_t i = 0; i < nx; ++i) {
    const uint64_t x = static_cast<uint64_t>(i) * step, x1 = std::min<uint64_t>(x + step, w);
    colW[i] = static_cast<uint32_t>(x * W / w);
    nW[i] = std::max<uint32_t>(1, static_cast<uint32_t>(x1 * W / w) - colW[i]);
    colP[i] = static_cast<uint32_t>(x * P / w);
    nP[i] = std::max<uint32_t>(1, static_cast<uint32_t>(x1 * P / w) - colP[i]);
    if (colP[i] + nP[i] > P) nP[i] = colP[i] < P ? P - colP[i] : 0;
    rowIncW += nW[i];
    rowIncP += nP[i];
    spread = spread || nW[i] != 1 || nP[i] != 1;
  }
  // ---- one pass: code counters. wave: W columns x 256 luma codes (raw Y);
  // par: 3 panels of P columns x 256 codes (R panel first), column-major;
  // vec: (V code, U code) grid. The vectorscope and histogram counters are
  // doubled (even / odd samples): a flat area hits the same bin sample after
  // sample, and one copy would serialise on that counter.
  constexpr size_t vsN = 256 * 256;
  const size_t panel = static_cast<size_t>(P) * 256;  // counters of one parade panel
  // Per-thread scratch reused across calls (fresh multi-MB allocations would
  // page-fault on every call).
  thread_local std::vector<uint32_t> wave, par, vec;
  wave.assign(static_cast<size_t>(W) * 256, 0);
  for (uint32_t i = 0; i < nx; ++i) {  // column -> counter offsets
    colW[i] *= 256;
    colP[i] *= 256;
  }
  par.assign(3 * panel + 1, 0);
  vec.assign(2 * vsN, 0);
  uint32_t* const wcnt = wave.data();
  uint32_t* const vc = vec.data();
  const uint32_t* const cW = colW.data();
  const uint32_t* const cP = colP.data();
  uint32_t* const pr = par.data();
  uint32_t* const pg = pr + panel;
  uint32_t* const pb = pg + panel;
  uint32_t samples = 0, rows = 0;
  for (uint32_t r = 0; r < h; r += step, ++rows) {
    const uint8_t* yr = y + static_cast<size_t>(r) * yStride;
    const uint8_t* cr = uv + static_cast<size_t>(r / 2) * uvStride;
    uint32_t i = 0;
    for (uint32_t x = 0; x < w; x += step, ++i) {
      const uint32_t Y = yr[x], U = cr[x & ~1u], V = cr[(x & ~1u) + 1];
      const int yy = yv[Y];
      const uint32_t R = clampTab[((yy + rv[V]) >> 8) + kClampOff];
      const uint32_t G = clampTab[((yy + gu[U] + gv[V]) >> 8) + kClampOff];
      const uint32_t B = clampTab[((yy + bu[U]) >> 8) + kClampOff];
      // each sample counts once, in its first output column
      ++wcnt[cW[i] + Y];
      const uint32_t cp = cP[i];
      ++pr[cp + R];
      ++pg[cp + G];
      ++pb[cp + B];
      ++vc[(i & 1) * vsN + V * 256 + U];
    }
    samples += i;
  }
  for (size_t i = 0; i < vsN; ++i) vec[i] += vec[vsN + i];
  if (!samples) return;
  // The histograms are the column sums of the waveform / parade counters
  // (before the columns are replicated below), so the pass above needs no
  // histogram increments.
  uint32_t hist[4][256] = {};  // R, G, B (computed codes), Y (raw code)
  for (uint32_t x = 0; x < W; ++x)
    for (uint32_t c = 0; c < 256; ++c) hist[3][c] += wave[static_cast<size_t>(x) * 256 + c];
  for (uint32_t k = 0; k < 3; ++k)
    for (uint32_t x = 0; x < P; ++x)
      for (uint32_t c = 0; c < 256; ++c) hist[k][c] += par[k * panel + static_cast<size_t>(x) * 256 + c];
  // A sample covering several output columns (fewer samples per row than
  // scope columns) fills the columns after its own with a copy.
  if (spread) {
    for (uint32_t i = 0; i < nx; ++i) {
      for (uint32_t k = 1; k < nW[i]; ++k)
        memcpy(&wave[colW[i] + k * 256], &wave[colW[i]], 256 * sizeof(uint32_t));
      for (uint32_t k = 1; k < nP[i]; ++k)
        for (uint32_t pl = 0; pl < 3; ++pl)
          memcpy(&par[pl * panel + colP[i] + k * 256], &par[pl * panel + colP[i]], 256 * sizeof(uint32_t));
    }
  }

  // ---- waveform: luma traces, green-tinted when sparse, white when dense.
  // Limited range: code 16..235 -> value 0..1 (box-expanded, not remapped).
  {
    Csr rowsY;
    buildRowTable(H, opt.full_range ? 0.0f : 16.0f, opt.full_range ? 255.0f : 219.0f, rowsY);
    const double perCol = static_cast<double>(rows) * static_cast<double>(rowIncW) / W;
    const ToneMap tm(std::max(3.0, perCol / 12), gain);
    renderColumns(wave.data(), W, H, W, rowsY, tm, {0.42f, 0.95f, 0.55f}, {1.0f, 1.0f, 1.0f}, out.waveform.data());
  }
  // ---- parade: R | G | B (codes are full range after the conversion above)
  if (P > 0) {
    Csr rowsRgb;
    buildRowTable(H, 0.0f, 255.0f, rowsRgb);
    const double perCol = static_cast<double>(rows) * static_cast<double>(rowIncP) / P;
    const ToneMap tm(std::max(3.0, perCol / 12), gain);
    const Rgb lo[3] = {{1.0f, 0.22f, 0.22f}, {0.25f, 1.0f, 0.3f}, {0.3f, 0.45f, 1.0f}};
    const Rgb hi[3] = {{1.0f, 0.85f, 0.85f}, {0.85f, 1.0f, 0.85f}, {0.85f, 0.9f, 1.0f}};
    for (uint32_t k = 0; k < 3; ++k)
      renderColumns(par.data() + k * panel, P, H, W, rowsRgb, tm, lo[k], hi[k],
                    out.parade.data() + static_cast<size_t>(k) * P * 4);
  }
  // ---- vectorscope: whitish traces with a hint of their own hue when sparse.
  // Each output pixel integrates the (U, V) code grid over its footprint
  // (separable: per-column U weights, per-row V weights).
  {
    const float range = opt.full_range ? 255.0f : 224.0f;
    Csr colU, rowV;
    buildChromaTable(VS, range, false, colU);
    buildChromaTable(VS, range, true, rowV);
    // expected samples per pixel when the samples spread over the grid cells
    const double avg = static_cast<double>(samples) / std::min<double>(static_cast<double>(VS) * VS, 65536.0);
    const ToneMap tm(std::max(4.0, avg * 16), gain);
    const float inv = 1.0f / (kVecScale * static_cast<float>(VS));
    const Rgb white{0.92f, 0.95f, 0.92f};
    // stage 1: integrate each occupied V row of the grid horizontally (U -> px);
    // stage 2: per output row, add the V rows it covers (vectorisable over px)
    thread_local std::vector<float> tmp, acc;
    tmp.assign(static_cast<size_t>(256) * VS, 0.0f);
    acc.resize(VS);
    bool vHas[256];
    for (uint32_t v = 0; v < 256; ++v) {
      const uint32_t* g = vec.data() + static_cast<size_t>(v) * 256;
      vHas[v] = false;
      float* trow = tmp.data() + static_cast<size_t>(v) * VS;
      for (uint32_t u = 0; u < 256; ++u) {
        if (!g[u]) continue;
        vHas[v] = true;
        const float fn = static_cast<float>(g[u]);
        for (uint32_t k = colU.cstart[u]; k < colU.cstart[u + 1]; ++k) trow[colU.pix[k]] += fn * colU.cw[k];
      }
    }
    for (uint32_t py = 0; py < VS; ++py) {
      std::fill(acc.begin(), acc.end(), 0.0f);
      bool any = false;
      for (uint32_t j = rowV.start[py]; j < rowV.start[py + 1]; ++j) {
        const uint32_t v = rowV.code[j];
        if (!vHas[v]) continue;
        any = true;
        const float wv = rowV.w[j];
        const float* trow = tmp.data() + static_cast<size_t>(v) * VS;
        float* a = acc.data();
        for (uint32_t px = 0; px < VS; ++px) a[px] += trow[px] * wv;
      }
      if (!any) continue;
      for (uint32_t px = 0; px < VS; ++px) {
        const float c = acc[px];
        if (!ToneMap::index(c)) continue;
        const float t = tm.t(c);
        const float cb = (static_cast<float>(px) + 0.5f - 0.5f * static_cast<float>(VS)) * inv;
        const float crv = (0.5f * static_cast<float>(VS) - static_cast<float>(py) - 0.5f) * inv;
        Rgb hue{0.5f + rCr * crv, 0, 0.5f + bCb * cb};
        hue.g = (0.5f - M.kr * hue.r - M.kb * hue.b) / kg;
        const float mx = std::max(hue.r, std::max(hue.g, std::max(hue.b, 1e-3f)));
        hue = {clamp01(hue.r / mx), clamp01(hue.g / mx), clamp01(hue.b / mx)};
        put(out.vectorscope.data() + (static_cast<size_t>(py) * VS + px) * 4, mixRgb(white, hue, 0.45f * (1 - t)),
            tm.alpha(c));
      }
    }
  }
  // ---- histogram: R, G, B additive bars + luma outline; sqrt-scaled counts
  {
    std::vector<float> col[4];
    float mx = 0;
    for (int k = 0; k < 4; ++k) {
      col[k].resize(W);
      float f[256] = {};
      if (k < 3) {
        // R, G, B codes: limited range input was expanded by 255/219, which
        // leaves every 7th or so code empty (a comb); a 3-code box (mass
        // preserving) closes those gaps. Full range input is exact.
        for (int v = 0; v < 256; ++v) {
          if (opt.full_range) {
            f[v] = static_cast<float>(hist[k][v]);
          } else {
            const int a = std::max(0, v - 1), b = std::min(255, v + 1);
            f[v] = static_cast<float>(hist[k][a] + hist[k][v] + hist[k][b]) * (1.0f / 3.0f);
          }
        }
        histColumns(f, 256, W, col[k].data());
      } else {
        // luma: raw codes, limited range spans 16..235 over the full width;
        // super-black / super-white fold into the end bins
        const uint32_t lo = opt.full_range ? 0 : 16, hi = opt.full_range ? 255 : 235;
        for (uint32_t v = 0; v < 256; ++v) f[std::min(hi, std::max(lo, v)) - lo] += static_cast<float>(hist[3][v]);
        histColumns(f, hi - lo + 1, W, col[k].data());
      }
      for (uint32_t x = 0; x < W; ++x) mx = std::max(mx, col[k][x]);
    }
    if (mx > 0) {
      for (auto& c : col)
        for (float& v : c) v = std::sqrt(v / mx) * static_cast<float>(H) * std::min(1.0f, gain);
      const Rgb chan[3] = {{0.85f, 0.12f, 0.12f}, {0.12f, 0.80f, 0.18f}, {0.15f, 0.25f, 0.95f}};
      uint8_t byMask[8][4] = {};  // additive colour of the covered channels (bit k = channel k)
      for (int m = 1; m < 8; ++m) {
        Rgb c{0, 0, 0};
        int n = 0;
        for (int k = 0; k < 3; ++k)
          if (m & (1 << k)) {
            c.r += chan[k].r;
            c.g += chan[k].g;
            c.b += chan[k].b;
            ++n;
          }
        put(byMask[m], {std::min(1.0f, c.r), std::min(1.0f, c.g), std::min(1.0f, c.b)}, 0.55f + 0.15f * static_cast<float>(n));
      }
      uint8_t outline[4];
      put(outline, {0.95f, 0.95f, 0.95f}, 1.0f);
      uint8_t* img = out.histogram.data();
      auto topRow = [&](uint32_t x) {  // highest covered row from the bottom, -1 if none
        return static_cast<int>(col[3][x] + 0.5f) - 1;
      };
      // per-column extents first, then fill row by row (contiguous writes)
      struct ColInfo {
        float c[3];
        int lo, hi, last;
      };
      std::vector<ColInfo> ci(W);
      int maxLast = -1;
      for (uint32_t x = 0; x < W; ++x) {
        int lo = topRow(x), hi = lo;
        if (x > 0) {
          lo = std::min(lo, topRow(x - 1));
          hi = std::max(hi, topRow(x - 1));
        }
        lo = std::max(lo, 0);
        const float top = std::max(col[0][x], std::max(col[1][x], col[2][x]));
        const int last = std::min(static_cast<int>(H) - 1, std::max(hi, static_cast<int>(top + 1)));
        ci[x] = {{col[0][x], col[1][x], col[2][x]}, lo, hi, last};
        maxLast = std::max(maxLast, last);
      }
      for (int yb = 0; yb <= maxLast; ++yb) {
        uint8_t* row = img + static_cast<size_t>(H - 1 - static_cast<uint32_t>(yb)) * W * 4;
        const float yc = static_cast<float>(yb) + 0.5f;
        for (uint32_t x = 0; x < W; ++x) {
          const ColInfo& c = ci[x];
          if (yb > c.last) continue;
          if (c.hi >= 0 && yb >= c.lo && yb <= c.hi) {
            memcpy(row + x * 4, outline, 4);  // luma outline
            continue;
          }
          const int m = (yc < c.c[0] ? 1 : 0) | (yc < c.c[1] ? 2 : 0) | (yc < c.c[2] ? 4 : 0);
          if (m) memcpy(row + x * 4, byMask[m], 4);
        }
      }
    }
  }
}

std::vector<VectorTarget> vectorscopeTargets(uint32_t matrix) {
  const Matrix M = matrix == 1 ? Matrix{0.299f, 0.114f} : Matrix{0.2126f, 0.0722f};
  struct P {
    const char* label;
    float r, g, b;
  };
  static const P prim[6] = {{"R", 1, 0, 0}, {"Mg", 1, 0, 1}, {"B", 0, 0, 1},
                            {"Cy", 0, 1, 1}, {"G", 0, 1, 0}, {"Yl", 1, 1, 0}};
  std::vector<VectorTarget> out;
  out.reserve(6);
  for (const P& p : prim) {
    const float Y = M.kr * p.r + (1 - M.kr - M.kb) * p.g + M.kb * p.b;
    const float cb = (p.b - Y) / (2 * (1 - M.kb)), cr = (p.r - Y) / (2 * (1 - M.kr));
    VectorTarget t;
    t.label = p.label;
    t.x100 = 0.5f + cb * kVecScale;
    t.y100 = 0.5f - cr * kVecScale;
    t.x75 = 0.5f + 0.75f * cb * kVecScale;
    t.y75 = 0.5f - 0.75f * cr * kVecScale;
    out.push_back(t);
  }
  return out;
}

float vectorscopeSkinLineAngle() { return 123.0f * kPi / 180.0f; }

}  // namespace owc
