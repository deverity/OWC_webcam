#include "decoder.h"

#include "util.h"

#include <codecapi.h>
#include <d3d11.h>
#include <d3d10.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace owc {
namespace detail {

// ------------------------------------------------------------- thread scope
MfThreadScope::MfThreadScope() {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  com_ = SUCCEEDED(hr);  // S_FALSE also needs CoUninitialize
  ok_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
}
MfThreadScope::~MfThreadScope() {
  if (ok_) MFShutdown();
  if (com_) CoUninitialize();
}

namespace {

// Shared D3D11 device with video support (lazily created, per decoder object).
struct D3D {
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<ID3D10Multithread> mt;
  ComPtr<IMFDXGIDeviceManager> mgr;
  UINT token = 0;
  bool tried = false;

  bool ensure(std::string& err) {
    if (mgr) return true;
    if (tried) { err = "D3D11 video device unavailable"; return false; }
    tried = true;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                               D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                               D3D_FEATURE_LEVEL_9_3};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { err = "D3D11CreateDevice(VIDEO_SUPPORT) failed " + hresultToString(hr); return false; }
    if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
    hr = MFCreateDXGIDeviceManager(&token, &mgr);
    if (SUCCEEDED(hr)) hr = mgr->ResetDevice(dev.Get(), token);
    if (FAILED(hr)) { mgr.Reset(); err = "IMFDXGIDeviceManager failed " + hresultToString(hr); return false; }
    return true;
  }
};

void setLowLatency(IMFTransform* mft, bool on) {
  ComPtr<IMFAttributes> attr;
  if (SUCCEEDED(mft->GetAttributes(&attr)) && attr)
    attr->SetUINT32(MF_LOW_LATENCY, on ? TRUE : FALSE);
  ComPtr<ICodecAPI> api;
  if (SUCCEEDED(mft->QueryInterface(IID_PPV_ARGS(&api)))) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = on ? 1 : 0;
    if (FAILED(api->SetValue(&CODECAPI_AVLowLatencyMode, &v))) {
      v.vt = VT_BOOL;
      v.boolVal = on ? VARIANT_TRUE : VARIANT_FALSE;
      api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }
  }
}

std::string friendlyName(IMFActivate* a) {
  WCHAR* name = nullptr;
  UINT32 len = 0;
  std::string s = "MFT";
  if (SUCCEEDED(a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len)) && name) {
    s = wideToUtf8(name);
    CoTaskMemFree(name);
  }
  return s;
}

}  // namespace

// ------------------------------------------------------------- decoder
struct MfDecoder::Impl {
  DecoderConfig cfg;
  D3D d3d;
  ComPtr<IMFTransform> mft;
  std::string mftName;
  bool d3dSet = false;
  bool dxgiSeen = false;
  bool providesSamples = false;
  DWORD outCb = 0;
  uint32_t streamChanges = 0;
  // current output geometry
  uint32_t codedW = 0, codedH = 0, dispW = 0, dispH = 0;
  int32_t defaultStride = 0;
  ComPtr<ID3D11Texture2D> staging;
  D3D11_TEXTURE2D_DESC stagingDesc{};
  ComPtr<IMFSample> outSample;     // when the MFT doesn't provide samples
  ComPtr<IMFMediaBuffer> outBuf;
  int64_t readbackUs = 0;
  uint32_t mtMatrix = 0;
  int mtFullRange = -1;
  std::string lastErr;

  bool setOutputNV12() {
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> t;
      HRESULT hr = mft->GetOutputAvailableType(0, i, &t);
      if (FAILED(hr)) return false;
      GUID sub{};
      t->GetGUID(MF_MT_SUBTYPE, &sub);
      if (sub != MFVideoFormat_NV12) continue;
      if (FAILED(mft->SetOutputType(0, t.Get(), 0))) return false;
      UINT32 w = 0, h = 0;
      MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
      codedW = w;
      codedH = h;
      dispW = w;
      dispH = h;
      MFVideoArea area{};
      if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area),
                               sizeof(area), nullptr))) {
        if (area.Area.cx > 0 && area.Area.cy > 0 && static_cast<UINT32>(area.Area.cx) <= w &&
            static_cast<UINT32>(area.Area.cy) <= h) {
          dispW = static_cast<uint32_t>(area.Area.cx);
          dispH = static_cast<uint32_t>(area.Area.cy);
        }
      }
      dispW &= ~1u;
      dispH &= ~1u;
      UINT32 v = 0;
      mtMatrix = 0;
      if (SUCCEEDED(t->GetUINT32(MF_MT_YUV_MATRIX, &v)))
        mtMatrix = v == MFVideoTransferMatrix_BT709 ? 2u : v == MFVideoTransferMatrix_BT601 ? 1u : 0u;
      mtFullRange = -1;
      if (SUCCEEDED(t->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &v)))
        mtFullRange = v == MFNominalRange_0_255 ? 1 : v == MFNominalRange_16_235 ? 0 : -1;
      UINT32 stride = 0;
      defaultStride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))
                          ? static_cast<int32_t>(stride)
                          : static_cast<int32_t>(w);
      MFT_OUTPUT_STREAM_INFO si{};
      mft->GetOutputStreamInfo(0, &si);
      providesSamples = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                       MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
      outCb = si.cbSize;
      outSample.Reset();
      outBuf.Reset();
      return true;
    }
  }

  bool open(const DecoderConfig& c, std::string& err) {
    close();
    cfg = c;
    const GUID sub = c.codec == Codec::H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC;
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, sub};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &in, nullptr, &acts, &count);
    if (SUCCEEDED(hr) && count == 0) {
      CoTaskMemFree(acts);
      acts = nullptr;
      hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags | MFT_ENUM_FLAG_UNTRUSTED_STOREMFT, &in,
                     nullptr, &acts, &count);
    }
    if (FAILED(hr) || count == 0) {
      CoTaskMemFree(acts);
      err = c.codec == Codec::Hevc
                ? "no HEVC decoder installed (install 'HEVC Video Extensions' from the "
                  "Microsoft Store, or use H.264)"
                : "no H.264 decoder MFT found";
      return false;
    }
    for (UINT32 i = 0; i < count && !mft; ++i) {
      ComPtr<IMFTransform> t;
      if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&t)))) {
        mft = t;
        mftName = friendlyName(acts[i]);
      }
    }
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (!mft) { err = "could not activate decoder MFT"; return false; }

    // Low-latency BEFORE media types (research §5.2).
    setLowLatency(mft.Get(), c.low_latency);

    // Hardware: hand the D3D11 device manager to a D3D11-aware MFT.
    d3dSet = false;
    if (c.mode != DecoderMode::Software) {
      ComPtr<IMFAttributes> attr;
      UINT32 aware = 0;
      if (SUCCEEDED(mft->GetAttributes(&attr)) && attr)
        attr->GetUINT32(MF_SA_D3D11_AWARE, &aware);
      std::string derr;
      if (aware && d3d.ensure(derr)) {
        hr = mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                 reinterpret_cast<ULONG_PTR>(d3d.mgr.Get()));
        d3dSet = SUCCEEDED(hr);
        if (!d3dSet) derr = "SET_D3D_MANAGER failed " + hresultToString(hr);
      } else if (!aware) {
        derr = "MFT is not D3D11 aware";
      }
      if (!d3dSet && c.mode == DecoderMode::Hardware) {
        err = "hardware decoding unavailable: " + derr;
        mft.Reset();
        return false;
      }
    }

    ComPtr<IMFMediaType> it;
    MFCreateMediaType(&it);
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    it->SetGUID(MF_MT_SUBTYPE, sub);
    if (c.width && c.height) MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, c.width, c.height);
    MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, c.fps ? c.fps : 30, 1);
    MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = mft->SetInputType(0, it.Get(), 0);
    if (FAILED(hr) && d3dSet) {
      // Some drivers refuse the size/profile in DXVA: retry in software.
      mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
      d3dSet = false;
      if (c.mode == DecoderMode::Hardware) {
        err = "hardware decoder rejected input type " + hresultToString(hr);
        mft.Reset();
        return false;
      }
      hr = mft->SetInputType(0, it.Get(), 0);
    }
    if (FAILED(hr)) {
      err = "SetInputType failed " + hresultToString(hr);
      mft.Reset();
      return false;
    }
    if (!setOutputNV12()) {
      err = "decoder has no NV12 output";
      mft.Reset();
      return false;
    }
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    streamChanges = 0;
    readbackUs = 0;
    dxgiSeen = false;
    return true;
  }

  void close() {
    if (mft) {
      mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
      if (d3dSet) mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }
    mft.Reset();
    outSample.Reset();
    outBuf.Reset();
    staging.Reset();
    d3dSet = false;
  }

  bool deliverDxgi(IMFDXGIBuffer* dx, int64_t t, const Sink& sink) {
    ComPtr<ID3D11Texture2D> tex;
    UINT sub = 0;
    if (FAILED(dx->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dx->GetSubresourceIndex(&sub)))
      return false;
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    if (!staging || stagingDesc.Width != td.Width || stagingDesc.Height != td.Height ||
        stagingDesc.Format != td.Format) {
      staging.Reset();
      D3D11_TEXTURE2D_DESC sd{};
      sd.Width = td.Width;
      sd.Height = td.Height;
      sd.MipLevels = 1;
      sd.ArraySize = 1;
      sd.Format = td.Format;
      sd.SampleDesc.Count = 1;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(d3d.dev->CreateTexture2D(&sd, nullptr, &staging))) return false;
      stagingDesc = sd;
    }
    // Copy + Map immediately: deferring the Map (staging ring) would add a
    // frame of latency. Measured on Intel Iris Xe: this GPU->staging copy
    // costs ~11 ms (720p) / ~21 ms (1080p) incl. waiting for the decode; see
    // DecoderMode::Auto, which benchmarks HW vs SW and keeps the faster.
    const int64_t t0 = pcNowUs();
    if (d3d.mt) d3d.mt->Enter();
    d3d.ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), sub, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = d3d.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    readbackUs = pcNowUs() - t0;
    if (SUCCEEDED(hr)) {
      DecodedImage img;
      img.y = static_cast<const uint8_t*>(m.pData);
      img.y_stride = m.RowPitch;
      img.uv = img.y + static_cast<size_t>(m.RowPitch) * td.Height;
      img.uv_stride = m.RowPitch;
      img.width = std::min<uint32_t>(dispW ? dispW : td.Width, td.Width) & ~1u;
      img.height = std::min<uint32_t>(dispH ? dispH : td.Height, td.Height) & ~1u;
      img.sample_time = t;
      sink(img);
      d3d.ctx->Unmap(staging.Get(), 0);
    }
    if (d3d.mt) d3d.mt->Leave();
    dxgiSeen = true;
    return SUCCEEDED(hr);
  }

  bool deliverMemory(IMFMediaBuffer* buf, int64_t t, const Sink& sink) {
    readbackUs = 0;  // no GPU read-back on the software path
    ComPtr<IMF2DBuffer> b2;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    DecodedImage img;
    img.width = dispW;
    img.height = dispH;
    img.sample_time = t;
    if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
      if (pitch <= 0) { b2->Unlock2D(); return false; }
      img.y = scan0;
      img.y_stride = static_cast<uint32_t>(pitch);
      img.uv = scan0 + static_cast<size_t>(pitch) * codedH;
      img.uv_stride = img.y_stride;
      sink(img);
      b2->Unlock2D();
      return true;
    }
    BYTE* p = nullptr;
    DWORD maxLen = 0, cur = 0;
    if (FAILED(buf->Lock(&p, &maxLen, &cur))) return false;
    const uint32_t stride = defaultStride > 0 ? static_cast<uint32_t>(defaultStride) : codedW;
    bool ok = cur >= static_cast<DWORD>(stride) * codedH * 3 / 2;
    if (ok) {
      img.y = p;
      img.y_stride = stride;
      img.uv = p + static_cast<size_t>(stride) * codedH;
      img.uv_stride = stride;
      sink(img);
    }
    buf->Unlock();
    return ok;
  }

  // Returns false on a hard error.
  bool drain(const Sink& sink) {
    for (int guard = 0; guard < 64; ++guard) {
      MFT_OUTPUT_DATA_BUFFER ob{};
      ob.dwStreamID = 0;
      if (!providesSamples) {
        // A NEW IMFSample per call (reusing the sample makes the MS software
        // decoder fail with E_FAIL on the 2nd frame); the buffer is reused.
        if (!outBuf) {
          if (FAILED(MFCreateMemoryBuffer(outCb ? outCb : codedW * codedH * 3 / 2, &outBuf)))
            return false;
        }
        outSample.Reset();
        outBuf->SetCurrentLength(0);
        if (FAILED(MFCreateSample(&outSample))) return false;
        outSample->AddBuffer(outBuf.Get());
        ob.pSample = outSample.Get();
      }
      DWORD status = 0;
      HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
      if (ob.pEvents) ob.pEvents->Release();
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        ++streamChanges;
        if (providesSamples && ob.pSample) ob.pSample->Release();
        if (!setOutputNV12()) { lastErr = "no NV12 output after stream change"; return false; }
        continue;
      }
      if (FAILED(hr)) {
        if (providesSamples && ob.pSample) ob.pSample->Release();
        lastErr = "ProcessOutput " + hresultToString(hr);
        return false;
      }
      IMFSample* s = ob.pSample;
      if (s) {
        LONGLONG t = 0;
        s->GetSampleTime(&t);
        ComPtr<IMFMediaBuffer> buf;
        if (SUCCEEDED(s->GetBufferByIndex(0, &buf))) {
          ComPtr<IMFDXGIBuffer> dx;
          if (SUCCEEDED(buf.As(&dx)))
            deliverDxgi(dx.Get(), t, sink);
          else
            deliverMemory(buf.Get(), t, sink);
        }
        if (providesSamples) s->Release();
      }
    }
    return true;
  }

  bool decode(const uint8_t* au, size_t n, int64_t t, int64_t dur, bool key, const Sink& sink) {
    if (!mft || n == 0) return false;
    ComPtr<IMFMediaBuffer> mb;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(n), &mb))) return false;
    BYTE* p = nullptr;
    if (FAILED(mb->Lock(&p, nullptr, nullptr))) return false;
    memcpy(p, au, n);
    mb->Unlock();
    mb->SetCurrentLength(static_cast<DWORD>(n));
    ComPtr<IMFSample> s;
    MFCreateSample(&s);
    s->AddBuffer(mb.Get());
    s->SetSampleTime(t);
    s->SetSampleDuration(dur);
    if (key) s->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    HRESULT hr = mft->ProcessInput(0, s.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
      if (!drain(sink)) return false;
      hr = mft->ProcessInput(0, s.Get(), 0);
    }
    if (FAILED(hr)) {
      lastErr = "ProcessInput " + hresultToString(hr);
      return false;
    }
    return drain(sink);
  }
};

MfDecoder::MfDecoder() : impl_(new Impl) {}
MfDecoder::~MfDecoder() { impl_->close(); }
bool MfDecoder::open(const DecoderConfig& cfg, std::string& err) { return impl_->open(cfg, err); }
void MfDecoder::close() { impl_->close(); }
bool MfDecoder::isOpen() const { return impl_->mft != nullptr; }
const DecoderConfig& MfDecoder::config() const { return impl_->cfg; }
bool MfDecoder::decode(const uint8_t* au, size_t n, int64_t t, int64_t dur, bool key,
                       const Sink& sink) {
  return impl_->decode(au, n, t, dur, key, sink);
}
void MfDecoder::flush() {
  if (impl_->mft) impl_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}
bool MfDecoder::hardware() const { return impl_->d3dSet && impl_->dxgiSeen; }
std::string MfDecoder::name() const {
  if (!impl_->mft) return std::string();
  return impl_->mftName + (impl_->d3dSet ? " (D3D11)" : " (software)");
}
uint32_t MfDecoder::streamChanges() const { return impl_->streamChanges; }
int64_t MfDecoder::lastReadbackUs() const { return impl_->readbackUs; }
std::string MfDecoder::lastError() const { return impl_->lastErr; }
void MfDecoder::outputColor(uint32_t& matrix, int& fullRange) const {
  matrix = impl_->mtMatrix;
  fullRange = impl_->mtFullRange;
}

}  // namespace detail
}  // namespace owc
