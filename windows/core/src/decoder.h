// Media Foundation H.264/HEVC decoder (sync MFT) with D3D11/DXVA acceleration
// and CPU NV12 read-back. Single-threaded: create, use and destroy on one
// thread that has called CoInitializeEx + MFStartup (see MfThreadScope).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <owc/client.h>
#include <owc/protocol.h>

namespace owc {
namespace detail {

// RAII CoInitializeEx(MTA) + MFStartup for the current thread.
class MfThreadScope {
 public:
  MfThreadScope();
  ~MfThreadScope();
  bool ok() const { return ok_; }

 private:
  bool com_ = false, ok_ = false;
};

struct DecoderConfig {
  Codec codec = Codec::H264;
  uint32_t width = 0, height = 0, fps = 30;
  DecoderMode mode = DecoderMode::Auto;
  bool low_latency = true;
};

struct DecodedImage {
  const uint8_t* y = nullptr;
  const uint8_t* uv = nullptr;
  uint32_t y_stride = 0, uv_stride = 0;
  uint32_t width = 0, height = 0;  // display size (even)
  int64_t sample_time = 0;         // 100 ns units, as passed to decode()
};

class MfDecoder {
 public:
  using Sink = std::function<void(const DecodedImage&)>;
  MfDecoder();
  ~MfDecoder();
  MfDecoder(const MfDecoder&) = delete;
  MfDecoder& operator=(const MfDecoder&) = delete;

  // (Re)creates the MFT. On failure returns false with a human readable err.
  bool open(const DecoderConfig& cfg, std::string& err);
  void close();
  bool isOpen() const;
  const DecoderConfig& config() const;

  // Feeds ONE access unit (Annex-B) and immediately drains every available
  // output through `sink` (synchronously, on this thread). Returns false on a
  // decode error (caller should flush + wait for / request a keyframe).
  // readback=false: outputs are still drained (and `sink` called, for
  // bookkeeping) but the D3D11 GPU->CPU copy is skipped: DecodedImage::y/uv
  // are null. Used for frames that will not be presented (decoder backlog).
  bool decode(const uint8_t* au, size_t n, int64_t sample_time, int64_t duration, bool keyframe,
              const Sink& sink, bool readback = true);
  // Discards everything buffered inside the decoder.
  void flush();

  bool hardware() const;           // D3D11 manager accepted and DXGI output seen
  bool d3dConfigured() const;      // D3D11 manager accepted by the MFT (after open)
  std::string mftName() const;     // MFT friendly name
  std::string name() const;        // MFT friendly name + mode
  uint32_t streamChanges() const;  // MF_E_TRANSFORM_STREAM_CHANGE count since open
  // Duration of the GPU->CPU read-back (copy + Map) of the last output, µs.
  int64_t lastReadbackUs() const;
  std::string lastError() const;   // reason of the last decode() failure
  // Colour info of the current output type (MF_MT_YUV_MATRIX /
  // MF_MT_VIDEO_NOMINAL_RANGE). matrix: 0 unknown, 1 BT.601, 2 BT.709
  // (same values as OWC_COLOR_*); fullRange: -1 unknown, 0 limited, 1 full.
  void outputColor(uint32_t& matrix, int& fullRange) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace detail
}  // namespace owc
