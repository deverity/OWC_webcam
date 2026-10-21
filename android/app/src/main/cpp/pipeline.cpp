#include "pipeline.h"

#include <algorithm>
#include <cstdlib>

#include <android/native_window.h>
#include <time.h>

#include <cstring>

#include "util/clock.h"
#include "util/debug_props.h"
#include "util/log.h"
#include "util/thread.h"

namespace owc {

Pipeline::Pipeline(ACameraManager* mgr, const EncoderCapsSet* enc, HighSpeedBackend* hs, LostFn onLost)
    : enc_(enc), onLost_(std::move(onLost)), camera_(mgr, this), hs_(hs) {}

Pipeline::~Pipeline() { stop(); }

void Pipeline::onCameraLost(uint64_t gen, int, const std::string& what) {
  if (onLost_) onLost_(gen, what);
}

bool Pipeline::start(ParamStore& ps, int bitrate, ANativeWindow* preview,
                     VideoEncoder::Sink* sink, bool& sizeChanged, std::string& err) {
  stop();
  sizeChanged = false;
  const CameraDesc& cd = ps.camera();
  const EncoderCaps& caps = enc_->get(ps.stream.codec);
  if (!caps.available) {
    err = "selected codec has no usable encoder";
    return false;
  }
  const bool hs = ps.stream.highSpeed && hs_ && cd.hsCameraFps(ps.stream.width, ps.stream.height) > 0;
  if (ps.stream.highSpeed && !hs) {
    err = "high-speed mode not available for this camera/size";
    return false;
  }

  // Candidate sizes: requested first, then smaller sizes of the camera (same
  // aspect ratio first), scrcpy-style downsizing.
  std::vector<VideoSize> sizes;
  const int rw = ps.stream.width, rh = ps.stream.height;
  sizes.push_back({static_cast<uint16_t>(rw), static_cast<uint16_t>(rh), 0});
  std::vector<VideoSize> smaller;
  for (const auto& s : cd.info.sizes)
    if (static_cast<int64_t>(s.width) * s.height < static_cast<int64_t>(rw) * rh) smaller.push_back(s);
  std::stable_sort(smaller.begin(), smaller.end(), [&](const VideoSize& a, const VideoSize& b) {
    bool sa = static_cast<int64_t>(a.width) * rh == static_cast<int64_t>(a.height) * rw;
    bool sb = static_cast<int64_t>(b.width) * rh == static_cast<int64_t>(b.height) * rw;
    return sa != sb ? sa : false;  // keep descending area inside each group
  });
  // High-speed sessions only accept their exact sizes: no downsizing there.
  for (size_t i = 0; !hs && i < smaller.size() && sizes.size() < 5; ++i) sizes.push_back(smaller[i]);

  VideoEncoder::Params p;
  p.rotationDeg = cd.info.sensor_orientation;
  p.mirror = cd.info.facing == Facing::Front;
  p.epochCounter = &epochCounter_;
  bool ok = false;
  std::string lastErr;
  for (size_t si = 0; si < sizes.size() && !ok; ++si) {
    ps.setActualSize(sizes[si].width, sizes[si].height);
    p.settings = ps.encoderSettings(bitrate);
    p.settings.operatingRateMult = static_cast<int>(debugProp("oprate_k", p.settings.operatingRateMult));
    p.settings.vendorLowLatency = debugProp("qti_ll", 1) != 0;
    static const int kAllStages[] = {0, 1, 2, 3};
    static const int kDownsizeStages[] = {1, 3};
    const int* stages = si == 0 ? kAllStages : kDownsizeStages;
    const int nStages = si == 0 ? 4 : 2;
    for (int k = 0; k < nStages && !ok; ++k) {
      p.epoch = epochCounter_.fetch_add(1) + 1;
      std::string e;
      if (!encoder_.configure(p, caps, enc_->apiLevel, stages[k], e) || !encoder_.start(sink, e)) {
        lastErr = e;
        continue;
      }
      ok = true;
      if (si > 0 || stages[k] > 0)
        LOGW("encoder %s: configured with fallback (stage %d%s %dx%d) after: %s",
             encoder_.name().c_str(), stages[k], si > 0 ? ", downsized to" : ",",
             sizes[si].width, sizes[si].height, lastErr.c_str());
    }
  }
  if (!ok) {
    ps.setActualSize(rw, rh);
    err = "encoder configure failed: " + lastErr;
    return false;
  }
  sizeChanged = ps.stream.width != rw || ps.stream.height != rh;

  std::string e;
  if (hs) {
    const int camFps = ps.cameraFps();
    const auto range = cd.hsRange(ps.stream.width, ps.stream.height);
    if (preview) LOGW("local preview is not available in high-speed mode");
    if (!hs_->open(cd, encoder_.inputSurface(), ps.stream.width, ps.stream.height, range.first,
                   range.second, ps.controls, static_cast<int>(debugProp("hs_usage", 2)), e)) {
      encoder_.stop();
      err = "high-speed session failed: " + e;
      return false;
    }
    hsActive_ = true;
    running_ = true;
    LOGI_PC("streaming camera %s HIGH-SPEED %dx%d sensor %d fps -> encoder %d fps, %s %s %d kbps (epoch %u)",
            cd.info.id.c_str(), ps.stream.width, ps.stream.height, camFps, ps.stream.fps,
            ps.stream.codec == Codec::Hevc ? "HEVC" : "H.264", encoder_.name().c_str(), bitrate / 1000,
            encoder_.epoch());
    return true;
  }
  if (!camera_.open(cd, encoder_.inputSurface(), preview, ps.controls, ps.stream.fps, e)) {
    if (!syntheticFallback_) {
      encoder_.stop();
      err = e;
      return false;
    }
    LOGW("camera unavailable (%s): TEST synthetic source", e.c_str());
    startSynthetic(ps.stream.width, ps.stream.height, ps.stream.fps);
  }
  running_ = true;
  LOGI_PC("streaming camera %s %dx%d@%d %s %s %d kbps (epoch %u)", cd.info.id.c_str(),
          ps.stream.width, ps.stream.height, ps.stream.fps,
          ps.stream.codec == Codec::Hevc ? "HEVC" : "H.264", encoder_.name().c_str(), bitrate / 1000,
          encoder_.epoch());
  return true;
}

void Pipeline::stop() {
  // Camera first: it is the producer of the encoder's input surface.
  camera_.close();
  if (hsActive_) {
    hs_->close();
    hsActive_ = false;
  }
  stopSynthetic();
  encoder_.stop();
  running_ = false;
}

void Pipeline::applyControls(const ParamStore& ps) {
  if (!running_) return;
  const bool ok = hsActive_ ? hs_->apply(ps.camera(), ps.controls) : camera_.apply(ps.controls, ps.stream.fps);
  if (!ok) LOGW("failed to apply camera controls");
}

void Pipeline::focusAt(float x, float y, const ParamStore& ps) {
  if (!running_) return;
  if (!hsActive_) {
    camera_.focusAt(x, y, ps.controls, ps.stream.fps);
    return;
  }
  if (ps.controls.afMode == 0) return;
  int32_t region[4];
  focusRegion(ps.camera(), ps.controls.zoom, x, y, region);
  hs_->focusAt(region);
}

void Pipeline::setBitrate(int bps) {
  if (running_) encoder_.setBitrate(bps);
}

void Pipeline::requestKeyframe(bool force) {
  if (running_) encoder_.requestKeyframe(force);
}

bool Pipeline::setPreview(ANativeWindow* w, const ParamStore& ps) {
  if (!running_) return true;
  if (hsActive_) {
    if (w) LOGW("local preview is not available in high-speed mode");
    return true;
  }
  std::string err;
  if (!camera_.setPreview(w, ps.controls, ps.stream.fps, err)) {
    LOGE("preview change failed: %s", err.c_str());
    return false;
  }
  return true;
}

}  // namespace owc

namespace owc {

void Pipeline::startSynthetic(int w, int h, int fps) {
  ANativeWindow* win = encoder_.inputSurface();
  ANativeWindow_acquire(win);
  synthStop_ = false;
  synthThread_ = std::thread([this, win, w, h, fps] {
    setThreadName("owc-synth");
    ANativeWindow_setBuffersGeometry(win, w, h, WINDOW_FORMAT_RGBA_8888);
    const int64_t periodUs = 1000000 / std::max(1, fps);
    int64_t next = monoUs();
    uint32_t n = 0, rng = 12345;
    while (!synthStop_) {
      ANativeWindow_Buffer b;
      if (ANativeWindow_lock(win, &b, nullptr) == 0) {
        auto* px = static_cast<uint32_t*>(b.bits);
        const int bar = static_cast<int>((n * 8) % static_cast<uint32_t>(b.width));
        for (int y = 0; y < b.height; ++y) {
          uint32_t* row = px + static_cast<size_t>(y) * b.stride;
          const uint32_t c = 0xFF000000u | (((y + n) & 0xFF) << 8) | ((y * 3) & 0xFF);
          for (int x = 0; x < b.width; ++x) row[x] = c;
          for (int x = bar; x < std::min(b.width, bar + 64); ++x) row[x] = 0xFFFFFFFFu;
          for (int k = 0; k < 8; ++k) {  // some noise so the bitrate is not trivial
            rng = rng * 1664525u + 1013904223u;
            row[(rng >> 8) % static_cast<uint32_t>(b.width)] = rng | 0xFF000000u;
          }
        }
        ANativeWindow_unlockAndPost(win);
        synthFrames_.fetch_add(1);
        ++n;
      }
      next += periodUs;
      int64_t wait = next - monoUs();
      if (wait > 0) {
        timespec ts{static_cast<time_t>(wait / 1000000), static_cast<long>((wait % 1000000) * 1000)};
        nanosleep(&ts, nullptr);
      } else {
        next = monoUs();
      }
    }
    ANativeWindow_release(win);
  });
}

void Pipeline::stopSynthetic() {
  synthStop_ = true;
  if (synthThread_.joinable()) synthThread_.join();
}

}  // namespace owc
