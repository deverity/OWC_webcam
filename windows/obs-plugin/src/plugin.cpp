// OWC Phone Camera - OBS Studio async video source fed from the OWC shared-memory
// frame bus (owc::ShmReader). GPL-2.0-or-later (links libobs).
#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <owc/shm.h>

#pragma warning(push, 0)
extern "C" {
#include <obs-module.h>
#include <util/platform.h>
#include <media-io/video-io.h>
}
#pragma warning(pop)

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("owc-obs-source", "en-US")

namespace {

constexpr const char* kSourceId = "owc_phone_source";
constexpr const char* kUnbuffered = "unbuffered";
constexpr const char* kColorSpace = "color_space";
constexpr const char* kColorRange = "color_range";
constexpr const char* kNoSignal = "no_signal";

enum ColorSpaceChoice : long long { kCsAuto = 0, kCs601 = 1, kCs709 = 2 };
enum RangeChoice : long long { kRangePartial = 0, kRangeFull = 1 };
enum NoSignalChoice : long long { kKeepLast = 0, kShowNothing = 1 };
enum State : int { kWaiting = 0, kLive = 1, kNoSignalState = 2 };

constexpr uint64_t kNoSignalNs = 1000ull * 1000 * 1000;     // 1 s stale => no signal
constexpr int64_t kReopenIntervalNs = 250ll * 1000 * 1000;  // retry OpenFileMapping
constexpr DWORD kIdleSleepMs = 100;                         // polling cadence while hidden

struct OwcSource {
  obs_source_t* source = nullptr;
  std::thread worker;
  std::atomic<bool> stop{false};

  // Settings (written by update(), read by the worker).
  std::atomic<long long> colorSpace{kCsAuto};
  std::atomic<long long> range{kRangePartial};
  std::atomic<long long> noSignal{kKeepLast};
  std::atomic<unsigned> settingsGen{1};

  // Status (written by the worker, read by get_properties()).
  std::atomic<int> state{kWaiting};
  std::atomic<uint32_t> width{0};
  std::atomic<uint32_t> height{0};
};

// 1 ms wait helper. Prefers a high-resolution waitable timer (Win10 1803+);
// falls back to a normal timer under timeBeginPeriod(1).
class Ticker {
 public:
  Ticker() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (!timer_) {
      timer_ = CreateWaitableTimerW(nullptr, TRUE, nullptr);
      if (timeBeginPeriod(1) == TIMERR_NOERROR) periodRaised_ = true;
    }
  }
  ~Ticker() {
    if (timer_) CloseHandle(timer_);
    if (periodRaised_) timeEndPeriod(1);
  }
  Ticker(const Ticker&) = delete;
  Ticker& operator=(const Ticker&) = delete;

  void wait(DWORD ms) {
    if (!timer_) { Sleep(ms); return; }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)ms * 10000;  // relative, 100 ns units
    if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) { Sleep(ms); return; }
    WaitForSingleObject(timer_, ms + 50);
  }

 private:
  HANDLE timer_ = nullptr;
  bool periodRaised_ = false;
};

int64_t qpcToNs(int64_t ticks, int64_t freq) {
  if (freq <= 0) return 0;
  const int64_t sec = ticks / freq;
  const int64_t rem = ticks % freq;
  return sec * 1000000000ll + rem * 1000000000ll / freq;
}

void fillColor(obs_source_frame& f, long long csChoice, long long rangeChoice, uint32_t height) {
  video_colorspace cs;
  if (csChoice == kCs601) cs = VIDEO_CS_601;
  else if (csChoice == kCs709) cs = VIDEO_CS_709;
  else cs = height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;
  const video_range_type range = rangeChoice == kRangeFull ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
  f.full_range = range == VIDEO_RANGE_FULL;
  video_format_get_parameters_for_format(cs, range, VIDEO_FORMAT_NV12, f.color_matrix,
                                         f.color_range_min, f.color_range_max);
}

void workerMain(OwcSource* s) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  Ticker ticker;
  owc::ShmReader reader;
  // Uninitialised on purpose: pages are only touched as far as frames actually reach.
  std::unique_ptr<uint8_t[]> buf(new uint8_t[OWC_SHM_SLOT_BYTES]);

  obs_source_frame frame = {};
  frame.format = VIDEO_FORMAT_NV12;
  uint64_t lastSeen = 0;
  unsigned colorGen = 0;
  uint32_t colorHeight = 0;
  uint64_t lastAliveNs = os_gettime_ns();
  uint64_t lastTsNs = 0;
  uint64_t lastOpenTryNs = 0;
  bool clearedForNoSignal = false;

  while (!s->stop.load(std::memory_order_acquire)) {
    // Not shown anywhere: skip all work (no copy, no color setup) until visible again.
    if (!obs_source_active(s->source) && !obs_source_showing(s->source)) {
      ticker.wait(kIdleSleepMs);
      continue;
    }
    ticker.wait(1);
    const uint64_t nowNs = os_gettime_ns();

    if (!reader.isOpen() && (int64_t)(nowNs - lastOpenTryNs) >= kReopenIntervalNs) {
      lastOpenTryNs = nowNs;
      if (reader.open()) lastSeen = 0;
    }

    if (reader.isOpen() && reader.producerAlive()) {
      lastAliveNs = nowNs;
      clearedForNoSignal = false;
      owc::ShmReader::FrameInfo info;
      if (reader.readLatest(lastSeen, buf.get(), OWC_SHM_SLOT_BYTES, info)) {
        lastSeen = info.frame_number;
        // Timestamp = capture time of this frame expressed in the os_gettime_ns clock:
        // read time minus how long the frame has been sitting in the bus. Monotonic and
        // never in the future. (Unbuffered mode displays on arrival and ignores it;
        // buffered mode keeps A/V sync honest.)
        uint64_t ts = nowNs;
        const int64_t freq = reader.header()->qpc_frequency;
        const int64_t age = qpcToNs(owc::qpcNow() - info.publish_qpc, freq);
        if (age > 0 && (uint64_t)age < ts) ts -= (uint64_t)age;
        if (ts <= lastTsNs) ts = lastTsNs + 1;
        lastTsNs = ts;

        const unsigned gen = s->settingsGen.load(std::memory_order_relaxed);
        if (gen != colorGen || info.height != colorHeight) {
          fillColor(frame, s->colorSpace.load(), s->range.load(), info.height);
          colorGen = gen;
          colorHeight = info.height;
        }
        frame.data[0] = buf.get();
        frame.linesize[0] = info.width;
        frame.data[1] = buf.get() + (size_t)info.width * info.height;
        frame.linesize[1] = info.width;
        frame.width = info.width;
        frame.height = info.height;
        frame.timestamp = ts;
        obs_source_output_video(s->source, &frame);  // copies; buffer is reusable at once
        s->width.store(info.width, std::memory_order_relaxed);
        s->height.store(info.height, std::memory_order_relaxed);
        s->state.store(kLive, std::memory_order_relaxed);
      }
    } else {
      if (reader.isOpen()) {
        // Producer gone or stale: drop the mapping so a restarted producer gets a fresh section.
        reader.close();
        lastOpenTryNs = nowNs;
      }
      if (nowNs - lastAliveNs >= kNoSignalNs) {
        s->state.store(kNoSignalState, std::memory_order_relaxed);
        if (!clearedForNoSignal) {
          clearedForNoSignal = true;
          if (s->noSignal.load() == kShowNothing) obs_source_output_video(s->source, nullptr);
        }
      } else if (s->state.load(std::memory_order_relaxed) != kLive) {
        s->state.store(kWaiting, std::memory_order_relaxed);
      }
    }
  }
}

const char* getName(void*) { return obs_module_text("OWCPhoneCamera"); }

void applySettings(OwcSource* s, obs_data_t* settings) {
  s->colorSpace.store(obs_data_get_int(settings, kColorSpace));
  s->range.store(obs_data_get_int(settings, kColorRange));
  s->noSignal.store(obs_data_get_int(settings, kNoSignal));
  s->settingsGen.fetch_add(1);
  obs_source_set_async_unbuffered(s->source, obs_data_get_bool(settings, kUnbuffered));
}

void* createSource(obs_data_t* settings, obs_source_t* source) {
  OwcSource* s = new OwcSource();
  s->source = source;
  applySettings(s, settings);
  s->worker = std::thread(workerMain, s);
  return s;
}

void destroySource(void* data) {
  OwcSource* s = static_cast<OwcSource*>(data);
  s->stop.store(true, std::memory_order_release);
  if (s->worker.joinable()) s->worker.join();
  delete s;
}

void updateSource(void* data, obs_data_t* settings) { applySettings(static_cast<OwcSource*>(data), settings); }

void getDefaults(obs_data_t* settings) {
  obs_data_set_default_bool(settings, kUnbuffered, true);
  obs_data_set_default_int(settings, kColorSpace, kCsAuto);
  obs_data_set_default_int(settings, kColorRange, kRangePartial);
  obs_data_set_default_int(settings, kNoSignal, kKeepLast);
}

std::string replaceFirst(std::string t, const char* key, const std::string& val) {
  const size_t pos = t.find(key);
  if (pos != std::string::npos) t.replace(pos, strlen(key), val);
  return t;
}

obs_properties_t* getProperties(void* data) {
  OwcSource* s = static_cast<OwcSource*>(data);
  obs_properties_t* p = obs_properties_create();

  std::string status;
  switch (s ? s->state.load() : (int)kWaiting) {
    case kLive:
      status = replaceFirst(replaceFirst(obs_module_text("Status.Live"), "%1", std::to_string(s->width.load())),
                            "%2", std::to_string(s->height.load()));
      break;
    case kNoSignalState: status = obs_module_text("Status.NoSignal"); break;
    default: status = obs_module_text("Status.Waiting"); break;
  }
  obs_property_t* st = obs_properties_add_text(p, "status", status.c_str(), OBS_TEXT_INFO);
  obs_property_text_set_info_type(st, OBS_TEXT_INFO_NORMAL);

  obs_properties_add_bool(p, kUnbuffered, obs_module_text("Unbuffered"));

  obs_property_t* cs = obs_properties_add_list(p, kColorSpace, obs_module_text("ColorSpace"),
                                               OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(cs, obs_module_text("ColorSpace.Auto"), kCsAuto);
  obs_property_list_add_int(cs, obs_module_text("ColorSpace.601"), kCs601);
  obs_property_list_add_int(cs, obs_module_text("ColorSpace.709"), kCs709);

  obs_property_t* rg = obs_properties_add_list(p, kColorRange, obs_module_text("ColorRange"),
                                               OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(rg, obs_module_text("ColorRange.Partial"), kRangePartial);
  obs_property_list_add_int(rg, obs_module_text("ColorRange.Full"), kRangeFull);

  obs_property_t* ns = obs_properties_add_list(p, kNoSignal, obs_module_text("OnNoSignal"),
                                               OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(ns, obs_module_text("OnNoSignal.Keep"), kKeepLast);
  obs_property_list_add_int(ns, obs_module_text("OnNoSignal.Clear"), kShowNothing);
  return p;
}

}  // namespace

bool obs_module_load(void) {
  obs_source_info info = {};
  info.id = kSourceId;
  info.type = OBS_SOURCE_TYPE_INPUT;
  info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
  info.icon_type = OBS_ICON_TYPE_CAMERA;
  info.get_name = getName;
  info.create = createSource;
  info.destroy = destroySource;
  info.update = updateSource;
  info.get_defaults = getDefaults;
  info.get_properties = getProperties;
  obs_register_source(&info);
  return true;
}

const char* obs_module_description(void) {
  return "OWC Phone Camera source (shared-memory feed from the OWC app)";
}
