#include "pairing.h"

#include <windows.h>
#include <bcrypt.h>

#include <owc/crypto.h>

#include <algorithm>

namespace owc {

ClientId newClientId() {
  ClientId id{};
  // Never return all zeros (connect() rejects it): retry the (astronomically
  // unlikely) zero draw; fall back to a timestamp-seeded value if CNG fails.
  for (int i = 0; i < 4; ++i) {
    if (BCryptGenRandom(nullptr, id.data(), static_cast<ULONG>(id.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 &&
        !detail::isZero(id))
      return id;
  }
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  const uint64_t a = static_cast<uint64_t>(q.QuadPart) ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
  const uint64_t b = GetTickCount64() * 0x9E3779B97F4A7C15ull + 1;
  for (size_t i = 0; i < 8; ++i) {
    id[i] = static_cast<uint8_t>(a >> (8 * i));
    id[8 + i] = static_cast<uint8_t>(b >> (8 * i));
  }
  return id;
}

namespace detail {

bool isZero(const Id16& id) {
  return std::all_of(id.begin(), id.end(), [](uint8_t b) { return b == 0; });
}

bool isZero(const Key32& key) {
  return std::all_of(key.begin(), key.end(), [](uint8_t b) { return b == 0; });
}

std::string normalizePairCode(const std::string& code) {
  size_t b = 0, e = code.size();
  auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (b < e && ws(code[b])) ++b;
  while (e > b && ws(code[e - 1])) --e;
  if (e - b != kPairCodeDigits) return std::string();
  for (size_t i = b; i < e; ++i)
    if (code[i] < '0' || code[i] > '9') return std::string();
  return code.substr(b, e - b);
}

const PairedPhone* findPairing(const std::vector<PairedPhone>& v, const PhoneId& phone_id) {
  for (const PairedPhone& p : v)
    if (p.phone_id == phone_id) return &p;
  return nullptr;
}

void upsertPairing(std::vector<PairedPhone>& v, const PhoneId& phone_id, const Key32& key) {
  removePairing(v, phone_id);
  PairedPhone p;
  p.phone_id = phone_id;
  p.key = key;
  v.push_back(p);
}

bool removePairing(std::vector<PairedPhone>& v, const PhoneId& phone_id) {
  const size_t before = v.size();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const PairedPhone& p) { return p.phone_id == phone_id; }),
          v.end());
  return v.size() != before;
}

std::string shortHex(const Id16& id) { return toHex(id.data(), 4); }

bool secureRandom(void* p, size_t n) {
  if (BCryptGenRandom(nullptr, static_cast<PUCHAR>(p), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
    return true;
  secureWipe(p, n);
  return false;
}

}  // namespace detail
}  // namespace owc
