#include "color_worker.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <owc/color_grade.h>

namespace owcapp {

namespace {

inline uint8_t u8(float v) {
  const float c = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
  return static_cast<uint8_t>(c + 0.5f);
}

// RGBA8 (gamma-encoded video RGB) -> NV12, BT.709 limited range.
void rgbaToNv12(const uint8_t* rgba, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
  out.resize(static_cast<size_t>(w) * h * 3 / 2);
  uint8_t* yp = out.data();
  uint8_t* uvp = out.data() + static_cast<size_t>(w) * h;
  const float kr = 0.2126f, kb = 0.0722f, kg = 1.0f - kr - kb;
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* s = rgba + static_cast<size_t>(y) * w * 4;
    for (uint32_t x = 0; x < w; ++x, s += 4) {
      const float l = (kr * s[0] + kg * s[1] + kb * s[2]) / 255.0f;
      yp[static_cast<size_t>(y) * w + x] = u8(16.0f + 219.0f * l);
    }
  }
  for (uint32_t y = 0; y < h; y += 2) {
    for (uint32_t x = 0; x < w; x += 2) {
      float r = 0, g = 0, b = 0;
      for (uint32_t dy = 0; dy < 2; ++dy)
        for (uint32_t dx = 0; dx < 2; ++dx) {
          const uint8_t* s = rgba + (static_cast<size_t>(y + dy) * w + x + dx) * 4;
          r += s[0];
          g += s[1];
          b += s[2];
        }
      r /= 4.0f * 255.0f;
      g /= 4.0f * 255.0f;
      b /= 4.0f * 255.0f;
      const float l = kr * r + kg * g + kb * b;
      const float cb = (b - l) / (2.0f * (1.0f - kb));
      const float cr = (r - l) / (2.0f * (1.0f - kr));
      uint8_t* d = uvp + static_cast<size_t>(y / 2) * w + x;
      d[0] = u8(128.0f + 224.0f * cb);
      d[1] = u8(128.0f + 224.0f * cr);
    }
  }
}

void hsv(float h, float s, float v, float& r, float& g, float& b) {
  h = h - std::floor(h);
  const float k[3] = {5.0f, 3.0f, 1.0f};
  float* o[3] = {&r, &g, &b};
  for (int i = 0; i < 3; ++i) {
    const float t = std::fmod(k[i] + h * 6.0f, 6.0f);
    *o[i] = v - v * s * std::max(0.0f, std::min(std::min(t, 4.0f - t), 1.0f));
  }
}

// Built-in neutral test image: 75% colour bars, skin tones, a hue sweep and a grey ramp.
std::vector<uint8_t> makeChart(uint32_t W, uint32_t H) {
  std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
  const float bars[7][3] = {{.75f, .75f, .75f}, {.75f, .75f, 0}, {0, .75f, .75f}, {0, .75f, 0},
                            {.75f, 0, .75f},    {.75f, 0, 0},    {0, 0, .75f}};
  const float skin[6][3] = {{0.98f, 0.84f, 0.74f}, {0.91f, 0.72f, 0.60f}, {0.82f, 0.60f, 0.47f},
                            {0.69f, 0.48f, 0.35f}, {0.53f, 0.35f, 0.24f}, {0.36f, 0.23f, 0.16f}};
  const uint32_t y1 = H * 80 / 288, y2 = H * 176 / 288, y3 = H * 232 / 288;
  for (uint32_t y = 0; y < H; ++y) {
    for (uint32_t x = 0; x < W; ++x) {
      float r = 0, g = 0, b = 0;
      const float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(W);
      if (y < y1) {
        const int i = std::min(6, static_cast<int>(fx * 7.0f));
        r = bars[i][0]; g = bars[i][1]; b = bars[i][2];
      } else if (y < y2) {
        const int i = std::min(5, static_cast<int>(fx * 6.0f));
        const float t = static_cast<float>(y - y1) / static_cast<float>(y2 - y1);
        const float u = fx * 6.0f - static_cast<float>(i);  // 0..1 across the patch
        const float shade = 1.10f - 0.22f * t - 0.06f * std::fabs(u - 0.5f);  // soft lighting
        r = skin[i][0] * shade; g = skin[i][1] * shade; b = skin[i][2] * shade;
      } else if (y < y3) {
        const float t = static_cast<float>(y - y2) / static_cast<float>(y3 - y2);
        hsv(fx, 0.85f - 0.55f * t, 0.85f, r, g, b);
      } else {
        r = g = b = fx;
      }
      uint8_t* d = &px[(static_cast<size_t>(y) * W + x) * 4];
      d[0] = u8(r * 255.0f);
      d[1] = u8(g * 255.0f);
      d[2] = u8(b * 255.0f);
      d[3] = 255;
    }
  }
  return px;
}

struct Chart {
  std::vector<uint8_t> rgba;
  Nv12Image nv12;
  std::shared_ptr<const Nv12Image> thumb;
};

const Chart& chart() {
  static const Chart c = [] {
    Chart k;
    k.rgba = makeChart(ColorWorker::kChartW, ColorWorker::kChartH);
    k.nv12.w = ColorWorker::kChartW;
    k.nv12.h = ColorWorker::kChartH;
    k.nv12.matrix = 2;  // BT.709
    k.nv12.full_range = false;
    rgbaToNv12(k.rgba.data(), k.nv12.w, k.nv12.h, k.nv12.data);
    auto t = std::make_shared<Nv12Image>();
    t->w = ColorWorker::kThumbW;
    t->h = ColorWorker::kThumbH;
    t->matrix = 2;
    t->full_range = false;
    const uint8_t* y = k.nv12.data.data();
    const uint8_t* uv = y + static_cast<size_t>(k.nv12.w) * k.nv12.h;
    owc::downscaleNV12(y, k.nv12.w, uv, k.nv12.w, k.nv12.w, k.nv12.h, t->w, t->h, t->data);
    k.thumb = std::move(t);
    return k;
  }();
  return c;
}

// Centre crop of an NV12 frame to 16:9, box-downscaled to the thumbnail size.
std::shared_ptr<Nv12Image> makeSnapshot(const PreviewFrame& f) {
  if (f.w < 16 || f.h < 16 || f.y.size() < static_cast<size_t>(f.w) * f.h ||
      f.uv.size() < static_cast<size_t>(f.w) * f.h / 2)
    return nullptr;
  uint32_t cw = f.w, ch = f.h;
  if (static_cast<uint64_t>(cw) * 9 > static_cast<uint64_t>(ch) * 16) cw = ch * 16 / 9;
  else ch = cw * 9 / 16;
  cw &= ~1u;
  ch &= ~1u;
  if (cw < 2 || ch < 2) return nullptr;
  const uint32_t x0 = ((f.w - cw) / 2) & ~1u, y0 = ((f.h - ch) / 2) & ~1u;
  auto s = std::make_shared<Nv12Image>();
  s->w = ColorWorker::kThumbW;
  s->h = ColorWorker::kThumbH;
  s->matrix = f.color_matrix != 0 ? f.color_matrix : (f.h >= 720 ? 2u : 1u);
  s->full_range = f.full_range;
  const uint8_t* y = f.y.data() + static_cast<size_t>(y0) * f.w + x0;
  const uint8_t* uv = f.uv.data() + static_cast<size_t>(y0 / 2) * f.w + x0;
  owc::downscaleNV12(y, f.w, uv, f.w, cw, ch, s->w, s->h, s->data);
  if (s->data.size() < static_cast<size_t>(s->w) * s->h * 3 / 2) return nullptr;
  return s;
}

}  // namespace

const std::vector<uint8_t>& ColorWorker::chartRgba() { return chart().rgba; }
std::shared_ptr<const Nv12Image> ColorWorker::chartThumbSource() { return chart().thumb; }

ColorWorker::~ColorWorker() { stop(); }

void ColorWorker::stop() {
  {
    std::lock_guard<std::mutex> l(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

void ColorWorker::ensureThread() {
  if (th_.joinable() || stop_) return;
  th_ = std::thread([this] { run(); });
}

bool ColorWorker::submitFrame(PreviewFrame& f, const owc::ScopeOptions& opt, bool snapshot) {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (has_frame_ || stop_) return false;
    std::swap(in_frame_, f);
    frame_opt_ = opt;
    frame_snapshot_ = snapshot;
    has_frame_ = true;
    ensureThread();
  }
  cv_.notify_one();
  return true;
}

bool ColorWorker::frameQueued() {
  std::lock_guard<std::mutex> l(mu_);
  return has_frame_;
}

void ColorWorker::submitChart(const owc::ColorGrade& g, const owc::ScopeOptions& opt) {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (stop_) return;
    chart_grade_ = g;
    chart_opt_ = opt;
    has_chart_ = true;
    ensureThread();
  }
  cv_.notify_one();
}

bool ColorWorker::takeScopes(owc::ScopeImages& out) {
  std::lock_guard<std::mutex> l(mu_);
  if (!scopes_new_) return false;
  std::swap(out, scopes_out_);
  scopes_new_ = false;
  return true;
}

bool ColorWorker::takeChart(std::vector<uint8_t>& gradedRgba) {
  std::lock_guard<std::mutex> l(mu_);
  if (!chart_new_) return false;
  std::swap(gradedRgba, chart_out_);
  chart_new_ = false;
  return true;
}

std::shared_ptr<const Nv12Image> ColorWorker::snapshot(uint64_t* gen) {
  std::lock_guard<std::mutex> l(mu_);
  if (gen) *gen = snap_gen_;
  return snap_;
}

void ColorWorker::requestThumb(const std::string& key, const owc::ColorGrade& g, std::shared_ptr<const Nv12Image> src,
                               uint64_t srcGen) {
  if (!src) return;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (stop_) return;
    for (auto& j : thumb_q_)
      if (j.key == key) {  // replace a queued request for the same entry
        j.grade = g;
        j.src = std::move(src);
        j.src_gen = srcGen;
        return;
      }
    thumb_q_.push_back(ThumbJob{key, g, std::move(src), srcGen});
    ensureThread();
  }
  cv_.notify_one();
}

bool ColorWorker::takeThumb(ThumbResult& out) {
  std::lock_guard<std::mutex> l(mu_);
  if (thumb_out_.empty()) return false;
  out = std::move(thumb_out_.front());
  thumb_out_.pop_front();
  return true;
}

size_t ColorWorker::thumbsQueued() {
  std::lock_guard<std::mutex> l(mu_);
  return thumb_q_.size();
}

void ColorWorker::run() {
  SetThreadDescription(GetCurrentThread(), L"owc-color");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  std::unique_lock<std::mutex> l(mu_);
  for (;;) {
    cv_.wait(l, [&] { return stop_ || has_frame_ || has_chart_ || !thumb_q_.empty(); });
    if (stop_) return;
    if (has_frame_) {
      // work_frame_ is only touched by this thread; the UI only swaps in_frame_ under the lock
      std::swap(in_frame_, work_frame_);
      has_frame_ = false;
      const owc::ScopeOptions opt = frame_opt_;
      const bool snap = frame_snapshot_;
      l.unlock();
      doFrame(opt, snap);
      l.lock();
      continue;
    }
    if (has_chart_) {
      const owc::ColorGrade g = chart_grade_;
      const owc::ScopeOptions opt = chart_opt_;
      has_chart_ = false;
      l.unlock();
      doChart(g, opt);
      l.lock();
      continue;
    }
    ThumbJob job = std::move(thumb_q_.front());
    thumb_q_.pop_front();
    l.unlock();
    ThumbResult r;
    r.key = job.key;
    r.src_gen = job.src_gen;
    r.w = job.src->w;
    r.h = job.src->h;
    owc::renderGradedRgba(job.grade, job.src->data.data(), job.src->w, job.src->h, job.src->matrix,
                          job.src->full_range, r.rgba, &r.err);
    if (r.rgba.size() != static_cast<size_t>(r.w) * r.h * 4) r.rgba.assign(static_cast<size_t>(r.w) * r.h * 4, 0);
    l.lock();
    thumb_out_.push_back(std::move(r));
    while (thumb_out_.size() > 64) thumb_out_.pop_front();
  }
}

void ColorWorker::doFrame(owc::ScopeOptions opt, bool snap) {
  const PreviewFrame& f = work_frame_;
  if (f.w < 2 || f.h < 2) return;
  opt.matrix = f.color_matrix;
  opt.full_range = f.full_range;
  owc::computeScopes(f.y.data(), f.w, f.uv.data(), f.w, f.w, f.h, opt, work_img_);
  std::shared_ptr<Nv12Image> s = snap ? makeSnapshot(f) : nullptr;
  std::lock_guard<std::mutex> l(mu_);
  std::swap(scopes_out_, work_img_);
  scopes_new_ = true;
  if (s) {
    snap_ = std::move(s);
    ++snap_gen_;
  }
}

void ColorWorker::doChart(const owc::ColorGrade& g, const owc::ScopeOptions& optIn) {
  const Chart& c = chart();
  const uint32_t W = c.nv12.w, H = c.nv12.h;
  std::vector<uint8_t> graded;
  if (g.enabled) {
    owc::renderGradedRgba(g, c.nv12.data.data(), W, H, c.nv12.matrix, c.nv12.full_range, graded);
    if (graded.size() != c.rgba.size()) graded = c.rgba;
  } else {
    graded = c.rgba;
  }
  // The scopes see what the output would carry: with compare_split the left half stays original.
  std::vector<uint8_t> scopeSrc;
  const std::vector<uint8_t>* src = &graded;
  if (g.enabled && g.compare_split) {
    scopeSrc = graded;
    const size_t half = static_cast<size_t>(W / 2) * 4;
    for (uint32_t y = 0; y < H; ++y)
      std::memcpy(&scopeSrc[static_cast<size_t>(y) * W * 4], &c.rgba[static_cast<size_t>(y) * W * 4], half);
    src = &scopeSrc;
  }
  std::vector<uint8_t> nv12;
  rgbaToNv12(src->data(), W, H, nv12);
  owc::ScopeOptions opt = optIn;
  opt.matrix = 2;
  opt.full_range = false;
  opt.step = 1;
  owc::computeScopes(nv12.data(), W, nv12.data() + static_cast<size_t>(W) * H, W, W, H, opt, work_img_);
  std::lock_guard<std::mutex> l(mu_);
  std::swap(chart_out_, graded);
  chart_new_ = true;
  std::swap(scopes_out_, work_img_);
  scopes_new_ = true;
}

}  // namespace owcapp
