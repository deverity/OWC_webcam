// AM_MEDIA_TYPE helpers and the format catalogue of the OWC virtual camera.
#pragma once

#include <windows.h>
#include <dshow.h>

#include <vector>

#include "convert.h"

namespace owc {
namespace vcam {

constexpr REFERENCE_TIME kInterval30 = 333333;
constexpr REFERENCE_TIME kInterval60 = 166666;
constexpr REFERENCE_TIME kMinInterval = 166666 - 2000;  // accept up to ~61 fps
constexpr REFERENCE_TIME kMaxInterval = 10000000;       // down to 1 fps

const GUID& subtypeFor(PixFmt f);
bool fmtFromSubtype(const GUID& g, PixFmt* out);

struct VideoFormat {
  PixFmt fmt = PixFmt::NV12;
  int width = 0, height = 0;
  REFERENCE_TIME avgTimePerFrame = kInterval30;
};

// What a negotiated media type means for the converter.
struct ParsedType {
  PixFmt fmt = PixFmt::NV12;
  int width = 0, height = 0;  // visible
  int stride = 0;             // bytes, first plane
  bool bottomUp = false;
  REFERENCE_TIME avgTimePerFrame = 0;  // 0 = unspecified
  size_t imageBytes = 0;
  OutputDesc desc() const {
    OutputDesc d;
    d.fmt = fmt;
    d.width = width;
    d.height = height;
    d.stride = stride;
    d.bottomUp = bottomUp;
    return d;
  }
};

// Fills `mt` (pbFormat = CoTaskMemAlloc'd VIDEOINFOHEADER). Free with freeMediaType.
HRESULT buildMediaType(const VideoFormat& f, AM_MEDIA_TYPE* mt);
// Validates a video media type (FORMAT_VideoInfo or FORMAT_VideoInfo2) with
// one of our subtypes. Size/fps are not checked against the catalogue here.
bool parseMediaType(const AM_MEDIA_TYPE* mt, ParsedType* out);

void freeMediaType(AM_MEDIA_TYPE& mt);
void deleteMediaType(AM_MEDIA_TYPE* mt);  // frees a CoTaskMemAlloc'd AM_MEDIA_TYPE*
HRESULT copyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src);
AM_MEDIA_TYPE* createMediaType(const AM_MEDIA_TYPE* src);  // CoTaskMemAlloc'd deep copy

// Catalogue: preferred size first (producer's nominal size when it is live,
// else 1280x720), then the standard sizes; per size 30 fps then 60 fps; per
// rate NV12, YUY2, I420, RGB32, RGB24.
std::vector<VideoFormat> buildCatalogue(int nominalW, int nominalH);

}  // namespace vcam
}  // namespace owc
