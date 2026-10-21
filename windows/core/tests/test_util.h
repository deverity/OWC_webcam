// Tiny assertion helpers for owc_core tests (no external framework).
#pragma once

#include <cstdio>
#include <cstdlib>

namespace owctest {
inline int& failures() {
  static int f = 0;
  return f;
}
}  // namespace owctest

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      ++owctest::failures();                                                     \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                            \
  } while (0)

#define CHECK_MSG(cond, ...)                                                     \
  do {                                                                           \
    if (!(cond)) {                                                               \
      ++owctest::failures();                                                     \
      std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);            \
      std::printf(__VA_ARGS__);                                                  \
      std::printf("\n");                                                         \
    }                                                                            \
  } while (0)

#define RUN(fn)                                                                  \
  do {                                                                           \
    const int before = owctest::failures();                                      \
    std::printf("[ RUN  ] %s\n", #fn);                                           \
    std::fflush(stdout);                                                         \
    fn();                                                                        \
    std::printf("[ %s ] %s\n", owctest::failures() == before ? " OK " : "FAIL", #fn); \
    std::fflush(stdout);                                                         \
  } while (0)
