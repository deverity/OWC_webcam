// Experiment overrides read from system properties (`adb shell setprop
// debug.owc.<name> <value>`), sampled when a stream starts. Unset = the
// production default, so they cost nothing in normal use. They exist to A/B
// latency experiments on a device without rebuilding or changing the
// protocol (docs/DEVICE_LATENCY_ADVANCED.md, "Resultados medidos").
#pragma once

#include <sys/system_properties.h>

#include <cstdio>
#include <cstdlib>

namespace owc {

inline long long debugProp(const char* name, long long def) {
  char key[PROP_NAME_MAX];
  char v[PROP_VALUE_MAX] = {0};
  snprintf(key, sizeof(key), "debug.owc.%s", name);
  if (__system_property_get(key, v) <= 0) return def;
  char* end = nullptr;
  long long r = strtoll(v, &end, 0);  // accepts 0x... for tag ids
  return end == v ? def : r;
}

}  // namespace owc
