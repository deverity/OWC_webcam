// CPU feature detection for the hardware AES-GCM backends. This is the only
// file of owc_protocol with platform-specific headers; everything else stays
// portable. Results are computed once (thread-safe static init).
#include "aes_gcm.h"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define OWC_CPU_X86_MSVC 1
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#define OWC_CPU_X86_GNU 1
#elif (defined(__aarch64__) || defined(__arm__)) && (defined(__linux__) || defined(__ANDROID__))
#include <sys/auxv.h>
#define OWC_CPU_ARM_LINUX 1
#elif defined(__aarch64__) && defined(__APPLE__)
#define OWC_CPU_ARM_APPLE 1  // every Apple arm64 core has AES + PMULL
#endif

namespace owc {
namespace aesgcm {
namespace {

bool probeX86() {
#if defined(OWC_CPU_X86_MSVC)
  int r[4] = {0, 0, 0, 0};
  __cpuid(r, 0);
  if (r[0] < 1) return false;
  __cpuid(r, 1);
  const unsigned ecx = static_cast<unsigned>(r[2]);
#elif defined(OWC_CPU_X86_GNU)
  unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
#else
  const unsigned ecx = 0;
#endif
  const unsigned kPclmul = 1u << 1, kSsse3 = 1u << 9, kAes = 1u << 25;
  return (ecx & (kPclmul | kSsse3 | kAes)) == (kPclmul | kSsse3 | kAes);
}

bool probeArm() {
#if defined(OWC_CPU_ARM_LINUX) && defined(__aarch64__)
  const unsigned long kHwcapAes = 1ul << 3, kHwcapPmull = 1ul << 4;  // asm/hwcap.h
  const unsigned long hw = getauxval(AT_HWCAP);
  return (hw & (kHwcapAes | kHwcapPmull)) == (kHwcapAes | kHwcapPmull);
#elif defined(OWC_CPU_ARM_LINUX)
#ifndef AT_HWCAP2
#define AT_HWCAP2 26
#endif
  const unsigned long kHwcap2Aes = 1ul << 0, kHwcap2Pmull = 1ul << 1;  // asm/hwcap.h (arm)
  const unsigned long hw2 = getauxval(AT_HWCAP2);
  return (hw2 & (kHwcap2Aes | kHwcap2Pmull)) == (kHwcap2Aes | kHwcap2Pmull);
#elif defined(OWC_CPU_ARM_APPLE)
  return true;
#else
  return false;
#endif
}

}  // namespace

bool cpuHasX86AesClmul() {
  static const bool v = probeX86();
  return v;
}

bool cpuHasArmAesPmull() {
  static const bool v = probeArm();
  return v;
}

}  // namespace aesgcm
}  // namespace owc
