// OWC Webcam DirectShow source filter (no BaseClasses).
//
// Object model: Filter is the COM object created by the class factory. Its
// single output pin is a member sub-object whose AddRef/Release delegate to
// the filter (the pin's lifetime is the filter's), as in the classic SDK.
//
// Locking: Filter::lock_ guards filter state + connection; it may be held
// while calling downstream during Connect (re-entrant on the same thread).
// The streaming thread NEVER takes lock_ (Stop holds it while joining the
// thread); it shares data with the rest through OutputPin::streamLock_
// (order: lock_ -> streamLock_).
#pragma once

#include <windows.h>
#include <dshow.h>

#include <vector>

#include <owc/shm.h>
#include <owc/vcam_ids.h>

#include "convert.h"
#include "mediatypes.h"

namespace owc {
namespace vcam {

extern volatile LONG g_objects;  // live COM objects (DllCanUnloadNow)
extern volatile LONG g_locks;    // IClassFactory::LockServer count
extern HMODULE g_module;

class Filter;

class OutputPin final : public IPin,
                        public IQualityControl,
                        public IAMStreamConfig,
                        public IKsPropertySet,
                        public IAMPushSource {
 public:
  explicit OutputPin(Filter* filter);
  ~OutputPin();
  OutputPin(const OutputPin&) = delete;
  OutputPin& operator=(const OutputPin&) = delete;

  // IUnknown (lifetime delegated to the filter)
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // IPin
  STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
  STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
  STDMETHODIMP Disconnect() override;
  STDMETHODIMP ConnectedTo(IPin** pPin) override;
  STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
  STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
  STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override;
  STDMETHODIMP QueryId(LPWSTR* Id) override;
  STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
  STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
  STDMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin) override;
  STDMETHODIMP EndOfStream() override;
  STDMETHODIMP BeginFlush() override;
  STDMETHODIMP EndFlush() override;
  STDMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;

  // IQualityControl
  STDMETHODIMP Notify(IBaseFilter* pSelf, Quality q) override;
  STDMETHODIMP SetSink(IQualityControl* piqc) override;

  // IAMStreamConfig
  STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
  STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
  STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override;
  STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override;

  // IKsPropertySet
  STDMETHODIMP Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData,
                   LPVOID pPropData, DWORD cbPropData) override;
  STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData,
                   LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) override;
  STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override;

  // IAMLatency / IAMPushSource
  STDMETHODIMP GetLatency(REFERENCE_TIME* prtLatency) override;
  STDMETHODIMP GetPushSourceFlags(ULONG* pFlags) override;
  STDMETHODIMP SetPushSourceFlags(ULONG Flags) override;
  STDMETHODIMP SetStreamOffset(REFERENCE_TIME rtOffset) override;
  STDMETHODIMP GetStreamOffset(REFERENCE_TIME* prtOffset) override;
  STDMETHODIMP GetMaxStreamOffset(REFERENCE_TIME* prtMaxOffset) override;
  STDMETHODIMP SetMaxStreamOffset(REFERENCE_TIME rtMaxOffset) override;

  // ---- used by Filter (filter lock held)
  HRESULT active();    // Stopped -> Paused: commit allocator, start thread
  void inactive();     // -> Stopped: decommit, join thread
  void setRunning(bool running, REFERENCE_TIME tStart, IReferenceClock* clock);
  void getStats(OwcVcamStats* s);
  void resetStats();

  // ---- used by the media type enumerator
  LONG typesVersion();
  // index < count: returns a CoTaskMemAlloc'd type; S_FALSE past the end;
  // VFW_E_ENUM_OUT_OF_SYNC when `version` is stale.
  HRESULT mediaTypeAt(ULONG index, LONG version, AM_MEDIA_TYPE** out);
  ULONG mediaTypeCount(LONG version);

 private:
  struct Pacer;
  static DWORD WINAPI threadProc(void* self);
  void streamLoop();
  bool checkMediaType(const AM_MEDIA_TYPE* mt);  // lock_ held
  bool sizeInCatalogue(int w, int h) const;
  HRESULT tryConnect(IPin* receiver, const AM_MEDIA_TYPE* mt);  // lock_ held
  HRESULT decideAllocator(size_t imageBytes);                     // lock_ held
  void releaseConnection();                                       // lock_ held
  VideoFormat defaultFormat() const;

  Filter* filter_;
  std::vector<VideoFormat> catalogue_;  // fixed after construction

  // connection state (filter lock)
  IPin* connected_ = nullptr;
  IMemInputPin* input_ = nullptr;
  IMemAllocator* alloc_ = nullptr;
  bool hasSetFormat_ = false;
  VideoFormat setFormat_;
  LONG typesVersion_ = 1;
  REFERENCE_TIME streamOffset_ = 0, maxStreamOffset_ = 0;

  // shared with the streaming thread (streamLock_)
  CRITICAL_SECTION streamLock_;
  AM_MEDIA_TYPE mt_ = {};  // connection type (downstream may replace it per sample)
  ParsedType cur_;
  REFERENCE_TIME interval_ = kInterval30;
  bool running_ = false;
  LONG runGeneration_ = 0;
  REFERENCE_TIME runStart_ = 0;
  IReferenceClock* runClock_ = nullptr;
  IMediaEventSink* eventSink_ = nullptr;  // weak (graph outlives streaming)
  OwcVcamStats stats_ = {};
  double convertSumMs_ = 0;

  HANDLE thread_ = nullptr;
  HANDLE stopEvent_ = nullptr;
  HANDLE wakeEvent_ = nullptr;

  // streaming-thread-owned
  owc::ShmReader shm_;
  Converter conv_;
  uint8_t* srcBuf_ = nullptr;  // VirtualAlloc'd, OWC_SHM_SLOT_BYTES
  std::vector<uint8_t> placeholder_;
  int placeholderW_ = 0, placeholderH_ = 0;
};

class Filter final : public IBaseFilter, public IAMFilterMiscFlags, public IOwcVcamStats {
 public:
  Filter();
  Filter(const Filter&) = delete;
  Filter& operator=(const Filter&) = delete;

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;
  // IPersist
  STDMETHODIMP GetClassID(CLSID* pClassID) override;
  // IMediaFilter
  STDMETHODIMP Stop() override;
  STDMETHODIMP Pause() override;
  STDMETHODIMP Run(REFERENCE_TIME tStart) override;
  STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override;
  STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override;
  STDMETHODIMP GetSyncSource(IReferenceClock** pClock) override;
  // IBaseFilter
  STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
  STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
  STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
  STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override;
  STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override;
  // IAMFilterMiscFlags
  STDMETHODIMP_(ULONG) GetMiscFlags() override;
  // IOwcVcamStats
  STDMETHODIMP GetStats(OwcVcamStats* stats) override;
  STDMETHODIMP ResetStats() override;

  OutputPin* pin() { return &pin_; }

 private:
  friend class OutputPin;
  ~Filter();

  volatile LONG ref_ = 1;
  CRITICAL_SECTION lock_;
  FILTER_STATE state_ = State_Stopped;
  IReferenceClock* clock_ = nullptr;
  REFERENCE_TIME tStart_ = 0;
  IFilterGraph* graph_ = nullptr;  // weak, per DirectShow rules
  WCHAR name_[MAX_FILTER_NAME] = {};
  OutputPin pin_;
};

HRESULT CreateFilterInstance(IUnknown* outer, REFIID riid, void** ppv);

}  // namespace vcam
}  // namespace owc
