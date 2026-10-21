// Background work of the colour page ("Cor / LUT"): video scopes, LUT gallery
// thumbnails and the graded test chart shown when there is no video.
// One thread ("owc-color"), started on first use. Every public method is
// non-blocking (short lock only) and meant for the UI thread.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <owc/client.h>
#include <owc/scopes.h>

#include "app.h"

namespace owcapp {

// Tightly packed NV12 image (Y plane then interleaved UV plane).
struct Nv12Image {
  std::vector<uint8_t> data;
  uint32_t w = 0, h = 0;
  uint32_t matrix = 0;     // OWC_COLOR_* (0 unknown, 1 BT.601, 2 BT.709)
  bool full_range = false;
};

struct ThumbResult {
  std::string key;
  uint64_t src_gen = 0;
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
  std::string err;
};

class ColorWorker {
 public:
  // Thumbnail / snapshot size (16:9, centre-cropped from the source).
  static constexpr uint32_t kThumbW = 320, kThumbH = 180;
  // Test chart size.
  static constexpr uint32_t kChartW = 512, kChartH = 288;

  ColorWorker() = default;
  ~ColorWorker();
  ColorWorker(const ColorWorker&) = delete;
  ColorWorker& operator=(const ColorWorker&) = delete;
  void stop();

  // ---- scopes from live (already graded) preview frames.
  // When the worker has no frame queued, swaps `f` with its input slot (no copy;
  // `f` gets back a spare buffer) and returns true. `snapshot` = this frame is
  // known to be ungraded: also keep a small centre-cropped copy as the gallery's
  // thumbnail source.
  bool submitFrame(PreviewFrame& f, const owc::ScopeOptions& opt, bool snapshot);
  bool frameQueued();

  // ---- test chart (no video): graded with `g` (honours enabled and
  // compare_split); its scopes are computed too. Latest request wins.
  void submitChart(const owc::ColorGrade& g, const owc::ScopeOptions& opt);

  // ---- results (swap out; false if nothing new)
  bool takeScopes(owc::ScopeImages& out);
  bool takeChart(std::vector<uint8_t>& gradedRgba);
  // Original (ungraded) test chart, RGBA8 kChartW x kChartH.
  static const std::vector<uint8_t>& chartRgba();
  // Test chart scaled to the thumbnail size (gallery source without video).
  static std::shared_ptr<const Nv12Image> chartThumbSource();

  // Last ungraded video snapshot (null if none yet) and its generation (0 = none).
  std::shared_ptr<const Nv12Image> snapshot(uint64_t* gen);

  // ---- thumbnails: rendered in FIFO order with owc::renderGradedRgba.
  void requestThumb(const std::string& key, const owc::ColorGrade& g, std::shared_ptr<const Nv12Image> src,
                    uint64_t srcGen);
  bool takeThumb(ThumbResult& out);
  size_t thumbsQueued();

 private:
  struct ThumbJob {
    std::string key;
    owc::ColorGrade grade;
    std::shared_ptr<const Nv12Image> src;
    uint64_t src_gen = 0;
  };
  void ensureThread();
  void run();
  void doFrame(owc::ScopeOptions opt, bool snap);
  void doChart(const owc::ColorGrade& g, const owc::ScopeOptions& opt);

  std::mutex mu_;
  std::condition_variable cv_;
  std::thread th_;
  bool stop_ = false;

  // video frame
  bool has_frame_ = false;
  PreviewFrame in_frame_, work_frame_;
  owc::ScopeOptions frame_opt_;
  bool frame_snapshot_ = false;

  // chart
  bool has_chart_ = false;
  owc::ColorGrade chart_grade_;
  owc::ScopeOptions chart_opt_;
  bool chart_new_ = false;
  std::vector<uint8_t> chart_out_;

  // scopes out (three buffers cycle: work_img_ -> scopes_out_ -> UI; no reallocation)
  bool scopes_new_ = false;
  owc::ScopeImages scopes_out_;
  owc::ScopeImages work_img_;  // worker thread only

  // snapshot
  std::shared_ptr<const Nv12Image> snap_;
  uint64_t snap_gen_ = 0;

  // thumbnails
  std::deque<ThumbJob> thumb_q_;
  std::deque<ThumbResult> thumb_out_;
};

}  // namespace owcapp
