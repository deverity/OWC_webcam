#include "app_core.h"

#include <android/native_window.h>

#include <cstdio>

#include "session.h"
#include "util/json.h"
#include "util/log.h"

namespace owc {

AppCore::AppCore() : info_(std::make_shared<SharedInfo>()) {}

AppCore::~AppCore() {
  stop();
  if (pendingPreview_) ANativeWindow_release(pendingPreview_);
}

bool AppCore::start(const StartOptions& opts, EventSink* sink, std::string& err) {
  std::lock_guard<std::mutex> lk(lifeMu_);
  if (worker_) {
    err = "already running";
    return false;
  }
  {
    std::lock_guard<std::mutex> ilk(info_->infoMu);
    info_->deviceName = opts.deviceName;
    info_->model = opts.model;
  }
  auto w = std::make_unique<Worker>(opts, sink, info_, pendingPreview_);
  if (!w->start(err)) {
    // Worker destructor releases the preview reference it was given.
    pendingPreview_ = nullptr;
    LOGE_LOCAL("start failed: %s", err.c_str());
    return false;
  }
  pendingPreview_ = nullptr;  // now owned by the worker
  worker_ = std::move(w);
  {
    std::lock_guard<std::mutex> slk(info_->statusMu);
    info_->status = SharedInfo::Status{};
    info_->status.state = "listening";
  }
  running_ = true;
  return true;
}

void AppCore::stop() {
  std::lock_guard<std::mutex> lk(lifeMu_);
  if (!worker_) return;
  worker_->stop();
  worker_.reset();
  running_ = false;
  std::lock_guard<std::mutex> slk(info_->statusMu);
  info_->status = SharedInfo::Status{};
}

void AppCore::setPreviewSurface(ANativeWindow* w) {
  std::lock_guard<std::mutex> lk(lifeMu_);
  if (worker_) {
    worker_->setPreview(w);
    return;
  }
  if (pendingPreview_) ANativeWindow_release(pendingPreview_);
  pendingPreview_ = w;
}

std::string AppCore::statusJson() {
  SharedInfo::Status s;
  {
    std::lock_guard<std::mutex> lk(info_->statusMu);
    s = info_->status;
  }
  char num[256];
  std::string o = "{\"state\":" + json::quote(s.state) + ",\"peer\":" + json::quote(s.peer) +
                  ",\"transport\":" + json::quote(s.transport) + ",\"cameraId\":" +
                  json::quote(s.cameraId);
  snprintf(num, sizeof(num), ",\"width\":%d,\"height\":%d,\"fps\":%.1f,", s.width, s.height, s.fps);
  o += num;
  o += "\"codec\":" + json::quote(s.codec);
  snprintf(num, sizeof(num), ",\"bitrateKbps\":%d,\"dropped\":%u,\"encodeLatencyMs\":%.1f}",
           s.bitrateKbps, s.dropped, s.encodeLatencyMs);
  o += num;
  return o;
}

void AppCore::updateDeviceStatus(int batteryPct, float tempC, bool charging, int thermal) {
  info_->batteryPct = batteryPct < -1 ? -1 : (batteryPct > 100 ? 100 : batteryPct);
  info_->batteryTempC = tempC;
  info_->charging = charging;
  info_->thermal = thermal < 0 ? 0 : (thermal > 6 ? 6 : thermal);
}

void AppCore::setDeviceName(const std::string& name) {
  std::lock_guard<std::mutex> lk(info_->infoMu);
  info_->deviceName = name;
}

void AppCore::screenModeChanged(int mode) {
  std::lock_guard<std::mutex> lk(lifeMu_);
  if (worker_) worker_->screenModeChanged(mode);
}

}  // namespace owc
