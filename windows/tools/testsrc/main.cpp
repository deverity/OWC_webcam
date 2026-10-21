// owc_testsrc: synthetic NV12 producer for the OWC shared-memory frame bus.
// Publishes BT.709 limited-range color bars, a moving gray bar and a 32-bit
// frame-counter strip at the bottom. The strip encoding is documented in
// README.md and must stay in sync with owc_shmview.

#include <owc/shm.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI onConsoleCtrl(DWORD) {
  g_stop.store(true);
  return TRUE;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// BT.709 limited-range {Y, U, V} of the 8 bars: white, yellow, cyan, green,
// magenta, red, blue, black. Derived from RGB with
//   Y = 16 + 219 * (0.2126 R + 0.7152 G + 0.0722 B)
//   Cb = 128 + 224 * (B' - Y') / 1.8556,  Cr = 128 + 224 * (R' - Y') / 1.5748
struct Yuv {
  uint8_t y, u, v;
};
constexpr Yuv kBars[8] = {
    {235, 128, 128},  // white
    {219, 16, 138},   // yellow
    {188, 154, 16},   // cyan
    {173, 42, 26},    // green
    {78, 214, 230},   // magenta
    {63, 102, 240},   // red
    {32, 240, 118},   // blue
    {16, 128, 128},   // black
};

constexpr uint32_t kStripX0 = 16;
constexpr uint32_t kStripBlock = 16;
constexpr uint32_t kStripBlocks = 32;
constexpr uint32_t kStripRows = 32;  // bottom rows reserved for the strip area
constexpr uint32_t kMinWidth = kStripX0 + kStripBlocks * kStripBlock;  // 528
constexpr uint32_t kMinHeight = 64;
constexpr uint32_t kGrayWidth = 32;
constexpr uint8_t kGrayY = 180;

struct Options {
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t fps = 30;
  double seconds = 0.0;  // 0 = run until Ctrl+C
};

void usage() {
  std::fprintf(stderr, "usage: owc_testsrc [--width W] [--height H] [--fps F] [--seconds S]\n");
}

bool parseU32(const char* s, uint32_t& out) {
  char* end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (end == s || *end != '\0' || errno != 0 || v > 0xFFFFFFFFull) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

bool parseArgs(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    const bool hasVal = (i + 1 < argc);
    if (hasVal && std::strcmp(a, "--width") == 0) {
      if (!parseU32(argv[++i], o.width)) return false;
    } else if (hasVal && std::strcmp(a, "--height") == 0) {
      if (!parseU32(argv[++i], o.height)) return false;
    } else if (hasVal && std::strcmp(a, "--fps") == 0) {
      if (!parseU32(argv[++i], o.fps)) return false;
    } else if (hasVal && std::strcmp(a, "--seconds") == 0) {
      char* end = nullptr;
      o.seconds = std::strtod(argv[++i], &end);
      if (end == argv[i] || *end != '\0' || !(o.seconds >= 0.0)) return false;
    } else {
      return false;
    }
  }
  if (o.width < kMinWidth || o.width > OWC_SHM_MAX_WIDTH || (o.width & 1u)) {
    std::fprintf(stderr, "error: --width must be even, %u..%u\n", kMinWidth, OWC_SHM_MAX_WIDTH);
    return false;
  }
  if (o.height < kMinHeight || o.height > OWC_SHM_MAX_HEIGHT || (o.height & 1u)) {
    std::fprintf(stderr, "error: --height must be even, %u..%u\n", kMinHeight, OWC_SHM_MAX_HEIGHT);
    return false;
  }
  if (o.fps == 0 || o.fps > 240) {
    std::fprintf(stderr, "error: --fps must be 1..240\n");
    return false;
  }
  return true;
}

// Sleeps `ticks` QPC ticks. Uses the waitable timer if available, else Sleep.
void sleepTicks(HANDLE timer, int64_t ticks, int64_t freq) {
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -((ticks * 10000000LL) / freq);  // negative = relative, 100 ns units
    if (due.QuadPart == 0) due.QuadPart = -1;
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
  } else {
    Sleep(static_cast<DWORD>(std::max<int64_t>(1, ticks * 1000 / freq)));
  }
}

// Waits until the absolute QPC time `target`. While idle it refreshes the
// producer heartbeat at least every 50 ms, so consumers never see "no signal".
void waitUntil(HANDLE timer, owc::ShmWriter& w, int64_t target, int64_t freq) {
  for (;;) {
    if (g_stop.load()) return;
    const int64_t remain = target - owc::qpcNow();
    if (remain <= 0) return;
    const int64_t chunk = std::min<int64_t>(remain, freq / 20);
    sleepTicks(timer, chunk, freq);
    w.heartbeat();
  }
}

struct Frame {
  std::vector<uint8_t> y;
  std::vector<uint8_t> uv;
  std::vector<uint8_t> barAt;  // bar index for each luma column
};

// Builds one NV12 frame. `n` is the producer's own frame index (drives the gray
// bar); `fn` is the frame_number the bus will assign (encoded in the strip).
void buildFrame(Frame& f, uint32_t W, uint32_t H, uint64_t n, uint32_t fn) {
  const uint32_t activeH = H - kStripRows;
  const uint32_t grayX = static_cast<uint32_t>((n * 8) % W);
  uint8_t* Y = f.y.data();
  uint8_t* UV = f.uv.data();

  // Luma: bars + gray bar above the strip area, black inside it.
  for (uint32_t y = 0; y < activeH; ++y) {
    uint8_t* row = Y + static_cast<size_t>(y) * W;
    for (uint32_t x = 0; x < W; ++x) row[x] = kBars[f.barAt[x]].y;
    for (uint32_t c = 0; c < kGrayWidth; ++c) row[(grayX + c) % W] = kGrayY;
  }
  for (uint32_t y = activeH; y < H; ++y) std::memset(Y + static_cast<size_t>(y) * W, 16, W);

  // Frame-counter strip: block i (16x16 at x = 16 + 16*i, y = H-32) is white
  // when bit i of fn is 1, black when 0.
  for (uint32_t i = 0; i < kStripBlocks; ++i) {
    const uint8_t v = ((fn >> i) & 1u) ? 235 : 16;
    const uint32_t x0 = kStripX0 + i * kStripBlock;
    for (uint32_t y = H - kStripRows; y < H - kStripRows + kStripBlock; ++y)
      std::memset(Y + static_cast<size_t>(y) * W + x0, v, kStripBlock);
  }

  // Chroma (interleaved U,V pairs, stride W). Chroma row r covers luma rows 2r, 2r+1.
  const uint32_t chromaRows = H / 2;
  const uint32_t activeC = activeH / 2;
  for (uint32_t r = 0; r < chromaRows; ++r) {
    uint8_t* row = UV + static_cast<size_t>(r) * W;
    if (r >= activeC) {  // strip area and black band: neutral chroma
      std::memset(row, 128, W);
      continue;
    }
    for (uint32_t cx = 0; cx < W / 2; ++cx) {
      const Yuv& b = kBars[f.barAt[cx * 2]];
      row[2 * cx] = b.u;
      row[2 * cx + 1] = b.v;
    }
    for (uint32_t c = 0; c < kGrayWidth; c += 2) {
      const uint32_t x = (grayX + c) % W;  // even, so x+1 is the V sample of the same pair
      row[x] = 128;
      row[x + 1] = 128;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!parseArgs(argc, argv, o)) {
    usage();
    return 2;
  }
  const uint32_t W = o.width;
  const uint32_t H = o.height;
  const uint32_t F = o.fps;

  LARGE_INTEGER fq;
  QueryPerformanceFrequency(&fq);
  const int64_t freq = fq.QuadPart;

  owc::ShmWriter writer;
  if (!writer.open()) {
    std::fprintf(stderr, "error: cannot create shared memory (GetLastError=%lu)\n", GetLastError());
    return 1;
  }
  writer.setActive(true, W, H, F, 1);
  SetConsoleCtrlHandler(onConsoleCtrl, TRUE);

  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  if (!timer) {
    timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    std::printf("high-resolution timer unavailable, using %s\n",
                timer ? "normal waitable timer" : "Sleep()");
  }

  Frame f;
  f.y.resize(static_cast<size_t>(W) * H);
  f.uv.resize(static_cast<size_t>(W) * (H / 2));
  f.barAt.resize(W);
  for (uint32_t x = 0; x < W; ++x) f.barAt[x] = static_cast<uint8_t>(x * 8 / W);

  std::printf("owc_testsrc %ux%u@%u NV12 (Ctrl+C to stop%s)\n", W, H, F,
              o.seconds > 0 ? ", or after --seconds" : "");

  const int64_t runTicks = static_cast<int64_t>(o.seconds * static_cast<double>(freq));
  int64_t start = owc::qpcNow();
  uint64_t n = 0;  // frame index on the current schedule
  uint64_t published = 0;
  int64_t statusT = start;
  uint64_t statusFrames = 0;

  while (!g_stop.load()) {
    // Absolute schedule: frame n is due at start + n/F, so the average rate is exact.
    const int64_t target = start + static_cast<int64_t>(n) * freq / static_cast<int64_t>(F);
    if (o.seconds > 0 && target - start >= runTicks) break;

    waitUntil(timer, writer, target, freq);
    if (g_stop.load()) break;

    const int64_t now = owc::qpcNow();
    if (now - target > freq) {  // stalled for over a second: rebase instead of bursting
      start = now;
      n = 0;
      continue;
    }

    const uint32_t fn = static_cast<uint32_t>(static_cast<uint64_t>(writer.header()->latest_frame_number) + 1);
    buildFrame(f, W, H, n, fn);
    writer.publishNV12(f.y.data(), W, f.uv.data(), W, W, H);
    ++n;
    ++published;
    ++statusFrames;

    const int64_t t = owc::qpcNow();
    if (t - statusT >= freq) {
      const double elapsed = static_cast<double>(t - statusT) / static_cast<double>(freq);
      std::printf("frames=%llu fps=%.2f\n", published, static_cast<double>(statusFrames) / elapsed);
      statusT = t;
      statusFrames = 0;
    }
  }

  writer.setActive(false);
  writer.close();
  if (timer) CloseHandle(timer);
  SetConsoleCtrlHandler(onConsoleCtrl, FALSE);
  std::printf("stopped: %llu frames published\n", published);
  return 0;
}
