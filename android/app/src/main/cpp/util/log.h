// Logging: logcat (tag "OWC") + optional forwarding to the connected PC.
//
// Warn/Error (and the explicit *_PC variants) are forwarded to the PC as
// LogMsg through a sink installed by the session. The sink is copied out of
// the registry under a mutex and invoked WITHOUT holding it, so the only rule
// is: never log while holding the Sender's queue mutex (the sink pushes into it).
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace owc::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

using Sink = std::function<void(Level, const std::string&)>;

// Installs (or clears with nullptr) the PC forwarding sink. Thread-safe.
void setPcSink(std::shared_ptr<Sink> sink);

// Also print every line to stderr (on-device self-test binary).
void mirrorToStderr(bool on);

void write(Level level, bool toPc, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

}  // namespace owc::log

#define LOGD(...) ::owc::log::write(::owc::log::Level::Debug, false, __VA_ARGS__)
#define LOGI(...) ::owc::log::write(::owc::log::Level::Info, false, __VA_ARGS__)
#define LOGI_PC(...) ::owc::log::write(::owc::log::Level::Info, true, __VA_ARGS__)
#define LOGW(...) ::owc::log::write(::owc::log::Level::Warn, true, __VA_ARGS__)
#define LOGE(...) ::owc::log::write(::owc::log::Level::Error, true, __VA_ARGS__)
// Warn/Error that must stay local (e.g. errors about the PC link itself).
#define LOGW_LOCAL(...) ::owc::log::write(::owc::log::Level::Warn, false, __VA_ARGS__)
#define LOGE_LOCAL(...) ::owc::log::write(::owc::log::Level::Error, false, __VA_ARGS__)
