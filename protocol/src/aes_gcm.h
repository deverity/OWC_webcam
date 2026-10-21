// Internal: hardware AES-128-GCM backends (not part of the public API).
//
// Each backend lives in its own translation unit, compiled with the ISA flags it
// needs (protocol/CMakeLists.txt) and called ONLY after cpu_features.cpp said
// the CPU has the instructions. Those translation units must not include any
// C++ standard header that instantiates inline/template code (the linker could
// keep the copy built with the extended ISA for the whole program).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace owc {
namespace aesgcm {

// Expanded key material; layout private to the backend that filled it.
struct Key {
  uint8_t rk[11 * 16];  // AES-128 round keys
  uint8_t h[8 * 16];    // H^1..H^8 (GHASH key powers, backend representation)
  uint8_t hk[8 * 16];   // Karatsuba helpers (hi ^ lo of each power)
};

struct Backend {
  const char* name;
  void (*init)(Key& k, const uint8_t key[16]);
  // In place. tag: 16 bytes out.
  void (*seal)(const Key& k, const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
               uint8_t* p, size_t n, uint8_t tag[16]);
  // In place decrypt; writes the tag it computed (the caller compares).
  void (*open)(const Key& k, const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
               uint8_t* p, size_t n, uint8_t tag[16]);
};

// nullptr when the backend is not compiled for this target.
const Backend* x86Backend();
const Backend* armBackend();

// cpu_features.cpp: the ONLY place that probes the CPU.
bool cpuHasX86AesClmul();  // AES-NI + PCLMULQDQ + SSSE3
bool cpuHasArmAesPmull();  // ARMv8 AES + PMULL (AArch64 or AArch32)

}  // namespace aesgcm
}  // namespace owc
