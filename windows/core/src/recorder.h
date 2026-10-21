// Passthrough MP4 recorder (Media Foundation Sink Writer, no re-encode).
// Producer side (onFormat/onFrame) is called from the decode thread and never
// blocks; muxing runs on the recorder's own thread.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <owc/client.h>

namespace owc {
namespace detail {

class Recorder {
 public:
  Recorder();
  ~Recorder();  // stops
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  // `current` = the stream's format if already known (else waits for onFormat).
  bool start(const RecordOptions& opts, const VideoFormat* current, std::string& err);
  RecordStatus stop();  // drains the queue, finalizes the file(s), joins the thread
  bool active() const;
  RecordStatus status() const;
  bool waitingForKeyframe() const;  // started, no file open yet / waiting after a split

  void onFormat(const VideoFormat& f);
  // One complete access unit as received (any flags).
  void onFrame(uint32_t epoch, uint32_t seq, uint64_t capture_ts_us, uint8_t flags,
               const uint8_t* au, size_t n);

  static std::string defaultPath();  // %USERPROFILE%\Videos\OWC\OWC_yyyyMMdd_HHmmss.mp4

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace detail
}  // namespace owc
