// OWC virtual camera: public identifiers + private diagnostics interface.
// Shared by owc_virtualcam.dll and the owc_dshow_probe tool.
#pragma once

#include <stdint.h>
#include <unknwn.h>

#define OWC_VCAM_FRIENDLY_NAME L"OWC Webcam"
#define OWC_VCAM_CLSID_STR L"{8433CA51-5EEA-4F62-A174-13FA1F6BADD5}"
#define OWC_VCAM_PIN_NAME L"Capture"

// {8433CA51-5EEA-4F62-A174-13FA1F6BADD5}
static const CLSID CLSID_OwcVirtualCam = {
    0x8433ca51, 0x5eea, 0x4f62, {0xa1, 0x74, 0x13, 0xfa, 0x1f, 0x6b, 0xad, 0xd5}};

// Diagnostics snapshot (filter-wide). All times in milliseconds unless noted.
struct OwcVcamStats {
  uint32_t cbSize;             // = sizeof(OwcVcamStats)
  uint32_t state;              // 0 stopped, 1 paused, 2 running
  uint64_t delivered;          // samples passed to IMemInputPin::Receive
  uint64_t freshFrames;        // samples carrying a newly published shm frame
  uint64_t repeatedFrames;     // samples repeating the previous shm frame
  uint64_t placeholderFrames;  // samples carrying the "no signal" placeholder
  uint64_t skippedSource;      // shm frames never delivered (producer faster than us)
  uint64_t receiveFailures;    // Receive() returned an error / S_FALSE
  uint64_t convertCount;
  double convertAvgMs;         // conversion (+scaling) time, average since reset
  double convertMaxMs;
  double convertLastMs;
  uint64_t lastSourceFrame;    // shm frame_number of the last fresh frame
  uint32_t srcWidth, srcHeight;
  uint32_t outWidth, outHeight;
  int32_t outStride;
  uint32_t outFormat;          // owc::vcam::PixFmt
  int64_t avgTimePerFrame;     // 100 ns units
  uint32_t allocBuffers, allocBufferBytes;
  int32_t lastError;           // last failing HRESULT on the streaming thread
  uint32_t mediaTypeChanges;   // dynamic type changes attached to samples by downstream
};

MIDL_INTERFACE("B048B6CA-F905-4735-942F-B4C45FFCA85B")
IOwcVcamStats : public IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE GetStats(OwcVcamStats* stats) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetStats() = 0;
};
