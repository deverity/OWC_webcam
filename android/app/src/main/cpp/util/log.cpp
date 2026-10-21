#include "util/log.h"

#include <android/log.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace owc::log {
namespace {
std::mutex g_mu;
std::shared_ptr<Sink> g_sink;  // guarded by g_mu
bool g_stderr = false;         // self-test mirrors logs to stderr

int androidPrio(Level l) {
  switch (l) {
    case Level::Debug: return ANDROID_LOG_DEBUG;
    case Level::Info: return ANDROID_LOG_INFO;
    case Level::Warn: return ANDROID_LOG_WARN;
    case Level::Error: return ANDROID_LOG_ERROR;
  }
  return ANDROID_LOG_INFO;
}
}  // namespace

void setPcSink(std::shared_ptr<Sink> sink) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_sink = std::move(sink);
}

void mirrorToStderr(bool on) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_stderr = on;
}

void write(Level level, bool toPc, const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  __android_log_write(androidPrio(level), "OWC", buf);

  std::shared_ptr<Sink> sink;
  bool err;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (toPc) sink = g_sink;
    err = g_stderr;
  }
  if (err) {
    static const char* kNames[] = {"D", "I", "W", "E"};
    fprintf(stderr, "[%s] %s\n", kNames[static_cast<int>(level)], buf);
  }
  if (sink && *sink) (*sink)(level, buf);
}

}  // namespace owc::log
