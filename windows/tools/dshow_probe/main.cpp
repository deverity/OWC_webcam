// owc_dshow_probe: test harness for the "OWC Webcam" DirectShow virtual camera.
//
//   owc_dshow_probe --list
//       enumerate video input devices (ICreateDevEnum) and the OWC media types
//   owc_dshow_probe --run [--format NV12|YUY2|I420|RGB24|RGB32|all] [--size WxH]
//                   [--fps 30|60] [--seconds S] [--expect live|placeholder]
//                   [--produce [WxH@F]] [--dll PATH] [--connect setformat|direct]
//                   [--sink-allocator]
//       build graph: OWC Webcam -> probe sink, run S seconds, report fps,
//       interval jitter, latency (sample receipt - shm publish_qpc of the
//       frame identified by its counter strip) and content checks
//   owc_dshow_probe --cycles N [--seconds S] [...]
//       N graph build/run/teardown cycles; reports handle/private-bytes growth
//       and whether the DLL reports DllCanUnloadNow() == S_OK afterwards
//   owc_dshow_probe --bench
//       NV12 -> {formats} x {sizes} conversion timings of the filter's code
//   owc_dshow_probe --produce WxH@F --seconds S
//       standalone shm test producer (same strip encoding as owc_testsrc)
//
// Exit code 0 = all checks passed, 1 = a check failed, 2 = usage/setup error,
// 4 = inconclusive (another process published on the shm bus during a run).

#include <windows.h>
#include <dshow.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <owc/shm.h>
#include <owc/vcam_ids.h>

#include "convert.h"
#include "mediatypes.h"
#include "sink.h"

using owc::vcam::ParsedType;
using owc::vcam::PixFmt;

namespace {

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

int64_t g_qpcFreq = 1;
double qpcMs(int64_t ticks) { return (double)ticks * 1000.0 / (double)g_qpcFreq; }

template <class T>
void safeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

std::string narrow(const wchar_t* w) {
  if (!w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s((size_t)(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}

// ---------------------------------------------------------------- options

enum class Expect { Any, Live, Placeholder };

struct Options {
  bool list = false, run = false, bench = false, produceOnly = false;
  int cycles = 0;
  std::vector<PixFmt> formats{PixFmt::NV12};
  int width = 1280, height = 720, fps = 30;
  double seconds = 5.0;
  bool produce = false;
  int pw = 1280, ph = 720, pfps = 30;
  std::wstring dll;
  bool connectDirect = false;
  bool sinkAllocator = false;
  bool reformat = false;
  Expect expect = Expect::Any;
};

void usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  owc_dshow_probe --list\n"
               "  owc_dshow_probe --run [--format F|all] [--size WxH] [--fps N] [--seconds S]\n"
               "                  [--expect live|placeholder] [--produce [WxH@F]] [--dll PATH]\n"
               "                  [--connect setformat|direct] [--sink-allocator]\n"
               "  owc_dshow_probe --cycles N [--seconds S] [run options]\n"
               "  owc_dshow_probe --reformat [--produce]   (SetFormat while connected -> ReconnectEx)\n"
               "  owc_dshow_probe --bench\n"
               "  owc_dshow_probe --produce WxH@F --seconds S\n");
}

bool parseSize(const char* s, int* w, int* h) { return sscanf_s(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0; }

bool parseArgs(int argc, char** argv, Options& o) {
  if (argc <= 1) {
    o.list = true;
    return true;
  }
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    const bool hasVal = i + 1 < argc && argv[i + 1][0] != '-';
    if (!std::strcmp(a, "--list")) {
      o.list = true;
    } else if (!std::strcmp(a, "--run")) {
      o.run = true;
    } else if (!std::strcmp(a, "--bench")) {
      o.bench = true;
    } else if (!std::strcmp(a, "--cycles") && hasVal) {
      o.cycles = std::atoi(argv[++i]);
    } else if (!std::strcmp(a, "--format") && hasVal) {
      const char* v = argv[++i];
      o.formats.clear();
      if (!_stricmp(v, "all")) {
        for (int f = 0; f < (int)PixFmt::Count; ++f) o.formats.push_back((PixFmt)f);
      } else {
        PixFmt f;
        if (!owc::vcam::pixFmtFromName(v, &f)) return false;
        o.formats.push_back(f);
      }
    } else if (!std::strcmp(a, "--size") && hasVal) {
      if (!parseSize(argv[++i], &o.width, &o.height)) return false;
    } else if (!std::strcmp(a, "--fps") && hasVal) {
      o.fps = std::atoi(argv[++i]);
    } else if (!std::strcmp(a, "--seconds") && hasVal) {
      o.seconds = std::atof(argv[++i]);
    } else if (!std::strcmp(a, "--expect") && hasVal) {
      const char* v = argv[++i];
      if (!_stricmp(v, "live"))
        o.expect = Expect::Live;
      else if (!_stricmp(v, "placeholder"))
        o.expect = Expect::Placeholder;
      else
        return false;
    } else if (!std::strcmp(a, "--produce")) {
      o.produce = true;
      if (hasVal && sscanf_s(argv[i + 1], "%dx%d@%d", &o.pw, &o.ph, &o.pfps) == 3) ++i;
    } else if (!std::strcmp(a, "--dll") && hasVal) {
      const char* v = argv[++i];
      o.dll.assign(v, v + std::strlen(v));
    } else if (!std::strcmp(a, "--connect") && hasVal) {
      const char* v = argv[++i];
      if (!_stricmp(v, "direct"))
        o.connectDirect = true;
      else if (_stricmp(v, "setformat"))
        return false;
    } else if (!std::strcmp(a, "--sink-allocator")) {
      o.sinkAllocator = true;
    } else if (!std::strcmp(a, "--reformat")) {
      o.reformat = true;
    } else {
      std::fprintf(stderr, "unknown or incomplete option: %s\n", a);
      return false;
    }
  }
  if (o.produce && !o.run && !o.cycles && !o.list && !o.bench && !o.reformat) o.produceOnly = true;
  return true;
}

// ---------------------------------------------------------------- shm producer (in-process)

struct Producer {
  std::atomic<bool> stop{false};
  std::thread th;
  bool ok = false;

  void start(int w, int h, int fps) {
    th = std::thread([this, w, h, fps] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);  // keep source jitter out of the measurement
      loop(w, h, fps);
    });
  }
  void finish() {
    stop = true;
    if (th.joinable()) th.join();
  }
  void loop(int W, int H, int F) {
    owc::ShmWriter wr;
    if (!wr.open()) {
      std::fprintf(stderr, "producer: cannot create shm (%lu)\n", GetLastError());
      return;
    }
    ok = true;
    wr.setActive(true, (uint32_t)W, (uint32_t)H, (uint32_t)F, 1);
    std::vector<uint8_t> y((size_t)W * H), uv((size_t)W * H / 2);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const int64_t start = owc::qpcNow();
    for (uint64_t n = 0; !stop; ++n) {
      const int64_t due = start + (int64_t)n * g_qpcFreq / F;
      for (;;) {
        const int64_t rem = due - owc::qpcNow();
        if (rem <= 0 || stop) break;
        LARGE_INTEGER d;
        d.QuadPart = -std::max<int64_t>(1, rem * 10000000 / g_qpcFreq);
        if (timer && SetWaitableTimer(timer, &d, 0, nullptr, nullptr, FALSE))
          WaitForSingleObject(timer, 100);
        else
          Sleep(1);
      }
      const uint32_t fn = (uint32_t)(wr.header()->latest_frame_number + 1);
      // 8 luma bars + moving bar, colored chroma, strip area black/neutral
      for (int r = 0; r < H; ++r) {
        uint8_t* row = &y[(size_t)r * W];
        if (r >= H - 32) {
          std::memset(row, 16, (size_t)W);
          continue;
        }
        for (int x = 0; x < W; ++x) row[x] = (uint8_t)(200 - (x * 8 / W) * 20);
        const int bx = (int)((n * 8) % (uint64_t)W);
        for (int x = bx; x < std::min(W, bx + 16); ++x) row[x] = 128;
      }
      for (int r = 0; r < H / 2; ++r) {
        uint8_t* row = &uv[(size_t)r * W];
        const bool strip = r >= (H - 32) / 2;
        for (int x = 0; x < W; x += 2) {
          row[x] = strip ? 128 : (uint8_t)(64 + (x * 4 / W) * 40);
          row[x + 1] = strip ? 128 : (uint8_t)(200 - (x * 4 / W) * 40);
        }
      }
      for (int i = 0; i < 32; ++i) {
        const uint8_t v = ((fn >> i) & 1u) ? 235 : 16;
        for (int r = H - 32; r < H - 16; ++r) std::memset(&y[(size_t)r * W + 16 + 16 * i], v, 16);
      }
      wr.publishNV12(y.data(), (uint32_t)W, uv.data(), (uint32_t)W, (uint32_t)W, (uint32_t)H);
    }
    if (timer) CloseHandle(timer);
    wr.setActive(false);
    wr.close();
  }
};

// Watches the shm slot headers and remembers publish_qpc per frame number,
// so a delivered frame's strip value can be turned into a latency.
struct PublishMonitor {
  struct Entry {
    volatile LONG64 fn;
    volatile LONG64 qpc;
  };
  static const int kSize = 8192;
  Entry table[kSize] = {};
  std::atomic<bool> stop{false};
  std::thread th;
  // Interference detection: the shm bus is session-global, so another
  // process (e.g. a parallel test) may start publishing during a run.
  std::atomic<uint32_t> expectedPid{0};  // 0 = no producer expected
  std::atomic<uint32_t> foreignPid{0};
  void resetRun(uint32_t expected) {
    expectedPid = expected;
    foreignPid = 0;
  }

  void start() {
    th = std::thread([this] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
      loop();
    });
  }
  void finish() {
    stop = true;
    if (th.joinable()) th.join();
  }
  bool lookup(uint32_t fn, int64_t* qpc) const {
    const Entry& e = table[fn % kSize];
    if ((uint32_t)e.fn != fn || e.fn == 0) return false;
    *qpc = e.qpc;
    return true;
  }
  void loop() {
    owc::ShmReader r;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    while (!stop) {
      if (!r.isOpen()) r.open();
      if (r.isOpen()) {
        const OwcShmHeader* h = r.header();
        const uint32_t exp = expectedPid.load();
        if (r.producerAlive() && h->producer_pid != exp) foreignPid = h->producer_pid;
        // our producer was deactivated/replaced by another process's ShmWriter
        if (exp != 0 && (!r.producerAlive() || h->producer_pid != exp) && foreignPid == 0)
          foreignPid = h->producer_pid != exp ? h->producer_pid : 0xFFFFFFFFu;
        for (uint32_t i = 0; i < OWC_SHM_SLOTS; ++i) {
          const OwcShmSlot& s = h->slots[i];
          const int64_t s1 = s.seq;
          MemoryBarrier();
          if (s1 & 1) continue;
          const uint64_t fn = s.frame_number;
          const int64_t q = s.publish_qpc;
          MemoryBarrier();
          if (s.seq != s1 || fn == 0) continue;
          Entry& e = table[(uint32_t)fn % kSize];
          if ((uint64_t)e.fn != (fn & 0xFFFFFFFFull)) {
            InterlockedExchange64(&e.qpc, q);
            InterlockedExchange64(&e.fn, (LONG64)(fn & 0xFFFFFFFFull));
          }
        }
      }
      LARGE_INTEGER d;
      d.QuadPart = -2500;  // 250 us
      if (timer && SetWaitableTimer(timer, &d, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, 50);
      else
        Sleep(1);
    }
    if (timer) CloseHandle(timer);
  }
};

bool producerAliveNow() {
  owc::ShmReader r;
  return r.open() && r.producerAlive();
}

// ---------------------------------------------------------------- device enumeration

struct DeviceInfo {
  std::wstring name, clsid, path;
};

// Returns the moniker of "OWC Webcam" (AddRef'd) or null; fills `all`.
IMoniker* findDevice(std::vector<DeviceInfo>* all) {
  ICreateDevEnum* de = nullptr;
  IMoniker* found = nullptr;
  if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&de)))
    return nullptr;
  IEnumMoniker* em = nullptr;
  if (de->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &em, 0) == S_OK && em) {
    IMoniker* m = nullptr;
    while (em->Next(1, &m, nullptr) == S_OK) {
      DeviceInfo d;
      IPropertyBag* bag = nullptr;
      if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&bag))) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR) d.name = v.bstrVal;
        VariantClear(&v);
        if (SUCCEEDED(bag->Read(L"CLSID", &v, nullptr)) && v.vt == VT_BSTR) d.clsid = v.bstrVal;
        VariantClear(&v);
        if (SUCCEEDED(bag->Read(L"DevicePath", &v, nullptr)) && v.vt == VT_BSTR) d.path = v.bstrVal;
        VariantClear(&v);
        bag->Release();
      }
      if (all) all->push_back(d);
      if (!found && d.name == OWC_VCAM_FRIENDLY_NAME) {
        found = m;
        found->AddRef();
      }
      m->Release();
    }
    em->Release();
  }
  de->Release();
  return found;
}

HMODULE g_dll = nullptr;

HRESULT createFilter(const Options& o, IBaseFilter** out) {
  *out = nullptr;
  if (!o.dll.empty()) {
    if (!g_dll) g_dll = LoadLibraryW(o.dll.c_str());
    if (!g_dll) return HRESULT_FROM_WIN32(GetLastError());
    auto get = (HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**))GetProcAddress(g_dll, "DllGetClassObject");
    if (!get) return E_NOINTERFACE;
    IClassFactory* cf = nullptr;
    HRESULT hr = get(CLSID_OwcVirtualCam, IID_IClassFactory, (void**)&cf);
    if (FAILED(hr)) return hr;
    hr = cf->CreateInstance(nullptr, IID_IBaseFilter, (void**)out);
    cf->Release();
    return hr;
  }
  IMoniker* m = findDevice(nullptr);
  if (!m) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  const HRESULT hr = m->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)out);
  m->Release();
  return hr;
}

IPin* firstPin(IBaseFilter* f) {
  IEnumPins* e = nullptr;
  IPin* p = nullptr;
  if (SUCCEEDED(f->EnumPins(&e)) && e) {
    if (e->Next(1, &p, nullptr) != S_OK) p = nullptr;
    e->Release();
  }
  return p;
}

std::string describe(const AM_MEDIA_TYPE* mt) {
  ParsedType pt;
  char buf[160];
  if (!owc::vcam::parseMediaType(mt, &pt)) return "(unparseable type)";
  std::snprintf(buf, sizeof(buf), "%s %dx%d stride=%d %s %.2f fps, %u bytes", owc::vcam::pixFmtName(pt.fmt),
                pt.width, pt.height, pt.stride,
                owc::vcam::pixFmtIsRgb(pt.fmt) ? (pt.bottomUp ? "bottom-up" : "top-down") : "",
                pt.avgTimePerFrame ? 1e7 / (double)pt.avgTimePerFrame : 0.0, (unsigned)pt.imageBytes);
  return buf;
}

int doList(const Options& o) {
  std::vector<DeviceInfo> all;
  IMoniker* m = findDevice(&all);
  std::printf("Video input devices (CLSID_VideoInputDeviceCategory): %zu\n", all.size());
  for (const DeviceInfo& d : all)
    std::printf("  - \"%s\" clsid=%s%s%s\n", narrow(d.name.c_str()).c_str(), narrow(d.clsid.c_str()).c_str(),
                d.path.empty() ? "" : " path=", narrow(d.path.c_str()).c_str());
  std::printf("OWC Webcam %s via the system device enumerator\n", m ? "FOUND" : "NOT FOUND");
  safeRelease(m);
  IBaseFilter* f = nullptr;
  HRESULT hr = createFilter(o, &f);
  if (FAILED(hr)) {
    std::printf("cannot create the filter (0x%08lX)\n", (unsigned long)hr);
    return 1;
  }
  IPin* pin = firstPin(f);
  IAMStreamConfig* sc = nullptr;
  if (pin) pin->QueryInterface(IID_IAMStreamConfig, (void**)&sc);
  if (sc) {
    int count = 0, size = 0;
    sc->GetNumberOfCapabilities(&count, &size);
    std::printf("IAMStreamConfig: %d capabilities (caps struct %d bytes)\n", count, size);
    for (int i = 0; i < count; ++i) {
      AM_MEDIA_TYPE* mt = nullptr;
      VIDEO_STREAM_CONFIG_CAPS caps;
      if (sc->GetStreamCaps(i, &mt, (BYTE*)&caps) == S_OK && mt) {
        std::printf("  [%2d] %s  (interval %lld..%lld)\n", i, describe(mt).c_str(), caps.MinFrameInterval,
                    caps.MaxFrameInterval);
        owc::vcam::deleteMediaType(mt);
      }
    }
    AM_MEDIA_TYPE* def = nullptr;
    if (SUCCEEDED(sc->GetFormat(&def)) && def) {
      std::printf("GetFormat (default/preferred): %s\n", describe(def).c_str());
      owc::vcam::deleteMediaType(def);
    }
  }
  IKsPropertySet* ks = nullptr;
  if (pin && SUCCEEDED(pin->QueryInterface(IID_IKsPropertySet, (void**)&ks)) && ks) {
    GUID cat = GUID_NULL;
    DWORD ret = 0;
    hr = ks->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, &cat, sizeof(cat), &ret);
    std::printf("pin category: %s\n",
                SUCCEEDED(hr) && cat == PIN_CATEGORY_CAPTURE ? "PIN_CATEGORY_CAPTURE" : "unexpected");
    ks->Release();
  }
  IAMFilterMiscFlags* mf = nullptr;
  if (SUCCEEDED(f->QueryInterface(IID_IAMFilterMiscFlags, (void**)&mf)) && mf) {
    std::printf("misc flags: 0x%lx\n", mf->GetMiscFlags());
    mf->Release();
  }
  safeRelease(sc);
  safeRelease(pin);
  safeRelease(f);
  return 0;
}

// ---------------------------------------------------------------- frame analysis

struct Record {
  int64_t qpc;
  REFERENCE_TIME st, et;
  bool hasTime, disc;
  long len;
  int64_t strip;  // decoded counter, -1 undecodable, -2 not applicable
  bool placeholderLike;
  bool letterboxOk;
};

int lumaAt(const uint8_t* buf, const ParsedType& pt, int x, int y) {
  switch (pt.fmt) {
    case PixFmt::NV12:
    case PixFmt::I420: return buf[(size_t)y * pt.stride + x];
    case PixFmt::YUY2: return buf[(size_t)y * pt.stride + 2 * x];
    case PixFmt::RGB32:
    case PixFmt::RGB24: {
      const int row = pt.bottomUp ? pt.height - 1 - y : y;
      const int bpp = pt.fmt == PixFmt::RGB32 ? 4 : 3;
      const uint8_t* p = buf + (size_t)row * pt.stride + (size_t)x * bpp;
      return (p[0] + 2 * p[1] + p[2]) / 4;
    }
    default: return 0;
  }
}

int64_t decodeStrip(const uint8_t* buf, const ParsedType& pt) {
  if (pt.width < 16 + 32 * 16 || pt.height < 64) return -2;
  uint32_t v = 0;
  const int y = pt.height - 32 + 8;
  for (int i = 0; i < 32; ++i) {
    const int l = lumaAt(buf, pt, 16 + 16 * i + 8, y);
    if (l > 190)
      v |= 1u << i;
    else if (l >= 60)
      return -1;
  }
  return v;
}

bool looksLikePlaceholder(const uint8_t* buf, const ParsedType& pt) {
  long sum = 0, n = 0;
  int mx = 0;
  for (int y = 0; y < pt.height; y += 4)
    for (int x = 0; x < pt.width; x += 4) {
      const int l = lumaAt(buf, pt, x, y);
      sum += l;
      ++n;
      mx = std::max(mx, l);
    }
  return n && sum / n < 70 && mx > 200;
}

struct Stats {
  double mean = 0, sd = 0, p50 = 0, p95 = 0, p99 = 0, mn = 0, mx = 0;
  size_t n = 0;
};

Stats stats(std::vector<double> v) {
  Stats s;
  s.n = v.size();
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (double x : v) sum += x;
  s.mean = sum / (double)v.size();
  double q = 0;
  for (double x : v) q += (x - s.mean) * (x - s.mean);
  s.sd = std::sqrt(q / (double)v.size());
  s.mn = v.front();
  s.mx = v.back();
  auto pct = [&](double p) { return v[std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5))]; };
  s.p50 = pct(0.50);
  s.p95 = pct(0.95);
  s.p99 = pct(0.99);
  return s;
}

uint32_t g_expectedPid = 0;  // producer we expect on the bus during runs (0 = none)

struct RunResult {
  bool setupOk = false;
  bool pass = false;
  bool interfered = false;
  double fps = 0;
};

// ---------------------------------------------------------------- one graph run

RunResult runGraph(const Options& o, PixFmt fmt, double seconds, bool quiet, PublishMonitor* mon) {
  RunResult rr;
  const REFERENCE_TIME wantAvg = o.fps >= 60 ? owc::vcam::kInterval60 : owc::vcam::kInterval30;
  IBaseFilter* src = nullptr;
  IGraphBuilder* graph = nullptr;
  IPin* outPin = nullptr;
  IAMStreamConfig* sc = nullptr;
  probe::SinkFilter* sink = nullptr;
  IMediaControl* mc = nullptr;
  AM_MEDIA_TYPE* chosen = nullptr;
  std::vector<Record> recs;
  recs.reserve((size_t)(seconds * 70.0) + 64);
  std::atomic<size_t> count{0};
  if (mon) mon->resetRun(g_expectedPid);
  ParsedType connType;

  auto fail = [&](const char* what, HRESULT hr) {
    std::printf("  SETUP FAILED: %s (0x%08lX)\n", what, (unsigned long)hr);
  };
  HRESULT hr = createFilter(o, &src);
  if (FAILED(hr)) {
    fail("create filter (is it registered? try --dll)", hr);
    return rr;
  }
  hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&graph);
  if (SUCCEEDED(hr)) hr = graph->AddFilter(src, OWC_VCAM_FRIENDLY_NAME);
  if (SUCCEEDED(hr)) {
    outPin = firstPin(src);
    if (!outPin) hr = E_FAIL;
  }
  if (SUCCEEDED(hr)) hr = outPin->QueryInterface(IID_IAMStreamConfig, (void**)&sc);
  if (FAILED(hr)) {
    fail("graph/pin/IAMStreamConfig", hr);
  } else {
    int nCaps = 0, capSize = 0;
    sc->GetNumberOfCapabilities(&nCaps, &capSize);
    for (int i = 0; i < nCaps && !chosen; ++i) {
      AM_MEDIA_TYPE* mt = nullptr;
      VIDEO_STREAM_CONFIG_CAPS caps;
      if (sc->GetStreamCaps(i, &mt, (BYTE*)&caps) != S_OK || !mt) continue;
      ParsedType pt;
      if (owc::vcam::parseMediaType(mt, &pt) && pt.fmt == fmt && pt.width == o.width && pt.height == o.height &&
          pt.avgTimePerFrame == wantAvg)
        chosen = mt;
      else
        owc::vcam::deleteMediaType(mt);
    }
    if (!chosen) {
      hr = VFW_E_NO_ACCEPTABLE_TYPES;
      fail("requested format not in GetStreamCaps", hr);
    }
  }
  if (SUCCEEDED(hr) && !o.connectDirect) {
    hr = sc->SetFormat(chosen);  // before connection, as OBS/Zoom/Chrome do
    if (FAILED(hr)) fail("IAMStreamConfig::SetFormat", hr);
    AM_MEDIA_TYPE* got = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED(sc->GetFormat(&got)) && got) {
      ParsedType a, b;
      owc::vcam::parseMediaType(got, &a);
      owc::vcam::parseMediaType(chosen, &b);
      if (a.fmt != b.fmt || a.width != b.width || a.height != b.height || a.avgTimePerFrame != b.avgTimePerFrame) {
        hr = E_FAIL;
        fail("GetFormat after SetFormat differs", hr);
      }
      owc::vcam::deleteMediaType(got);
    }
  }
  if (SUCCEEDED(hr)) {
    sink = new probe::SinkFilter(owc::vcam::subtypeFor(fmt), o.width, o.height, o.sinkAllocator,
                                 [&](const probe::SampleInfo& si) {
                                   const size_t i = count.load();
                                   if (i >= recs.capacity()) return;
                                   Record r = {};
                                   r.qpc = si.qpcReceived;
                                   r.hasTime = si.sample->GetTime(&r.st, &r.et) == S_OK;
                                   r.disc = si.sample->IsDiscontinuity() == S_OK;
                                   r.len = si.sample->GetActualDataLength();
                                   BYTE* p = nullptr;
                                   si.sample->GetPointer(&p);
                                   r.strip = -2;
                                   if (p) {
                                     r.placeholderLike = looksLikePlaceholder(p, connType);
                                     r.strip = r.placeholderLike ? -2 : decodeStrip(p, connType);
                                     r.letterboxOk = true;
                                     const owc::vcam::Rect fit =
                                         owc::vcam::fitRect(o.pw, o.ph, connType.width, connType.height);
                                     if (fit.y > 2)
                                       r.letterboxOk = lumaAt(p, connType, connType.width / 2, 0) <= 20 &&
                                                       lumaAt(p, connType, connType.width / 2, connType.height - 1) <= 20;
                                     else if (fit.x > 2)
                                       r.letterboxOk = lumaAt(p, connType, 0, connType.height / 2) <= 20 &&
                                                       lumaAt(p, connType, connType.width - 1, connType.height / 2) <= 20;
                                   }
                                   recs.push_back(r);
                                   count.store(i + 1);
                                 });
    hr = graph->AddFilter(sink, L"Probe Sink");
    if (SUCCEEDED(hr)) {
      const int64_t c0 = owc::qpcNow();
      hr = graph->ConnectDirect(outPin, sink->pin(), o.connectDirect ? chosen : nullptr);
      if (!quiet) std::printf("  connect: %.2f ms\n", qpcMs(owc::qpcNow() - c0));
    }
    if (FAILED(hr)) fail("ConnectDirect", hr);
  }
  if (SUCCEEDED(hr)) {
    AM_MEDIA_TYPE cmt = {};
    hr = outPin->ConnectionMediaType(&cmt);
    if (SUCCEEDED(hr)) {
      owc::vcam::parseMediaType(&cmt, &connType);
      if (!quiet) std::printf("  connected: %s\n", describe(&cmt).c_str());
      owc::vcam::freeMediaType(cmt);
    }
  }
  IOwcVcamStats* vs = nullptr;
  if (SUCCEEDED(hr)) {
    src->QueryInterface(__uuidof(IOwcVcamStats), (void**)&vs);
    hr = graph->QueryInterface(IID_IMediaControl, (void**)&mc);
  }
  if (SUCCEEDED(hr)) {
    hr = mc->Run();
    if (FAILED(hr)) fail("IMediaControl::Run", hr);
  }
  if (SUCCEEDED(hr)) {
    rr.setupOk = true;
    Sleep((DWORD)(seconds * 1000.0));
    OAFilterState st;
    mc->GetState(0, &st);
    const int64_t s0 = owc::qpcNow();
    mc->Stop();
    if (!quiet) std::printf("  stop: %.2f ms\n", qpcMs(owc::qpcNow() - s0));
  }
  OwcVcamStats fst = {};
  if (vs) {
    fst.cbSize = sizeof(fst);
    vs->GetStats(&fst);
  }
  safeRelease(vs);
  safeRelease(mc);
  if (chosen) owc::vcam::deleteMediaType(chosen);
  safeRelease(sc);
  safeRelease(outPin);
  if (graph && sink) graph->RemoveFilter(sink);
  if (graph && src) graph->RemoveFilter(src);
  safeRelease(graph);
  if (sink) sink->Release();
  safeRelease(src);
  if (!rr.setupOk) return rr;

  // ---- analysis
  const size_t n = count.load();
  std::vector<double> intervals, lat, pubIv;
  size_t tsBad = 0, durBad = 0, laterDisc = 0, lenBad = 0, decOk = 0, decFail = 0, back = 0, repeats = 0,
         gaps = 0, phLike = 0, lbBad = 0, noTime = 0;
  int64_t prevStrip = -1;
  REFERENCE_TIME prevSt = LLONG_MIN;
  for (size_t i = 0; i < n; ++i) {
    const Record& r = recs[i];
    if (i) intervals.push_back(qpcMs(r.qpc - recs[i - 1].qpc));
    if (!r.hasTime) {
      ++noTime;
    } else {
      if (prevSt != LLONG_MIN && r.st <= prevSt) ++tsBad;
      if (connType.avgTimePerFrame && r.et - r.st != connType.avgTimePerFrame) ++durBad;
      prevSt = r.st;
    }
    if (i && r.disc) ++laterDisc;
    if ((size_t)r.len != connType.imageBytes) ++lenBad;
    if (r.placeholderLike) ++phLike;
    if (!r.letterboxOk) ++lbBad;
    if (r.strip >= 0) {
      ++decOk;
      if (prevStrip >= 0) {
        if (r.strip < prevStrip)
          ++back;
        else if (r.strip == prevStrip)
          ++repeats;
        else
          gaps += (size_t)(r.strip - prevStrip - 1);
      }
      if (r.strip != prevStrip && mon) {
        int64_t pq;
        if (mon->lookup((uint32_t)r.strip, &pq)) lat.push_back(qpcMs(r.qpc - pq));
      }
      prevStrip = r.strip;
    } else if (r.strip == -1) {
      ++decFail;
    }
  }
  // producer side: publish intervals of the frames published during the run
  if (mon) {
    int64_t first = -1, last = -1;
    for (size_t i = 0; i < n; ++i)
      if (recs[i].strip >= 0) {
        if (first < 0) first = recs[i].strip;
        last = recs[i].strip;
      }
    int64_t prevQ = 0;
    for (int64_t fn = first; first >= 0 && fn <= last; ++fn) {
      int64_t q;
      if (!mon->lookup((uint32_t)fn, &q)) {
        prevQ = 0;
        continue;
      }
      if (prevQ) pubIv.push_back(qpcMs(q - prevQ));
      prevQ = q;
    }
  }
  const double span = n > 1 ? qpcMs(recs[n - 1].qpc - recs[0].qpc) / 1000.0 : 0;
  rr.fps = span > 0 ? (double)(n - 1) / span : 0;
  const Stats iv = stats(intervals);
  const Stats lt = stats(lat);
  const double nominal = 1e7 / (double)(connType.avgTimePerFrame ? connType.avgTimePerFrame : wantAvg);
  const bool sameSize = connType.width == o.pw && connType.height == o.ph;

  // +-3%, but at least 1.5 frames over the window (short runs: one early frame is already >3%)
  const double fpsTol = std::max(0.03, span > 0 ? 1.5 / (span * nominal) : 1.0);
  bool pass = n > 2 && std::fabs(rr.fps - nominal) / nominal < fpsTol && tsBad == 0 && durBad == 0 && lenBad == 0 &&
              laterDisc == 0 && recs[0].disc && noTime == 0;
  if (o.expect == Expect::Live) {
    if (sameSize) pass = pass && decOk == n && back == 0 && lat.size() > 0;
    else pass = pass && phLike == 0 && lbBad == 0;
  } else if (o.expect == Expect::Placeholder) {
    pass = pass && phLike == n;
  }
  rr.pass = pass;
  const uint32_t foreign = mon ? mon->foreignPid.load() : 0;
  rr.interfered = foreign != 0;

  std::printf("  samples=%zu over %.2fs -> %.3f fps (nominal %.3f)\n", n, span, rr.fps, nominal);
  std::printf("  interval ms: mean %.3f sd %.3f min %.3f p50 %.3f p99 %.3f max %.3f\n", iv.mean, iv.sd, iv.mn,
              iv.p50, iv.p99, iv.mx);
  std::printf("  timestamps: first discontinuity=%s, later discontinuities=%zu, non-increasing=%zu, "
              "bad duration=%zu, untimed=%zu; bad lengths=%zu\n",
              n && recs[0].disc ? "yes" : "NO", laterDisc, tsBad, durBad, noTime, lenBad);
  if (sameSize)
    std::printf("  content: strip decoded %zu/%zu (undecodable %zu), counter backwards=%zu, repeats=%zu, "
                "skipped source frames=%zu, placeholder-like=%zu\n",
                decOk, n, decFail, back, repeats, gaps, phLike);
  else
    std::printf("  content: scaled output (no strip check), placeholder-like=%zu/%zu, letterbox/pillarbox bars "
                "wrong=%zu\n",
                phLike, n, lbBad);
  if (!pubIv.empty()) {
    const Stats pv = stats(pubIv);
    std::printf("  producer publish interval ms: mean %.3f sd %.3f min %.3f max %.3f (source-side jitter)\n", pv.mean,
                pv.sd, pv.mn, pv.mx);
  }
  if (!lat.empty())
    std::printf("  latency publish->receipt ms (n=%zu): mean %.3f p50 %.3f p95 %.3f max %.3f min %.3f\n", lt.n,
                lt.mean, lt.p50, lt.p95, lt.mx, lt.mn);
  if (fst.cbSize)
    std::printf("  filter: delivered=%llu fresh=%llu repeated=%llu placeholder=%llu skippedSrc=%llu "
                "recvFail=%llu convert avg %.3f ms max %.3f ms (src %ux%u) alloc %ux%u B typeChanges=%u\n",
                (unsigned long long)fst.delivered, (unsigned long long)fst.freshFrames,
                (unsigned long long)fst.repeatedFrames, (unsigned long long)fst.placeholderFrames,
                (unsigned long long)fst.skippedSource, (unsigned long long)fst.receiveFailures, fst.convertAvgMs,
                fst.convertMaxMs, fst.srcWidth, fst.srcHeight, fst.allocBuffers, fst.allocBufferBytes,
                fst.mediaTypeChanges);
  if (rr.interfered)
    std::printf("  RESULT: INCONCLUSIVE - foreign producer pid %u published on the shm bus during the run (%s)\n",
                foreign, pass ? "checks passed anyway" : "checks failed");
  else
    std::printf("  RESULT: %s\n", pass ? "PASS" : "FAIL");
  return rr;
}

// ---------------------------------------------------------------- leak cycles

struct ProcSnap {
  DWORD handles = 0, threads = 0;
  SIZE_T priv = 0;
};

ProcSnap snap() {
  ProcSnap s;
  GetProcessHandleCount(GetCurrentProcess(), &s.handles);
  PROCESS_MEMORY_COUNTERS_EX pm = {};
  pm.cb = sizeof(pm);
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm))) s.priv = pm.PrivateUsage;
  HANDLE th = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (th != INVALID_HANDLE_VALUE) {
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(th, &te); ok; ok = Thread32Next(th, &te))
      if (te.th32OwnerProcessID == GetCurrentProcessId()) ++s.threads;
    CloseHandle(th);
  }
  return s;
}

// Connect with format A, run, stop, then IAMStreamConfig::SetFormat(B) while
// connected (the filter must reconnect through IFilterGraph2::ReconnectEx),
// run again and verify the samples now carry format B.
AM_MEDIA_TYPE* findCaps(IAMStreamConfig* sc, PixFmt f, int w, int h, REFERENCE_TIME avg) {
  int n = 0, sz = 0;
  sc->GetNumberOfCapabilities(&n, &sz);
  for (int i = 0; i < n; ++i) {
    AM_MEDIA_TYPE* mt = nullptr;
    VIDEO_STREAM_CONFIG_CAPS caps;
    if (sc->GetStreamCaps(i, &mt, (BYTE*)&caps) != S_OK || !mt) continue;
    ParsedType pt;
    if (owc::vcam::parseMediaType(mt, &pt) && pt.fmt == f && pt.width == w && pt.height == h &&
        pt.avgTimePerFrame == avg)
      return mt;
    owc::vcam::deleteMediaType(mt);
  }
  return nullptr;
}

int doReformat(const Options& o) {
  struct Phase {
    PixFmt f;
    int w, h;
  };
  const Phase phases[2] = {{PixFmt::NV12, 1280, 720}, {PixFmt::RGB32, 640, 360}};
  std::atomic<long> samples{0}, goodLen{0};
  std::atomic<size_t> expectBytes{0};
  IBaseFilter* src = nullptr;
  IGraphBuilder* g = nullptr;
  IPin* pin = nullptr;
  IAMStreamConfig* sc = nullptr;
  IMediaControl* mc = nullptr;
  int rc = 0;
  HRESULT hr = createFilter(o, &src);
  if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&g);
  if (SUCCEEDED(hr)) hr = g->AddFilter(src, OWC_VCAM_FRIENDLY_NAME);
  if (SUCCEEDED(hr)) pin = firstPin(src);
  if (SUCCEEDED(hr) && pin) hr = pin->QueryInterface(IID_IAMStreamConfig, (void**)&sc);
  if (SUCCEEDED(hr) && sc) hr = g->QueryInterface(IID_IMediaControl, (void**)&mc);
  if (FAILED(hr) || !pin || !sc || !mc) {
    std::printf("reformat: setup failed 0x%08lX\n", (unsigned long)hr);
    safeRelease(mc); safeRelease(sc); safeRelease(pin); safeRelease(g); safeRelease(src);
    return 1;
  }
  // accept any OWC type, so the sink's QueryAccept allows the change
  probe::SinkFilter* sink = new probe::SinkFilter(GUID_NULL, 0, 0, false, [&](const probe::SampleInfo& si) {
    ++samples;
    if ((size_t)si.sample->GetActualDataLength() == expectBytes.load()) ++goodLen;
  });
  g->AddFilter(sink, L"Probe Sink");
  for (int i = 0; i < 2 && rc == 0; ++i) {
    const Phase& ph = phases[i];
    AM_MEDIA_TYPE* mt = findCaps(sc, ph.f, ph.w, ph.h, owc::vcam::kInterval30);
    if (!mt) { rc = 1; break; }
    hr = sc->SetFormat(mt);
    std::printf("phase %d: SetFormat(%s) %s -> 0x%08lX\n", i + 1, describe(mt).c_str(),
                i == 0 ? "before connection" : "while connected (expects ReconnectEx)", (unsigned long)hr);
    owc::vcam::deleteMediaType(mt);
    if (FAILED(hr)) { rc = 1; break; }
    if (i == 0) {
      hr = g->ConnectDirect(pin, sink->pin(), nullptr);
      if (FAILED(hr)) { std::printf("  ConnectDirect failed 0x%08lX\n", (unsigned long)hr); rc = 1; break; }
    }
    AM_MEDIA_TYPE cmt = {};
    ParsedType pt;
    if (FAILED(pin->ConnectionMediaType(&cmt)) || !owc::vcam::parseMediaType(&cmt, &pt) || pt.fmt != ph.f ||
        pt.width != ph.w || pt.height != ph.h) {
      std::printf("  connection type is NOT the requested one: %s\n", describe(&cmt).c_str());
      owc::vcam::freeMediaType(cmt);
      rc = 1;
      break;
    }
    std::printf("  connected: %s\n", describe(&cmt).c_str());
    owc::vcam::freeMediaType(cmt);
    expectBytes = pt.imageBytes;
    samples = 0;
    goodLen = 0;
    mc->Run();
    Sleep(1500);
    mc->Stop();
    std::printf("  samples=%ld with the expected size=%ld\n", samples.load(), goodLen.load());
    if (samples.load() < 30 || goodLen.load() != samples.load()) rc = 1;
  }
  g->RemoveFilter(sink);
  g->RemoveFilter(src);
  sink->Release();
  safeRelease(mc); safeRelease(sc); safeRelease(pin); safeRelease(g); safeRelease(src);
  std::printf("reformat: %s\n", rc ? "FAIL" : "PASS");
  return rc;
}

int doCycles(const Options& o, PublishMonitor* mon) {
  std::vector<ProcSnap> s;
  s.push_back(snap());
  int fails = 0;
  for (int i = 0; i < o.cycles; ++i) {
    const PixFmt f = o.formats[(size_t)i % o.formats.size()];
    std::printf("cycle %d/%d (%s)\n", i + 1, o.cycles, owc::vcam::pixFmtName(f));
    const RunResult r = runGraph(o, f, o.seconds, true, mon);
    if (!r.setupOk || (!r.pass && !r.interfered)) ++fails;
    s.push_back(snap());
    std::printf("  process: handles=%lu threads=%lu private=%.1f MB\n", s.back().handles, s.back().threads,
                (double)s.back().priv / 1048576.0);
  }
  const ProcSnap& a = s[std::min<size_t>(2, s.size() - 1)];
  const ProcSnap& b = s.back();
  std::printf("leak check: after cycle 2 -> after cycle %d: handles %lu -> %lu (%+ld), threads %lu -> %lu, "
              "private %.2f -> %.2f MB (%+.2f MB)\n",
              o.cycles, a.handles, b.handles, (long)b.handles - (long)a.handles, a.threads, b.threads,
              (double)a.priv / 1048576.0, (double)b.priv / 1048576.0,
              ((double)b.priv - (double)a.priv) / 1048576.0);
  HMODULE m = GetModuleHandleW(L"owc_virtualcam.dll");
  if (m) {
    auto canUnload = (HRESULT(STDAPICALLTYPE*)())GetProcAddress(m, "DllCanUnloadNow");
    const HRESULT hr = canUnload ? canUnload() : E_FAIL;
    std::printf("DllCanUnloadNow() after teardown: %s\n", hr == S_OK ? "S_OK (no live objects)" : "S_FALSE (LEAK)");
    if (hr != S_OK) ++fails;
    if (o.dll.empty()) {
      CoFreeUnusedLibrariesEx(0, 0);
      std::printf("after CoFreeUnusedLibrariesEx(0): owc_virtualcam.dll %s\n",
                  GetModuleHandleW(L"owc_virtualcam.dll") ? "still loaded" : "unloaded");
    }
  }
  std::printf("cycles: %d failed of %d\n", fails, o.cycles);
  return fails ? 1 : 0;
}

// ---------------------------------------------------------------- benchmark

int doBench() {
  char cpu[128] = "?";
  DWORD sz = sizeof(cpu);
  RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
               RRF_RT_REG_SZ, nullptr, cpu, &sz);
  std::printf("CPU: %s (single thread, SSE2 build)\n", cpu);
  const int srcSizes[][2] = {{1280, 720}, {1920, 1080}};
  const int outSizes[][2] = {{1920, 1080}, {1280, 720}, {960, 540}, {640, 480}, {640, 360}};
  std::printf("per-frame ms, median (min) of 60 iterations; 'fresh' = new source frame (scale+convert),\n"
              "'repeat' = same frame again (scaled intermediate reused, convert only)\n");
  std::printf("%-10s %-10s %-6s %18s %18s\n", "source", "output", "format", "fresh", "repeat");
  for (const auto& ss : srcSizes) {
    const int W = ss[0], H = ss[1];
    std::vector<uint8_t> src((size_t)W * H * 3 / 2);
    uint32_t seed = 12345;
    for (size_t i = 0; i < src.size(); ++i) {
      seed = seed * 1664525u + 1013904223u;
      src[i] = (uint8_t)((i % (size_t)W) * 255 / (size_t)W ^ (seed >> 28));
    }
    owc::vcam::Nv12View v;
    v.y = src.data();
    v.uv = src.data() + (size_t)W * H;
    v.width = W;
    v.height = H;
    v.yStride = v.uvStride = W;
    for (const auto& os : outSizes)
      for (int f = 0; f < (int)PixFmt::Count; ++f) {
        owc::vcam::OutputDesc d;
        d.fmt = (PixFmt)f;
        d.width = os[0];
        d.height = os[1];
        d.stride = owc::vcam::defaultStride(d.fmt, d.width);
        d.bottomUp = owc::vcam::pixFmtIsRgb(d.fmt);
        std::vector<uint8_t> dst(owc::vcam::outputBytes(d));
        owc::vcam::Converter conv;
        const int iters = 60;
        for (int i = 0; i < 5; ++i) conv.convert(v, 1000000 + (uint64_t)i, dst.data(), dst.size(), d);
        std::vector<double> tf, tr;
        for (int i = 0; i < iters; ++i) {
          const int64_t t0 = owc::qpcNow();
          conv.convert(v, (uint64_t)i, dst.data(), dst.size(), d);
          tf.push_back(qpcMs(owc::qpcNow() - t0));
        }
        for (int i = 0; i < iters; ++i) {
          const int64_t t0 = owc::qpcNow();
          conv.convert(v, 7, dst.data(), dst.size(), d);
          tr.push_back(qpcMs(owc::qpcNow() - t0));
        }
        const Stats sf = stats(tf), sr = stats(tr);
        char a[32], b[32], c[32], e[32];
        std::snprintf(a, sizeof(a), "%dx%d", W, H);
        std::snprintf(b, sizeof(b), "%dx%d", os[0], os[1]);
        std::snprintf(c, sizeof(c), "%.3f (%.3f)", sf.p50, sf.mn);
        std::snprintf(e, sizeof(e), "%.3f (%.3f)", sr.p50, sr.mn);
        std::printf("%-10s %-10s %-6s %18s %18s\n", a, b, owc::vcam::pixFmtName(d.fmt), c, e);
      }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpcFreq = f.QuadPart;
  setvbuf(stdout, nullptr, _IONBF, 0);
  Options o;
  if (!parseArgs(argc, argv, o)) {
    usage();
    return 2;
  }
  if (o.bench) return doBench();

  Producer prod;
  if (o.produce) {
    if (producerAliveNow()) {
      std::fprintf(stderr, "a producer is already publishing on the shm bus; not starting --produce\n");
      return 2;
    }
    g_expectedPid = GetCurrentProcessId();
    prod.start(o.pw, o.ph, o.pfps);
    Sleep(200);
    if (!prod.ok) return 2;
    std::printf("in-process producer: %dx%d@%d\n", o.pw, o.ph, o.pfps);
    if (o.produceOnly) {
      Sleep((DWORD)(o.seconds * 1000.0));
      prod.finish();
      return 0;
    }
  } else {
    owc::ShmReader r;
    if (r.open() && r.producerAlive()) {
      o.pw = (int)r.header()->nominal_width;
      o.ph = (int)r.header()->nominal_height;
      std::printf("external producer live: %dx%d @ %u/%u\n", o.pw, o.ph, r.header()->nominal_fps_num,
                  r.header()->nominal_fps_den);
      g_expectedPid = r.header()->producer_pid;
    } else {
      std::printf("no producer on the shm bus (placeholder expected)\n");
    }
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
  int rc = 0;
  PublishMonitor mon;
  mon.start();
  if (o.list) rc |= doList(o);
  if (o.run) {
    for (PixFmt fmt : o.formats) {
      std::printf("run: %s %dx%d @%d fps for %.1fs (%s, sink allocator %s)\n", owc::vcam::pixFmtName(fmt), o.width,
                  o.height, o.fps, o.seconds, o.connectDirect ? "ConnectDirect with type" : "SetFormat + Connect",
                  o.sinkAllocator ? "offered" : "not offered");
      const RunResult r = runGraph(o, fmt, o.seconds, false, &mon);
      if (r.interfered)
        rc |= 4;
      else if (!r.setupOk || !r.pass)
        rc |= 1;
    }
  }
  if (o.reformat) rc |= doReformat(o);
  if (o.cycles > 0) rc |= doCycles(o, &mon);
  mon.finish();
  if (o.produce) prod.finish();
  CoUninitialize();
  return rc;
}
