#include "color_grade.h"

#include "util.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string_view>

namespace owc {

// ================================================================ built-in looks
const std::vector<BuiltinLook>& builtinLooks() {
  static const std::vector<BuiltinLook> looks = {
      {"natural", "Natural", "Contraste suave e leve calor; bom padrão para webcam"},
      {"teal_orange", "Cinema (teal & orange)", "Pele quente e sombras azul-petróleo, visual de cinema"},
      {"warm", "Quente", "Tons dourados e aconchegantes"},
      {"cool", "Frio", "Azulado, limpo e moderno"},
      {"vivid", "Cores vivas", "Realça cores apagadas preservando a pele"},
      {"film", "Filme", "Pretos levantados, altas luzes suaves e leve desbotado"},
      {"bw", "Preto e branco", "Monocromático com tonalidade equilibrada"},
      {"bw_contrast", "P&B contraste alto", "Monocromático dramático, pretos profundos"},
  };
  return looks;
}

namespace detail {
namespace {

// ------------------------------------------------------------------ math
inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float smoothstep(float e0, float e1, float x) {
  const float t = clamp01((x - e0) / (e1 - e0));
  return t * t * (3 - 2 * t);
}
inline float luma709(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
// floor() for |x| < 2^31 without the CRT call (hot in the hue stage).
inline float floorFast(float x) {
  const float t = static_cast<float>(static_cast<int>(x));
  return t > x ? t - 1 : t;
}
// Display gamma ~2.2 (the stream's transfer is close enough for grading).
inline float decodeGamma(float x) { return x <= 0 ? 0 : std::pow(x, 2.2f); }
inline float encodeGamma(float x) { return x <= 0 ? 0 : std::pow(x, 1 / 2.2f); }

// Symmetric power sigmoid through (0,0), (0.5,0.5), (1,1); slope g at mid grey.
inline float sigmoidPow(float x, float g) {
  x = clamp01(x);
  const float a = std::pow(x, g), b = std::pow(1 - x, g);
  return a / (a + b);
}
// Contrast S-curve around 0.5, c in -1..1. A blend of two monotonic curves, so
// it is monotonic; black and white stay put.
inline float contrastCurve(float x, float c) {
  if (c > 0) return x + 0.7f * c * (sigmoidPow(x, 2.0f) - x);
  if (c < 0) return x - 0.6f * c * (sigmoidPow(x, 0.5f) - x);
  return x;
}
// Smooth bump lift of the dark tones (peak +0.18*a at 0.25, 0 at 0 and 1).
// Slope >= 1 - 0.18*9.48*0.25 > 0: monotonic.
inline float liftDark(float L, float a) {
  const float u = 1 - L;
  return L + 0.18f * a * 9.4815f * L * u * u * u;
}
// Deepens the dark tones with a gamma faded out towards white; slope >= 1 - (1-L)^2.
inline float deepenDark(float L, float a) {
  const float u = 1 - L;
  return L + (std::pow(L, 1 + 1.5f * a) - L) * u * u;
}
inline float shadowsCurve(float L, float s) {
  if (s > 0) return liftDark(L, s);
  if (s < 0) return deepenDark(L, -s);
  return L;
}
inline float highlightsCurve(float L, float h) {
  if (h < 0) return 1 - liftDark(1 - L, -h);   // recover: darken the bright tones
  if (h > 0) return 1 - deepenDark(1 - L, h);  // brighten the bright tones
  return L;
}
// Contrast around a pivot: x^a maps the pivot to 0.5 (a = log 0.5 / log pivot),
// the 0.5-centred S-curve is applied there and mapped back. 0, 1 and the pivot
// stay put; monotonic. Outside 0..1 the value passes through unchanged.
inline float contrastPivot(float x, float c, float a) {
  if (x <= 0 || x >= 1) return x;
  if (a == 1) return contrastCurve(x, c);
  return std::pow(contrastCurve(std::pow(x, a), c), 1 / a);
}

// Opponent (BT.709 YCbCr-like) chroma used by the looks and the skin mask.
inline void toChroma(const float* c, float& L, float& cb, float& cr) {
  L = luma709(c);
  cb = (c[2] - L) / 1.8556f;
  cr = (c[0] - L) / 1.5748f;
}
inline void fromChroma(float L, float cb, float cr, float* c) {
  c[0] = L + 1.5748f * cr;
  c[2] = L + 1.8556f * cb;
  c[1] = (L - 0.2126f * c[0] - 0.0722f * c[2]) / 0.7152f;
}
// 1 on skin hues (~128 deg in the Cb/Cr plane: from pale to dark skin), 0 elsewhere.
inline float skinMask(float cb, float cr) {
  const float C = std::sqrt(cb * cb + cr * cr);
  if (C < 1e-5f) return 0;
  const float cosA = (cb * -0.6157f + cr * 0.7880f) / C;
  return smoothstep(0.75f, 0.95f, cosA) * smoothstep(0.01f, 0.05f, C);
}

// ------------------------------------------------------------------ looks
// Helpers on gamma-encoded RGB (0..1).
void whiteGains(float* c, float r, float g, float b) {  // linear light, luminance kept
  const float n = 0.2126f * r + 0.7152f * g + 0.0722f * b;
  const float k[3] = {r / n, g / n, b / n};
  for (int i = 0; i < 3; ++i) c[i] = encodeGamma(decodeGamma(clamp01(c[i])) * k[i]);
}
void contrastRGB(float* c, float amount) {
  for (int i = 0; i < 3; ++i) c[i] = contrastCurve(clamp01(c[i]), amount);
}
void saturate(float* c, float k) {
  const float L = luma709(c);
  for (int i = 0; i < 3; ++i) c[i] = L + (c[i] - L) * k;
}
// Adds a chroma tint (unit direction (dcb, dcr) times amount) weighted to the
// shadows (sh) or highlights (hi).
void splitTone(float* c, float shCb, float shCr, float shAmt, float hiCb, float hiCr, float hiAmt) {
  float L, cb, cr;
  toChroma(c, L, cb, cr);
  const float sh = 1 - smoothstep(0.0f, 0.55f, L), hi = smoothstep(0.5f, 1.0f, L);
  cb += shCb * shAmt * sh + hiCb * hiAmt * hi;
  cr += shCr * shAmt * sh + hiCr * hiAmt * hi;
  fromChroma(L, cb, cr, c);
}
// Cb/Cr unit directions of a few hues.
constexpr float kTealCb = 0.225f, kTealCr = -0.974f;      // ~283 deg
constexpr float kOrangeCb = -0.707f, kOrangeCr = 0.707f;  // ~135 deg
constexpr float kGoldCb = -0.866f, kGoldCr = 0.5f;        // ~150 deg
constexpr float kPi = 3.14159265f;

inline float wrapDeg(float d) {
  while (d > 180) d -= 360;
  while (d < -180) d += 360;
  return d;
}

void lookNatural(float* c) {
  whiteGains(c, 1.035f, 1.0f, 0.965f);
  contrastRGB(c, 0.12f);
  float L, cb, cr;
  toChroma(c, L, cb, cr);
  const float C = std::sqrt(cb * cb + cr * cr);
  const float k = 1 + 0.08f * (1 - smoothstep(0, 0.3f, C)) * (1 - 0.6f * skinMask(cb, cr));
  fromChroma(L, cb * k, cr * k, c);
}

void lookTealOrange(float* c) {
  contrastRGB(c, 0.18f);
  float L, cb, cr;
  toChroma(c, L, cb, cr);
  float C = std::sqrt(cb * cb + cr * cr);
  if (C > 1e-6f) {
    float a = std::atan2(cr, cb) * 180 / kPi;
    const float d = wrapDeg(a - 128);
    const float warmW = 1 - smoothstep(35, 75, std::fabs(d));  // skin, orange, red, yellow
    const float coolW = smoothstep(55, 95, std::fabs(d));      // greens, cyans, blues
    a -= d * 0.25f * warmW;                                     // tighten warm hues on skin
    a += wrapDeg(283 - a) * 0.45f * coolW;                      // cool hues towards teal
    C *= (1 + 0.12f * warmW) * (1 + 0.10f * coolW);
    cb = C * std::cos(a * kPi / 180);
    cr = C * std::sin(a * kPi / 180);
  }
  fromChroma(L, cb, cr, c);
  splitTone(c, kTealCb, kTealCr, 0.030f, kOrangeCb, kOrangeCr, 0.020f);
}

void lookWarm(float* c) {
  whiteGains(c, 1.10f, 1.0f, 0.85f);
  splitTone(c, 0, 0, 0, kGoldCb, kGoldCr, 0.015f);
  contrastRGB(c, 0.08f);
  saturate(c, 1.04f);
}

void lookCool(float* c) {
  whiteGains(c, 0.90f, 1.0f, 1.12f);
  splitTone(c, kTealCb, kTealCr, 0.015f, 0, 0, 0);
  contrastRGB(c, 0.12f);
  saturate(c, 0.92f);
}

void lookVivid(float* c) {
  contrastRGB(c, 0.18f);
  float L, cb, cr;
  toChroma(c, L, cb, cr);
  const float C = std::sqrt(cb * cb + cr * cr);
  const float k = 1.06f + 0.40f * (1 - smoothstep(0, 0.25f, C)) * (1 - 0.65f * skinMask(cb, cr));
  fromChroma(L, cb * k, cr * k, c);
}

void lookFilm(float* c) {
  contrastRGB(c, 0.30f);
  splitTone(c, -0.342f, -0.940f, 0.015f, kOrangeCb, kOrangeCr, 0.015f);  // green-teal / warm
  saturate(c, 0.85f);
  for (int i = 0; i < 3; ++i) c[i] = 0.04f + 0.90f * clamp01(c[i]);  // lifted blacks, soft whites
}

void lookBw(float* c, float wr, float wg, float wb, float contrast) {
  const float L = contrastCurve(clamp01(wr * c[0] + wg * c[1] + wb * c[2]), contrast);
  c[0] = c[1] = c[2] = L;
}

bool evalLook(const std::string& key, float* c) {
  if (key == "natural") lookNatural(c);
  else if (key == "teal_orange") lookTealOrange(c);
  else if (key == "warm") lookWarm(c);
  else if (key == "cool") lookCool(c);
  else if (key == "vivid") lookVivid(c);
  else if (key == "film") lookFilm(c);
  else if (key == "bw") lookBw(c, 0.30f, 0.59f, 0.11f, 0.18f);
  else if (key == "bw_contrast") lookBw(c, 0.50f, 0.42f, 0.08f, 0.75f);  // red filter
  else return false;
  for (int i = 0; i < 3; ++i) c[i] = clamp01(c[i]);
  return true;
}

// ------------------------------------------------------------------ LUT resolution
uint64_t fileStamp(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA a;
  if (!GetFileAttributesExW(utf8ToWide(path).c_str(), GetFileExInfoStandard, &a)) return 0;
  const uint64_t t = (static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) |
                     a.ftLastWriteTime.dwLowDateTime;
  return t ^ (static_cast<uint64_t>(a.nFileSizeLow) << 1) ^ 1;  // never 0 when it exists
}

const char* kBuiltinPrefix = "builtin:";

bool isBuiltinSpec(const std::string& s) { return s.compare(0, 8, kBuiltinPrefix) == 0; }

}  // namespace

// ------------------------------------------------------------------ .cube cache
// Shared by the bake worker, exportCube() and renderGradedRgba() (the gallery
// renders many thumbnails with the same files). A changed file gets a new
// stamp and is reloaded; parse errors are cached per stamp too. Bounded by
// memory, least recently used evicted first.
std::shared_ptr<const CubeLut> cachedCubeFile(const std::string& path, std::string& err) {
  struct Entry {
    uint64_t stamp = 0;
    std::shared_ptr<const CubeLut> lut;
    std::string err;
    size_t bytes = 0;
    uint64_t used = 0;
  };
  static std::mutex mx;
  static std::map<std::string, Entry> cache;
  static uint64_t clock = 0;
  static size_t total = 0;
  constexpr size_t kMaxBytes = 192u << 20;
  const uint64_t stamp = fileStamp(path);
  if (stamp) {
    std::lock_guard<std::mutex> lk(mx);
    auto it = cache.find(path);
    if (it != cache.end() && it->second.stamp == stamp) {
      it->second.used = ++clock;
      err = it->second.err;
      return it->second.lut;
    }
  }
  // Load outside the lock (a big file takes a while; other paths stay served).
  auto c = std::make_shared<CubeLut>();
  std::string e;
  std::shared_ptr<const CubeLut> lut;
  if (loadCubeFile(path, *c, e)) lut = std::move(c);
  err = e;
  if (!stamp) return lut;  // missing (or vanished): nothing to key on
  std::lock_guard<std::mutex> lk(mx);
  Entry& en = cache[path];
  total -= en.bytes;
  en.stamp = stamp;
  en.lut = lut;
  en.err = e;
  en.bytes = (lut ? lut->rgb.size() * sizeof(float) : 0) + path.size() + 64;
  en.used = ++clock;
  total += en.bytes;
  while (total > kMaxBytes && cache.size() > 1) {
    auto victim = cache.end();
    for (auto i = cache.begin(); i != cache.end(); ++i)
      if (i->first != path && (victim == cache.end() || i->second.used < victim->second.used)) victim = i;
    if (victim == cache.end()) break;
    total -= victim->second.bytes;
    cache.erase(victim);
  }
  return lut;
}

// "" -> no LUT; "builtin:<key>" -> generated look; else a .cube path.
bool resolveLut(const std::string& spec, std::shared_ptr<const CubeLut>& lut, std::string& title,
                std::string& err) {
  lut.reset();
  title.clear();
  err.clear();
  if (spec.empty()) return true;
  if (isBuiltinSpec(spec)) {
    const std::string key = spec.substr(8);
    lut = builtinLookLut(key);
    if (!lut) {
      err = "look embutido desconhecido: " + key;
      return false;
    }
    for (const BuiltinLook& l : builtinLooks())
      if (key == l.key) title = l.name;
    return true;
  }
  lut = cachedCubeFile(spec, err);
  if (!lut) return false;
  title = lut->title;
  if (title.empty()) {
    const size_t s = spec.find_last_of("/\\");
    title = s == std::string::npos ? spec : spec.substr(s + 1);
  }
  return true;
}

namespace {

// ------------------------------------------------------------------ .cube text
inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
  while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
  return s;
}

// Splits on whitespace into at most `cap` tokens; returns the token count
// (cap + 1 when there are more).
int tokenize(std::string_view s, std::string_view* tok, int cap) {
  int n = 0;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && isSpace(s[i])) ++i;
    if (i >= s.size()) break;
    size_t j = i;
    while (j < s.size() && !isSpace(s[j])) ++j;
    if (n == cap) return cap + 1;
    tok[n++] = s.substr(i, j - i);
    i = j;
  }
  return n;
}

bool parseFloat(std::string_view s, float& v) {
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  if (s.empty()) return false;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(v);
}

bool parseInt(std::string_view s, long long& v) {
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  if (s.empty()) return false;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

std::string lineErr(int line, const std::string& msg) { return "linha " + std::to_string(line) + ": " + msg; }

void appendFloat(std::string& out, float v) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, 6);
  out.append(buf, r.ptr);
}

}  // namespace

// ================================================================ builtin LUT cache
std::shared_ptr<const CubeLut> builtinLookLut(const std::string& key) {
  static std::mutex mx;
  static std::map<std::string, std::shared_ptr<const CubeLut>> cache;
  std::lock_guard<std::mutex> lk(mx);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  float probe[3] = {0.5f, 0.5f, 0.5f};
  if (!evalLook(key, probe)) return nullptr;
  auto lut = std::make_shared<CubeLut>();
  constexpr int N = 33;
  lut->title = key;
  lut->is3d = true;
  lut->size = N;
  lut->rgb.resize(static_cast<size_t>(N) * N * N * 3);
  float* o = lut->rgb.data();
  for (int b = 0; b < N; ++b)
    for (int g = 0; g < N; ++g)
      for (int r = 0; r < N; ++r, o += 3) {
        o[0] = static_cast<float>(r) / (N - 1);
        o[1] = static_cast<float>(g) / (N - 1);
        o[2] = static_cast<float>(b) / (N - 1);
        evalLook(key, o);
      }
  cache[key] = lut;
  return lut;
}

// ================================================================ LUT lookup
void lookupLut(const CubeLut& lut, const float in[3], float out[3]) {
  const int N = lut.size;
  float f[3];
  int i0[3];
  for (int c = 0; c < 3; ++c) {
    const float t = clamp01((in[c] - lut.domain_min[c]) / (lut.domain_max[c] - lut.domain_min[c]));
    const float p = t * static_cast<float>(N - 1);
    int i = static_cast<int>(p);
    if (i > N - 2) i = N - 2;
    i0[c] = i;
    f[c] = p - static_cast<float>(i);
  }
  const float* d = lut.rgb.data();
  if (!lut.is3d) {
    for (int c = 0; c < 3; ++c) {
      const float a = d[i0[c] * 3 + c], b = d[(i0[c] + 1) * 3 + c];
      out[c] = a + (b - a) * f[c];
    }
    return;
  }
  // Tetrahedral interpolation (red fastest in memory).
  const size_t sr = 3, sg = 3 * static_cast<size_t>(N), sb = sg * N;
  const float* c000 = d + i0[0] * sr + i0[1] * sg + i0[2] * sb;
  const float* c111 = c000 + sr + sg + sb;
  const float fr = f[0], fg = f[1], fb = f[2];
  const float *cA, *cB;  // the two intermediate vertices
  float w0, wA, wB, w1;
  if (fr > fg) {
    if (fg > fb) {         // r > g > b
      cA = c000 + sr; cB = c000 + sr + sg; w0 = 1 - fr; wA = fr - fg; wB = fg - fb; w1 = fb;
    } else if (fr > fb) {  // r > b >= g
      cA = c000 + sr; cB = c000 + sr + sb; w0 = 1 - fr; wA = fr - fb; wB = fb - fg; w1 = fg;
    } else {               // b >= r > g
      cA = c000 + sb; cB = c000 + sr + sb; w0 = 1 - fb; wA = fb - fr; wB = fr - fg; w1 = fg;
    }
  } else {
    if (fb > fg) {         // b > g >= r
      cA = c000 + sb; cB = c000 + sg + sb; w0 = 1 - fb; wA = fb - fg; wB = fg - fr; w1 = fr;
    } else if (fb > fr) {  // g >= b > r
      cA = c000 + sg; cB = c000 + sg + sb; w0 = 1 - fg; wA = fg - fb; wB = fb - fr; w1 = fr;
    } else {               // g >= r >= b
      cA = c000 + sg; cB = c000 + sr + sg; w0 = 1 - fg; wA = fg - fr; wB = fr - fb; w1 = fb;
    }
  }
  for (int c = 0; c < 3; ++c) out[c] = w0 * c000[c] + wA * cA[c] + wB * cB[c] + w1 * c111[c];
}

/// ================================================================ curves
namespace {
inline float curveAt(const std::vector<float>& t, float x) {
  const int N = static_cast<int>(t.size());
  const float p = clamp01(x) * static_cast<float>(N - 1);
  int i = static_cast<int>(p);
  if (i > N - 2) i = N - 2;
  const float f = p - static_cast<float>(i);
  return t[i] + (t[i + 1] - t[i]) * f;
}
inline float sgn(float v) { return v > 0 ? 1.0f : (v < 0 ? -1.0f : 0.0f); }

// Fritsch-Carlson tangent in its Fritsch-Butland form (weighted harmonic mean
// of the two secants, 0 at a local extremum): monotone, never overshoots.
inline float pchipTangent(float h0, float d0, float h1, float d1) {
  if (d0 * d1 <= 0) return 0;
  const float w1 = 2 * h1 + h0, w2 = h1 + 2 * h0;
  return (w1 + w2) / (w1 / d0 + w2 / d1);
}
// One-sided end tangent (shape-preserving three-point formula).
inline float pchipEnd(float h0, float d0, float h1, float d1) {
  float m = ((2 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
  if (sgn(m) != sgn(d0)) m = 0;
  else if (sgn(d0) != sgn(d1) && std::fabs(m) > 3 * std::fabs(d0)) m = 3 * d0;
  return m;
}
constexpr float kMergeDx = 1e-4f;  // points closer than this in x are merged
}  // namespace

PreparedCurve::PreparedCurve(const std::vector<CurvePoint>& pts, Kind kind) : kind_(kind) {
  const bool periodic = kind == Kind::Hue;
  std::vector<CurvePoint> p;
  p.reserve(pts.size() + 2);
  for (const CurvePoint& q : pts) {
    if (!std::isfinite(q.x) || !std::isfinite(q.y)) continue;
    CurvePoint c{clamp01(q.x), clamp01(q.y)};
    if (periodic && c.x >= 1) c.x = 0;  // x = 1 is x = 0 on the hue circle
    p.push_back(c);
  }
  std::stable_sort(p.begin(), p.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
  // merge (near) duplicate x into their mean
  std::vector<CurvePoint> m;
  m.reserve(p.size() + 2);
  for (size_t i = 0; i < p.size();) {
    size_t j = i + 1;
    float sx = p[i].x, sy = p[i].y;
    while (j < p.size() && p[j].x - p[i].x < kMergeDx) {
      sx += p[j].x;
      sy += p[j].y;
      ++j;
    }
    const float n = static_cast<float>(j - i);
    m.push_back({sx / n, sy / n});
    i = j;
  }
  if (periodic && m.size() >= 2 && m.front().x + 1 - m.back().x < kMergeDx) {
    m.front().y = (m.front().y + m.back().y) / 2;
    m.pop_back();
  }
  neutral_ = true;
  for (const CurvePoint& q : m)
    if (kind == Kind::Custom ? q.y != q.x : q.y != 0.5f) neutral_ = false;
  if (neutral_) return;
  if (kind == Kind::Custom) {
    // Implicit end points (0,0) / (1,1) when the curve has none of its own.
    if (m.front().x < kMergeDx) m.front().x = 0;
    else m.insert(m.begin(), CurvePoint{0, 0});
    if (m.back().x > 1 - kMergeDx) m.back().x = 1;
    else m.push_back(CurvePoint{1, 1});
  }
  const size_t n = m.size();
  x_.resize(n);
  y_.resize(n);
  m_.assign(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    x_[i] = m[i].x;
    y_[i] = m[i].y;
  }
  if (n == 1) return;  // constant (hue / luma-vs-sat curve with a single point)
  if (periodic) {
    // secant k joins knot k to knot k+1 (the last one wraps to knot 0, x + 1)
    std::vector<float> h(n), d(n);
    for (size_t k = 0; k < n; ++k) {
      const float x1 = k + 1 < n ? x_[k + 1] : x_[0] + 1;
      const float y1 = k + 1 < n ? y_[k + 1] : y_[0];
      h[k] = x1 - x_[k];
      d[k] = (y1 - y_[k]) / h[k];
    }
    for (size_t k = 0; k < n; ++k) {
      const size_t km = (k + n - 1) % n;
      m_[k] = pchipTangent(h[km], d[km], h[k], d[k]);
    }
    return;
  }
  std::vector<float> h(n - 1), d(n - 1);
  for (size_t k = 0; k + 1 < n; ++k) {
    h[k] = x_[k + 1] - x_[k];
    d[k] = (y_[k + 1] - y_[k]) / h[k];
  }
  if (n == 2) {
    m_[0] = m_[1] = d[0];
    return;
  }
  m_[0] = pchipEnd(h[0], d[0], h[1], d[1]);
  m_[n - 1] = pchipEnd(h[n - 2], d[n - 2], h[n - 3], d[n - 3]);
  for (size_t k = 1; k + 1 < n; ++k) m_[k] = pchipTangent(h[k - 1], d[k - 1], h[k], d[k]);
}

float PreparedCurve::eval(float x) const {
  if (neutral_) return kind_ == Kind::Custom ? clamp01(x) : 0.5f;
  const size_t n = x_.size();
  if (n == 1) return y_[0];
  float x0, x1, y0, y1, m0, m1;
  if (kind_ == Kind::Hue) {
    x -= std::floor(x);
    size_t k;
    if (x < x_[0]) {
      x += 1;
      k = n - 1;
    } else {
      k = static_cast<size_t>(std::upper_bound(x_.begin(), x_.end(), x) - x_.begin()) - 1;
    }
    const size_t k1 = k + 1 < n ? k + 1 : 0;
    x0 = x_[k];
    x1 = k + 1 < n ? x_[k1] : x_[0] + 1;
    y0 = y_[k];
    y1 = y_[k1];
    m0 = m_[k];
    m1 = m_[k1];
  } else {
    x = clamp01(x);
    if (x <= x_[0]) return y_[0];  // luma vs sat: flat beyond its end points
    if (x >= x_[n - 1]) return y_[n - 1];
    const size_t k = static_cast<size_t>(std::upper_bound(x_.begin(), x_.end(), x) - x_.begin()) - 1;
    x0 = x_[k];
    x1 = x_[k + 1];
    y0 = y_[k];
    y1 = y_[k + 1];
    m0 = m_[k];
    m1 = m_[k + 1];
  }
  const float h = x1 - x0;
  const float t = (x - x0) / h, t2 = t * t, t3 = t2 * t;
  const float y = (2 * t3 - 3 * t2 + 1) * y0 + (t3 - 2 * t2 + t) * h * m0 + (3 * t2 - 2 * t3) * y1 +
                  (t3 - t2) * h * m1;
  return clamp01(y);
}

float satGainOfCurveY(float y) {
  y = clamp01(y);
  const float g = std::exp2(4 * (y - 0.5f));
  if (y >= 0.5f) return g;
  const float u = 1 - 2 * y;
  return std::max(0.0f, g - 0.25f * u * u);
}

namespace {
// Cb/Cr angles (BT.709 opponent chroma) of R, Yl, G, Cy, B, Mg relative to
// red, counter-clockwise in [0, 2pi), plus 2pi: the hue anchors 0, 1/6 .. 1.
struct HueAnchors {
  float red = 0;  // absolute angle of red
  float a[7] = {};
  HueAnchors() {
    const float prim[6][3] = {{1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}};
    for (int i = 0; i < 6; ++i) {
      float L, cb, cr;
      toChroma(prim[i], L, cb, cr);
      const float ang = std::atan2(cr, cb);
      if (i == 0) red = ang;
      float d = ang - red;
      while (d < 0) d += 2 * kPi;
      a[i] = i == 0 ? 0 : d;
    }
    a[6] = 2 * kPi;
  }
};
// Namespace-scope (no guard per call); only depends on inline math.
const HueAnchors kHueAnchors;
inline const HueAnchors& hueAnchors() { return kHueAnchors; }
}  // namespace

float hueFromChromaAngle(float rad) {
  const HueAnchors& A = hueAnchors();
  float d = rad - A.red;
  d -= 2 * kPi * floorFast(d * (1 / (2 * kPi)));
  if (d >= 2 * kPi) d -= 2 * kPi;
  int k = 0;
  while (k < 5 && d >= A.a[k + 1]) ++k;
  const float h = (static_cast<float>(k) + (d - A.a[k]) / (A.a[k + 1] - A.a[k])) / 6;
  return h >= 1 ? h - 1 : h;
}

float chromaAngleFromHue(float hue) {
  const HueAnchors& A = hueAnchors();
  const float p = (hue - floorFast(hue)) * 6;
  int k = static_cast<int>(p);
  if (k > 5) k = 5;
  return A.red + A.a[k] + (p - static_cast<float>(k)) * (A.a[k + 1] - A.a[k]);
}

// ================================================================ pipeline
namespace {
// Master + channel of a wheel (each clamped to -1..1); true when any is != 0.
bool wheelSums(const ColorWheel& w, float out[3]) {
  const float m = clampf(w.y, -1, 1);
  out[0] = m + clampf(w.r, -1, 1);
  out[1] = m + clampf(w.g, -1, 1);
  out[2] = m + clampf(w.b, -1, 1);
  return out[0] != 0 || out[1] != 0 || out[2] != 0;
}
// Below this chroma the hue is mostly noise: the hue-keyed curves fade out
// (no hue-dependent rotation / gain of grain around grey).
constexpr float kHueFade0 = 0.004f, kHueFade1 = 0.03f;

// Branchless polynomial atan2 for the hue stage (once per bake sample and
// per thumbnail pixel; libm's, with its data-dependent branches, costs ~3x on
// varied hues). Max error ~1e-5 rad (1.6e-6 turns).
inline float fastAtan2(float y, float x) {
  const float ax = std::fabs(x), ay = std::fabs(y);
  const float mx = std::max(ax, ay), mn = std::min(ax, ay);
  const float a = mn / std::max(mx, 1e-30f), s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  r += static_cast<float>(ay > ax) * (1.57079637f - 2 * r);
  r += static_cast<float>(x < 0) * (3.14159274f - 2 * r);
  return std::copysign(r, y);
}
}  // namespace

void GradePipeline::prepare(const ColorGrade& gIn, std::shared_ptr<const CubeLut> lut, int curveN) {
  const int N = std::max(2, curveN);
  ColorGrade g = gIn;
  g.exposure_ev = clampf(g.exposure_ev, -3, 3);
  g.contrast = clampf(g.contrast, -1, 1);
  g.highlights = clampf(g.highlights, -1, 1);
  g.shadows = clampf(g.shadows, -1, 1);
  g.saturation = clampf(g.saturation, -1, 1);
  g.vibrance = clampf(g.vibrance, -1, 1);
  g.temperature = clampf(g.temperature, -1, 1);
  g.tint = clampf(g.tint, -1, 1);
  g.pivot = std::isfinite(g.pivot) ? clampf(g.pivot, 0.1f, 0.9f) : 0.435f;
  const float mix = g.lut_strength;

  // (A) white balance + exposure, per channel in ~linear light. Gains are
  // normalized so the luminance of white is unchanged; when the result would
  // exceed white, a C1 shoulder maps [knee, W] onto [knee, 1] instead of clipping.
  const bool useA = g.temperature != 0 || g.tint != 0 || g.exposure_ev != 0;
  float gain[3] = {1, 1, 1};
  bool shoulder = false;
  float knee = 1, a = 1;
  if (useA) {
    gain[0] = std::exp(0.30f * g.temperature);
    gain[1] = std::exp(-0.20f * g.tint);
    gain[2] = std::exp(-0.30f * g.temperature);
    const float n = 0.2126f * gain[0] + 0.7152f * gain[1] + 0.0722f * gain[2];
    const float ev = std::exp2(g.exposure_ev);
    float maxGain = 0;
    for (float& v : gain) {
      v = v / n * ev;
      maxGain = std::max(maxGain, v);
    }
    const float W = std::max(1.0f, maxGain);
    shoulder = W > 1.0001f;
    if (shoulder) {
      knee = std::max(0.5f, 1 - 0.35f * std::log2(W));
      // g(x) = (1 - e^(-a x)) / a has g(0) = 0, g'(0) = 1; solve g(xW) = 1.
      const float xW = (W - knee) / (1 - knee);
      float lo = 1e-6f, hi = 1;
      for (int it = 0; it < 60; ++it) {
        const float m = (lo + hi) / 2;
        if (1 - std::exp(-m * xW) - m > 0) lo = m; else hi = m;
      }
      a = (lo + hi) / 2;
    }
  }
  // (B) primaries (Resolve semantics, per channel, m + c of each wheel).
  float off[3], lif[3], gam[3], gai[3], gamExp[3], gainK[3];
  const bool useOff = wheelSums(g.offset, off), useLift = wheelSums(g.lift, lif);
  const bool useGamma = wheelSums(g.gamma, gam), useGain = wheelSums(g.gain, gai);
  for (int c = 0; c < 3; ++c) {
    gamExp[c] = std::exp2(-gam[c]);
    gainK[c] = std::exp2(gai[c]);
  }
  // (C) contrast around the pivot.
  const bool useContrast = g.contrast != 0;
  const float pivA = g.pivot == 0.5f ? 1.0f : std::log(0.5f) / std::log(g.pivot);
  // A -> offset -> lift -> gamma -> gain -> contrast are all per channel:
  // folded into one table per channel. Values may leave 0..1 in between
  // (offset / gain); contrast only reshapes 0..1 and passes the rest through.
  usePre_ = useA || useOff || useLift || useGamma || useGain || useContrast;
  if (usePre_) {
    for (int c = 0; c < 3; ++c) {
      curvePre_[c].resize(N);
      for (int i = 0; i < N; ++i) {
        const float x = static_cast<float>(i) / (N - 1);
        float v = x;
        if (useA) {
          v = decodeGamma(x) * gain[c];
          if (shoulder && v > knee) v = knee + (1 - knee) * (1 - std::exp(-a * (v - knee) / (1 - knee))) / a;
          v = encodeGamma(v);
        }
        if (useOff) v += 0.25f * off[c];
        if (useLift) v += 0.25f * lif[c] * (1 - v);
        if (useGamma && v > 0) v = std::pow(v, gamExp[c]);  // negative values pass through
        if (useGain) v *= gainK[c];
        if (useContrast) v = contrastPivot(v, g.contrast, pivA);
        curvePre_[c][i] = v;
      }
    }
  }
  // (D) shadows / highlights: luminance delta added to all channels.
  useTone_ = g.shadows != 0 || g.highlights != 0;
  if (useTone_) {
    curveTone_.resize(N);
    for (int i = 0; i < N; ++i) {
      const float L = static_cast<float>(i) / (N - 1);
      curveTone_[i] = highlightsCurve(shadowsCurve(L, g.shadows), g.highlights) - L;
    }
  }
  // (E) custom curves: master (Y) then the channel's own, one table per
  // channel (a channel with both neutral is left alone, not even clamped).
  const PreparedCurve cy(g.curve_y, PreparedCurve::Kind::Custom);
  const PreparedCurve cch[3] = {PreparedCurve(g.curve_r, PreparedCurve::Kind::Custom),
                                PreparedCurve(g.curve_g, PreparedCurve::Kind::Custom),
                                PreparedCurve(g.curve_b, PreparedCurve::Kind::Custom)};
  useCurves_ = false;
  for (int c = 0; c < 3; ++c) {
    postOn_[c] = !cy.neutral() || !cch[c].neutral();
    useCurves_ = useCurves_ || postOn_[c];
    if (!postOn_[c]) continue;
    curvePost_[c].resize(N);
    for (int i = 0; i < N; ++i)
      curvePost_[c][i] = cch[c].eval(cy.eval(static_cast<float>(i) / (N - 1)));
  }
  // (F) hue vs hue, hue vs sat (both keyed on the incoming hue, like the
  // curves of one Resolve node), luma vs sat.
  const PreparedCurve hh(g.hue_vs_hue, PreparedCurve::Kind::Hue);
  const PreparedCurve hs(g.hue_vs_sat, PreparedCurve::Kind::Hue);
  const PreparedCurve ls(g.luma_vs_sat, PreparedCurve::Kind::LumaSat);
  useHueShift_ = !hh.neutral();
  useHueSat_ = !hs.neutral();
  useLumaSat_ = !ls.neutral();
  useHue_ = useHueShift_ || useHueSat_ || useLumaSat_;
  // Tabulated over the Cb/Cr angle (not over hue), so per sample there is
  // one atan2 and no hue mapping / sin / cos: the rotation is stored as its
  // cos and sin (continuous even for a half-turn shift).
  if (useHueShift_) {
    hueRot_.resize(2 * kHueN);
    for (int i = 0; i < kHueN; ++i) {
      const float ang = 2 * kPi * static_cast<float>(i) / kHueN;
      const float hue = hueFromChromaAngle(ang);
      const float d = chromaAngleFromHue(hue + hh.eval(hue) - 0.5f) - ang;
      hueRot_[2 * i] = std::cos(d);
      hueRot_[2 * i + 1] = std::sin(d);
    }
  }
  if (useHueSat_) {
    hueSat_.resize(kHueN);
    for (int i = 0; i < kHueN; ++i)
      hueSat_[i] = satGainOfCurveY(hs.eval(hueFromChromaAngle(2 * kPi * static_cast<float>(i) / kHueN)));
  }
  if (useLumaSat_) {
    lumaSat_.resize(N);
    for (int i = 0; i < N; ++i)
      lumaSat_[i] = satGainOfCurveY(ls.eval(static_cast<float>(i) / (N - 1)));
  }
  // (G) saturation / vibrance.
  sat_ = g.saturation;
  vib_ = g.vibrance;
  useSat_ = sat_ != 0 || vib_ != 0;
  // (H) LUT mixed by strength.
  setLut(std::move(lut), mix);
}

void GradePipeline::setLut(std::shared_ptr<const CubeLut> lut, float strength) {
  lut_ = std::move(lut);
  lutMix_ = clamp01(strength);
  useLut_ = lut_ && lut_->size >= 2 && lutMix_ > 0;
  // Global chroma transform (complex ratio out/in in the Cb/Cr plane) around
  // mid grey: the fallback for out-of-gamut excess whose clamped point is ~grey.
  ratioRe_ = 1;
  ratioIm_ = 0;
  if (!noop()) {
    float re = 0, im = 0;
    for (int k = 0; k < 8; ++k) {
      const float ang = static_cast<float>(k) * kPi / 4;
      float c[3];
      fromChroma(0.5f, 0.04f * std::cos(ang), 0.04f * std::sin(ang), c);
      float o[3] = {c[0], c[1], c[2]};
      core(o);
      for (float& v : o) v = clamp01(v);
      float r, i;
      chromaRatio(c, o, r, i);
      re += r / 8;
      im += i / 8;
    }
    ratioRe_ = re;
    ratioIm_ = im;
  }
}

// Complex ratio z(out) / z(in), z = Cb + i Cr; magnitude capped at 2. With a
// near-grey input the ratio is ill-conditioned: blended to the global one.
void GradePipeline::chromaRatio(const float* in, const float* out, float& re, float& im) const {
  float Li, cbi, cri, Lo, cbo, cro;
  toChroma(in, Li, cbi, cri);
  toChroma(out, Lo, cbo, cro);
  const float m2 = cbi * cbi + cri * cri;
  float r = ratioRe_, i = ratioIm_;
  if (m2 > 1e-10f) {
    const float lr = (cbo * cbi + cro * cri) / m2, li = (cro * cbi - cbo * cri) / m2;
    const float w = smoothstep(0.005f, 0.03f, std::sqrt(m2));
    r = r + (lr - r) * w;
    i = i + (li - i) * w;
  }
  const float mag = std::sqrt(r * r + i * i);
  if (mag > 2) {
    r *= 2 / mag;
    i *= 2 / mag;
  }
  re = r;
  im = i;
}

void GradePipeline::run(float rgb[3]) const {
  float c[3] = {clamp01(rgb[0]), clamp01(rgb[1]), clamp01(rgb[2])};
  float e[3] = {rgb[0] - c[0], rgb[1] - c[1], rgb[2] - c[2]};
  if (noop()) return;  // rgb unchanged
  const float in[3] = {c[0], c[1], c[2]};
  core(c);
  for (float& v : c) v = clamp01(v);
  if (e[0] != 0 || e[1] != 0 || e[2] != 0) {
    // Out-of-gamut input (common for YUV): its excess beyond the RGB cube is
    // kept, its luma part as is (super-whites/blacks), its chroma part carried
    // through the grade's local chroma transform (a B&W look removes it, a
    // neutral grade keeps it, a hue shift rotates it).
    float r, i;
    chromaRatio(in, c, r, i);
    if (r != 1 || i != 0) {
      float L, cb, cr;
      toChroma(e, L, cb, cr);
      fromChroma(L, cb * r - cr * i, cb * i + cr * r, e);
    }
  }
  for (int k = 0; k < 3; ++k) rgb[k] = c[k] + e[k];
}

// Hue curves on the BT.709 opponent chroma: luma is kept, the Cb/Cr vector is
// rotated (hue vs hue) and scaled (hue vs sat, luma vs sat). Near grey the hue
// is noise, so the hue-keyed effects fade out below kHueFade1.
void GradePipeline::hueStage(float c[3]) const {
  float L, cb, cr;
  toChroma(c, L, cb, cr);
  float k = useLumaSat_ ? curveAt(lumaSat_, L) : 1.0f;
  if (useHueShift_ || useHueSat_) {
    const float C = std::sqrt(cb * cb + cr * cr);
    const float w = smoothstep(kHueFade0, kHueFade1, C);
    if (w > 0) {
      // position in the angle tables (periodic, kHueN a power of two)
      float t = fastAtan2(cr, cb) * (kHueN / (2 * kPi));
      t += static_cast<float>(t < 0) * kHueN;
      int i = static_cast<int>(t);
      const float f = t - static_cast<float>(i);
      i &= kHueN - 1;
      const int j = (i + 1) & (kHueN - 1);
      if (useHueSat_) k *= 1 + (hueSat_[i] + (hueSat_[j] - hueSat_[i]) * f - 1) * w;
      if (useHueShift_) {
        const float* a = &hueRot_[2 * i];
        const float* b = &hueRot_[2 * j];
        float rc = a[0] + (b[0] - a[0]) * f, rs = a[1] + (b[1] - a[1]) * f;
        if (w < 1) {  // fading out: part of the way from no rotation, unit length
          rc = 1 + (rc - 1) * w;
          rs *= w;
          const float n = 1 / std::sqrt(std::max(rc * rc + rs * rs, 1e-12f));
          rc *= n;
          rs *= n;
        }
        const float nb = cb * rc - cr * rs;
        cr = cb * rs + cr * rc;
        cb = nb;
      }
    }
  }
  fromChroma(L, cb * k, cr * k, c);
}

void GradePipeline::core(float c[3]) const {
  if (usePre_)
    for (int i = 0; i < 3; ++i) c[i] = curveAt(curvePre_[i], c[i]);
  if (useTone_) {
    const float d = curveAt(curveTone_, luma709(c));
    for (int i = 0; i < 3; ++i) c[i] += d;
  }
  if (useCurves_)
    for (int i = 0; i < 3; ++i)
      if (postOn_[i]) c[i] = curveAt(curvePost_[i], c[i]);
  if (useHue_) hueStage(c);
  if (useSat_) {
    const float L = luma709(c);
    float k = 1 + sat_;
    if (vib_ != 0) {
      const float mx = clamp01(std::max(c[0], std::max(c[1], c[2])));
      const float mn = clamp01(std::min(c[0], std::min(c[1], c[2])));
      float w = 1 - smoothstep(0, 0.6f, mx - mn);  // muted colours move most
      if (vib_ > 0) w *= 1 - 0.7f * skinMask((c[2] - L) / 1.8556f, (c[0] - L) / 1.5748f);
      k *= 1 + vib_ * w;
    }
    for (int i = 0; i < 3; ++i) c[i] = L + (c[i] - L) * k;
  }
  if (useLut_) {
    float o[3];
    lookupLut(*lut_, c, o);
    for (int i = 0; i < 3; ++i) c[i] += (o[i] - c[i]) * lutMix_;
  }
}

// ================================================================ table
void bakeGradeTable(const GradePipeline& p, uint32_t matrix, bool fullRange, GradeTable& t,
                    int threads) {
  constexpr int N = GradeTable::kNodes;
  t.matrix = matrix;
  t.fullRange = fullRange;
  t.y.assign(static_cast<size_t>(N) * N * 256, 0);
  t.uv.assign(static_cast<size_t>(N) * N * 256 * 2, 0);
  const float kr = matrix == 1 ? 0.299f : 0.2126f;
  const float kb = matrix == 1 ? 0.114f : 0.0722f;
  const float kg = 1 - kr - kb;
  const float yOff = fullRange ? 0.0f : 16.0f;
  const float yScale = fullRange ? 255.0f : 219.0f;
  const float cScale = fullRange ? 255.0f : 224.0f;
  const float rCr = 2 * (1 - kr), bCb = 2 * (1 - kb);
  auto q = [](float v, float hi) {
    return static_cast<uint16_t>(std::lround(clampf(v, 0, hi) * 64));
  };
  auto work = [&](int n0, int n1) {
    for (int n = n0; n < n1; ++n) {
      const float cb = (static_cast<float>((n % N) * 8) - 128) / cScale;
      const float cr = (static_cast<float>((n / N) * 8) - 128) / cScale;
      uint16_t* ty = t.y.data() + static_cast<size_t>(n) * 256;
      uint16_t* tuv = t.uv.data() + static_cast<size_t>(n) * 512;
      for (int Y = 0; Y < 256; ++Y) {
        const float yn = (static_cast<float>(Y) - yOff) / yScale;
        float c[3];
        c[0] = yn + rCr * cr;
        c[2] = yn + bCb * cb;
        c[1] = (yn - kr * c[0] - kb * c[2]) / kg;
        p.run(c);
        const float y2 = kr * c[0] + kg * c[1] + kb * c[2];
        ty[Y] = q(y2 * yScale + yOff, 255);
        tuv[Y * 2] = q((c[2] - y2) / bCb * cScale + 128, 256);
        tuv[Y * 2 + 1] = q((c[0] - y2) / rCr * cScale + 128, 256);
      }
    }
  };
  const int total = N * N;
  threads = std::max(1, std::min(threads, 16));
  if (threads == 1) {
    work(0, total);
    return;
  }
  // Helpers inherit the caller's priority (the bake worker runs below normal).
  const int prio = GetThreadPriority(GetCurrentThread());
  std::vector<std::thread> th;
  for (int i = 1; i < threads; ++i)
    th.emplace_back([&work, prio, a = total * i / threads, b = total * (i + 1) / threads] {
      SetThreadPriority(GetCurrentThread(), prio);
      work(a, b);
    });
  work(0, total / threads);
  for (auto& x : th) x.join();
}

void applyGradeRows(const GradeTable& t, const uint8_t* sy, size_t sys, const uint8_t* suv,
                    size_t suvs, uint8_t* dy, size_t dys, uint8_t* duv, size_t duvs, uint32_t x0,
                    uint32_t w, uint32_t r2Begin, uint32_t r2End) {
  constexpr uint32_t N = GradeTable::kNodes;
  const uint16_t* TY = t.y.data();
  const uint16_t* TUV = t.uv.data();
  for (uint32_t r2 = r2Begin; r2 < r2End; ++r2) {
    const uint8_t* s0 = sy + static_cast<size_t>(2 * r2) * sys;
    const uint8_t* s1 = s0 + sys;
    const uint8_t* sc = suv + static_cast<size_t>(r2) * suvs;
    uint8_t* d0 = dy + static_cast<size_t>(2 * r2) * dys;
    uint8_t* d1 = d0 + dys;
    uint8_t* dc = duv + static_cast<size_t>(r2) * duvs;
    for (uint32_t x = x0; x < w; x += 2) {
      // read the whole block first (src may be dst)
      const uint32_t u = sc[x], v = sc[x + 1];
      const uint32_t y00 = s0[x], y01 = s0[x + 1], y10 = s1[x], y11 = s1[x + 1];
      const uint32_t fu = u & 7, fv = v & 7;
      const uint32_t w00 = (8 - fu) * (8 - fv), w01 = fu * (8 - fv), w10 = (8 - fu) * fv,
                     w11 = fu * fv;
      const size_t n = static_cast<size_t>(v >> 3) * N + (u >> 3);
      const uint16_t* a = TY + n * 256;
      const uint16_t* b = a + 256;
      const uint16_t* c = a + N * 256;
      const uint16_t* d = c + 256;
      d0[x] = static_cast<uint8_t>((w00 * a[y00] + w01 * b[y00] + w10 * c[y00] + w11 * d[y00] + 2048) >> 12);
      d0[x + 1] = static_cast<uint8_t>((w00 * a[y01] + w01 * b[y01] + w10 * c[y01] + w11 * d[y01] + 2048) >> 12);
      d1[x] = static_cast<uint8_t>((w00 * a[y10] + w01 * b[y10] + w10 * c[y10] + w11 * d[y10] + 2048) >> 12);
      d1[x + 1] = static_cast<uint8_t>((w00 * a[y11] + w01 * b[y11] + w10 * c[y11] + w11 * d[y11] + 2048) >> 12);
      const uint32_t ya = (y00 + y01 + y10 + y11 + 2) >> 2;
      const uint16_t* ua = TUV + (n * 256 + ya) * 2;
      const uint16_t* ub = ua + 512;
      const uint16_t* uc = ua + N * 512;
      const uint16_t* ud = uc + 512;
      const uint32_t ou = (w00 * ua[0] + w01 * ub[0] + w10 * uc[0] + w11 * ud[0] + 2048) >> 12;
      const uint32_t ov = (w00 * ua[1] + w01 * ub[1] + w10 * uc[1] + w11 * ud[1] + 2048) >> 12;
      dc[x] = static_cast<uint8_t>(ou > 255 ? 255 : ou);
      dc[x + 1] = static_cast<uint8_t>(ov > 255 ? 255 : ov);
    }
  }
}

// ================================================================ BandPool
BandPool::BandPool(int participants) {
  for (int i = 1; i < participants; ++i) threads_.emplace_back([this] { workerMain(); });
}

BandPool::~BandPool() {
  {
    std::lock_guard<std::mutex> lk(mx_);
    quit_ = true;
  }
  cv_.notify_all();
  for (auto& t : threads_) t.join();
}

void BandPool::run(int n, void (*fn)(void*, int), void* ctx) {
  if (threads_.empty() || n <= 1) {
    for (int i = 0; i < n; ++i) fn(ctx, i);
    return;
  }
  {
    std::lock_guard<std::mutex> lk(mx_);
    fn_ = fn;
    ctx_ = ctx;
    n_ = n;
    next_ = 0;
    busy_ = static_cast<int>(threads_.size());
    ++gen_;
  }
  cv_.notify_all();
  for (int i; (i = next_++) < n;) fn(ctx, i);
  std::unique_lock<std::mutex> lk(mx_);
  doneCv_.wait(lk, [this] { return busy_ == 0; });
}

void BandPool::workerMain() {
  setThreadName(L"owc-grade");
  uint64_t seen = 0;
  std::unique_lock<std::mutex> lk(mx_);
  for (;;) {
    cv_.wait(lk, [&] { return quit_ || gen_ != seen; });
    if (quit_) return;
    seen = gen_;
    void (*fn)(void*, int) = fn_;
    void* ctx = ctx_;
    const int n = n_;
    lk.unlock();
    for (int i; (i = next_++) < n;) fn(ctx, i);
    lk.lock();
    if (--busy_ == 0) doneCv_.notify_one();
  }
}

// ================================================================ ColorGrader
namespace {
int poolSize() {
  const int hw = static_cast<int>(std::thread::hardware_concurrency());
  return std::max(1, std::min(4, hw / 2));
}

struct ApplyJob {
  const GradeTable* t;
  const uint8_t *sy, *suv;
  size_t sys, suvs;
  uint8_t *dy, *duv;
  size_t dys, duvs;
  uint32_t x0, w, rows2;
  int bands;
  bool copyLeft;
};

void applyBand(void* ctx, int i) {
  const ApplyJob& j = *static_cast<const ApplyJob*>(ctx);
  const uint32_t r0 = static_cast<uint32_t>(static_cast<uint64_t>(j.rows2) * i / j.bands);
  const uint32_t r1 = static_cast<uint32_t>(static_cast<uint64_t>(j.rows2) * (i + 1) / j.bands);
  if (j.copyLeft)
    for (uint32_t r = r0; r < r1; ++r) {
      memcpy(j.dy + 2 * r * j.dys, j.sy + 2 * r * j.sys, j.x0);
      memcpy(j.dy + (2 * r + 1) * j.dys, j.sy + (2 * r + 1) * j.sys, j.x0);
      memcpy(j.duv + r * j.duvs, j.suv + r * j.suvs, j.x0);
    }
  applyGradeRows(*j.t, j.sy, j.sys, j.suv, j.suvs, j.dy, j.dys, j.duv, j.duvs, j.x0, j.w, r0, r1);
}
}  // namespace

ColorGrader::~ColorGrader() {
  {
    std::lock_guard<std::mutex> lk(mx_);
    quit_ = true;
  }
  cv_.notify_all();
  if (bakeThread_.joinable()) bakeThread_.join();
  pool_.reset();
}

bool ColorGrader::apply(const ColorGrade& g, uint32_t matrix, bool fullRange, const uint8_t*& y,
                        uint32_t& ys, const uint8_t*& uv, uint32_t& uvs, uint32_t w, uint32_t h,
                        std::vector<uint8_t>& buf) {
  if (isIdentity(g) || w < 2 || h < 2 || (w & 1) || (h & 1)) {
    if (engaged_) {  // went idle: stop the worker's polling, free the table
      engaged_ = false;
      std::lock_guard<std::mutex> lk(mx_);
      haveWant_ = false;
      ++wantGen_;
      table_.reset();
      active_ = false;
      cv_.notify_one();
    }
    return false;
  }
  engaged_ = true;
  // Copy-assignment into the persistent probe reuses its vectors' capacity:
  // no allocation per frame once the curves have been seen.
  Key& k = probe_;
  k.g = g;
  k.g.enabled = true;
  k.g.compare_split = false;
  k.matrix = matrix;
  k.fullRange = fullRange;
  std::shared_ptr<const GradeTable> t;
  {
    std::lock_guard<std::mutex> lk(mx_);
    if (!bakeThread_.joinable()) bakeThread_ = std::thread([this] { bakeMain(); });
    if (!haveWant_ || !(want_ == k)) {
      want_ = k;
      haveWant_ = true;
      ++wantGen_;
      cv_.notify_one();
    }
    t = table_;
    if (!t || t->matrix != matrix || t->fullRange != fullRange) {
      active_ = false;
      return false;
    }
  }
  const int64_t t0 = qpcNowRaw();
  if (!pool_) pool_.reset(new BandPool(poolSize()));
  ApplyJob j;
  j.t = t.get();
  j.sy = y;
  j.suv = uv;
  j.sys = ys;
  j.suvs = uvs;
  const bool inPlace = !buf.empty() && y >= buf.data() && y < buf.data() + buf.size();
  if (inPlace) {
    j.dy = const_cast<uint8_t*>(y);
    j.duv = const_cast<uint8_t*>(uv);
    j.dys = ys;
    j.duvs = uvs;
  } else {
    buf.resize(static_cast<size_t>(w) * h * 3 / 2);
    j.dy = buf.data();
    j.duv = buf.data() + static_cast<size_t>(w) * h;
    j.dys = j.duvs = w;
  }
  j.x0 = g.compare_split ? (w / 2) & ~1u : 0;
  j.w = w;
  j.rows2 = h / 2;
  j.bands = pool_->participants() * 4;
  j.copyLeft = j.x0 > 0 && !inPlace;
  pool_->run(j.bands, applyBand, &j);
  y = j.dy;
  uv = j.duv;
  ys = static_cast<uint32_t>(j.dys);
  uvs = static_cast<uint32_t>(j.duvs);
  const double ms = static_cast<double>(qpcNowRaw() - t0) * 1000.0 / static_cast<double>(qpcFreq());
  std::lock_guard<std::mutex> lk(mx_);
  applyMs_.add(ms);
  active_ = true;
  return true;
}

ColorGrader::Status ColorGrader::status() const {
  std::lock_guard<std::mutex> lk(mx_);
  Status s;
  s.active = active_;
  s.applyMsAvg = applyMs_.avg();
  s.bakeMs = bakeMs_;
  s.lutSize = lutSize_;
  s.lutTitle = lutTitle_;
  s.error = error_;
  return s;
}

bool ColorGrader::waitBaked(int timeoutMs) {
  std::unique_lock<std::mutex> lk(mx_);
  return bakedCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [this] { return bakedGen_ == wantGen_; });
}

void ColorGrader::bakeMain() {
  setThreadName(L"owc-grade-bake");
  // LUT state: this thread only.
  std::string lutSpec;
  bool lutLoaded = false;
  uint64_t lutStamp = 0;
  std::shared_ptr<const CubeLut> lut;
  std::string lutTitle, lutErr;
  int64_t lastStat = steadyMs();
  // Measured (8 logical cores): 46 ms on 1 thread, 25 on 2, 16 on 4, 15 on 8.
  // 4 threads below normal priority: continuous slider drags re-bake back to
  // back, and must not starve the decode thread or the UI.
  const int bakeThreads = poolSize();
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

  std::unique_lock<std::mutex> lk(mx_);
  while (!quit_) {
    bool reload = false;
    if (bakedGen_ == wantGen_) {
      cv_.wait_for(lk, std::chrono::milliseconds(1000), [this] { return quit_ || bakedGen_ != wantGen_; });
      if (quit_) break;
      if (bakedGen_ == wantGen_) {
        // Idle: poll the .cube file's modification time (~1 Hz, never on the decode thread).
        if (!haveWant_ || want_.g.lut.empty() || isBuiltinSpec(want_.g.lut) ||
            steadyMs() - lastStat < 900)
          continue;
        const std::string path = want_.g.lut;
        lk.unlock();
        const uint64_t st = fileStamp(path);
        lastStat = steadyMs();
        lk.lock();
        if (path == lutSpec && st == lutStamp) continue;
        reload = true;
      }
    }
    if (!haveWant_) {
      bakedGen_ = wantGen_;
      bakedCv_.notify_all();
      continue;
    }
    const Key k = want_;
    const uint64_t gen = wantGen_;
    lk.unlock();

    const int64_t t0 = qpcNowRaw();
    if (!lutLoaded || reload || k.g.lut != lutSpec) {
      lutSpec = k.g.lut;
      lutLoaded = true;
      const bool file = !lutSpec.empty() && !isBuiltinSpec(lutSpec);
      lutStamp = file ? fileStamp(lutSpec) : 0;
      lastStat = steadyMs();
      resolveLut(lutSpec, lut, lutTitle, lutErr);
    }
    GradePipeline p;
    p.prepare(k.g, lut);
    auto table = std::make_shared<GradeTable>();
    bakeGradeTable(p, k.matrix, k.fullRange, *table, bakeThreads);
    const double ms = static_cast<double>(qpcNowRaw() - t0) * 1000.0 / static_cast<double>(qpcFreq());

    lk.lock();
    if (haveWant_) table_ = std::move(table);
    // A newer request may have arrived meanwhile: then bakedGen_ != wantGen_
    // and the loop bakes again (only the latest value, never a queue).
    bakedGen_ = gen;
    bakeMs_ = ms;
    lutSize_ = lut ? static_cast<uint32_t>(lut->size) : 0;
    lutTitle_ = lutTitle;
    error_ = lutErr;
    bakedCv_.notify_all();
  }
}

}  // namespace detail

// ================================================================ .cube I/O
bool parseCube(const std::string& text, CubeLut& out, std::string& err) {
  using namespace detail;
  CubeLut lut;
  int size3 = 0, size1 = 0;
  size_t expected = 0, count = 0;
  int line = 0;
  size_t pos = 0;
  std::string_view tok[5];
  const std::string_view all(text);
  while (pos < all.size()) {
    size_t e = all.find('\n', pos);
    if (e == std::string_view::npos) e = all.size();
    std::string_view s = all.substr(pos, e - pos);
    pos = e + 1;
    ++line;
    if (line == 1 && s.size() >= 3 && s.substr(0, 3) == "\xEF\xBB\xBF") s.remove_prefix(3);
    s = trim(s);
    if (s.empty() || s.front() == '#') continue;
    const char c0 = s.front();
    if ((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')) {
      size_t k = 0;
      while (k < s.size() && !isSpace(s[k])) ++k;
      const std::string_view kw = s.substr(0, k);
      std::string_view rest = trim(s.substr(k));
      if (kw == "TITLE") {
        if (rest.size() >= 2 && rest.front() == '"') {
          const size_t q = rest.find('"', 1);
          rest = q == std::string_view::npos ? rest.substr(1) : rest.substr(1, q - 1);
        }
        lut.title.assign(rest.data(), rest.size());
        continue;
      }
      const size_t hash = rest.find('#');
      if (hash != std::string_view::npos) rest = trim(rest.substr(0, hash));
      const int nt = tokenize(rest, tok, 4);
      if (kw == "LUT_3D_SIZE" || kw == "LUT_1D_SIZE") {
        const bool is3 = kw == "LUT_3D_SIZE";
        long long n = 0;
        if (nt != 1 || !parseInt(tok[0], n)) {
          err = lineErr(line, std::string(kw) + " inválido");
          return false;
        }
        if (count > 0) {
          err = lineErr(line, std::string(kw) + " depois dos dados");
          return false;
        }
        if (is3 ? (n < 2 || n > 256) : (n < 2 || n > 65536)) {
          err = lineErr(line, std::string(kw) + " fora do intervalo (" + (is3 ? "2..256" : "2..65536") + ")");
          return false;
        }
        if ((is3 && size1) || (!is3 && size3)) {
          err = lineErr(line, "LUT 1D e 3D no mesmo arquivo não é suportado");
          return false;
        }
        if (is3) size3 = static_cast<int>(n); else size1 = static_cast<int>(n);
        expected = is3 ? static_cast<size_t>(n) * n * n : static_cast<size_t>(n);
        continue;
      }
      if (kw == "DOMAIN_MIN" || kw == "DOMAIN_MAX") {
        float v[3];
        if (nt != 3 || !parseFloat(tok[0], v[0]) || !parseFloat(tok[1], v[1]) || !parseFloat(tok[2], v[2])) {
          err = lineErr(line, std::string(kw) + ": esperados 3 números");
          return false;
        }
        float* d = kw == "DOMAIN_MIN" ? lut.domain_min : lut.domain_max;
        for (int i = 0; i < 3; ++i) d[i] = v[i];
        continue;
      }
      if (kw == "LUT_3D_INPUT_RANGE" || kw == "LUT_1D_INPUT_RANGE") {
        float lo, hi;
        if (nt != 2 || !parseFloat(tok[0], lo) || !parseFloat(tok[1], hi)) {
          err = lineErr(line, std::string(kw) + ": esperados 2 números");
          return false;
        }
        for (int i = 0; i < 3; ++i) {
          lut.domain_min[i] = lo;
          lut.domain_max[i] = hi;
        }
        continue;
      }
      // Other keywords (LUT_IN_VIDEO_RANGE, vendor extensions...) are ignored.
      continue;
    }
    // data line
    const size_t hash = s.find('#');
    if (hash != std::string_view::npos) s = trim(s.substr(0, hash));
    if (!expected) {
      err = lineErr(line, "dados antes de LUT_3D_SIZE / LUT_1D_SIZE");
      return false;
    }
    const int nt = tokenize(s, tok, 3);
    if (nt != 3) {
      err = lineErr(line, "esperados 3 números");
      return false;
    }
    if (count >= expected) {
      err = lineErr(line, "mais valores do que o tamanho declarado (" + std::to_string(expected) + ")");
      return false;
    }
    if (lut.rgb.empty()) lut.rgb.reserve(expected * 3);
    for (int i = 0; i < 3; ++i) {
      float v;
      if (!parseFloat(tok[i], v)) {
        err = lineErr(line, "número inválido '" + std::string(tok[i]) + "'");
        return false;
      }
      lut.rgb.push_back(v);
    }
    ++count;
  }
  if (!expected) {
    err = "LUT_3D_SIZE ou LUT_1D_SIZE ausente";
    return false;
  }
  if (count != expected) {
    err = lineErr(line, "fim do arquivo com " + std::to_string(count) + " valores RGB, esperados " +
                            std::to_string(expected));
    return false;
  }
  for (int i = 0; i < 3; ++i)
    if (!(lut.domain_max[i] > lut.domain_min[i])) {
      err = "DOMAIN_MAX deve ser maior que DOMAIN_MIN";
      return false;
    }
  lut.is3d = size3 != 0;
  lut.size = size3 ? size3 : size1;
  out = std::move(lut);
  err.clear();
  return true;
}

bool loadCubeFile(const std::string& path, CubeLut& out, std::string& err) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, detail::utf8ToWide(path).c_str(), L"rb") != 0 || !f) {
    err = "não foi possível abrir " + path;
    return false;
  }
  std::string text;
  _fseeki64(f, 0, SEEK_END);
  const long long size = _ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
  if (size < 0 || size > (1LL << 30)) {
    fclose(f);
    err = "arquivo grande demais: " + path;
    return false;
  }
  text.resize(static_cast<size_t>(size));
  const size_t got = size ? fread(&text[0], 1, text.size(), f) : 0;
  fclose(f);
  if (got != text.size()) {
    err = "erro de leitura: " + path;
    return false;
  }
  if (!parseCube(text, out, err)) {
    err = path + ": " + err;
    return false;
  }
  return true;
}

bool exportCube(const ColorGrade& g, const std::string& path, int size, std::string& err) {
  if (size < 2 || size > 256) {
    err = "tamanho do LUT fora do intervalo (2..256)";
    return false;
  }
  std::shared_ptr<const CubeLut> lut;
  std::string title;
  if (!detail::resolveLut(g.lut, lut, title, err)) return false;
  detail::GradePipeline p;
  p.prepare(g, lut);
  std::string text = "TITLE \"OWC grade\"\nLUT_3D_SIZE " + std::to_string(size) +
                     "\nDOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n";
  text.reserve(text.size() + static_cast<size_t>(size) * size * size * 28);
  const float inv = 1.0f / static_cast<float>(size - 1);
  for (int b = 0; b < size; ++b)
    for (int gg = 0; gg < size; ++gg)
      for (int r = 0; r < size; ++r) {
        float c[3] = {static_cast<float>(r) * inv, static_cast<float>(gg) * inv, static_cast<float>(b) * inv};
        p.run(c);
        for (int i = 0; i < 3; ++i) {
          detail::appendFloat(text, detail::clamp01(c[i]));
          text.push_back(i == 2 ? '\n' : ' ');
        }
      }
  FILE* f = nullptr;
  if (_wfopen_s(&f, detail::utf8ToWide(path).c_str(), L"wb") != 0 || !f) {
    err = "não foi possível criar " + path;
    return false;
  }
  const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  const bool closed = fclose(f) == 0;
  if (!ok || !closed) {
    err = "erro ao gravar " + path;
    return false;
  }
  err.clear();
  return true;
}

// ================================================================ neutral checks
namespace {
// Cheap (no allocation, per frame) and conservative: true only when the
// prepared curve is certainly neutral too (all points on the diagonal / at
// 0.5; clamping and merging such points keeps them there).
bool customCurveNeutral(const std::vector<CurvePoint>& pts) {
  for (const CurvePoint& p : pts)
    if (!(p.y == p.x)) return false;
  return true;
}
bool flatCurveNeutral(const std::vector<CurvePoint>& pts) {
  for (const CurvePoint& p : pts)
    if (!(p.y == 0.5f)) return false;
  return true;
}
}  // namespace

bool isIdentity(const ColorGrade& g) {
  if (!g.enabled) return true;
  const bool neutral = g.exposure_ev == 0 && g.contrast == 0 && g.highlights == 0 &&
                       g.shadows == 0 && g.saturation == 0 && g.vibrance == 0 &&
                       g.temperature == 0 && g.tint == 0 && g.lift.isNeutral() &&
                       g.gamma.isNeutral() && g.gain.isNeutral() && g.offset.isNeutral() &&
                       customCurveNeutral(g.curve_y) && customCurveNeutral(g.curve_r) &&
                       customCurveNeutral(g.curve_g) && customCurveNeutral(g.curve_b) &&
                       flatCurveNeutral(g.hue_vs_hue) && flatCurveNeutral(g.hue_vs_sat) &&
                       flatCurveNeutral(g.luma_vs_sat);
  return neutral && (g.lut.empty() || !(g.lut_strength > 0));
}

// ================================================================ wheels
// The puck direction is the BT.709 vectorscope direction (Cb right, Cr up) of
// the colour push: d = the zero-sum RGB whose opponent chroma points along the
// puck (adding grey does not change chroma, so d = rgb(chroma) - its mean),
// scaled so max |component| = |p|.
void wheelToRgb(float px, float py, float& r, float& g, float& b) {
  r = g = b = 0;
  const float rho = std::sqrt(px * px + py * py);
  if (!(rho > 0) || !std::isfinite(rho)) return;
  float d[3];
  detail::fromChroma(0, px / rho, py / rho, d);
  const float mean = (d[0] + d[1] + d[2]) / 3;
  float mx = 0;
  for (float& v : d) {
    v -= mean;
    mx = std::max(mx, std::fabs(v));
  }
  const float k = rho / mx;
  r = d[0] * k;
  g = d[1] * k;
  b = d[2] * k;
}

void rgbToWheel(float r, float g, float b, float& px, float& py) {
  px = py = 0;
  const float mean = (r + g + b) / 3;  // project onto the zero-sum plane
  const float d[3] = {r - mean, g - mean, b - mean};
  const float mx = std::max(std::fabs(d[0]), std::max(std::fabs(d[1]), std::fabs(d[2])));
  if (!(mx > 0) || !std::isfinite(mx)) return;
  float L, cb, cr;
  detail::toChroma(d, L, cb, cr);
  const float C = std::sqrt(cb * cb + cr * cr);
  if (!(C > 0)) return;
  px = mx * cb / C;
  py = mx * cr / C;
}

// ================================================================ curves (public)
namespace {
// The UI evaluates the same curve at hundreds of x per frame: keep the last
// prepared curve of each kind per thread (rebuilt when the points change).
const detail::PreparedCurve& preparedFor(const std::vector<CurvePoint>& pts, detail::PreparedCurve::Kind kind) {
  struct Slot {
    std::vector<CurvePoint> pts;
    std::unique_ptr<detail::PreparedCurve> curve;
  };
  thread_local Slot slots[3];
  Slot& s = slots[static_cast<int>(kind)];
  if (!s.curve || s.pts != pts) {
    s.curve.reset(new detail::PreparedCurve(pts, kind));
    s.pts = pts;
  }
  return *s.curve;
}
}  // namespace

float evalCurve(const std::vector<CurvePoint>& pts, float x) {
  if (pts.empty()) return detail::clamp01(x);
  return preparedFor(pts, detail::PreparedCurve::Kind::Custom).eval(x);
}

float evalHueCurve(const std::vector<CurvePoint>& pts, float x) {
  if (pts.empty()) return 0.5f;
  return preparedFor(pts, detail::PreparedCurve::Kind::Hue).eval(x);
}

float evalLumaSatCurve(const std::vector<CurvePoint>& pts, float x) {
  if (pts.empty()) return 0.5f;
  return preparedFor(pts, detail::PreparedCurve::Kind::LumaSat).eval(x);
}

// ================================================================ gallery previews
namespace {
// Box filter of one plane (ch interleaved channels) from sw x sh to dw x dh.
// Every destination pixel averages its source rectangle (at least 1 pixel).
void boxPlane(const uint8_t* src, size_t stride, uint32_t sw, uint32_t sh, uint32_t ch, uint8_t* dst,
              uint32_t dw, uint32_t dh) {
  std::vector<uint32_t> x0(dw), x1(dw), acc(static_cast<size_t>(sw) * ch);
  for (uint32_t x = 0; x < dw; ++x) {
    x0[x] = static_cast<uint32_t>(static_cast<uint64_t>(x) * sw / dw);
    x1[x] = std::max(x0[x] + 1, static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * sw / dw));
  }
  for (uint32_t r = 0; r < dh; ++r) {
    const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(r) * sh / dh);
    const uint32_t y1 = std::max(y0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(r + 1) * sh / dh));
    std::fill(acc.begin(), acc.end(), 0u);
    for (uint32_t sy = y0; sy < y1; ++sy) {
      const uint8_t* s = src + static_cast<size_t>(sy) * stride;
      for (size_t i = 0; i < acc.size(); ++i) acc[i] += s[i];
    }
    uint8_t* d = dst + static_cast<size_t>(r) * dw * ch;
    for (uint32_t x = 0; x < dw; ++x) {
      const uint32_t n = (x1[x] - x0[x]) * (y1 - y0);
      for (uint32_t c = 0; c < ch; ++c) {
        uint32_t sum = 0;
        for (uint32_t sx = x0[x]; sx < x1[x]; ++sx) sum += acc[static_cast<size_t>(sx) * ch + c];
        d[x * ch + c] = static_cast<uint8_t>((sum + n / 2) / n);
      }
    }
  }
}
}  // namespace

void downscaleNV12(const uint8_t* y, size_t yStride, const uint8_t* uv, size_t uvStride, uint32_t w,
                   uint32_t h, uint32_t dstW, uint32_t dstH, std::vector<uint8_t>& dst) {
  dstW &= ~1u;
  dstH &= ~1u;
  if (!y || !uv || w < 2 || h < 2 || dstW < 2 || dstH < 2) {
    dst.clear();
    return;
  }
  dst.resize(static_cast<size_t>(dstW) * dstH * 3 / 2);
  boxPlane(y, yStride, w, h, 1, dst.data(), dstW, dstH);
  boxPlane(uv, uvStride, w / 2, h / 2, 2, dst.data() + static_cast<size_t>(dstW) * dstH, dstW / 2, dstH / 2);
}

bool renderGradedRgba(const ColorGrade& g, const uint8_t* nv12, uint32_t w, uint32_t h, uint32_t matrix,
                      bool fullRange, std::vector<uint8_t>& rgba, std::string* err) {
  if (err) err->clear();
  if (!nv12 || w < 2 || h < 2) {
    rgba.clear();
    if (err) *err = "imagem vazia";
    return false;
  }
  rgba.resize(static_cast<size_t>(w) * h * 4);
  bool ok = true;
  std::shared_ptr<const CubeLut> lut;
  if (!g.lut.empty() && g.lut_strength > 0) {
    std::string title, e;
    if (!detail::resolveLut(g.lut, lut, title, e)) {
      ok = false;
      lut.reset();
      if (err) *err = e;
    }
  }
  // The gallery renders the same grade with many LUTs: the LUT-less part is
  // prepared once and reused (keyed by the grade without its LUT).
  static std::mutex baseMx;
  static ColorGrade baseKey;
  static std::shared_ptr<const detail::GradePipeline> baseCached;
  ColorGrade key = g;
  key.enabled = true;
  key.compare_split = false;
  key.lut.clear();
  key.lut_strength = 0;
  std::shared_ptr<const detail::GradePipeline> base;
  {
    std::lock_guard<std::mutex> lk(baseMx);
    if (baseCached && baseKey == key) base = baseCached;
  }
  if (!base) {
    auto b = std::make_shared<detail::GradePipeline>();
    b->prepare(key, nullptr, 1024);
    base = b;
    std::lock_guard<std::mutex> lk(baseMx);
    baseKey = key;
    baseCached = base;
  }
  detail::GradePipeline p = *base;
  if (lut) p.setLut(lut, g.lut_strength);
  const bool noop = p.noop();
  if (matrix != 1 && matrix != 2) matrix = h >= 720 ? 2 : 1;
  const float kr = matrix == 1 ? 0.299f : 0.2126f;
  const float kb = matrix == 1 ? 0.114f : 0.0722f;
  const float kg = 1 - kr - kb;
  const float yOff = fullRange ? 0.0f : 16.0f;
  const float yScale = fullRange ? 255.0f : 219.0f;
  const float cScale = fullRange ? 255.0f : 224.0f;
  const float rCr = 2 * (1 - kr), bCb = 2 * (1 - kb);
  // same YUV -> RGB as the table bake, so thumbnails match the live grade
  float yLut[256], cLut[256];
  for (int i = 0; i < 256; ++i) {
    yLut[i] = (static_cast<float>(i) - yOff) / yScale;
    cLut[i] = (static_cast<float>(i) - 128) / cScale;
  }
  const size_t uvStride = (static_cast<size_t>(w) + 1) & ~static_cast<size_t>(1);
  const uint8_t* uvPlane = nv12 + static_cast<size_t>(w) * h;
  uint8_t* o = rgba.data();
  for (uint32_t r = 0; r < h; ++r) {
    const uint8_t* yr = nv12 + static_cast<size_t>(r) * w;
    const uint8_t* cr = uvPlane + static_cast<size_t>(r / 2) * uvStride;
    for (uint32_t x = 0; x < w; ++x, o += 4) {
      const float yn = yLut[yr[x]], cb = cLut[cr[x & ~1u]], crv = cLut[cr[(x & ~1u) + 1]];
      float c[3];
      c[0] = yn + rCr * crv;
      c[2] = yn + bCb * cb;
      c[1] = (yn - kr * c[0] - kb * c[2]) / kg;
      if (!noop) p.run(c);
      for (int k = 0; k < 3; ++k) o[k] = static_cast<uint8_t>(detail::clamp01(c[k]) * 255.0f + 0.5f);
      o[3] = 255;
    }
  }
  return ok;
}

}  // namespace owc
