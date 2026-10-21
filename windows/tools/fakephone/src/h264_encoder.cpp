#include "h264_encoder.h"

#include <windows.h>

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace owc {
namespace fake {

MfScope::MfScope() {
  com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  ok_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
}
MfScope::~MfScope() {
  if (ok_) MFShutdown();
  if (com_) CoUninitialize();
}

namespace {
std::string hrs(HRESULT hr) {
  char b[16];
  snprintf(b, sizeof(b), "0x%08lX", static_cast<unsigned long>(hr));
  return b;
}
HRESULT setU4(ICodecAPI* api, const GUID& g, ULONG v) {
  VARIANT var;
  VariantInit(&var);
  var.vt = VT_UI4;
  var.ulVal = v;
  return api->SetValue(&g, &var);
}
HRESULT setBool(ICodecAPI* api, const GUID& g, bool v) {
  VARIANT var;
  VariantInit(&var);
  var.vt = VT_BOOL;
  var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
  HRESULT hr = api->SetValue(&g, &var);
  if (FAILED(hr)) hr = setU4(api, g, v ? 1 : 0);
  return hr;
}
}  // namespace

struct H264Encoder::Impl {
  EncoderConfig cfg;
  ComPtr<IMFTransform> mft;
  ComPtr<ICodecAPI> api;
  DWORD outCb = 0;
  bool providesSamples = false;

  void applyCodecApi() {
    if (!api) return;
    setU4(api.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    setU4(api.Get(), CODECAPI_AVEncCommonMeanBitRate, cfg.bitrate);
    setU4(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, cfg.bframes);
    if (cfg.gop_frames) setU4(api.Get(), CODECAPI_AVEncMPVGOPSize, cfg.gop_frames);
    setBool(api.Get(), CODECAPI_AVLowLatencyMode, cfg.bframes == 0);
  }

  bool open(const EncoderConfig& c, std::string& err) {
    close();
    cfg = c;
    HRESULT hr = CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&mft));
    if (FAILED(hr)) { err = "CoCreateInstance(MS H.264 encoder) " + hrs(hr); return false; }
    mft.As(&api);
    applyCodecApi();
    ComPtr<IMFMediaType> ot;
    MFCreateMediaType(&ot);
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    ot->SetUINT32(MF_MT_AVG_BITRATE, cfg.bitrate);
    MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height);
    MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, cfg.fps, 1);
    MFSetAttributeRatio(ot.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    const UINT32 prof = cfg.profile >= 2 ? eAVEncH264VProfile_High
                        : cfg.profile == 1 ? eAVEncH264VProfile_Main
                                           : eAVEncH264VProfile_Base;
    ot->SetUINT32(MF_MT_MPEG2_PROFILE, prof);
    hr = mft->SetOutputType(0, ot.Get(), 0);
    if (FAILED(hr)) { err = "encoder SetOutputType " + hrs(hr); mft.Reset(); return false; }
    ComPtr<IMFMediaType> it;
    MFCreateMediaType(&it);
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height);
    MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, cfg.fps, 1);
    MFSetAttributeRatio(it.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = mft->SetInputType(0, it.Get(), 0);
    if (FAILED(hr)) { err = "encoder SetInputType " + hrs(hr); mft.Reset(); return false; }
    applyCodecApi();
    MFT_OUTPUT_STREAM_INFO si{};
    mft->GetOutputStreamInfo(0, &si);
    outCb = si.cbSize ? si.cbSize : cfg.width * cfg.height * 2;
    providesSamples = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                     MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
  }

  void close() {
    if (mft) mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    api.Reset();
    mft.Reset();
  }

  bool drain(const Sink& sink) {
    for (int guard = 0; guard < 32; ++guard) {
      MFT_OUTPUT_DATA_BUFFER ob{};
      ComPtr<IMFSample> own;
      if (!providesSamples) {
        ComPtr<IMFMediaBuffer> mb;
        if (FAILED(MFCreateMemoryBuffer(outCb, &mb))) return false;
        MFCreateSample(&own);
        own->AddBuffer(mb.Get());
        ob.pSample = own.Get();
      }
      DWORD st = 0;
      HRESULT hr = mft->ProcessOutput(0, 1, &ob, &st);
      if (ob.pEvents) ob.pEvents->Release();
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        ComPtr<IMFMediaType> t;
        if (SUCCEEDED(mft->GetOutputAvailableType(0, 0, &t))) mft->SetOutputType(0, t.Get(), 0);
        continue;
      }
      if (FAILED(hr)) return false;
      IMFSample* s = ob.pSample;
      LONGLONG pts = 0;
      s->GetSampleTime(&pts);
      UINT32 clean = 0;
      s->GetUINT32(MFSampleExtension_CleanPoint, &clean);
      ComPtr<IMFMediaBuffer> buf;
      if (SUCCEEDED(s->ConvertToContiguousBuffer(&buf))) {
        BYTE* p = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
          sink(p, len, pts, clean != 0);
          buf->Unlock();
        }
      }
      if (providesSamples) s->Release();
    }
    return true;
  }

  bool encode(const uint8_t* nv12, int64_t pts, int64_t dur, bool forceKey, const Sink& sink) {
    if (!mft) return false;
    const DWORD n = cfg.width * cfg.height * 3 / 2;
    ComPtr<IMFMediaBuffer> mb;
    if (FAILED(MFCreateMemoryBuffer(n, &mb))) return false;
    BYTE* p = nullptr;
    mb->Lock(&p, nullptr, nullptr);
    memcpy(p, nv12, n);
    mb->Unlock();
    mb->SetCurrentLength(n);
    ComPtr<IMFSample> s;
    MFCreateSample(&s);
    s->AddBuffer(mb.Get());
    s->SetSampleTime(pts);
    s->SetSampleDuration(dur);
    if (forceKey && api) setU4(api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    HRESULT hr = mft->ProcessInput(0, s.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
      if (!drain(sink)) return false;
      hr = mft->ProcessInput(0, s.Get(), 0);
    }
    if (FAILED(hr)) return false;
    return drain(sink);
  }
};

H264Encoder::H264Encoder() : impl_(new Impl) {}
H264Encoder::~H264Encoder() { impl_->close(); }
bool H264Encoder::open(const EncoderConfig& c, std::string& err) { return impl_->open(c, err); }
void H264Encoder::close() { impl_->close(); }
bool H264Encoder::encode(const uint8_t* nv12, int64_t pts, int64_t dur, bool forceKey,
                         const Sink& sink) {
  return impl_->encode(nv12, pts, dur, forceKey, sink);
}
void H264Encoder::setBitrate(uint32_t bps) {
  impl_->cfg.bitrate = bps;
  if (impl_->api) setU4(impl_->api.Get(), CODECAPI_AVEncCommonMeanBitRate, bps);
}
const EncoderConfig& H264Encoder::config() const { return impl_->cfg; }

}  // namespace fake
}  // namespace owc
