// Internal pairing helpers for owc_core (pure functions, unit tested).
#pragma once

#include <owc/client.h>

#include <string>
#include <vector>

namespace owc {
namespace detail {

// PC-side bound of a pairing: the phone's code lifetime (AuthResult{CodeShown}::seconds)
// plus this margin, so the phone's own Expired normally arrives first.
constexpr int64_t kPairExpiryMarginMs = 5000;

bool isZero(const Id16& id);
bool isZero(const Key32& key);

// `code` with surrounding ASCII whitespace removed if it is exactly
// kPairCodeDigits ASCII digits; "" otherwise.
std::string normalizePairCode(const std::string& code);

// Key stored for `phone_id` (nullptr if none).
const PairedPhone* findPairing(const std::vector<PairedPhone>& v, const PhoneId& phone_id);
// Adds or replaces the key of `phone_id`.
void upsertPairing(std::vector<PairedPhone>& v, const PhoneId& phone_id, const Key32& key);
// Removes every entry of `phone_id`; true if something was removed.
bool removePairing(std::vector<PairedPhone>& v, const PhoneId& phone_id);

// Short, human-readable id for logs: first 8 hex chars.
std::string shortHex(const Id16& id);

}  // namespace detail
}  // namespace owc
