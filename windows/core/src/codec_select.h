// Codec negotiation policy (pure; unit tested).
#pragma once

#include <cstdint>
#include <string>

#include <owc/client.h>

namespace owc {
namespace detail {

const char* codecName(Codec c);          // "H.264", "HEVC", "AV1"
std::string installHint(Codec c);        // e.g. "install 'HEVC Video Extensions'"

struct CodecChoice {
  Codec codec = Codec::H264;
  std::string reason;   // human readable, for ClientStats::codec_selection and logs
};

// pref        user preference
// phoneMask   HelloAck::codec_mask (codecBit(...) of the phone's encoders)
// caps        what this PC can decode
// blockedMask codecBit(...) of codecs that failed earlier in this session
// Auto: first of AV1 > HEVC advertised by the phone, decodable in HARDWARE
// here and not blocked; else H.264. Explicit: that codec if advertised and
// decodable (HW or SW) and not blocked; else H.264. H.264 is never blocked.
CodecChoice chooseCodec(CodecPreference pref, uint32_t phoneMask, const DecoderCapabilities& caps,
                        uint32_t blockedMask);

}  // namespace detail
}  // namespace owc
