#include "sps_patch.h"

namespace owc {
namespace detail {

bool patchAccessUnitSps(Codec codec, std::vector<uint8_t>& au) {
  if (au.empty()) return false;
  std::vector<uint8_t> out;
  // The caller only invokes this for streams without B-frames.
  const size_t n = patchAnnexBSps(au.data(), au.size(), codec, out, false);
  if (n == 0) return false;
  au.swap(out);
  return true;
}

}  // namespace detail
}  // namespace owc
