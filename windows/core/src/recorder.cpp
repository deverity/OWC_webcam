#include "recorder.h"

#include "annexb.h"
#include "decoder.h"  // MfThreadScope
#include "util.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace owc {
namespace detail {

namespace {

struct Item {
  bool format = false;
  VideoFormat fmt;
  uint32_t epoch = 0, seq = 0;
  uint64_t ts = 0;
  uint8_t flags = 0;
  std::vector<uint8_t> data;
};

std::string hrMessage(HRESULT hr) {
  if (hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) || hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL))
    return "disk full";
  if (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) return "access denied";
  return hresultToString(hr);
}

std::string withSuffix(const std::string& path, int index) {
  if (index <= 1) return path;
  const size_t dot = path.find_last_of('.');
  const size_t slash = path.find_last_of("\\/");
  const std::string suffix = "_" + std::to_string(index);
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path + suffix;
  return path.substr(0, dot) + suffix + path.substr(dot);
}

bool ensureParentDir(const std::string& path) {
  const std::wstring w = utf8ToWide(path);
  const size_t slash = w.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return true;
  const int rc = SHCreateDirectoryExW(nullptr, w.substr(0, slash).c_str(), nullptr);
  return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS;
}

// Removes repeated parameter sets (e.g. SPS,PPS,SPS,PPS): the MP4 sink can't
// build the avcC/hvcC box from duplicates (MF_E_SINK_HEADERS_NOT_FOUND).
std::vector<uint8_t> dedupeParamSets(Codec c, const std::vector<uint8_t>& csd) {
  if (c == Codec::Av1) return csd;
  std::vector<std::vector<uint8_t>> seen;
  std::vector<uint8_t> out;
  forEachNal(csd.data(), csd.size(), [&](const uint8_t* nal, size_t len) {
    if (!isParamSetNal(c, nalType(c, nal[0]))) return;
    std::vector<uint8_t> v(nal, nal + len);
    for (auto& x : seen)
      if (x == v) return;
    seen.push_back(v);
    appendNal(out, nal, len);
  });
  return out.empty() ? csd : out;
}

}  // namespace

std::string Recorder::defaultPath() {
  PWSTR p = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &p)) && p) dir = p;
  if (p) CoTaskMemFree(p);
  if (dir.empty()) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    dir = std::wstring(buf, n < MAX_PATH ? n : 0) + L"\\Videos";
  }
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[64];
  swprintf(name, 64, L"\\OWC\\OWC_%04u%02u%02u_%02u%02u%02u.mp4", t.wYear, t.wMonth, t.wDay, t.wHour,
           t.wMinute, t.wSecond);
  return wideToUtf8(dir + name);
}

struct Recorder::Impl {
  // ---- shared (mx)
  mutable std::mutex mx;
  std::condition_variable cv;
  std::deque<Item> q;
  size_t qBytes = 0;
  bool stopReq = false;
  bool running = false;
  RecordStatus st;
  RecordOptions opts;
  std::string basePath;
  // producer-side continuity
  bool haveSeq = false;
  uint32_t lastSeq = 0, lastEpoch = 0;
  bool needKey = true;
  bool started = false;  // first keyframe queued
  std::thread th;

  // ---- writer thread only
  bool haveFmt = false;
  VideoFormat fmt;
  std::vector<uint8_t> csd;
  ComPtr<IMFSinkWriter> writer;
  DWORD stream = 0;
  Codec fileCodec = Codec::H264;
  uint32_t fileW = 0, fileH = 0;
  int fileIndex = 0;
  uint64_t fileFrames = 0;
  uint64_t fileBaseTs = 0;
  int64_t lastSampleTime = -1, fileDuration = 0;
  double closedDuration = 0;
  bool haveFirstTs = false;
  uint64_t firstTs = 0, lastTs = 0;

  void setError(const std::string& e) {
    std::lock_guard<std::mutex> lk(mx);
    st.state = RecordState::Error;
    st.last_error = e;
  }

  void closeFile() {
    if (!writer) return;
    const HRESULT hr = writer->Finalize();
    if (FAILED(hr)) setError("finalize failed: " + hrMessage(hr));
    writer.Reset();
    closedDuration += static_cast<double>(fileDuration) / 1e7;
    fileDuration = 0;
    lastSampleTime = -1;
  }

  int64_t frameDuration() const {
    const uint32_t f = opts.mode == RecordMode::SlowMotion ? std::max<uint32_t>(1, opts.slowmo_output_fps)
                                                         : (fmt.fps ? fmt.fps : 30);
    return 10000000LL / f;
  }

  bool openFile() {
    ++fileIndex;
    const std::string path = withSuffix(basePath, fileIndex);
    if (!ensureParentDir(path)) {
      setError("cannot create folder for " + path);
      return false;
    }
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 3);
    attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    HRESULT hr = MFCreateSinkWriterFromURL(utf8ToWide(path).c_str(), nullptr, attr.Get(), &writer);
    if (FAILED(hr)) {
      setError("cannot create " + path + ": " + hrMessage(hr));
      return false;
    }
    const GUID sub = fmt.codec == Codec::H264   ? MFVideoFormat_H264
                     : fmt.codec == Codec::Hevc ? MFVideoFormat_HEVC
                                                : MFVideoFormat_AV1;
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, sub);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, fmt.width, fmt.height);
    const uint32_t rate = opts.mode == RecordMode::SlowMotion ? std::max<uint32_t>(1, opts.slowmo_output_fps)
                                                            : (fmt.fps ? fmt.fps : 30);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, rate, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
    if (fmt.rotation_deg % 360 != 0)
      t->SetUINT32(MF_MT_VIDEO_ROTATION, static_cast<UINT32>(((fmt.rotation_deg % 360) + 360) % 360));
    const std::vector<uint8_t> header = dedupeParamSets(fmt.codec, csd);
    if (!header.empty())
      t->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), static_cast<UINT32>(header.size()));
    hr = writer->AddStream(t.Get(), &stream);
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(stream, t.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = writer->BeginWriting();
    if (FAILED(hr)) {
      writer.Reset();
      setError(std::string("MP4 sink rejected ") +
               (fmt.codec == Codec::H264 ? "H.264" : fmt.codec == Codec::Hevc ? "HEVC" : "AV1") +
               " passthrough: " + hrMessage(hr));
      return false;
    }
    fileCodec = fmt.codec;
    fileW = fmt.width;
    fileH = fmt.height;
    fileFrames = 0;
    lastSampleTime = -1;
    fileDuration = 0;
    std::lock_guard<std::mutex> lk(mx);
    st.path = path;
    st.files.push_back(path);
    st.state = RecordState::Recording;
    return true;
  }

  void writeFrame(Item& it) {
    const bool key = (it.flags & kFrameKey) != 0;
    const bool annexB = fmt.codec != Codec::Av1;
    if (annexB && (it.flags & kFrameConfig)) {
      std::vector<uint8_t> ps;
      bool picture = false;
      forEachNal(it.data.data(), it.data.size(), [&](const uint8_t* nal, size_t len) {
        const int t = nalType(fmt.codec, nal[0]);
        if (isParamSetNal(fmt.codec, t)) appendNal(ps, nal, len);
        else if (isVclNal(fmt.codec, t)) picture = true;
      });
      if (!ps.empty() && !writer) csd.swap(ps);
      if (!picture) return;
    }
    if (!haveFmt || static_cast<uint16_t>(it.epoch) != static_cast<uint16_t>(fmt.epoch & 0xFFFF)) {
      std::lock_guard<std::mutex> lk(mx);
      ++st.frames_dropped;
      return;
    }
    if (!writer) {
      if (!key) {
        std::lock_guard<std::mutex> lk(mx);
        ++st.frames_dropped;
        return;
      }
      if (!openFile()) return;
      fileBaseTs = it.ts;
    }
    const int64_t dur = frameDuration();
    int64_t t;
    if (opts.mode == RecordMode::SlowMotion) {
      t = static_cast<int64_t>(fileFrames) * dur;
    } else {
      t = static_cast<int64_t>(it.ts - fileBaseTs) * 10;  // phone clock, gaps preserved
      if (t <= lastSampleTime) t = lastSampleTime + 1;
    }
    ComPtr<IMFMediaBuffer> mb;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(it.data.size()), &mb);
    BYTE* p = nullptr;
    if (SUCCEEDED(hr)) hr = mb->Lock(&p, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
      memcpy(p, it.data.data(), it.data.size());
      mb->Unlock();
      mb->SetCurrentLength(static_cast<DWORD>(it.data.size()));
    }
    ComPtr<IMFSample> s;
    if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
    if (SUCCEEDED(hr)) {
      s->AddBuffer(mb.Get());
      s->SetSampleTime(t);
      s->SetSampleDuration(dur);
      if (key) s->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
      hr = writer->WriteSample(stream, s.Get());
    }
    if (FAILED(hr)) {
      setError("write failed: " + hrMessage(hr));
      writer.Reset();  // Finalize would likely fail too; the file may be truncated
      return;
    }
    lastSampleTime = t;
    fileDuration = t + dur;
    ++fileFrames;
    if (!haveFirstTs) {
      haveFirstTs = true;
      firstTs = it.ts;
    }
    lastTs = it.ts;
    std::lock_guard<std::mutex> lk(mx);
    ++st.frames_written;
    st.bytes += it.data.size();
    st.media_duration_s = closedDuration + static_cast<double>(fileDuration) / 1e7;
    st.capture_span_s = static_cast<double>(lastTs - firstTs) / 1e6;
    if (st.capture_span_s > 0 && st.frames_written > 1)
      st.capture_fps = static_cast<double>(st.frames_written - 1) / st.capture_span_s;
    if (opts.mode == RecordMode::SlowMotion && st.capture_fps > 0)
      st.slowmo_factor = st.capture_fps / std::max<uint32_t>(1, opts.slowmo_output_fps);
  }

  void onFormatItem(const VideoFormat& f) {
    const bool geometryChange =
        haveFmt && (f.codec != fmt.codec || f.width != fmt.width || f.height != fmt.height);
    fmt = f;
    haveFmt = true;
    if (!f.csd.empty()) csd = f.csd;
    if (writer && (geometryChange || f.codec != fileCodec || f.width != fileW || f.height != fileH)) {
      closeFile();  // next keyframe opens name_N.mp4
      std::lock_guard<std::mutex> lk(mx);
      if (st.state != RecordState::Error) st.state = RecordState::WaitingForKeyframe;
    }
  }

  void run() {
    setThreadName(L"owc-record");
    MfThreadScope mf;
    if (!mf.ok()) setError("MFStartup failed");
    for (;;) {
      Item it;
      {
        std::unique_lock<std::mutex> lk(mx);
        cv.wait(lk, [&] { return stopReq || !q.empty(); });
        if (q.empty()) break;  // stop requested and drained
        it = std::move(q.front());
        q.pop_front();
        qBytes -= it.data.size();
        if (st.state == RecordState::Error) continue;
      }
      if (it.format) onFormatItem(it.fmt);
      else if (mf.ok()) writeFrame(it);
    }
    closeFile();
  }
};

Recorder::Recorder() : impl_(new Impl) {}
Recorder::~Recorder() { stop(); }

bool Recorder::start(const RecordOptions& opts, const VideoFormat* current, std::string& err) {
  Impl& m = *impl_;
  {
    std::lock_guard<std::mutex> lk(m.mx);
    if (m.running) {
      err = "already recording";
      return false;
    }
  }
  if (m.th.joinable()) m.th.join();
  const std::string path = opts.path.empty() ? defaultPath() : opts.path;
  if (!ensureParentDir(path)) {
    err = "cannot create the folder for " + path;
    return false;
  }
  // Fail fast if the file can't be created (permissions, bad path).
  {
    const std::wstring w = utf8ToWide(path);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      err = "cannot create " + path + " (error " + std::to_string(GetLastError()) + ")";
      return false;
    }
    CloseHandle(h);
    DeleteFileW(w.c_str());
  }
  std::lock_guard<std::mutex> lk(m.mx);
  m.opts = opts;
  if (m.opts.slowmo_output_fps == 0) m.opts.slowmo_output_fps = 30;
  m.basePath = path;
  m.st = RecordStatus();
  m.st.state = RecordState::WaitingForKeyframe;
  m.st.mode = opts.mode;
  m.st.path = path;
  m.q.clear();
  m.qBytes = 0;
  m.stopReq = false;
  m.haveSeq = false;
  m.needKey = true;
  m.started = false;
  m.haveFmt = false;
  m.csd.clear();
  m.writer.Reset();
  m.fileIndex = 0;
  m.closedDuration = 0;
  m.haveFirstTs = false;
  if (current) {
    Item it;
    it.format = true;
    it.fmt = *current;
    m.q.push_back(std::move(it));
  }
  m.running = true;
  m.th = std::thread([&m] { m.run(); });
  return true;
}

RecordStatus Recorder::stop() {
  Impl& m = *impl_;
  {
    std::lock_guard<std::mutex> lk(m.mx);
    if (!m.running) return m.st;
    m.stopReq = true;
    m.cv.notify_all();
  }
  if (m.th.joinable()) m.th.join();
  std::lock_guard<std::mutex> lk(m.mx);
  m.running = false;
  if (m.st.state != RecordState::Error) m.st.state = RecordState::Idle;
  return m.st;
}

bool Recorder::active() const {
  std::lock_guard<std::mutex> lk(impl_->mx);
  return impl_->running && !impl_->stopReq;
}

bool Recorder::waitingForKeyframe() const {
  std::lock_guard<std::mutex> lk(impl_->mx);
  return impl_->running && !impl_->stopReq && impl_->st.state == RecordState::WaitingForKeyframe;
}

RecordStatus Recorder::status() const {
  std::lock_guard<std::mutex> lk(impl_->mx);
  return impl_->st;
}

void Recorder::onFormat(const VideoFormat& f) {
  Impl& m = *impl_;
  std::lock_guard<std::mutex> lk(m.mx);
  if (!m.running || m.stopReq) return;
  Item it;
  it.format = true;
  it.fmt = f;
  m.q.push_back(std::move(it));
  m.needKey = true;  // a new epoch starts with a keyframe
  m.haveSeq = false;
  m.cv.notify_one();
}

void Recorder::onFrame(uint32_t epoch, uint32_t seq, uint64_t ts, uint8_t flags, const uint8_t* au,
                       size_t n) {
  Impl& m = *impl_;
  std::lock_guard<std::mutex> lk(m.mx);
  if (!m.running || m.stopReq || m.st.state == RecordState::Error) return;
  ++m.st.frames_received;
  if (m.haveSeq && epoch == m.lastEpoch && seq != m.lastSeq + 1u && seqNewer(seq, m.lastSeq)) {
    ++m.st.gaps;
    m.st.frames_missing += seq - m.lastSeq - 1u;
    m.needKey = true;  // references are broken until the next keyframe
  }
  m.haveSeq = true;
  m.lastSeq = seq;
  m.lastEpoch = epoch;
  const bool key = (flags & kFrameKey) != 0;
  if (m.needKey && !key) {
    if (m.started) ++m.st.frames_dropped;  // frames before the very first keyframe aren't drops
    return;
  }
  const size_t cap = static_cast<size_t>(m.opts.max_queue_mb) << 20;
  if (m.qBytes + n > cap) {
    ++m.st.frames_dropped;
    m.needKey = true;
    return;
  }
  m.needKey = false;
  m.started = true;
  Item it;
  it.epoch = epoch;
  it.seq = seq;
  it.ts = ts;
  it.flags = flags;
  it.data.assign(au, au + n);
  m.qBytes += n;
  m.q.push_back(std::move(it));
  m.cv.notify_one();
}

}  // namespace detail
}  // namespace owc
