// owc_shmview: consumer for the OWC shared-memory frame bus. Polls for the newest
// frame every ~1 ms, verifies the frame-counter strip written by owc_testsrc
// (see windows/tools/testsrc/README.md) and prints one status line per second.

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

constexpr uint32_t kStripX0 = 16;
constexpr uint32_t kStripBlock = 16;
constexpr uint32_t kStripBlocks = 32;
constexpr uint32_t kStripRows = 32;
constexpr uint32_t kMinWidth = kStripX0 + kStripBlocks * kStripBlock;  // 528
constexpr uint32_t kMinHeight = 64;

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

// Decodes the frame-counter strip from a tightly packed NV12 frame (stride == w).
// Samples the center pixel of each 16x16 block; Y >= 128 is a 1 bit (LSB first).
uint32_t decodeStrip(const uint8_t* y, uint32_t w, uint32_t h) {
  const uint32_t cy = h - kStripRows + kStripBlock / 2;
  uint32_t v = 0;
  for (uint32_t i = 0; i < kStripBlocks; ++i) {
    const uint32_t cx = kStripX0 + i * kStripBlock + kStripBlock / 2;
    if (y[static_cast<size_t>(cy) * w + cx] >= 128) v |= 1u << i;
  }
  return v;
}

bool parseSeconds(int argc, char** argv, double& seconds) {
  for (int i = 1; i < argc; ++i) {
    if (i + 1 < argc && std::strcmp(argv[i], "--seconds") == 0) {
      char* end = nullptr;
      seconds = std::strtod(argv[++i], &end);
      if (end == argv[i] || *end != '\0' || !(seconds >= 0.0)) return false;
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  double seconds = 0.0;
  if (!parseSeconds(argc, argv, seconds)) {
    std::fprintf(stderr, "usage: owc_shmview [--seconds S]\n");
    return 2;
  }

  LARGE_INTEGER fq;
  QueryPerformanceFrequency(&fq);
  const int64_t freq = fq.QuadPart;

  SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

  const int64_t pollTicks = freq / 1000;   // 1 ms
  const int64_t retryTicks = freq / 2;     // 500 ms
  const int64_t runTicks = static_cast<int64_t>(seconds * static_cast<double>(freq));
  const int64_t start = owc::qpcNow();

  owc::ShmReader reader;
  std::vector<uint8_t> buf(OWC_SHM_SLOT_BYTES);  // 3840x2160 NV12 max

  uint64_t lastSeen = 0;
  uint64_t framesTotal = 0, framesSec = 0;
  uint64_t stripOk = 0, stripMismatch = 0, missed = 0;
  double latSum = 0.0, latMax = 0.0;
  uint64_t latCount = 0;
  int64_t nextRetry = 0;
  int64_t secStart = start;

  while (!g_stop.load()) {
    const int64_t now = owc::qpcNow();
    if (seconds > 0 && now - start >= runTicks) break;

    if (!reader.isOpen()) {
      if (now >= nextRetry) {
        if (reader.open()) {
          std::printf("producer found, attaching\n");
          lastSeen = 0;
        } else {
          std::printf("waiting for producer\n");
          nextRetry = now + retryTicks;
        }
      }
      sleepTicks(timer, pollTicks, freq);
      continue;
    }

    const OwcShmHeader* hd = reader.header();
    const int64_t hf = hd->qpc_frequency > 0 ? hd->qpc_frequency : freq;

    owc::ShmReader::FrameInfo info;
    if (reader.readLatest(lastSeen, buf.data(), buf.size(), info)) {
      if (lastSeen != 0 && info.frame_number > lastSeen + 1) missed += info.frame_number - lastSeen - 1;
      lastSeen = info.frame_number;
      ++framesTotal;
      ++framesSec;

      if (info.width >= kMinWidth && info.height >= kMinHeight) {
        const uint32_t v = decodeStrip(buf.data(), info.width, info.height);
        if (v == static_cast<uint32_t>(info.frame_number)) ++stripOk;
        else ++stripMismatch;
      } else {
        ++stripMismatch;
      }

      const double ms = static_cast<double>(owc::qpcNow() - info.publish_qpc) * 1000.0 / static_cast<double>(hf);
      latSum += ms;
      latMax = std::max(latMax, ms);
      ++latCount;
    }

    const int64_t t = owc::qpcNow();
    if (t - secStart >= freq) {
      const double elapsed = static_cast<double>(t - secStart) / static_cast<double>(hf);
      const bool alive = reader.producerAlive();
      const uint32_t den = hd->nominal_fps_den ? hd->nominal_fps_den : 1u;
      std::printf("alive=%d nominal=%ux%u@%.2f frames=%llu fps=%.2f strip_ok=%llu strip_mismatch=%llu "
                  "lat_avg=%.3fms lat_max=%.3fms missed=%llu\n",
                  alive ? 1 : 0, hd->nominal_width, hd->nominal_height,
                  static_cast<double>(hd->nominal_fps_num) / den, framesTotal,
                  static_cast<double>(framesSec) / elapsed, stripOk, stripMismatch,
                  latCount ? latSum / static_cast<double>(latCount) : 0.0, latMax, missed);
      framesSec = 0;
      latSum = 0.0;
      latMax = 0.0;
      latCount = 0;
      secStart = t;
      if (!alive) {  // producer gone: drop our mapping and go back to waiting
        std::printf("producer not alive, detaching\n");
        reader.close();
        lastSeen = 0;
        continue;
      }
    }

    sleepTicks(timer, pollTicks, freq);
  }

  reader.close();
  if (timer) CloseHandle(timer);
  SetConsoleCtrlHandler(onConsoleCtrl, FALSE);
  std::printf("done: frames=%llu strip_ok=%llu strip_mismatch=%llu missed=%llu\n", framesTotal,
              stripOk, stripMismatch, missed);
  return 0;
}
