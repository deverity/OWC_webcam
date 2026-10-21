#include "codec_select.h"

namespace owc {
namespace detail {

const char* codecName(Codec c) {
  switch (c) {
    case Codec::H264: return "H.264";
    case Codec::Hevc: return "HEVC";
    case Codec::Av1: return "AV1";
  }
  return "?";
}

std::string installHint(Codec c) {
  if (c == Codec::Hevc) return "install 'HEVC Video Extensions'";
  if (c == Codec::Av1) return "install 'AV1 Video Extension'";
  return std::string();
}

namespace {
// Why codec c can't be used (empty = usable). requireHw: Auto mode.
std::string blocker(Codec c, uint32_t phoneMask, const DecoderCapabilities& caps,
                    uint32_t blockedMask, bool requireHw) {
  const std::string n = codecName(c);
  if (blockedMask & codecBit(c)) return n + " failed earlier in this session";
  if (!(phoneMask & codecBit(c))) return "phone has no " + n + " encoder";
  const DecoderCapabilities::Entry& e = caps.get(c);
  if (!e.any()) return "no " + n + " decoder: " + installHint(c);
  if (requireHw && !e.hardware) return n + " decoder is software-only";
  return std::string();
}
}  // namespace

CodecChoice chooseCodec(CodecPreference pref, uint32_t phoneMask, const DecoderCapabilities& caps,
                        uint32_t blockedMask) {
  CodecChoice ch;
  if (pref == CodecPreference::Auto) {
    std::string firstWhy;
    for (Codec c : {Codec::Av1, Codec::Hevc}) {
      const std::string why = blocker(c, phoneMask, caps, blockedMask, true);
      if (why.empty()) {
        ch.codec = c;
        ch.reason = std::string("auto: ") + codecName(c) + " (hardware decoder present)";
        return ch;
      }
      // Report the most relevant reason: the best codec the PHONE can encode.
      if (firstWhy.empty() && (phoneMask & codecBit(c))) firstWhy = why;
    }
    ch.codec = Codec::H264;
    ch.reason = "auto: H.264 (" + (firstWhy.empty() ? std::string("phone encodes H.264 only") : firstWhy) + ")";
    return ch;
  }
  const Codec want = pref == CodecPreference::Hevc  ? Codec::Hevc
                     : pref == CodecPreference::Av1 ? Codec::Av1
                                                    : Codec::H264;
  if (want == Codec::H264) {
    ch.reason = "user: H.264";
    return ch;
  }
  const std::string why = blocker(want, phoneMask, caps, blockedMask, false);
  if (!why.empty()) {
    ch.codec = Codec::H264;
    ch.reason = std::string("user wants ") + codecName(want) + ", using H.264 (" + why + ")";
    return ch;
  }
  ch.codec = want;
  ch.reason = std::string("user: ") + codecName(want) +
              (caps.get(want).hardware ? " (hardware decoder)" : " (software decoder)");
  return ch;
}

}  // namespace detail
}  // namespace owc
