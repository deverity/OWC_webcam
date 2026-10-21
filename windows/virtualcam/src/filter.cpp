#include "filter.h"

#include <avrt.h>
#include <mmsystem.h>

#include <limits.h>
#include <new>

namespace owc {
namespace vcam {

namespace {

const uint64_t kPlaceholderId = 0x8000000000000000ull;

class AutoLock {
 public:
  explicit AutoLock(CRITICAL_SECTION* cs) : cs_(cs) { EnterCriticalSection(cs_); }
  ~AutoLock() { LeaveCriticalSection(cs_); }
  AutoLock(const AutoLock&) = delete;
  AutoLock& operator=(const AutoLock&) = delete;

 private:
  CRITICAL_SECTION* cs_;
};

int64_t qpcFrequency() {
  static int64_t f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v.QuadPart;
  }();
  return f;
}

// QPC in 100 ns units (the pacing time base).
int64_t qpcToHns(int64_t q) {
  const int64_t f = qpcFrequency();
  return (q / f) * 10000000 + (q % f) * 10000000 / f;
}
int64_t nowHns() { return qpcToHns(owc::qpcNow()); }

double qpcToMs(int64_t ticks) { return (double)ticks * 1000.0 / (double)qpcFrequency(); }

bool isPartial(const AM_MEDIA_TYPE* mt) {
  return IsEqualGUID(mt->majortype, GUID_NULL) || IsEqualGUID(mt->subtype, GUID_NULL) ||
         IsEqualGUID(mt->formattype, GUID_NULL) || !mt->pbFormat;
}

bool matchesPartial(const AM_MEDIA_TYPE* partial, const AM_MEDIA_TYPE* mt) {
  if (!partial) return true;
  if (!IsEqualGUID(partial->majortype, GUID_NULL) && !IsEqualGUID(partial->majortype, mt->majortype)) return false;
  if (!IsEqualGUID(partial->subtype, GUID_NULL) && !IsEqualGUID(partial->subtype, mt->subtype)) return false;
  if (!IsEqualGUID(partial->formattype, GUID_NULL) && !IsEqualGUID(partial->formattype, mt->formattype)) return false;
  return true;
}

// ---------------------------------------------------------------- IEnumPins

class PinEnum final : public IEnumPins {
 public:
  PinEnum(Filter* f, ULONG pos) : f_(f), pos_(pos) {
    f_->AddRef();
    InterlockedIncrement(&g_objects);
  }
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumPins)) {
      *ppv = static_cast<IEnumPins*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
  STDMETHODIMP_(ULONG) Release() override {
    const LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return (ULONG)r;
  }
  STDMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override {
    if (!ppPins) return E_POINTER;
    if (cPins != 1 && !pcFetched) return E_POINTER;
    ULONG n = 0;
    while (n < cPins && pos_ < 1) {
      ppPins[n] = f_->pin();
      ppPins[n]->AddRef();
      ++n;
      ++pos_;
    }
    if (pcFetched) *pcFetched = n;
    return n == cPins ? S_OK : S_FALSE;
  }
  STDMETHODIMP Skip(ULONG cPins) override {
    if (pos_ + cPins > 1) {
      pos_ = 1;
      return S_FALSE;
    }
    pos_ += cPins;
    return S_OK;
  }
  STDMETHODIMP Reset() override {
    pos_ = 0;
    return S_OK;
  }
  STDMETHODIMP Clone(IEnumPins** ppEnum) override {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new (std::nothrow) PinEnum(f_, pos_);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
  }

 private:
  ~PinEnum() {
    f_->Release();
    InterlockedDecrement(&g_objects);
  }
  volatile LONG ref_ = 1;
  Filter* f_;
  ULONG pos_;
};

// ---------------------------------------------------------------- IEnumMediaTypes

class TypeEnum final : public IEnumMediaTypes {
 public:
  TypeEnum(OutputPin* p, ULONG pos, LONG version) : p_(p), pos_(pos), version_(version) {
    p_->AddRef();
    InterlockedIncrement(&g_objects);
  }
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumMediaTypes)) {
      *ppv = static_cast<IEnumMediaTypes*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
  STDMETHODIMP_(ULONG) Release() override {
    const LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return (ULONG)r;
  }
  STDMETHODIMP Next(ULONG cTypes, AM_MEDIA_TYPE** ppTypes, ULONG* pcFetched) override {
    if (!ppTypes) return E_POINTER;
    if (cTypes != 1 && !pcFetched) return E_POINTER;
    ULONG n = 0;
    while (n < cTypes) {
      const HRESULT hr = p_->mediaTypeAt(pos_, version_, &ppTypes[n]);
      if (hr == VFW_E_ENUM_OUT_OF_SYNC) {
        for (ULONG i = 0; i < n; ++i) {
          deleteMediaType(ppTypes[i]);
          ppTypes[i] = nullptr;
        }
        if (pcFetched) *pcFetched = 0;
        return hr;
      }
      if (hr != S_OK) break;
      ++n;
      ++pos_;
    }
    if (pcFetched) *pcFetched = n;
    return n == cTypes ? S_OK : S_FALSE;
  }
  STDMETHODIMP Skip(ULONG cTypes) override {
    const ULONG count = p_->mediaTypeCount(version_);
    if (count == ULONG_MAX) return VFW_E_ENUM_OUT_OF_SYNC;
    if (pos_ + cTypes > count) {
      pos_ = count;
      return S_FALSE;
    }
    pos_ += cTypes;
    return S_OK;
  }
  STDMETHODIMP Reset() override {
    pos_ = 0;
    version_ = p_->typesVersion();
    return S_OK;
  }
  STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new (std::nothrow) TypeEnum(p_, pos_, version_);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
  }

 private:
  ~TypeEnum() {
    p_->Release();
    InterlockedDecrement(&g_objects);
  }
  volatile LONG ref_ = 1;
  OutputPin* p_;
  ULONG pos_;
  LONG version_;
};

}  // namespace

// ================================================================ OutputPin

OutputPin::OutputPin(Filter* filter) : filter_(filter) {
  InitializeCriticalSection(&streamLock_);
  int nw = 0, nh = 0;
  {
    owc::ShmReader probe;
    if (probe.open() && probe.producerAlive()) {
      nw = (int)probe.header()->nominal_width;
      nh = (int)probe.header()->nominal_height;
    }
  }
  catalogue_ = buildCatalogue(nw, nh);
  stats_.cbSize = sizeof(stats_);
}

OutputPin::~OutputPin() {
  inactive();
  releaseConnection();
  if (runClock_) runClock_->Release();
  DeleteCriticalSection(&streamLock_);
}

STDMETHODIMP OutputPin::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IPin))
    *ppv = static_cast<IPin*>(this);
  else if (IsEqualIID(riid, IID_IQualityControl))
    *ppv = static_cast<IQualityControl*>(this);
  else if (IsEqualIID(riid, IID_IAMStreamConfig))
    *ppv = static_cast<IAMStreamConfig*>(this);
  else if (IsEqualIID(riid, IID_IKsPropertySet))
    *ppv = static_cast<IKsPropertySet*>(this);
  else if (IsEqualIID(riid, IID_IAMPushSource) || IsEqualIID(riid, IID_IAMLatency))
    *ppv = static_cast<IAMPushSource*>(this);
  else {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) OutputPin::AddRef() { return filter_->AddRef(); }
STDMETHODIMP_(ULONG) OutputPin::Release() { return filter_->Release(); }

// ---------------------------------------------------------------- media types

bool OutputPin::sizeInCatalogue(int w, int h) const {
  for (const VideoFormat& f : catalogue_)
    if (f.width == w && f.height == h) return true;
  return false;
}

VideoFormat OutputPin::defaultFormat() const { return catalogue_.front(); }

bool OutputPin::checkMediaType(const AM_MEDIA_TYPE* mt) {
  ParsedType pt;
  if (!parseMediaType(mt, &pt)) return false;
  if (pt.avgTimePerFrame != 0 && (pt.avgTimePerFrame < kMinInterval || pt.avgTimePerFrame > kMaxInterval))
    return false;
  if (hasSetFormat_)
    return pt.fmt == setFormat_.fmt && pt.width == setFormat_.width && pt.height == setFormat_.height;
  return sizeInCatalogue(pt.width, pt.height);
}

LONG OutputPin::typesVersion() {
  AutoLock l(&filter_->lock_);
  return typesVersion_;
}

ULONG OutputPin::mediaTypeCount(LONG version) {
  AutoLock l(&filter_->lock_);
  if (version != typesVersion_) return ULONG_MAX;
  return hasSetFormat_ ? 1u : (ULONG)catalogue_.size();
}

HRESULT OutputPin::mediaTypeAt(ULONG index, LONG version, AM_MEDIA_TYPE** out) {
  *out = nullptr;
  AutoLock l(&filter_->lock_);
  if (version != typesVersion_) return VFW_E_ENUM_OUT_OF_SYNC;
  const ULONG count = hasSetFormat_ ? 1u : (ULONG)catalogue_.size();
  if (index >= count) return S_FALSE;
  const VideoFormat& vf = hasSetFormat_ ? setFormat_ : catalogue_[index];
  AM_MEDIA_TYPE* mt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
  if (!mt) return E_OUTOFMEMORY;
  const HRESULT hr = buildMediaType(vf, mt);
  if (FAILED(hr)) {
    CoTaskMemFree(mt);
    return hr;
  }
  *out = mt;
  return S_OK;
}

// ---------------------------------------------------------------- connection

STDMETHODIMP OutputPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
  if (!pReceivePin) return E_POINTER;
  AutoLock l(&filter_->lock_);
  if (connected_) return VFW_E_ALREADY_CONNECTED;
  if (filter_->state_ != State_Stopped) return VFW_E_NOT_STOPPED;
  PIN_DIRECTION dir;
  if (FAILED(pReceivePin->QueryDirection(&dir)) || dir != PINDIR_INPUT) return VFW_E_INVALID_DIRECTION;

  if (pmt && !isPartial(pmt)) {
    const HRESULT hr = tryConnect(pReceivePin, pmt);
    return SUCCEEDED(hr) ? S_OK : (hr == E_FAIL ? VFW_E_TYPE_NOT_ACCEPTED : hr);
  }
  // 1) our types, preferred first (only the SetFormat type if one is set)
  const ULONG count = hasSetFormat_ ? 1u : (ULONG)catalogue_.size();
  for (ULONG i = 0; i < count; ++i) {
    AM_MEDIA_TYPE mt;
    if (FAILED(buildMediaType(hasSetFormat_ ? setFormat_ : catalogue_[i], &mt))) continue;
    HRESULT hr = E_FAIL;
    if (matchesPartial(pmt, &mt)) hr = tryConnect(pReceivePin, &mt);
    freeMediaType(mt);
    if (SUCCEEDED(hr)) return S_OK;
  }
  // 2) the receiver's types that we can produce
  IEnumMediaTypes* e = nullptr;
  if (SUCCEEDED(pReceivePin->EnumMediaTypes(&e)) && e) {
    AM_MEDIA_TYPE* mt = nullptr;
    ULONG got = 0;
    while (e->Next(1, &mt, &got) == S_OK && got == 1 && mt) {
      HRESULT hr = E_FAIL;
      if (matchesPartial(pmt, mt)) hr = tryConnect(pReceivePin, mt);
      deleteMediaType(mt);
      mt = nullptr;
      if (SUCCEEDED(hr)) {
        e->Release();
        return S_OK;
      }
    }
    e->Release();
  }
  return VFW_E_NO_ACCEPTABLE_TYPES;
}

HRESULT OutputPin::tryConnect(IPin* receiver, const AM_MEDIA_TYPE* mt) {
  if (!checkMediaType(mt)) return VFW_E_TYPE_NOT_ACCEPTED;
  ParsedType pt;
  parseMediaType(mt, &pt);
  HRESULT hr = receiver->ReceiveConnection(this, mt);
  if (FAILED(hr)) return hr;
  IMemInputPin* in = nullptr;
  hr = receiver->QueryInterface(IID_IMemInputPin, (void**)&in);
  if (FAILED(hr) || !in) {
    receiver->Disconnect();
    return VFW_E_NO_TRANSPORT;
  }
  connected_ = receiver;
  connected_->AddRef();
  input_ = in;
  {
    AutoLock s(&streamLock_);
    freeMediaType(mt_);
    copyMediaType(&mt_, mt);
    cur_ = pt;
    interval_ = pt.avgTimePerFrame ? pt.avgTimePerFrame
                                   : (hasSetFormat_ ? setFormat_.avgTimePerFrame : kInterval30);
  }
  hr = decideAllocator(pt.imageBytes);
  if (FAILED(hr)) {
    receiver->Disconnect();
    releaseConnection();
    return hr;
  }
  return S_OK;
}

HRESULT OutputPin::decideAllocator(size_t imageBytes) {
  ALLOCATOR_PROPERTIES req = {};
  input_->GetAllocatorRequirements(&req);  // E_NOTIMPL is fine
  ALLOCATOR_PROPERTIES want = {};
  want.cBuffers = req.cBuffers > 3 ? req.cBuffers : 3;  // ours + renderer + one in flight
  want.cbBuffer = req.cbBuffer > (long)imageBytes ? req.cbBuffer : (long)imageBytes;
  want.cbAlign = req.cbAlign > 0 ? req.cbAlign : 1;
  want.cbPrefix = req.cbPrefix > 0 ? req.cbPrefix : 0;
  ALLOCATOR_PROPERTIES actual = {};
  auto attempt = [&](IMemAllocator* a) -> HRESULT {
    HRESULT hr = a->SetProperties(&want, &actual);
    if (FAILED(hr)) return hr;
    if (actual.cbBuffer < (long)imageBytes || actual.cBuffers < 1) return VFW_E_SIZENOTSET;
    return input_->NotifyAllocator(a, FALSE);
  };
  IMemAllocator* a = nullptr;
  HRESULT hr = input_->GetAllocator(&a);
  if (SUCCEEDED(hr) && a) {
    hr = attempt(a);
    if (FAILED(hr)) {
      a->Release();
      a = nullptr;
    }
  }
  if (!a) {
    hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator, (void**)&a);
    if (FAILED(hr)) return hr;
    hr = attempt(a);
    if (FAILED(hr)) {
      a->Release();
      return hr;
    }
  }
  alloc_ = a;
  AutoLock s(&streamLock_);
  stats_.allocBuffers = (uint32_t)actual.cBuffers;
  stats_.allocBufferBytes = (uint32_t)actual.cbBuffer;
  return S_OK;
}

void OutputPin::releaseConnection() {
  if (alloc_) {
    alloc_->Release();
    alloc_ = nullptr;
  }
  if (input_) {
    input_->Release();
    input_ = nullptr;
  }
  if (connected_) {
    connected_->Release();
    connected_ = nullptr;
  }
  AutoLock s(&streamLock_);
  freeMediaType(mt_);
  ZeroMemory(&mt_, sizeof(mt_));
}

STDMETHODIMP OutputPin::ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) { return E_UNEXPECTED; }

STDMETHODIMP OutputPin::Disconnect() {
  AutoLock l(&filter_->lock_);
  if (filter_->state_ != State_Stopped) return VFW_E_NOT_STOPPED;
  if (!connected_) return S_FALSE;
  releaseConnection();
  return S_OK;
}

STDMETHODIMP OutputPin::ConnectedTo(IPin** pPin) {
  if (!pPin) return E_POINTER;
  AutoLock l(&filter_->lock_);
  *pPin = connected_;
  if (!connected_) return VFW_E_NOT_CONNECTED;
  connected_->AddRef();
  return S_OK;
}

STDMETHODIMP OutputPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
  if (!pmt) return E_POINTER;
  AutoLock l(&filter_->lock_);
  if (!connected_) {
    ZeroMemory(pmt, sizeof(*pmt));
    return VFW_E_NOT_CONNECTED;
  }
  AutoLock s(&streamLock_);
  return copyMediaType(pmt, &mt_);
}

STDMETHODIMP OutputPin::QueryPinInfo(PIN_INFO* pInfo) {
  if (!pInfo) return E_POINTER;
  pInfo->pFilter = filter_;
  filter_->AddRef();
  pInfo->dir = PINDIR_OUTPUT;
  wcscpy_s(pInfo->achName, MAX_PIN_NAME, OWC_VCAM_PIN_NAME);
  return S_OK;
}

STDMETHODIMP OutputPin::QueryDirection(PIN_DIRECTION* pPinDir) {
  if (!pPinDir) return E_POINTER;
  *pPinDir = PINDIR_OUTPUT;
  return S_OK;
}

STDMETHODIMP OutputPin::QueryId(LPWSTR* Id) {
  if (!Id) return E_POINTER;
  const size_t bytes = sizeof(OWC_VCAM_PIN_NAME);
  *Id = (LPWSTR)CoTaskMemAlloc(bytes);
  if (!*Id) return E_OUTOFMEMORY;
  memcpy(*Id, OWC_VCAM_PIN_NAME, bytes);
  return S_OK;
}

STDMETHODIMP OutputPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
  if (!pmt) return E_POINTER;
  AutoLock l(&filter_->lock_);
  return checkMediaType(pmt) ? S_OK : S_FALSE;
}

STDMETHODIMP OutputPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
  if (!ppEnum) return E_POINTER;
  *ppEnum = new (std::nothrow) TypeEnum(this, 0, typesVersion());
  return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP OutputPin::QueryInternalConnections(IPin**, ULONG*) { return E_NOTIMPL; }
STDMETHODIMP OutputPin::EndOfStream() { return E_UNEXPECTED; }
STDMETHODIMP OutputPin::BeginFlush() { return E_UNEXPECTED; }
STDMETHODIMP OutputPin::EndFlush() { return E_UNEXPECTED; }
STDMETHODIMP OutputPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

// ---------------------------------------------------------------- IQualityControl

// Live source: we cannot slow down or skip ahead in response to quality
// messages, and frames are always the newest anyway.
STDMETHODIMP OutputPin::Notify(IBaseFilter*, Quality) { return S_OK; }
STDMETHODIMP OutputPin::SetSink(IQualityControl*) { return S_OK; }

// ---------------------------------------------------------------- IAMStreamConfig

STDMETHODIMP OutputPin::SetFormat(AM_MEDIA_TYPE* pmt) {
  IFilterGraph* graph = nullptr;
  AM_MEDIA_TYPE nmt = {};
  {
    AutoLock l(&filter_->lock_);
    if (!pmt) {  // reset to the default catalogue
      if (filter_->state_ != State_Stopped) return VFW_E_NOT_STOPPED;
      hasSetFormat_ = false;
      ++typesVersion_;
      return S_OK;
    }
    ParsedType pt;
    if (!parseMediaType(pmt, &pt) || !sizeInCatalogue(pt.width, pt.height)) return VFW_E_INVALID_MEDIA_TYPE;
    REFERENCE_TIME avg = pt.avgTimePerFrame;
    if (avg == 0) avg = kInterval30;
    if (avg < kMinInterval || avg > kMaxInterval) return VFW_E_INVALID_MEDIA_TYPE;
    VideoFormat vf;
    vf.fmt = pt.fmt;
    vf.width = pt.width;
    vf.height = pt.height;
    vf.avgTimePerFrame = avg;

    bool sameAsConnection = false;
    if (connected_) {
      AutoLock s(&streamLock_);
      sameAsConnection = cur_.fmt == vf.fmt && cur_.width == vf.width && cur_.height == vf.height &&
                         interval_ == vf.avgTimePerFrame;
    }
    if (filter_->state_ != State_Stopped && !sameAsConnection) return VFW_E_NOT_STOPPED;
    if (connected_ && !sameAsConnection) {
      HRESULT hr = buildMediaType(vf, &nmt);
      if (FAILED(hr)) return hr;
      if (connected_->QueryAccept(&nmt) != S_OK) {
        freeMediaType(nmt);
        return VFW_E_INVALID_MEDIA_TYPE;
      }
      graph = filter_->graph_;
      if (graph) graph->AddRef();
    }
    setFormat_ = vf;
    hasSetFormat_ = true;
    ++typesVersion_;
    if (!graph) {
      freeMediaType(nmt);
      return S_OK;
    }
  }
  // Reconnect outside our lock (the graph manager calls back into both pins).
  HRESULT hr;
  IFilterGraph2* g2 = nullptr;
  if (SUCCEEDED(graph->QueryInterface(IID_IFilterGraph2, (void**)&g2)) && g2) {
    hr = g2->ReconnectEx(static_cast<IPin*>(this), &nmt);
    g2->Release();
  } else {
    hr = graph->Reconnect(static_cast<IPin*>(this));
  }
  graph->Release();
  freeMediaType(nmt);
  return hr;
}

STDMETHODIMP OutputPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
  if (!ppmt) return E_POINTER;
  *ppmt = nullptr;
  AutoLock l(&filter_->lock_);
  if (connected_) {
    AutoLock s(&streamLock_);
    *ppmt = createMediaType(&mt_);
    return *ppmt ? S_OK : E_OUTOFMEMORY;
  }
  AM_MEDIA_TYPE* mt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
  if (!mt) return E_OUTOFMEMORY;
  const HRESULT hr = buildMediaType(hasSetFormat_ ? setFormat_ : defaultFormat(), mt);
  if (FAILED(hr)) {
    CoTaskMemFree(mt);
    return hr;
  }
  *ppmt = mt;
  return S_OK;
}

STDMETHODIMP OutputPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
  if (!piCount || !piSize) return E_POINTER;
  *piCount = (int)catalogue_.size();
  *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
  return S_OK;
}

STDMETHODIMP OutputPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
  if (!ppmt || !pSCC) return E_POINTER;
  *ppmt = nullptr;
  if (iIndex < 0) return E_INVALIDARG;
  if (iIndex >= (int)catalogue_.size()) return S_FALSE;
  const VideoFormat& vf = catalogue_[(size_t)iIndex];
  AM_MEDIA_TYPE* mt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
  if (!mt) return E_OUTOFMEMORY;
  const HRESULT hr = buildMediaType(vf, mt);
  if (FAILED(hr)) {
    CoTaskMemFree(mt);
    return hr;
  }
  *ppmt = mt;
  VIDEO_STREAM_CONFIG_CAPS* c = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
  ZeroMemory(c, sizeof(*c));
  const SIZE sz = {vf.width, vf.height};
  c->guid = FORMAT_VideoInfo;
  c->VideoStandard = 0;
  c->InputSize = sz;
  c->MinCroppingSize = sz;
  c->MaxCroppingSize = sz;
  c->CropGranularityX = 1;
  c->CropGranularityY = 1;
  c->CropAlignX = 1;
  c->CropAlignY = 1;
  c->MinOutputSize = sz;
  c->MaxOutputSize = sz;
  c->OutputGranularityX = 1;  // never 0: some apps loop min..max by granularity
  c->OutputGranularityY = 1;
  c->StretchTapsX = 0;
  c->StretchTapsY = 0;
  c->ShrinkTapsX = 0;
  c->ShrinkTapsY = 0;
  c->MinFrameInterval = vf.avgTimePerFrame;
  c->MaxFrameInterval = vf.avgTimePerFrame;
  const VIDEOINFOHEADER* vih = (const VIDEOINFOHEADER*)mt->pbFormat;
  c->MinBitsPerSecond = (LONG)vih->dwBitRate;
  c->MaxBitsPerSecond = (LONG)vih->dwBitRate;
  return S_OK;
}

// ---------------------------------------------------------------- IKsPropertySet

STDMETHODIMP OutputPin::Set(REFGUID guidPropSet, DWORD, LPVOID, DWORD, LPVOID, DWORD) {
  if (!IsEqualGUID(guidPropSet, AMPROPSETID_Pin)) return E_PROP_SET_UNSUPPORTED;
  return E_PROP_ID_UNSUPPORTED;  // the category is read-only
}

STDMETHODIMP OutputPin::Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID, DWORD, LPVOID pPropData,
                            DWORD cbPropData, DWORD* pcbReturned) {
  if (!IsEqualGUID(guidPropSet, AMPROPSETID_Pin)) return E_PROP_SET_UNSUPPORTED;
  if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
  if (!pPropData && !pcbReturned) return E_POINTER;
  if (pcbReturned) *pcbReturned = sizeof(GUID);
  if (!pPropData) return S_OK;
  if (cbPropData < sizeof(GUID)) return E_UNEXPECTED;
  *(GUID*)pPropData = PIN_CATEGORY_CAPTURE;
  return S_OK;
}

STDMETHODIMP OutputPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
  if (!IsEqualGUID(guidPropSet, AMPROPSETID_Pin)) return E_PROP_SET_UNSUPPORTED;
  if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
  if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
  return S_OK;
}

// ---------------------------------------------------------------- IAMLatency / IAMPushSource

// Samples are time-stamped with the graph clock right before Receive(); the
// only latency between the stamp and delivery is the conversion (~1 ms).
STDMETHODIMP OutputPin::GetLatency(REFERENCE_TIME* prtLatency) {
  if (!prtLatency) return E_POINTER;
  *prtLatency = 10000;
  return S_OK;
}
STDMETHODIMP OutputPin::GetPushSourceFlags(ULONG* pFlags) {
  if (!pFlags) return E_POINTER;
  *pFlags = 0;  // live, time-stamped with the graph clock
  return S_OK;
}
STDMETHODIMP OutputPin::SetPushSourceFlags(ULONG) { return E_NOTIMPL; }
STDMETHODIMP OutputPin::SetStreamOffset(REFERENCE_TIME rtOffset) {
  AutoLock s(&streamLock_);
  streamOffset_ = rtOffset;
  return S_OK;
}
STDMETHODIMP OutputPin::GetStreamOffset(REFERENCE_TIME* prtOffset) {
  if (!prtOffset) return E_POINTER;
  AutoLock s(&streamLock_);
  *prtOffset = streamOffset_;
  return S_OK;
}
STDMETHODIMP OutputPin::GetMaxStreamOffset(REFERENCE_TIME* prtMaxOffset) {
  if (!prtMaxOffset) return E_POINTER;
  AutoLock s(&streamLock_);
  *prtMaxOffset = maxStreamOffset_;
  return S_OK;
}
STDMETHODIMP OutputPin::SetMaxStreamOffset(REFERENCE_TIME rtMaxOffset) {
  AutoLock s(&streamLock_);
  maxStreamOffset_ = rtMaxOffset;
  return S_OK;
}

// ---------------------------------------------------------------- state transitions

HRESULT OutputPin::active() {
  if (thread_) return S_OK;
  if (!connected_ || !alloc_) return S_OK;  // unconnected pin: nothing to stream
  HRESULT hr = alloc_->Commit();
  if (FAILED(hr)) return hr;
  {
    AutoLock s(&streamLock_);
    eventSink_ = nullptr;
    IMediaEventSink* sink = nullptr;
    if (filter_->graph_ &&
        SUCCEEDED(filter_->graph_->QueryInterface(IID_IMediaEventSink, (void**)&sink)) && sink) {
      eventSink_ = sink;  // weak, as in the SDK base classes
      sink->Release();
    }
    running_ = false;
  }
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (stopEvent_ && wakeEvent_) thread_ = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
  if (!thread_) {
    alloc_->Decommit();
    if (stopEvent_) CloseHandle(stopEvent_);
    if (wakeEvent_) CloseHandle(wakeEvent_);
    stopEvent_ = wakeEvent_ = nullptr;
    return E_OUTOFMEMORY;
  }
  return S_OK;
}

void OutputPin::inactive() {
  if (alloc_) alloc_->Decommit();  // releases a GetBuffer() wait
  if (thread_) {
    SetEvent(stopEvent_);
    WaitForSingleObject(thread_, INFINITE);
    CloseHandle(thread_);
    thread_ = nullptr;
  }
  if (stopEvent_) CloseHandle(stopEvent_);
  if (wakeEvent_) CloseHandle(wakeEvent_);
  stopEvent_ = wakeEvent_ = nullptr;
  AutoLock s(&streamLock_);
  eventSink_ = nullptr;
  running_ = false;
  if (runClock_) {
    runClock_->Release();
    runClock_ = nullptr;
  }
}

void OutputPin::setRunning(bool running, REFERENCE_TIME tStart, IReferenceClock* clock) {
  {
    AutoLock s(&streamLock_);
    if (running && clock) clock->AddRef();
    if (runClock_) runClock_->Release();
    runClock_ = running ? clock : nullptr;
    running_ = running;
    runStart_ = tStart;
    if (running) ++runGeneration_;
  }
  if (wakeEvent_) SetEvent(wakeEvent_);
}

void OutputPin::getStats(OwcVcamStats* out) {
  AutoLock s(&streamLock_);
  *out = stats_;
  out->convertAvgMs = stats_.convertCount ? convertSumMs_ / (double)stats_.convertCount : 0.0;
  out->outWidth = (uint32_t)cur_.width;
  out->outHeight = (uint32_t)cur_.height;
  out->outStride = cur_.stride;
  out->outFormat = (uint32_t)cur_.fmt;
  out->avgTimePerFrame = interval_;
}

void OutputPin::resetStats() {
  AutoLock s(&streamLock_);
  const uint32_t bufs = stats_.allocBuffers, bytes = stats_.allocBufferBytes;
  stats_ = OwcVcamStats{};
  stats_.cbSize = sizeof(stats_);
  stats_.allocBuffers = bufs;
  stats_.allocBufferBytes = bytes;
  convertSumMs_ = 0;
}

// ---------------------------------------------------------------- streaming

DWORD WINAPI OutputPin::threadProc(void* self) {
  static_cast<OutputPin*>(self)->streamLoop();
  return 0;
}

// Pacing (all times in 100 ns, QPC domain): a GCRA-style scheduler.
//  * `tat` is the theoretical time of the next delivery; the long-term rate
//    never exceeds the negotiated one.
//  * A newly published shm frame may go out up to T/4 early, so a producer
//    running at our rate is forwarded immediately on arrival (latency =
//    poll granularity, ~1 ms) instead of waiting for a fixed grid.
//  * If the producer's measured interval is within 15% of T, the grid is
//    also pulled towards the arrival phase (phase lock), otherwise (e.g.
//    60 fps source into a 30 fps pin) we take the newest frame at each slot.
//  * With nothing new, the last frame (or the placeholder) is repeated on
//    the grid, slightly late (T/8, max 4 ms; T/2 when locked) so a late
//    frame is not preceded by a needless repeat.
// Samples are stamped with the graph clock's stream time at delivery; the
// QPC only drives pacing (real cameras are paced by their own clock too).
void OutputPin::streamLoop() {
  const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
  if (!mmcss) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  bool coarseTimer = false;
  if (!timer) {  // pre-1803 Windows 10
    timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    timeBeginPeriod(1);
    coarseTimer = true;
  }

  LONG gen = -1;
  bool halted = false, discontinuity = true;
  int64_t tat = 0;
  uint64_t curNumber = 0;  // shm frame held in srcBuf_ (0 = none)
  owc::ShmReader::FrameInfo curInfo;
  uint64_t observedNumber = 0;
  int64_t observedAt = 0;
  bool observedGenuine = false;  // observedAt is a real arrival time (seen while polling)
  int64_t lastPollAt = 0;
  double producerInterval = 0;  // EMA of arrival intervals, hns
  int64_t nextOpenAttempt = 0, lastAliveAt = 0;
  REFERENCE_TIME lastStamp = LLONG_MIN;

  for (;;) {
    if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;

    bool running;
    LONG g;
    REFERENCE_TIME interval, tStart, offset;
    IReferenceClock* clk;
    {
      AutoLock s(&streamLock_);
      running = running_;
      g = runGeneration_;
      interval = interval_;
      tStart = runStart_;
      offset = streamOffset_;
      clk = runClock_;
      if (clk) clk->AddRef();
    }
    if (!running || (g == gen && halted)) {  // paused (live source: no data) or downstream refused
      if (clk) clk->Release();
      HANDLE hs[2] = {stopEvent_, wakeEvent_};
      WaitForMultipleObjects(2, hs, FALSE, 100);
      continue;
    }
    int64_t now = nowHns();
    if (g != gen) {
      gen = g;
      halted = false;
      discontinuity = true;
      tat = now;
      lastStamp = LLONG_MIN;
      lastPollAt = 0;  // the first observation of a run is not an arrival time
    }
    const int64_t T = interval > 0 ? interval : kInterval30;
    const int64_t tau = T / 4;
    int64_t slack = T / 8 < 40000 ? T / 8 : 40000;

    // ---- poll the producer
    bool alive = false;
    uint64_t latest = 0;
    if (!shm_.isOpen() && now >= nextOpenAttempt) {
      if (!shm_.open()) nextOpenAttempt = now + 2500000;  // retry every 250 ms
    }
    if (shm_.isOpen()) {
      alive = shm_.producerAlive();
      if (alive) {
        lastAliveAt = now;
        latest = shm_.latestFrameNumber();
      } else if (now - lastAliveAt > 30000000) {
        // Producer gone for 3 s: drop our view so the section can be freed.
        shm_.close();
        curNumber = observedNumber = 0;
        nextOpenAttempt = now + 2500000;
      }
    }
    if (alive && latest != 0 && latest != observedNumber) {
      // Only a change seen by continuous polling dates an arrival; a frame
      // found after a pause/start was published at some unknown earlier time
      // and would poison the interval estimate (and delay the phase lock).
      const bool genuine = observedNumber != 0 && lastPollAt != 0 && now - lastPollAt < 50000;
      if (genuine && observedGenuine) {
        const double d = (double)(now - observedAt);
        producerInterval = producerInterval > 0 ? producerInterval * 0.875 + d * 0.125 : d;
      }
      observedNumber = latest;
      observedAt = now;
      observedGenuine = genuine;
    }
    lastPollAt = now;
    double srcInterval = producerInterval;
    if (srcInterval <= 0 && alive && shm_.header()->nominal_fps_num)  // until measured: declared rate
      srcInterval = 1e7 * (double)shm_.header()->nominal_fps_den / (double)shm_.header()->nominal_fps_num;
    // Locked: the producer runs at our rate. A missing frame then is far more
    // likely late (network/decoder jitter) than dropped, so wait up to T/2
    // before repeating; a repeat followed by the late frame would give a
    // short/long interval pair and push the following frames behind the grid.
    const bool locked = alive && srcInterval > 0.85 * (double)T && srcInterval < 1.15 * (double)T;
    if (locked) slack = T / 2;
    const bool pending = alive && latest != 0 && latest != curNumber;
    const bool deliverFresh = pending && now >= tat - tau;
    const bool deliverRepeat = !pending && now >= tat + slack;

    if (deliverFresh || deliverRepeat) {
      IMediaSample* sample = nullptr;
      HRESULT hr = alloc_->GetBuffer(&sample, nullptr, nullptr, 0);  // may block on downstream
      if (FAILED(hr) || !sample) {
        if (clk) clk->Release();
        WaitForSingleObject(stopEvent_, 5);
        continue;
      }
      AM_MEDIA_TYPE* smt = nullptr;
      if (sample->GetMediaType(&smt) == S_OK && smt) {  // downstream-initiated change (stride etc.)
        ParsedType pt;
        if (parseMediaType(smt, &pt)) {
          AutoLock s(&streamLock_);
          freeMediaType(mt_);
          copyMediaType(&mt_, smt);
          cur_ = pt;
          ++stats_.mediaTypeChanges;
        }
        deleteMediaType(smt);
      }
      ParsedType out;
      {
        AutoLock s(&streamLock_);
        out = cur_;
      }
      BYTE* buf = nullptr;
      sample->GetPointer(&buf);
      const long cap = sample->GetSize();

      // newest frame right now (GetBuffer may have waited)
      bool fresh = false;
      if (shm_.isOpen() && shm_.producerAlive()) {
        if (!srcBuf_)
          srcBuf_ = (uint8_t*)VirtualAlloc(nullptr, OWC_SHM_SLOT_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        owc::ShmReader::FrameInfo fi;
        if (srcBuf_ && shm_.readLatest(curNumber, srcBuf_, OWC_SHM_SLOT_BYTES, fi)) {
          if (curNumber != 0 && fi.frame_number > curNumber + 1) {
            AutoLock s(&streamLock_);
            stats_.skippedSource += fi.frame_number - curNumber - 1;
          }
          curNumber = fi.frame_number;
          curInfo = fi;
          fresh = true;
        }
      } else {
        curNumber = 0;  // show the placeholder; the next live frame counts as fresh
      }
      Nv12View src;
      uint64_t srcId;
      if (curNumber != 0) {
        src.y = srcBuf_;
        src.uv = srcBuf_ + (size_t)curInfo.width * curInfo.height;
        src.width = (int)curInfo.width;
        src.height = (int)curInfo.height;
        src.yStride = src.uvStride = (int)curInfo.width;
        srcId = curNumber;
      } else {
        if (placeholderW_ != out.width || placeholderH_ != out.height) {
          placeholder_.resize((size_t)out.width * out.height * 3 / 2);
          drawPlaceholder(placeholder_.data(), out.width, out.height);
          placeholderW_ = out.width;
          placeholderH_ = out.height;
        }
        src.y = placeholder_.data();
        src.uv = src.y + (size_t)out.width * out.height;
        src.width = out.width;
        src.height = out.height;
        src.yStride = src.uvStride = out.width;
        srcId = kPlaceholderId;
      }
      const int64_t c0 = owc::qpcNow();
      const bool ok = buf && conv_.convert(src, srcId, buf, cap > 0 ? (size_t)cap : 0, out.desc());
      const double convMs = qpcToMs(owc::qpcNow() - c0);

      if (ok) {
        sample->SetActualDataLength((long)outputBytes(out.desc()));
        REFERENCE_TIME rt = 0;
        if (clk && SUCCEEDED(clk->GetTime(&rt))) {
          REFERENCE_TIME st = rt - tStart + offset;
          if (lastStamp != LLONG_MIN && st <= lastStamp) st = lastStamp + 1;
          REFERENCE_TIME et = st + T;
          sample->SetTime(&st, &et);
          lastStamp = st;
        } else {
          sample->SetTime(nullptr, nullptr);
        }
        sample->SetSyncPoint(TRUE);
        sample->SetDiscontinuity(discontinuity ? TRUE : FALSE);
        sample->SetPreroll(FALSE);
        hr = input_->Receive(sample);
        discontinuity = false;
      } else {
        hr = E_UNEXPECTED;  // buffer too small for the negotiated type
      }
      sample->Release();
      {
        AutoLock s(&streamLock_);
        if (ok) {
          ++stats_.delivered;
          if (curNumber == 0)
            ++stats_.placeholderFrames;
          else if (fresh)
            ++stats_.freshFrames;
          else
            ++stats_.repeatedFrames;
          ++stats_.convertCount;
          convertSumMs_ += convMs;
          stats_.convertLastMs = convMs;
          if (convMs > stats_.convertMaxMs) stats_.convertMaxMs = convMs;
          stats_.lastSourceFrame = curNumber;
          stats_.srcWidth = curNumber ? curInfo.width : (uint32_t)out.width;
          stats_.srcHeight = curNumber ? curInfo.height : (uint32_t)out.height;
        }
        if (hr != S_OK) {
          ++stats_.receiveFailures;
          stats_.lastError = (int32_t)(FAILED(hr) ? hr : S_FALSE);
        }
      }
      if (hr != S_OK) {
        // S_FALSE: downstream wants no more data until the next run; error: abort.
        if (FAILED(hr) && eventSink_) eventSink_->Notify(EC_ERRORABORT, hr, 0);
        halted = true;
      }
      // advance the schedule (decision time `now`)
      if (deliverFresh) {
        const int64_t base = tat > now ? tat : now;
        int64_t pull = 0;
        if (locked) {
          // how long the frame waited for us: exact from its publish time
          // (same QPC clock), else from when polling first saw it
          const int64_t arrival = fresh && curInfo.publish_qpc > 0 ? qpcToHns(curInfo.publish_qpc)
                                  : observedGenuine                ? observedAt
                                                                   : now;
          pull = now - arrival;
          if (pull < 0) pull = 0;
          if (pull > T / 4) pull = T / 4;
        }
        tat = base + T - pull;
      } else {
        tat += T;
        if (now - tat > T) tat = now;  // long stall (blocked downstream): resync
      }
    }
    if (clk) clk->Release();

    // ---- sleep until the next decision point, polling at ~1 ms
    now = nowHns();
    const bool pendingNow = alive && shm_.isOpen() && shm_.latestFrameNumber() != curNumber;
    int64_t wait = (pendingNow ? tat - tau : tat + slack) - now;
    if (wait > 10000) wait = 10000;
    if (wait > 200) {
      LARGE_INTEGER due;
      due.QuadPart = -wait;
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      HANDLE hs[3] = {stopEvent_, wakeEvent_, timer};
      WaitForMultipleObjects(3, hs, FALSE, 50);
    }
  }

  if (timer) {
    CancelWaitableTimer(timer);
    CloseHandle(timer);
  }
  if (coarseTimer) timeEndPeriod(1);
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  shm_.close();
  if (srcBuf_) {
    VirtualFree(srcBuf_, 0, MEM_RELEASE);
    srcBuf_ = nullptr;
  }
  conv_.reset();
  std::vector<uint8_t>().swap(placeholder_);
  placeholderW_ = placeholderH_ = 0;
  if (SUCCEEDED(hrCo)) CoUninitialize();
}

// ================================================================ Filter

Filter::Filter() : pin_(this) {
  InitializeCriticalSection(&lock_);
  InterlockedIncrement(&g_objects);
}

Filter::~Filter() {
  {
    AutoLock l(&lock_);
    if (state_ != State_Stopped) {  // released without Stop(): be defensive
      pin_.setRunning(false, 0, nullptr);
      pin_.inactive();
      state_ = State_Stopped;
    }
  }
  if (clock_) clock_->Release();
  DeleteCriticalSection(&lock_);
  InterlockedDecrement(&g_objects);
}

STDMETHODIMP Filter::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IPersist) || IsEqualIID(riid, IID_IMediaFilter) ||
      IsEqualIID(riid, IID_IBaseFilter))
    *ppv = static_cast<IBaseFilter*>(this);
  else if (IsEqualIID(riid, IID_IAMFilterMiscFlags))
    *ppv = static_cast<IAMFilterMiscFlags*>(this);
  else if (IsEqualIID(riid, __uuidof(IOwcVcamStats)))
    *ppv = static_cast<IOwcVcamStats*>(this);
  else {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) Filter::AddRef() { return (ULONG)InterlockedIncrement(&ref_); }

STDMETHODIMP_(ULONG) Filter::Release() {
  const LONG r = InterlockedDecrement(&ref_);
  if (r == 0) delete this;
  return (ULONG)r;
}

STDMETHODIMP Filter::GetClassID(CLSID* pClassID) {
  if (!pClassID) return E_POINTER;
  *pClassID = CLSID_OwcVirtualCam;
  return S_OK;
}

STDMETHODIMP Filter::Stop() {
  AutoLock l(&lock_);
  if (state_ != State_Stopped) {
    pin_.setRunning(false, 0, nullptr);
    pin_.inactive();
    state_ = State_Stopped;
  }
  return S_OK;
}

STDMETHODIMP Filter::Pause() {
  AutoLock l(&lock_);
  if (state_ == State_Stopped) {
    const HRESULT hr = pin_.active();
    if (FAILED(hr)) return hr;
  } else if (state_ == State_Running) {
    pin_.setRunning(false, 0, nullptr);
  }
  state_ = State_Paused;
  return S_OK;
}

STDMETHODIMP Filter::Run(REFERENCE_TIME tStart) {
  AutoLock l(&lock_);
  if (state_ == State_Stopped) {
    const HRESULT hr = pin_.active();
    if (FAILED(hr)) return hr;
  }
  tStart_ = tStart;
  state_ = State_Running;
  pin_.setRunning(true, tStart, clock_);
  return S_OK;
}

STDMETHODIMP Filter::GetState(DWORD, FILTER_STATE* State) {
  if (!State) return E_POINTER;
  AutoLock l(&lock_);
  *State = state_;
  return state_ == State_Paused ? VFW_S_CANT_CUE : S_OK;  // live source
}

STDMETHODIMP Filter::SetSyncSource(IReferenceClock* pClock) {
  AutoLock l(&lock_);
  if (pClock) pClock->AddRef();
  if (clock_) clock_->Release();
  clock_ = pClock;
  return S_OK;
}

STDMETHODIMP Filter::GetSyncSource(IReferenceClock** pClock) {
  if (!pClock) return E_POINTER;
  AutoLock l(&lock_);
  *pClock = clock_;
  if (clock_) clock_->AddRef();
  return S_OK;
}

STDMETHODIMP Filter::EnumPins(IEnumPins** ppEnum) {
  if (!ppEnum) return E_POINTER;
  *ppEnum = new (std::nothrow) PinEnum(this, 0);
  return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP Filter::FindPin(LPCWSTR Id, IPin** ppPin) {
  if (!Id || !ppPin) return E_POINTER;
  if (wcscmp(Id, OWC_VCAM_PIN_NAME) == 0) {
    *ppPin = &pin_;
    pin_.AddRef();
    return S_OK;
  }
  *ppPin = nullptr;
  return VFW_E_NOT_FOUND;
}

STDMETHODIMP Filter::QueryFilterInfo(FILTER_INFO* pInfo) {
  if (!pInfo) return E_POINTER;
  AutoLock l(&lock_);
  wcscpy_s(pInfo->achName, MAX_FILTER_NAME, name_);
  pInfo->pGraph = graph_;
  if (graph_) graph_->AddRef();
  return S_OK;
}

STDMETHODIMP Filter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) {
  AutoLock l(&lock_);
  graph_ = pGraph;  // not AddRef'd: the graph owns us
  if (pName)
    wcsncpy_s(name_, MAX_FILTER_NAME, pName, _TRUNCATE);
  else
    name_[0] = 0;
  return S_OK;
}

STDMETHODIMP Filter::QueryVendorInfo(LPWSTR*) { return E_NOTIMPL; }

STDMETHODIMP_(ULONG) Filter::GetMiscFlags() { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

STDMETHODIMP Filter::GetStats(OwcVcamStats* stats) {
  if (!stats) return E_POINTER;
  pin_.getStats(stats);
  AutoLock l(&lock_);
  stats->state = (uint32_t)state_;
  return S_OK;
}

STDMETHODIMP Filter::ResetStats() {
  pin_.resetStats();
  return S_OK;
}

HRESULT CreateFilterInstance(IUnknown* outer, REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  *ppv = nullptr;
  if (outer) return CLASS_E_NOAGGREGATION;
  Filter* f = new (std::nothrow) Filter();
  if (!f) return E_OUTOFMEMORY;
  const HRESULT hr = f->QueryInterface(riid, ppv);
  f->Release();
  return hr;
}

}  // namespace vcam
}  // namespace owc
