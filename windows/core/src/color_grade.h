// Colour grading internals: the RGB pipeline, the YUV-domain table it is baked
// into, the per-frame NV12 kernel and the ColorGrader that owns the bake
// worker + apply pool (used by Client::Impl::present()).
//
// Why a YUV-domain table: the pipeline (white balance, exposure, curves,
// saturation, a 3D LUT) is far too expensive per pixel, and a classic RGB 3D
// LUT would still need YUV->RGB->YUV per pixel. The table maps (Y, U, V) codes
// straight to graded codes: for every chroma node (U, V on a 33x33 grid, step
// 8) and every Y value 0..255 it stores Y', U', V' (x64 fixed point). Per 2x2
// block the 4 bilinear UV weights are computed once; each pixel's Y' is 4
// loads at its exact Y (no banding in Y: no interpolation along Y) and the
// block's U'V' come from the same 4 rows at the block's average Y.
#pragma once

#include <owc/color_grade.h>

#include "metrics.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace owc {
namespace detail {

// Built-in look as a 33^3 3D LUT (generated once, cached). null if unknown key.
std::shared_ptr<const CubeLut> builtinLookLut(const std::string& key);

// .cube file through a process-wide cache keyed by path + modification stamp
// (thread-safe, bounded by memory). null + err when it cannot be loaded.
std::shared_ptr<const CubeLut> cachedCubeFile(const std::string& path, std::string& err);

// "" -> null (ok), "builtin:<key>" -> generated look, else a .cube path (cached).
bool resolveLut(const std::string& spec, std::shared_ptr<const CubeLut>& lut, std::string& title,
                std::string& err);

// A custom / hue / luma-vs-sat curve prepared once for fast evaluation (the
// public evalCurve / evalHueCurve / evalLumaSatCurve use it too, with a
// per-thread cache of the last curve of each kind).
// Monotone cubic Hermite (Fritsch-Carlson tangents) through the cleaned points.
class PreparedCurve {
 public:
  enum class Kind { Custom, Hue, LumaSat };
  PreparedCurve(const std::vector<CurvePoint>& pts, Kind kind);
  float eval(float x) const;
  // Custom: y == x everywhere. Hue / LumaSat: y == 0.5 everywhere.
  bool neutral() const { return neutral_; }

 private:
  Kind kind_;
  bool neutral_ = true;
  std::vector<float> x_, y_, m_;  // knots (periodic: x_ in [0,1), wrap implied)
};

// Saturation gain of a hue-vs-sat / luma-vs-sat y value: 2^(4(y-0.5)) for
// y >= 0.5; below, 2^(4(y-0.5)) - 0.25(1-2y)^2 so that y = 0 is exactly 0
// (grey), C1 at 0.5, monotonic.
float satGainOfCurveY(float y);

// Hue coordinate of the hue curves (0..1, R 0, Yl 1/6, G 1/3, Cy 1/2, B 2/3,
// Mg 5/6; piecewise linear in the BT.709 Cb/Cr angle between those anchors)
// <-> Cb/Cr angle (radians, counter-clockwise from +Cb).
float hueFromChromaAngle(float rad);
float chromaAngleFromHue(float hue);

// The RGB pipeline of a ColorGrade, prepared once (curves precomputed into 1D
// tables). Works on the stream's gamma-encoded RGB (0..1); luminance-based
// steps use BT.709 weights so the result does not depend on the YUV matrix
// (exportCube() and the live grade match).
class GradePipeline {
 public:
  // `lut` may be null. Keeps a reference to it. `curveN` = resolution of the
  // 1D tables (4096 for the table bake / export; the gallery thumbnails use
  // 1024: 4x cheaper to prepare, differences far below 8-bit on screen).
  void prepare(const ColorGrade& g, std::shared_ptr<const CubeLut> lut, int curveN = kCurveN);
  // Replaces the LUT (mixed by `strength`) of a prepared pipeline.
  void setLut(std::shared_ptr<const CubeLut> lut, float strength);
  // In-place. Any input is accepted: the steps see the input clamped to 0..1,
  // the result is clamped to 0..1 and the input's out-of-range excess is added
  // back (luma as is, chroma through the grade's local chroma transform), so
  // out-of-gamut YUV survives a neutral grade unchanged and a B&W look still
  // turns it grey.
  void run(float rgb[3]) const;
  // True when run() is an exact identity.
  bool noop() const { return !usePre_ && !useTone_ && !useCurves_ && !useHue_ && !useSat_ && !useLut_; }

 private:
  static constexpr int kCurveN = 4096;
  static constexpr int kHueN = 1024;  // periodic angle tables (power of two)

  void core(float c[3]) const;  // the steps, on 0..1 input, unclamped output
  void hueStage(float c[3]) const;
  void chromaRatio(const float* in, const float* out, float& re, float& im) const;

  bool usePre_ = false, useTone_ = false, useCurves_ = false, useSat_ = false, useLut_ = false;
  bool useHue_ = false, useHueShift_ = false, useHueSat_ = false, useLumaSat_ = false;
  bool postOn_[3] = {false, false, false};  // custom curves active on this channel
  // per channel: white balance + exposure -> offset -> lift / gamma / gain ->
  // contrast (pivot), folded into one table on 0..1 (output unclamped)
  std::vector<float> curvePre_[3];
  std::vector<float> curveTone_;   // luminance delta (shadows / highlights)
  std::vector<float> curvePost_[3];  // custom curves: channel curve of the master curve
  // over the Cb/Cr angle 0..2pi (kHueN steps, periodic):
  std::vector<float> hueRot_;      // [kHueN][2] cos, sin of the hue vs hue rotation
  std::vector<float> hueSat_;      // [kHueN] saturation gain (hue vs sat)
  std::vector<float> lumaSat_;     // [curveN] saturation gain over luma 0..1
  float sat_ = 0, vib_ = 0, lutMix_ = 0;
  float ratioRe_ = 1, ratioIm_ = 0;  // global chroma ratio (see chromaRatio)
  std::shared_ptr<const CubeLut> lut_;
};

// Applies a 3D/1D LUT (domain-mapped, tetrahedral / linear) to rgb.
void lookupLut(const CubeLut& lut, const float in[3], float out[3]);

// The baked YUV-domain table (see top of file).
struct GradeTable {
  static constexpr int kNodes = 33;    // chroma nodes per axis: 0, 8, ..., 256
  uint32_t matrix = 2;                 // 1 = BT.601, 2 = BT.709
  bool fullRange = false;
  std::vector<uint16_t> y;             // [node][256] Y' * 64 (0..255*64)
  std::vector<uint16_t> uv;            // [node][256][2] U',V' * 64 (0..256*64)
};

// Bakes `p` for a stream of the given matrix/range into `t` using up to
// `threads` threads (the caller included).
void bakeGradeTable(const GradePipeline& p, uint32_t matrix, bool fullRange, GradeTable& t,
                    int threads);

// Grades chroma rows [r2Begin, r2End) of an NV12 image, columns [x0, w) (x0
// even). src and dst may be the same buffer (same pointers and strides).
void applyGradeRows(const GradeTable& t, const uint8_t* sy, size_t sys, const uint8_t* suv,
                    size_t suvs, uint8_t* dy, size_t dys, uint8_t* duv, size_t duvs, uint32_t x0,
                    uint32_t w, uint32_t r2Begin, uint32_t r2End);

// Small persistent fan-out pool: run(n, fn) calls fn(ctx, i) for i in [0, n)
// on the workers and the calling thread, and returns when all are done.
class BandPool {
 public:
  explicit BandPool(int participants);  // participants - 1 worker threads
  ~BandPool();
  BandPool(const BandPool&) = delete;
  BandPool& operator=(const BandPool&) = delete;
  int participants() const { return static_cast<int>(threads_.size()) + 1; }
  void run(int n, void (*fn)(void*, int), void* ctx);

 private:
  void workerMain();
  std::mutex mx_;
  std::condition_variable cv_, doneCv_;
  std::vector<std::thread> threads_;
  uint64_t gen_ = 0;
  bool quit_ = false;
  void (*fn_)(void*, int) = nullptr;
  void* ctx_ = nullptr;
  int n_ = 0;
  int busy_ = 0;
  std::atomic<int> next_{0};
};

// Per-client grader. apply() runs on the decode thread; a background worker
// (re)loads the LUT and bakes the table whenever the key (grade values, LUT
// file mtime, matrix, range) changes, coalescing bursts to the latest value.
// The decode thread never waits for a bake: it keeps applying the previous
// table (when it matches the frame's matrix/range) or passes frames through.
class ColorGrader {
 public:
  ColorGrader() = default;
  ~ColorGrader();
  ColorGrader(const ColorGrader&) = delete;
  ColorGrader& operator=(const ColorGrader&) = delete;

  // Grades the NV12 frame (w, h even). matrix: 1 = BT.601, 2 = BT.709 (already
  // resolved). On success y/uv/strides point to the graded image and true is
  // returned: graded in place when y points into `buf` (the caller's own
  // scratch, e.g. the transform output), otherwise written into `buf`. Returns
  // false with the pointers untouched (no copy) when disabled, identity or no
  // table is ready yet.
  bool apply(const ColorGrade& g, uint32_t matrix, bool fullRange, const uint8_t*& y,
             uint32_t& ys, const uint8_t*& uv, uint32_t& uvs, uint32_t w, uint32_t h,
             std::vector<uint8_t>& buf);

  struct Status {
    bool active = false;
    double applyMsAvg = 0;
    double bakeMs = 0;
    uint32_t lutSize = 0;
    std::string lutTitle;
    std::string error;
  };
  Status status() const;

  // Tests: waits until the table for the last requested key is baked.
  bool waitBaked(int timeoutMs);

 private:
  struct Key {
    ColorGrade g;  // enabled/compare_split normalized away
    uint32_t matrix = 2;
    bool fullRange = false;
    bool operator==(const Key& o) const {
      return g == o.g && matrix == o.matrix && fullRange == o.fullRange;
    }
  };
  void bakeMain();

  mutable std::mutex mx_;
  std::condition_variable cv_, bakedCv_;
  std::thread bakeThread_;
  std::unique_ptr<BandPool> pool_;   // decode thread only
  Key probe_;                        // decode thread only: reassigned per frame (keeps its
                                     // vectors' capacity, so the key check does not allocate)
  bool engaged_ = false;             // decode thread only: a non-identity grade was requested
  bool quit_ = false;
  bool haveWant_ = false;
  Key want_;
  uint64_t wantGen_ = 0, bakedGen_ = 0;     // bakedGen_ == wantGen_: up to date
  std::shared_ptr<const GradeTable> table_;
  // status (mx_)
  bool active_ = false;
  SampleWindow applyMs_{60};
  double bakeMs_ = 0;
  uint32_t lutSize_ = 0;
  std::string lutTitle_, error_;
};

}  // namespace detail
}  // namespace owc
