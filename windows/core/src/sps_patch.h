// SPS low-latency patch integration point (research sec. 5.3).
#pragma once

#include <cstdint>
#include <vector>

#include <owc/protocol.h>

namespace owc {
namespace detail {

// Rewrites every SPS inside the Annex-B access unit `au` (in place) so the
// decoder does not buffer for reordering. Returns true if an SPS was changed.
// Implemented on top of the protocol/ library's SPS patcher when available.
bool patchAccessUnitSps(Codec codec, std::vector<uint8_t>& au);

}  // namespace detail
}  // namespace owc
