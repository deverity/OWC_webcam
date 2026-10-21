#include "encoder/video_encoder.h"

#include <android/native_window.h>
#include <media/NdkMediaFormat.h>

#include <cstring>

#include "util/clock.h"
#include "util/log.h"
#include "util/thread.h"

namespace owc {
namespace {

constexpr int64_t kDequeueTimeoutUs = 20000;   // bounded stop latency; returns as soon as data is ready
constexpr int64_t kKeyframeMinGapMs = 150;     // ignore non-forced IDR requests right after an IDR
constexpr int64_t kSyncPendingMs = 500;        // don't re-request while a request is in flight
constexpr uint32_t kFlagKeyFrame = 1;          // BUFFER_FLAG_KEY_FRAME (SYNC_FRAME)
constexpr uint32_t kFlagCodecConfig = 2;
constexpr uint32_t kFlagEos = 4;

// Iterates Annex-B NAL units: cb(nal_ptr, nal_len) -> false stops.
template <class F>
void forEachNal(const uint8_t* p, size_t n, F&& cb) {
  size_t i = 0;
  auto findStart = [&](size_t from, size_t& scLen) -> size_t {
    for (size_t k = from; k + 3 <= n; ++k) {
      if (p[k] == 0 && p[k + 1] == 0) {
        if (p[k + 2] == 1) { scLen = 3; return k; }
        if (k + 4 <= n && p[k + 2] == 0 && p[k + 3] == 1) { scLen = 4; return k; }
      }
    }
    scLen = 0;
    return n;
  };
  size_t sc = 0;
  i = findStart(0, sc);
  while (i < n) {
    size_t begin = i + sc;
    size_t sc2 = 0;
    size_t next = findStart(begin, sc2);
    if (begin < next && !cb(p + begin, next - begin)) return;
    i = next;
    sc = sc2;
  }
}

bool isParamSet(Codec c, uint8_t hdr) {
  if (c == Codec::Hevc) {
    int t = (hdr >> 1) & 0x3F;
    return t == 32 || t == 33 || t == 34;
  }
  int t = hdr & 0x1F;
  return t == 7 || t == 8;
}

bool isVcl(Codec c, uint8_t hdr) {
  if (c == Codec::Hevc) return ((hdr >> 1) & 0x3F) < 32;
  int t = hdr & 0x1F;
  return t >= 1 && t <= 5;
}

// Byte length of the leading parameter-set NALs (up to the first VCL NAL).
size_t paramSetPrefix(Codec c, const uint8_t* p, size_t n, bool& found) {
  found = false;
  size_t end = 0;
  forEachNal(p, n, [&](const uint8_t* nal, size_t) {
    if (isVcl(c, nal[0])) {
      // The start code of this NAL begins up to 4 bytes before it.
      size_t off = static_cast<size_t>(nal - p);
      end = off >= 4 && p[off - 4] == 0 ? off - 4 : off - 3;
      return false;
    }
    if (isParamSet(c, nal[0])) found = true;
    return true;
  });
  return found ? end : 0;
}

}  // namespace

VideoEncoder::~VideoEncoder() { stop(); }

void VideoEncoder::release() {
  if (codec_) {
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
  }
  if (surface_) {
    ANativeWindow_release(surface_);
    surface_ = nullptr;
  }
}

bool VideoEncoder::configure(const Params& p, const EncoderCaps& caps, int apiLevel, int stage,
                             std::string& err) {
  stop();
  params_ = p;
  curEpoch_ = p.epoch;
  const char* mime = p.settings.codec == Codec::Hevc ? "video/hevc" : "video/avc";
  if (!caps.name.empty()) codec_ = AMediaCodec_createCodecByName(caps.name.c_str());
  if (!codec_) codec_ = AMediaCodec_createEncoderByType(mime);
  if (!codec_) {
    err = std::string("no encoder for ") + mime;
    return false;
  }
  name_ = codecName(codec_);
  if (name_.empty()) name_ = caps.name;

  std::vector<AppliedKey> applied;
  AMediaFormat* f = buildEncoderFormat(p.settings, caps, apiLevel, stage, &applied);
  media_status_t st = AMediaCodec_configure(codec_, f, nullptr, nullptr,
                                            AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
  AMediaFormat_delete(f);
  std::string keys;
  for (const auto& k : applied) keys += k.key + "=" + k.value + " ";
  if (st != AMEDIA_OK) {
    err = "configure failed (" + std::to_string(st) + ") stage " + std::to_string(stage) +
          " " + std::to_string(p.settings.width) + "x" + std::to_string(p.settings.height);
    LOGI("encoder %s: %s keys: %s", name_.c_str(), err.c_str(), keys.c_str());
    release();
    return false;
  }
  st = AMediaCodec_createInputSurface(codec_, &surface_);
  if (st != AMEDIA_OK || !surface_) {
    err = "createInputSurface failed (" + std::to_string(st) + ")";
    surface_ = nullptr;
    release();
    return false;
  }
  LOGI("encoder %s configured (stage %d): %s", name_.c_str(), stage, keys.c_str());
  return true;
}

bool VideoEncoder::start(Sink* sink, std::string& err) {
  if (!codec_ || !surface_) {
    err = "encoder not configured";
    return false;
  }
  media_status_t st = AMediaCodec_start(codec_);
  if (st != AMEDIA_OK) {
    err = "AMediaCodec_start failed (" + std::to_string(st) + ")";
    release();
    return false;
  }
  sink_ = sink;
  csd_.clear();
  formatSent_ = false;
  lastPts_ = -1;
  framesOut_ = 0;
  bytesOut_ = 0;
  lastKeyMs_ = 0;
  syncRequestedMs_ = 0;
  bframesSeen_ = false;
  stopFlag_ = false;
  {
    std::lock_guard<std::mutex> lk(ctlMu_);
    started_ = true;
  }
  running_ = true;
  thread_ = std::thread(&VideoEncoder::outputLoop, this);
  return true;
}

void VideoEncoder::stop() {
  stopFlag_ = true;
  if (thread_.joinable()) thread_.join();
  {
    std::lock_guard<std::mutex> lk(ctlMu_);
    started_ = false;
    release();
  }
  running_ = false;
  sink_ = nullptr;
}

void VideoEncoder::setBitrate(int bps) {
  std::lock_guard<std::mutex> lk(ctlMu_);
  if (!started_ || !codec_) return;
  AMediaFormat* f = AMediaFormat_new();
  AMediaFormat_setInt32(f, "video-bitrate", bps);
  media_status_t st = AMediaCodec_setParameters(codec_, f);
  AMediaFormat_delete(f);
  if (st != AMEDIA_OK) LOGW_LOCAL("setParameters(video-bitrate=%d) failed: %d", bps, st);
}

void VideoEncoder::requestKeyframe(bool force) {
  const int64_t now = monoMs();
  const int64_t req = syncRequestedMs_.load();
  const int64_t key = lastKeyMs_.load();
  if (req > key && now - req < kSyncPendingMs) return;           // already pending
  if (!force && key > 0 && now - key < kKeyframeMinGapMs) return;  // IDR just left
  std::lock_guard<std::mutex> lk(ctlMu_);
  if (!started_ || !codec_) return;
  syncRequestedMs_ = now;
  AMediaFormat* f = AMediaFormat_new();
  AMediaFormat_setInt32(f, "request-sync", 0);
  media_status_t st = AMediaCodec_setParameters(codec_, f);
  AMediaFormat_delete(f);
  if (st != AMEDIA_OK) LOGW_LOCAL("setParameters(request-sync) failed: %d", st);
}

int64_t VideoEncoder::toMonotonic(int64_t ptsUs) {
  // presentationTimeUs = camera buffer timestamp / 1000. Depending on the
  // timestamp source and on whether cameraserver already rebased video-encoder
  // streams to MONOTONIC (it does on P+ for REALTIME sources), the value is
  // either MONOTONIC or BOOTTIME. Choose the candidate that is in the past and
  // closest to now (true capture->output latency is a few ms..~100 ms; the
  // wrong candidate is off by the whole BOOTTIME-MONOTONIC offset).
  const int64_t now = monoUs();
  const int64_t off = bootMinusMonoUs();
  const int64_t c0 = ptsUs, c1 = ptsUs - off;
  const int64_t d0 = now - c0, d1 = now - c1;
  constexpr int64_t kMaxPlausibleUs = 5000000;
  int mode;
  int64_t best;
  if (d0 >= -2000 && d0 <= kMaxPlausibleUs && (d1 < -2000 || d1 > kMaxPlausibleUs || d0 <= d1)) {
    mode = 0;
    best = c0;
  } else if (d1 >= -2000 && d1 <= kMaxPlausibleUs) {
    mode = 1;
    best = c1;
  } else {
    mode = 2;
    best = now;
  }
  if (mode != tsMode_) {
    if (mode == 2)
      LOGW("encoder timestamps not in a known clock (pts=%lld us); using output time",
           static_cast<long long>(ptsUs));
    else
      LOGI_PC("encoder timestamps: %s (now-pts as MONOTONIC %.1f ms, as BOOTTIME %.1f ms, "
              "BOOTTIME-MONOTONIC %.1f ms)",
              mode == 0 ? "CLOCK_MONOTONIC" : "CLOCK_BOOTTIME (converted)", d0 / 1000.0, d1 / 1000.0,
              off / 1000.0);
    tsMode_ = mode;
  }
  return best > now ? now : best;
}

void VideoEncoder::handleConfig(const uint8_t* data, size_t len) {
  std::vector<uint8_t> csd(data, data + len);
  if (formatSent_ && csd == csd_) return;
  if (formatSent_ && params_.epochCounter) {
    // New parameter sets mid-stream: the decoder must reinitialize.
    curEpoch_ = params_.epochCounter->fetch_add(1) + 1;
  }
  csd_ = std::move(csd);
  VideoFormat vf;
  vf.epoch = curEpoch_.load();
  vf.codec = params_.settings.codec;
  vf.width = static_cast<uint16_t>(params_.settings.width);
  vf.height = static_cast<uint16_t>(params_.settings.height);
  vf.fps = static_cast<uint16_t>(params_.settings.fps);
  vf.rotation_deg = static_cast<int16_t>(params_.rotationDeg);
  vf.mirror = params_.mirror;
  vf.csd = csd_;
  formatSent_ = true;
  if (sink_) sink_->onVideoFormat(vf);
}

void VideoEncoder::handleFrame(const uint8_t* data, size_t len, int64_t ptsUs, uint32_t flags) {
  const Codec codec = params_.settings.codec;
  const bool key = (flags & kFlagKeyFrame) != 0;
  bool hasPs = false;
  size_t psLen = 0;
  if (key) psLen = paramSetPrefix(codec, data, len, hasPs);
  if (!formatSent_) {
    // No CODEC_CONFIG buffer seen: derive the CSD from the IDR's prefix.
    if (!key || !hasPs || psLen == 0) return;  // undecodable without config
    handleConfig(data, psLen);
  }
  const int64_t now = monoUs();
  const int64_t cap = toMonotonic(ptsUs);
  if (lastPts_ >= 0 && ptsUs < lastPts_) bframesSeen_ = true;
  lastPts_ = ptsUs;

  auto f = std::make_shared<EncodedFrame>();
  f->epoch = curEpoch_.load();
  f->captureUs = cap;
  f->flags = key ? kFrameKey : 0;
  if (key && !hasPs && !csd_.empty()) {
    // Encoder ignored prepend-sps-pps-to-idr-frames: prepend it ourselves.
    f->au.reserve(csd_.size() + len);
    f->au.assign(csd_.begin(), csd_.end());
    f->au.insert(f->au.end(), data, data + len);
    f->flags |= kFrameConfig;
  } else {
    f->au.assign(data, data + len);
    if (hasPs) f->flags |= kFrameConfig;
  }
  if (key) lastKeyMs_ = now / 1000;
  framesOut_.fetch_add(1, std::memory_order_relaxed);
  bytesOut_.fetch_add(len, std::memory_order_relaxed);
  // EWMA(1/8) of capture -> encoder output latency.
  const int32_t sample = static_cast<int32_t>((now - cap) / 10);  // ms * 100
  const int32_t prev = latencyMsX100_.load(std::memory_order_relaxed);
  latencyMsX100_.store(prev == 0 ? sample : prev + (sample - prev) / 8, std::memory_order_relaxed);

  if (sink_ && sink_->onEncodedFrame(std::move(f))) requestKeyframe(true);
}

void VideoEncoder::outputLoop() {
  setThreadName("owc-enc-out");
  int nice = raiseThreadPriority(-16);
  LOGI("encoder output thread started (nice %d)", nice);
  while (!stopFlag_.load(std::memory_order_relaxed)) {
    AMediaCodecBufferInfo info{};
    ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, kDequeueTimeoutUs);
    if (idx >= 0) {
      size_t cap = 0;
      uint8_t* buf = AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(idx), &cap);
      const uint32_t flags = info.flags;
      if (buf && info.size > 0 && static_cast<size_t>(info.offset) + info.size <= cap) {
        const uint8_t* d = buf + info.offset;
        const size_t n = static_cast<size_t>(info.size);
        if (flags & kFlagCodecConfig) {
          handleConfig(d, n);
          AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(idx), false);
        } else {
          // Copy out, release the codec buffer first, then deliver (delivery may block).
          std::vector<uint8_t> tmp(d, d + n);
          AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(idx), false);
          handleFrame(tmp.data(), tmp.size(), info.presentationTimeUs, flags);
        }
      } else {
        AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(idx), false);
      }
      if (flags & kFlagEos) break;
    } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      if (AMediaFormat* f = AMediaCodec_getOutputFormat(codec_)) {
        LOGI("encoder output format: %s", AMediaFormat_toString(f));
        if (!formatSent_) {
          // Some encoders only publish csd-0/csd-1 here.
          void* b0 = nullptr; size_t n0 = 0;
          void* b1 = nullptr; size_t n1 = 0;
          std::vector<uint8_t> csd;
          if (AMediaFormat_getBuffer(f, "csd-0", &b0, &n0) && b0)
            csd.insert(csd.end(), static_cast<uint8_t*>(b0), static_cast<uint8_t*>(b0) + n0);
          if (AMediaFormat_getBuffer(f, "csd-1", &b1, &n1) && b1)
            csd.insert(csd.end(), static_cast<uint8_t*>(b1), static_cast<uint8_t*>(b1) + n1);
          if (!csd.empty()) handleConfig(csd.data(), csd.size());
        }
        AMediaFormat_delete(f);
      }
    } else if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER ||
               idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      continue;
    } else {
      LOGE("encoder dequeueOutputBuffer error %zd", idx);
      if (sink_) sink_->onEncoderError("dequeueOutputBuffer error " + std::to_string(idx));
      break;
    }
  }
  LOGI("encoder output thread exit");
}

}  // namespace owc
