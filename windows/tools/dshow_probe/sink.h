// Minimal DirectShow sink ("grabber/renderer") used by owc_dshow_probe:
// one input pin with IMemInputPin; every sample is handed to a callback on
// the upstream streaming thread. No BaseClasses, no qedit.h.
#pragma once

#include <windows.h>
#include <dshow.h>

#include <functional>

namespace probe {

struct SampleInfo {
  int64_t qpcReceived;
  IMediaSample* sample;  // valid during the callback only
  const AM_MEDIA_TYPE* connectionType;
};

class SinkFilter;

class SinkPin final : public IPin, public IMemInputPin {
 public:
  explicit SinkPin(SinkFilter* f) : f_(f) {}
  ~SinkPin();
  // IUnknown (delegates)
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;
  // IPin
  STDMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
  STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
  STDMETHODIMP Disconnect() override;
  STDMETHODIMP ConnectedTo(IPin** pPin) override;
  STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
  STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
  STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override;
  STDMETHODIMP QueryId(LPWSTR* Id) override;
  STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
  STDMETHODIMP EnumMediaTypes(IEnumMediaTypes**) override { return E_NOTIMPL; }
  STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
  STDMETHODIMP EndOfStream() override { return S_OK; }
  STDMETHODIMP BeginFlush() override { return S_OK; }
  STDMETHODIMP EndFlush() override { return S_OK; }
  STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }
  // IMemInputPin
  STDMETHODIMP GetAllocator(IMemAllocator** ppAllocator) override;
  STDMETHODIMP NotifyAllocator(IMemAllocator* pAllocator, BOOL bReadOnly) override;
  STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES* pProps) override;
  STDMETHODIMP Receive(IMediaSample* pSample) override;
  STDMETHODIMP ReceiveMultiple(IMediaSample** pSamples, long nSamples, long* nSamplesProcessed) override;
  STDMETHODIMP ReceiveCanBlock() override { return S_FALSE; }

 private:
  friend class SinkFilter;
  bool accepts(const AM_MEDIA_TYPE* pmt) const;
  SinkFilter* f_;
  IPin* connected_ = nullptr;
  AM_MEDIA_TYPE mt_ = {};
  IMemAllocator* alloc_ = nullptr;
};

class SinkFilter final : public IBaseFilter {
 public:
  using Callback = std::function<void(const SampleInfo&)>;
  // `want`: subtype/width/height the pin accepts (GUID_NULL / 0 = any).
  SinkFilter(const GUID& wantSubtype, int wantW, int wantH, bool offerAllocator, Callback cb);

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
  STDMETHODIMP_(ULONG) Release() override;
  STDMETHODIMP GetClassID(CLSID* pClassID) override;
  STDMETHODIMP Stop() override;
  STDMETHODIMP Pause() override;
  STDMETHODIMP Run(REFERENCE_TIME tStart) override;
  STDMETHODIMP GetState(DWORD, FILTER_STATE* State) override;
  STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override;
  STDMETHODIMP GetSyncSource(IReferenceClock** pClock) override;
  STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
  STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
  STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
  STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override;
  STDMETHODIMP QueryVendorInfo(LPWSTR*) override { return E_NOTIMPL; }

  IPin* pin() { return &pin_; }
  static LONG liveObjects();

 private:
  friend class SinkPin;
  ~SinkFilter();
  volatile LONG ref_ = 1;
  CRITICAL_SECTION lock_;
  FILTER_STATE state_ = State_Stopped;
  IReferenceClock* clock_ = nullptr;
  IFilterGraph* graph_ = nullptr;
  WCHAR name_[MAX_FILTER_NAME] = {};
  GUID wantSubtype_;
  int wantW_, wantH_;
  bool offerAllocator_;
  Callback cb_;
  SinkPin pin_;
};

}  // namespace probe
