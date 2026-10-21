#include "mediatypes.h"

#include <dvdmedia.h>  // VIDEOINFOHEADER2

#include <stdlib.h>

namespace owc {
namespace vcam {

namespace {

constexpr DWORD fourcc(char a, char b, char c, char d) {
  return (DWORD)(uint8_t)a | ((DWORD)(uint8_t)b << 8) | ((DWORD)(uint8_t)c << 16) |
         ((DWORD)(uint8_t)d << 24);
}

// FOURCC subtypes: {FOURCC-0000-0010-8000-00AA00389B71}
const GUID kNV12 = {fourcc('N', 'V', '1', '2'), 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kYUY2 = {fourcc('Y', 'U', 'Y', '2'), 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kI420 = {fourcc('I', '4', '2', '0'), 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
// MEDIASUBTYPE_RGB24 / RGB32 (uuids.h values, spelled out to avoid strmiids for these)
const GUID kRGB24 = {0xe436eb7d, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
const GUID kRGB32 = {0xe436eb7e, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};

DWORD compressionFor(PixFmt f) {
  switch (f) {
    case PixFmt::NV12: return fourcc('N', 'V', '1', '2');
    case PixFmt::YUY2: return fourcc('Y', 'U', 'Y', '2');
    case PixFmt::I420: return fourcc('I', '4', '2', '0');
    default: return BI_RGB;
  }
}

size_t imageBytesFor(PixFmt f, int stride, int rows) {
  OutputDesc d;
  d.fmt = f;
  d.stride = stride;
  d.height = rows;
  return outputBytes(d);
}

const int kSizes[][2] = {{1920, 1080}, {1280, 720}, {960, 540}, {640, 480}, {640, 360}};
const PixFmt kFmtOrder[] = {PixFmt::NV12, PixFmt::YUY2, PixFmt::I420, PixFmt::RGB32, PixFmt::RGB24};

}  // namespace

const GUID& subtypeFor(PixFmt f) {
  switch (f) {
    case PixFmt::NV12: return kNV12;
    case PixFmt::YUY2: return kYUY2;
    case PixFmt::I420: return kI420;
    case PixFmt::RGB24: return kRGB24;
    default: return kRGB32;
  }
}

bool fmtFromSubtype(const GUID& g, PixFmt* out) {
  for (int i = 0; i < (int)PixFmt::Count; ++i) {
    if (IsEqualGUID(g, subtypeFor((PixFmt)i))) {
      *out = (PixFmt)i;
      return true;
    }
  }
  return false;
}

HRESULT buildMediaType(const VideoFormat& f, AM_MEDIA_TYPE* mt) {
  if (!mt) return E_POINTER;
  ZeroMemory(mt, sizeof(*mt));
  VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
  if (!vih) return E_OUTOFMEMORY;
  ZeroMemory(vih, sizeof(*vih));
  const int stride = defaultStride(f.fmt, f.width);
  const size_t bytes = imageBytesFor(f.fmt, stride, f.height);
  vih->AvgTimePerFrame = f.avgTimePerFrame;
  if (f.avgTimePerFrame > 0)
    vih->dwBitRate = (DWORD)((double)bytes * 8.0 * 10000000.0 / (double)f.avgTimePerFrame);
  BITMAPINFOHEADER& b = vih->bmiHeader;
  b.biSize = sizeof(BITMAPINFOHEADER);
  b.biWidth = f.width;
  b.biHeight = f.height;  // RGB: bottom-up DIB; YUV: always top-down
  b.biPlanes = 1;
  b.biBitCount = (WORD)pixFmtBitCount(f.fmt);
  b.biCompression = compressionFor(f.fmt);
  b.biSizeImage = (DWORD)bytes;
  mt->majortype = MEDIATYPE_Video;
  mt->subtype = subtypeFor(f.fmt);
  mt->bFixedSizeSamples = TRUE;
  mt->bTemporalCompression = FALSE;
  mt->lSampleSize = (ULONG)bytes;
  mt->formattype = FORMAT_VideoInfo;
  mt->pUnk = nullptr;
  mt->cbFormat = sizeof(VIDEOINFOHEADER);
  mt->pbFormat = (BYTE*)vih;
  return S_OK;
}

bool parseMediaType(const AM_MEDIA_TYPE* mt, ParsedType* out) {
  if (!mt || !out) return false;
  if (!IsEqualGUID(mt->majortype, MEDIATYPE_Video)) return false;
  PixFmt fmt;
  if (!fmtFromSubtype(mt->subtype, &fmt)) return false;
  const BITMAPINFOHEADER* b = nullptr;
  const RECT* target = nullptr;
  REFERENCE_TIME avg = 0;
  if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo)) {
    if (!mt->pbFormat || mt->cbFormat < sizeof(VIDEOINFOHEADER)) return false;
    const VIDEOINFOHEADER* v = (const VIDEOINFOHEADER*)mt->pbFormat;
    b = &v->bmiHeader;
    target = &v->rcTarget;
    avg = v->AvgTimePerFrame;
  } else if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo2)) {
    if (!mt->pbFormat || mt->cbFormat < sizeof(VIDEOINFOHEADER2)) return false;
    const VIDEOINFOHEADER2* v = (const VIDEOINFOHEADER2*)mt->pbFormat;
    if (v->dwInterlaceFlags & AMINTERLACE_IsInterlaced) return false;
    b = &v->bmiHeader;
    target = &v->rcTarget;
    avg = v->AvgTimePerFrame;
  } else {
    return false;
  }
  if (b->biWidth <= 0 || b->biHeight == 0 || b->biWidth > 16384 || abs(b->biHeight) > 16384) return false;
  if (b->biBitCount != pixFmtBitCount(fmt)) return false;
  const DWORD comp = compressionFor(fmt);
  if (b->biCompression != comp) return false;
  const int rows = abs(b->biHeight);
  int w = b->biWidth, h = rows;
  if (target->right > target->left && target->bottom > target->top) {
    // A destination rect is only meaningful to us as "visible width inside a
    // wider stride" anchored at the origin, with the full height.
    if (target->left != 0 || target->top != 0 || target->right > b->biWidth || target->bottom != rows)
      return false;
    w = target->right;
  }
  if (w < 2 || h < 2 || (w & 1) || (h & 1)) return false;
  int stride = defaultStride(fmt, b->biWidth);
  if (fmt == PixFmt::I420 && (stride & 1)) return false;
  out->fmt = fmt;
  out->width = w;
  out->height = h;
  out->stride = stride;
  out->bottomUp = pixFmtIsRgb(fmt) && b->biHeight > 0;
  out->avgTimePerFrame = avg;
  out->imageBytes = imageBytesFor(fmt, stride, rows);
  return true;
}

void freeMediaType(AM_MEDIA_TYPE& mt) {
  if (mt.cbFormat != 0 || mt.pbFormat) CoTaskMemFree(mt.pbFormat);
  mt.cbFormat = 0;
  mt.pbFormat = nullptr;
  if (mt.pUnk) {
    mt.pUnk->Release();
    mt.pUnk = nullptr;
  }
}

void deleteMediaType(AM_MEDIA_TYPE* mt) {
  if (!mt) return;
  freeMediaType(*mt);
  CoTaskMemFree(mt);
}

HRESULT copyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src) {
  if (!dst || !src) return E_POINTER;
  *dst = *src;
  if (src->cbFormat && src->pbFormat) {
    dst->pbFormat = (BYTE*)CoTaskMemAlloc(src->cbFormat);
    if (!dst->pbFormat) {
      dst->cbFormat = 0;
      dst->pUnk = nullptr;
      return E_OUTOFMEMORY;
    }
    memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
  } else {
    dst->pbFormat = nullptr;
    dst->cbFormat = 0;
  }
  if (dst->pUnk) dst->pUnk->AddRef();
  return S_OK;
}

AM_MEDIA_TYPE* createMediaType(const AM_MEDIA_TYPE* src) {
  AM_MEDIA_TYPE* mt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
  if (!mt) return nullptr;
  if (FAILED(copyMediaType(mt, src))) {
    CoTaskMemFree(mt);
    return nullptr;
  }
  return mt;
}

std::vector<VideoFormat> buildCatalogue(int nominalW, int nominalH) {
  std::vector<std::pair<int, int>> sizes;
  const bool nominalOk = nominalW >= 160 && nominalH >= 120 && nominalW <= 3840 && nominalH <= 2160 &&
                         !(nominalW & 1) && !(nominalH & 1);
  if (nominalOk) sizes.emplace_back(nominalW, nominalH);
  else sizes.emplace_back(1280, 720);
  for (const auto& s : kSizes)
    if (!(s[0] == sizes[0].first && s[1] == sizes[0].second)) sizes.emplace_back(s[0], s[1]);
  std::vector<VideoFormat> out;
  out.reserve(sizes.size() * 2 * 5);
  for (const auto& s : sizes)
    for (REFERENCE_TIME iv : {kInterval30, kInterval60})
      for (PixFmt f : kFmtOrder) {
        VideoFormat v;
        v.fmt = f;
        v.width = s.first;
        v.height = s.second;
        v.avgTimePerFrame = iv;
        out.push_back(v);
      }
  return out;
}

}  // namespace vcam
}  // namespace owc
