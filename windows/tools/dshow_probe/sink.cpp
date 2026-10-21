#include "sink.h"

#include <owc/shm.h>

#include <new>

#include "mediatypes.h"

using owc::vcam::copyMediaType;
using owc::vcam::freeMediaType;

namespace probe {

namespace {

volatile LONG g_live = 0;

// {6A0C8C3E-6C4B-4E59-9C31-5F7B6A0D7E11} (private, never registered)
const CLSID CLSID_ProbeSink = {0x6a0c8c3e, 0x6c4b, 0x4e59, {0x9c, 0x31, 0x5f, 0x7b, 0x6a, 0x0d, 0x7e, 0x11}};

struct Lock {
  explicit Lock(CRITICAL_SECTION* c) : c_(c) { EnterCriticalSection(c_); }
  ~Lock() { LeaveCriticalSection(c_); }
  CRITICAL_SECTION* c_;
};

class OnePinEnum final : public IEnumPins {
 public:
  OnePinEnum(IBaseFilter* f, IPin* p, ULONG pos) : f_(f), p_(p), pos_(pos) { f_->AddRef(); }
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IEnumPins) {
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
    if (!r) {
      f_->Release();
      delete this;
    }
    return (ULONG)r;
  }
  STDMETHODIMP Next(ULONG c, IPin** pp, ULONG* fetched) override {
    if (!pp) return E_POINTER;
    ULONG n = 0;
    while (n < c && pos_ < 1) {
      pp[n++] = p_;
      p_->AddRef();
      ++pos_;
    }
    if (fetched) *fetched = n;
    return n == c ? S_OK : S_FALSE;
  }
  STDMETHODIMP Skip(ULONG c) override {
    pos_ += c;
    if (pos_ > 1) {
      pos_ = 1;
      return S_FALSE;
    }
    return S_OK;
  }
  STDMETHODIMP Reset() override {
    pos_ = 0;
    return S_OK;
  }
  STDMETHODIMP Clone(IEnumPins** pp) override {
    if (!pp) return E_POINTER;
    *pp = new (std::nothrow) OnePinEnum(f_, p_, pos_);
    return *pp ? S_OK : E_OUTOFMEMORY;
  }

 private:
  volatile LONG ref_ = 1;
  IBaseFilter* f_;
  IPin* p_;
  ULONG pos_;
};

}  // namespace

LONG SinkFilter::liveObjects() { return g_live; }

// ---------------------------------------------------------------- SinkPin

SinkPin::~SinkPin() {
  if (alloc_) alloc_->Release();
  if (connected_) connected_->Release();
  freeMediaType(mt_);
}

STDMETHODIMP SinkPin::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IPin)
    *ppv = static_cast<IPin*>(this);
  else if (riid == IID_IMemInputPin)
    *ppv = static_cast<IMemInputPin*>(this);
  else {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}
STDMETHODIMP_(ULONG) SinkPin::AddRef() { return f_->AddRef(); }
STDMETHODIMP_(ULONG) SinkPin::Release() { return f_->Release(); }

bool SinkPin::accepts(const AM_MEDIA_TYPE* pmt) const {
  if (!pmt || pmt->majortype != MEDIATYPE_Video) return false;
  if (f_->wantSubtype_ != GUID_NULL && pmt->subtype != f_->wantSubtype_) return false;
  owc::vcam::ParsedType pt;
  if (!owc::vcam::parseMediaType(pmt, &pt)) return false;
  if (f_->wantW_ && (pt.width != f_->wantW_ || pt.height != f_->wantH_)) return false;
  return true;
}

STDMETHODIMP SinkPin::ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) {
  if (!pConnector || !pmt) return E_POINTER;
  Lock l(&f_->lock_);
  if (connected_) return VFW_E_ALREADY_CONNECTED;
  if (f_->state_ != State_Stopped) return VFW_E_NOT_STOPPED;
  if (!accepts(pmt)) return VFW_E_TYPE_NOT_ACCEPTED;
  connected_ = pConnector;
  connected_->AddRef();
  freeMediaType(mt_);
  copyMediaType(&mt_, pmt);
  return S_OK;
}

STDMETHODIMP SinkPin::Disconnect() {
  Lock l(&f_->lock_);
  if (!connected_) return S_FALSE;
  connected_->Release();
  connected_ = nullptr;
  if (alloc_) {
    alloc_->Release();
    alloc_ = nullptr;
  }
  freeMediaType(mt_);
  ZeroMemory(&mt_, sizeof(mt_));
  return S_OK;
}

STDMETHODIMP SinkPin::ConnectedTo(IPin** pPin) {
  if (!pPin) return E_POINTER;
  Lock l(&f_->lock_);
  *pPin = connected_;
  if (!connected_) return VFW_E_NOT_CONNECTED;
  connected_->AddRef();
  return S_OK;
}

STDMETHODIMP SinkPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
  if (!pmt) return E_POINTER;
  Lock l(&f_->lock_);
  if (!connected_) {
    ZeroMemory(pmt, sizeof(*pmt));
    return VFW_E_NOT_CONNECTED;
  }
  return copyMediaType(pmt, &mt_);
}

STDMETHODIMP SinkPin::QueryPinInfo(PIN_INFO* pInfo) {
  if (!pInfo) return E_POINTER;
  pInfo->pFilter = f_;
  f_->AddRef();
  pInfo->dir = PINDIR_INPUT;
  wcscpy_s(pInfo->achName, MAX_PIN_NAME, L"In");
  return S_OK;
}

STDMETHODIMP SinkPin::QueryDirection(PIN_DIRECTION* d) {
  if (!d) return E_POINTER;
  *d = PINDIR_INPUT;
  return S_OK;
}

STDMETHODIMP SinkPin::QueryId(LPWSTR* Id) {
  if (!Id) return E_POINTER;
  *Id = (LPWSTR)CoTaskMemAlloc(sizeof(L"In"));
  if (!*Id) return E_OUTOFMEMORY;
  memcpy(*Id, L"In", sizeof(L"In"));
  return S_OK;
}

STDMETHODIMP SinkPin::QueryAccept(const AM_MEDIA_TYPE* pmt) { return accepts(pmt) ? S_OK : S_FALSE; }

STDMETHODIMP SinkPin::GetAllocator(IMemAllocator** pp) {
  if (!pp) return E_POINTER;
  *pp = nullptr;
  if (!f_->offerAllocator_) return VFW_E_NO_ALLOCATOR;
  return CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator, (void**)pp);
}

STDMETHODIMP SinkPin::NotifyAllocator(IMemAllocator* a, BOOL) {
  if (!a) return E_POINTER;
  Lock l(&f_->lock_);
  a->AddRef();
  if (alloc_) alloc_->Release();
  alloc_ = a;
  return S_OK;
}

STDMETHODIMP SinkPin::GetAllocatorRequirements(ALLOCATOR_PROPERTIES* p) {
  if (!p) return E_POINTER;
  if (!f_->offerAllocator_) return E_NOTIMPL;
  ZeroMemory(p, sizeof(*p));
  p->cbAlign = 16;  // exercise the requirement path
  p->cBuffers = 2;
  return S_OK;
}

STDMETHODIMP SinkPin::Receive(IMediaSample* s) {
  if (!s) return E_POINTER;
  SampleInfo si;
  si.qpcReceived = owc::qpcNow();
  si.sample = s;
  si.connectionType = &mt_;
  if (f_->state_ == State_Stopped) return VFW_E_WRONG_STATE;
  if (f_->cb_) f_->cb_(si);
  return S_OK;
}

STDMETHODIMP SinkPin::ReceiveMultiple(IMediaSample** pSamples, long n, long* done) {
  if (!pSamples || !done) return E_POINTER;
  *done = 0;
  for (long i = 0; i < n; ++i) {
    const HRESULT hr = Receive(pSamples[i]);
    if (hr != S_OK) return hr;
    ++*done;
  }
  return S_OK;
}

// ---------------------------------------------------------------- SinkFilter

SinkFilter::SinkFilter(const GUID& wantSubtype, int wantW, int wantH, bool offerAllocator, Callback cb)
    : wantSubtype_(wantSubtype), wantW_(wantW), wantH_(wantH), offerAllocator_(offerAllocator),
      cb_(std::move(cb)), pin_(this) {
  InitializeCriticalSection(&lock_);
  InterlockedIncrement(&g_live);
}

SinkFilter::~SinkFilter() {
  if (clock_) clock_->Release();
  DeleteCriticalSection(&lock_);
  InterlockedDecrement(&g_live);
}

STDMETHODIMP SinkFilter::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
    *ppv = static_cast<IBaseFilter*>(this);
    AddRef();
    return S_OK;
  }
  *ppv = nullptr;
  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) SinkFilter::Release() {
  const LONG r = InterlockedDecrement(&ref_);
  if (!r) delete this;
  return (ULONG)r;
}

STDMETHODIMP SinkFilter::GetClassID(CLSID* p) {
  if (!p) return E_POINTER;
  *p = CLSID_ProbeSink;
  return S_OK;
}
STDMETHODIMP SinkFilter::Stop() {
  Lock l(&lock_);
  state_ = State_Stopped;
  return S_OK;
}
STDMETHODIMP SinkFilter::Pause() {
  Lock l(&lock_);
  state_ = State_Paused;
  return S_OK;
}
STDMETHODIMP SinkFilter::Run(REFERENCE_TIME) {
  Lock l(&lock_);
  state_ = State_Running;
  return S_OK;
}
STDMETHODIMP SinkFilter::GetState(DWORD, FILTER_STATE* s) {
  if (!s) return E_POINTER;
  *s = state_;
  return S_OK;
}
STDMETHODIMP SinkFilter::SetSyncSource(IReferenceClock* c) {
  Lock l(&lock_);
  if (c) c->AddRef();
  if (clock_) clock_->Release();
  clock_ = c;
  return S_OK;
}
STDMETHODIMP SinkFilter::GetSyncSource(IReferenceClock** c) {
  if (!c) return E_POINTER;
  Lock l(&lock_);
  *c = clock_;
  if (clock_) clock_->AddRef();
  return S_OK;
}
STDMETHODIMP SinkFilter::EnumPins(IEnumPins** pp) {
  if (!pp) return E_POINTER;
  *pp = new (std::nothrow) OnePinEnum(this, &pin_, 0);
  return *pp ? S_OK : E_OUTOFMEMORY;
}
STDMETHODIMP SinkFilter::FindPin(LPCWSTR id, IPin** pp) {
  if (!id || !pp) return E_POINTER;
  if (wcscmp(id, L"In") == 0) {
    *pp = &pin_;
    pin_.AddRef();
    return S_OK;
  }
  *pp = nullptr;
  return VFW_E_NOT_FOUND;
}
STDMETHODIMP SinkFilter::QueryFilterInfo(FILTER_INFO* p) {
  if (!p) return E_POINTER;
  wcscpy_s(p->achName, MAX_FILTER_NAME, name_);
  p->pGraph = graph_;
  if (graph_) graph_->AddRef();
  return S_OK;
}
STDMETHODIMP SinkFilter::JoinFilterGraph(IFilterGraph* g, LPCWSTR name) {
  Lock l(&lock_);
  graph_ = g;
  if (name)
    wcsncpy_s(name_, MAX_FILTER_NAME, name, _TRUNCATE);
  else
    name_[0] = 0;
  return S_OK;
}

}  // namespace probe
