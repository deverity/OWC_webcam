// OWC shared-memory frame bus (Windows). One producer (OWC app, which decodes
// the phone stream) and any number of consumers (DirectShow virtual camera
// loaded inside Zoom/Teams/Chrome/OBS, the OBS source plugin, tools).
//
// Layout: [OwcShmHeader][pad to 4096][slot 0 data][slot 1 data][slot 2 data]
// Pixel format is NV12 (Y plane, then interleaved UV plane, both with `stride`).
//
// Concurrency: lock-free triple buffer with a per-slot seqlock.
//   Writer: slot = (latest_slot + 1) % OWC_SHM_SLOTS
//           seq++ (now odd) -> write pixels + fields -> seq++ (even)
//           latest_slot = slot; latest_frame_number++
//   Reader: idx = latest_slot; s1 = seq; if odd -> retry
//           copy fields+pixels; s2 = seq; if s1 != s2 -> retry (max few times)
// Consumers poll latest_frame_number (≈1 ms period) — no kernel events, so any
// number of consumers works without coordination.
//
// Liveness: producer refreshes producer_heartbeat_qpc at least every 100 ms
// while running. Consumers treat the bus as "no signal" when the mapping is
// absent, producer_active == 0, or heartbeat is older than 1 s; they then
// output their own placeholder (e.g. dark frame with "OWC - no signal").
//
// The section is created by the producer (owner/SYSTEM full access, everyone
// incl. AppContainer read-only, low integrity no-write-up label) and opened
// read-only by consumers. Only one producer at a time (marker object
// "<name>_Producer"). Name lives in the session-local namespace and can be
// overridden with the OWC_SHM_NAME environment variable (tests).
#pragma once

#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#define OWC_SHM_NAME L"Local\\OwcWebcamFrames_v1"
#define OWC_SHM_MAGIC 0x4D48534Fu /* "OSHM" */
#define OWC_SHM_VERSION 1u
#define OWC_SHM_SLOTS 3u
#define OWC_SHM_MAX_WIDTH 3840u
#define OWC_SHM_MAX_HEIGHT 2160u
#define OWC_SHM_SLOT_BYTES (OWC_SHM_MAX_WIDTH * OWC_SHM_MAX_HEIGHT * 3u / 2u)
#define OWC_SHM_DATA_OFFSET 4096u
#define OWC_SHM_TOTAL_BYTES \
  ((uint64_t)OWC_SHM_DATA_OFFSET + (uint64_t)OWC_SHM_SLOTS * OWC_SHM_SLOT_BYTES)
#define OWC_SHM_STALE_MS 1000u

#define OWC_PIXFMT_NV12 1u

#define OWC_COLOR_UNKNOWN 0u /* consumer decides (e.g. 709 for >= 720p, else 601) */
#define OWC_COLOR_BT601 1u
#define OWC_COLOR_BT709 2u

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)
typedef struct OwcShmSlot {
  volatile int64_t seq;      /* seqlock counter, odd while being written */
  uint32_t width;            /* even, <= OWC_SHM_MAX_WIDTH */
  uint32_t height;           /* even, <= OWC_SHM_MAX_HEIGHT */
  uint32_t stride;           /* bytes per row for Y and UV planes (>= width) */
  uint32_t format;           /* OWC_PIXFMT_NV12 */
  uint64_t frame_number;     /* producer's monotonically increasing counter */
  int64_t capture_qpc;       /* phone capture time mapped to this PC's QPC (0 = unknown) */
  int64_t publish_qpc;       /* QPC when the producer published the frame */
  uint32_t data_offset;      /* from start of mapping; Y at +0, UV at +stride*height */
  uint32_t rotation_applied; /* informational: degrees already applied by producer */
  uint32_t color_matrix;     /* OWC_COLOR_* (YUV->RGB matrix of this frame) */
  uint32_t full_range;       /* 1 = full range (0..255), 0 = limited (16..235) */
  uint32_t reserved[6];
} OwcShmSlot;

typedef struct OwcShmHeader {
  uint32_t magic;            /* OWC_SHM_MAGIC */
  uint32_t version;          /* OWC_SHM_VERSION */
  uint32_t header_size;      /* sizeof(OwcShmHeader) */
  uint32_t slot_count;       /* OWC_SHM_SLOTS */
  uint32_t slot_bytes;       /* OWC_SHM_SLOT_BYTES */
  uint32_t max_width;
  uint32_t max_height;
  uint32_t producer_pid;
  volatile int64_t latest_frame_number; /* 0 = nothing published yet */
  volatile int32_t latest_slot;
  volatile int32_t producer_active;     /* 1 while a stream is live */
  volatile int64_t producer_heartbeat_qpc;
  int64_t qpc_frequency;
  uint32_t nominal_fps_num;  /* current stream fps, e.g. 30/1 */
  uint32_t nominal_fps_den;
  uint32_t nominal_width;    /* current stream size (consumers may pick it) */
  uint32_t nominal_height;
  uint32_t reserved[32];
  OwcShmSlot slots[OWC_SHM_SLOTS];
} OwcShmHeader;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(OwcShmHeader) <= OWC_SHM_DATA_OFFSET, "header must fit before data");
}  // extern "C"
#endif

#if defined(__cplusplus) && defined(_WIN32)
#include <intrin.h>
#include <sddl.h>
#include <string.h>
#pragma comment(lib, "advapi32.lib")

namespace owc {

// Bus name. Tests set the environment variable OWC_SHM_NAME (e.g.
// "Local\OwcTest_1234") so parallel test runs never touch the real bus.
// Resolved once per process.
struct ShmNameBuf { wchar_t s[128]; };
inline const wchar_t* shmName() {
  static const ShmNameBuf name = [] {
    ShmNameBuf b{};
    const DWORD n = GetEnvironmentVariableW(L"OWC_SHM_NAME", b.s, 128);
    if (n == 0 || n >= 128) lstrcpynW(b.s, OWC_SHM_NAME, 128);
    return b;
  }();
  return name.s;
}

inline int64_t qpcNow() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

// Atomic 64-bit load (plain volatile reads can tear in 32-bit consumers).
// Must not write: consumers map the section read-only, so no lock cmpxchg.
inline int64_t shmLoad64(const volatile int64_t* p) {
#if defined(_M_X64) || defined(_M_ARM64)
  return *p;  // aligned 64-bit loads are atomic on 64-bit targets
#else
  return __iso_volatile_load64((const volatile __int64*)p);  // SSE2/x87 single load
#endif
}

// ---------------------------------------------------------------- producer
// Only ONE producer may own the bus: open() fails while another live
// ShmWriter (any process) holds the "<name>_Producer" marker object.
class ShmWriter {
 public:
  ShmWriter() = default;
  ~ShmWriter() { close(); }
  ShmWriter(const ShmWriter&) = delete;
  ShmWriter& operator=(const ShmWriter&) = delete;

  bool open() {
    if (base_) return true;
    wchar_t lockName[160];
    lstrcpynW(lockName, shmName(), 128);
    lstrcatW(lockName, L"_Producer");
    lock_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READONLY, 0, 4096, lockName);
    if (!lock_) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {  // another producer is alive
      CloseHandle(lock_);
      lock_ = nullptr;
      return false;
    }
    // Owner/SYSTEM full access, everyone (incl. AppContainer) read-only, low
    // integrity label with no-write-up so sandboxed readers can still map it.
    PSECURITY_DESCRIPTOR psd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;OW)(A;;GA;;;SY)(A;;GR;;;WD)(A;;GR;;;AC)S:(ML;;NW;;;LW)",
            SDDL_REVISION_1, &psd, nullptr))
      sa.lpSecurityDescriptor = psd;
    const uint64_t total = OWC_SHM_TOTAL_BYTES;
    map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, psd ? &sa : nullptr, PAGE_READWRITE,
                              (DWORD)(total >> 32), (DWORD)(total & 0xFFFFFFFFu), shmName());
    if (psd) LocalFree(psd);
    if (!map_) { close(); return false; }
    base_ = (uint8_t*)MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!base_) { close(); return false; }
    OwcShmHeader* h = header();
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    h->header_size = sizeof(OwcShmHeader);
    h->slot_count = OWC_SHM_SLOTS;
    h->slot_bytes = OWC_SHM_SLOT_BYTES;
    h->max_width = OWC_SHM_MAX_WIDTH;
    h->max_height = OWC_SHM_MAX_HEIGHT;
    h->producer_pid = GetCurrentProcessId();
    h->qpc_frequency = f.QuadPart;
    for (uint32_t i = 0; i < OWC_SHM_SLOTS; ++i)
      h->slots[i].data_offset = OWC_SHM_DATA_OFFSET + i * OWC_SHM_SLOT_BYTES;
    h->version = OWC_SHM_VERSION;
    MemoryBarrier();
    h->magic = OWC_SHM_MAGIC;
    heartbeat();
    return true;
  }

  void close() {
    if (base_) {
      InterlockedExchange((volatile LONG*)&header()->producer_active, 0);
      UnmapViewOfFile(base_);
      base_ = nullptr;
    }
    if (map_) { CloseHandle(map_); map_ = nullptr; }
    if (lock_) { CloseHandle(lock_); lock_ = nullptr; }
  }

  bool isOpen() const { return base_ != nullptr; }
  OwcShmHeader* header() const { return (OwcShmHeader*)base_; }

  void setActive(bool on, uint32_t w = 0, uint32_t h = 0, uint32_t fpsNum = 30,
                 uint32_t fpsDen = 1) {
    if (!base_) return;
    OwcShmHeader* hd = header();
    if (on) {
      hd->nominal_width = w;
      hd->nominal_height = h;
      hd->nominal_fps_num = fpsNum;
      hd->nominal_fps_den = fpsDen ? fpsDen : 1;
    }
    MemoryBarrier();
    InterlockedExchange((volatile LONG*)&hd->producer_active, on ? 1 : 0);
    heartbeat();
  }

  void heartbeat() {
    if (base_) InterlockedExchange64((volatile LONG64*)&header()->producer_heartbeat_qpc, qpcNow());
  }

  // Copies an NV12 frame (separate Y/UV pointers + source strides) into the
  // next slot and publishes it. Returns false if size is out of range.
  bool publishNV12(const uint8_t* y, uint32_t yStride, const uint8_t* uv, uint32_t uvStride,
                   uint32_t width, uint32_t height, int64_t captureQpc = 0,
                   uint32_t colorMatrix = OWC_COLOR_UNKNOWN, bool fullRange = false) {
    if (!base_ || width == 0 || height == 0 || (width & 1) || (height & 1) ||
        width > OWC_SHM_MAX_WIDTH || height > OWC_SHM_MAX_HEIGHT)
      return false;
    OwcShmHeader* h = header();
    const int32_t idx = (int32_t)((h->latest_slot + 1) % (int32_t)OWC_SHM_SLOTS);
    OwcShmSlot& s = h->slots[idx];
    InterlockedIncrement64((volatile LONG64*)&s.seq);  // odd: writing
    const uint32_t stride = width;
    uint8_t* dst = base_ + s.data_offset;
    for (uint32_t r = 0; r < height; ++r) memcpy(dst + (size_t)r * stride, y + (size_t)r * yStride, width);
    uint8_t* dstUV = dst + (size_t)stride * height;
    for (uint32_t r = 0; r < height / 2; ++r)
      memcpy(dstUV + (size_t)r * stride, uv + (size_t)r * uvStride, width);
    s.width = width;
    s.height = height;
    s.stride = stride;
    s.format = OWC_PIXFMT_NV12;
    s.color_matrix = colorMatrix;
    s.full_range = fullRange ? 1u : 0u;
    s.frame_number = (uint64_t)h->latest_frame_number + 1;
    s.capture_qpc = captureQpc;
    s.publish_qpc = qpcNow();
    InterlockedIncrement64((volatile LONG64*)&s.seq);  // even: stable
    InterlockedExchange((volatile LONG*)&h->latest_slot, idx);
    InterlockedIncrement64((volatile LONG64*)&h->latest_frame_number);
    InterlockedExchange64((volatile LONG64*)&h->producer_heartbeat_qpc, s.publish_qpc);
    return true;
  }

 private:
  HANDLE map_ = nullptr;
  HANDLE lock_ = nullptr;
  uint8_t* base_ = nullptr;
};

// ---------------------------------------------------------------- consumer
// Runs inside foreign processes (Zoom, Chrome, OBS...): never trusts the
// section contents; every offset/size is bounds-checked against the mapping.
class ShmReader {
 public:
  struct FrameInfo {
    uint32_t width = 0, height = 0, stride = 0;
    uint64_t frame_number = 0;
    int64_t capture_qpc = 0, publish_qpc = 0;
    uint32_t color_matrix = OWC_COLOR_UNKNOWN;
    bool full_range = false;
  };

  ShmReader() = default;
  ~ShmReader() { close(); }
  ShmReader(const ShmReader&) = delete;
  ShmReader& operator=(const ShmReader&) = delete;

  // Cheap to call repeatedly; returns false while the producer isn't running.
  bool open() {
    if (base_) return true;
    map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, shmName());
    if (!map_) return false;
    base_ = (const uint8_t*)MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0);
    MEMORY_BASIC_INFORMATION mbi{};
    if (base_ && VirtualQuery(base_, &mbi, sizeof(mbi)) == sizeof(mbi)) mapSize_ = mbi.RegionSize;
    if (!base_ || mapSize_ < OWC_SHM_TOTAL_BYTES || header()->magic != OWC_SHM_MAGIC ||
        header()->version != OWC_SHM_VERSION) {
      close();
      return false;
    }
    return true;
  }

  void close() {
    if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
    if (map_) { CloseHandle(map_); map_ = nullptr; }
    mapSize_ = 0;
  }

  bool isOpen() const { return base_ != nullptr; }
  const OwcShmHeader* header() const { return (const OwcShmHeader*)base_; }

  // True when a live producer is publishing (active + fresh heartbeat).
  bool producerAlive() const {
    if (!base_) return false;
    const OwcShmHeader* h = header();
    if (!h->producer_active) return false;
    const int64_t age = qpcNow() - shmLoad64(&h->producer_heartbeat_qpc);
    return age >= 0 && age < h->qpc_frequency * (int64_t)OWC_SHM_STALE_MS / 1000;
  }

  uint64_t latestFrameNumber() const {
    return base_ ? (uint64_t)shmLoad64(&header()->latest_frame_number) : 0;
  }

  // Copies the newest frame into dst (tightly packed NV12, stride == width,
  // dst must hold width*height*3/2). Returns false if nothing new since
  // `lastSeen`, if the slot looks corrupt, or if the copy kept racing with
  // the writer.
  bool readLatest(uint64_t lastSeen, uint8_t* dst, size_t dstCap, FrameInfo& info) const {
    if (!base_) return false;
    const OwcShmHeader* h = header();
    for (int attempt = 0; attempt < 4; ++attempt) {
      if ((uint64_t)shmLoad64(&h->latest_frame_number) == lastSeen) return false;
      const int32_t idx = h->latest_slot;
      if (idx < 0 || idx >= (int32_t)OWC_SHM_SLOTS) return false;
      const OwcShmSlot& s = h->slots[idx];
      const int64_t s1 = shmLoad64(&s.seq);
      if (s1 & 1) continue;
      FrameInfo fi;
      fi.width = s.width;
      fi.height = s.height;
      fi.stride = s.stride;
      fi.frame_number = s.frame_number;
      fi.capture_qpc = s.capture_qpc;
      fi.publish_qpc = s.publish_qpc;
      fi.color_matrix = s.color_matrix;
      fi.full_range = s.full_range != 0;
      const uint64_t off = s.data_offset;
      if (fi.width == 0 || fi.height == 0 || fi.width > OWC_SHM_MAX_WIDTH ||
          fi.height > OWC_SHM_MAX_HEIGHT || fi.stride < fi.width || fi.stride > OWC_SHM_MAX_WIDTH ||
          off < OWC_SHM_DATA_OFFSET ||
          off + (uint64_t)fi.stride * fi.height * 3 / 2 > (uint64_t)mapSize_)
        return false;
      const size_t need = (size_t)fi.width * fi.height * 3 / 2;
      if (dstCap < need) return false;
      const uint8_t* src = base_ + off;
      for (uint32_t r = 0; r < fi.height * 3 / 2; ++r)
        memcpy(dst + (size_t)r * fi.width, src + (size_t)r * fi.stride, fi.width);
      MemoryBarrier();
      if (shmLoad64(&s.seq) != s1) continue;
      info = fi;
      return true;
    }
    return false;
  }

 private:
  HANDLE map_ = nullptr;
  const uint8_t* base_ = nullptr;
  size_t mapSize_ = 0;
};

}  // namespace owc
#endif
