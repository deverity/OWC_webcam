#include "app.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "params_meta.h"
#include "platform.h"

namespace owcapp {

namespace {

AppSettings initialSettings(bool persist) {
  AppSettings s;
  s.opts.keep_latest_frame = false;  // the preview is fed through onFrame
  if (persist) {
    loadSettings(s);
    s.opts.keep_latest_frame = false;
  }
  return s;
}

std::string toUtf8Smart(const std::string& raw) {
  if (raw.empty()) return raw;
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
  if (n > 0) return raw;  // already valid UTF-8
  int w = MultiByteToWideChar(CP_OEMCP, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
  if (w <= 0) return raw;
  std::wstring ws(static_cast<size_t>(w), L'\0');
  MultiByteToWideChar(CP_OEMCP, 0, raw.data(), static_cast<int>(raw.size()), ws.data(), w);
  return toUtf8(ws);
}

}  // namespace

const char* reasonText(owc::DisconnectReason r) {
  using R = owc::DisconnectReason;
  switch (r) {
    case R::None: return "";
    case R::UserRequested: return "desconectado pelo usuário";
    case R::ConnectFailed: return "não foi possível conectar (recusado/inalcançável)";
    case R::NoResponse: return "o app OWC não respondeu (está aberto no celular?)";
    case R::Busy: return "o celular já atende outro PC";
    case R::VersionMismatch: return "versão do protocolo incompatível";
    case R::BadMagic: return "o destino não é um celular OWC";
    case R::PeerBye: return "o celular encerrou a sessão";
    case R::PeerClosed: return "conexão fechada pelo celular";
    case R::Timeout: return "o celular parou de responder";
    case R::ProtocolError: return "erro de protocolo";
    case R::SocketError: return "erro de rede";
  }
  return "";
}

const char* stateText(owc::ClientState s) {
  using S = owc::ClientState;
  switch (s) {
    case S::Idle: return "Desconectado";
    case S::Connecting: return "Conectando...";
    case S::Handshaking: return "Negociando...";
    case S::Connected: return "Conectado";
    case S::Streaming: return "Transmitindo";
    case S::Reconnecting: return "Reconectando...";
    case S::Failed: return "Falhou";
  }
  return "";
}

// ============================================================================
App::App(bool persist_)
    : persist(persist_),
      settings(initialSettings(persist_)),
      client(this, settings.opts) {
  frame_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  lat_ring_.assign(120, 0.0);
  if (persist) loadUserPresets(user_presets);
  vcam_registered = virtualCamRegistered(&vcam_per_user);
  obs_installed = obsPluginInstalled();
}

App::~App() {
  stop_ = true;
  usb_cancel_ = true;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (usb_answer_ == 0) usb_answer_ = 2;
  }
  usb_cv_.notify_all();
  adb_cv_.notify_all();
  client.disconnect();
  discovery.stop();
  if (usb_th_.joinable()) usb_th_.join();
  if (adb_th_.joinable()) adb_th_.join();
  if (script_th_.joinable()) script_th_.join();
  if (persist) saveNow();
  if (frame_event_) CloseHandle(frame_event_);
}

void App::startServices() {
  owc::Discovery::Options o;
  discovery.start(o);
  adb_th_ = std::thread([this] { adbLoop(); });
}

void App::adbLoop() {
  while (!stop_) {
    std::vector<owc::AdbDevice> devs;
    std::string err;
    if (!adb.available()) {
      err = "adb.exe não encontrado (instale o Android platform-tools ou coloque adb.exe ao lado do OWC.exe)";
    } else if (!adb.listDevices(devs, &err)) {
      if (err.empty()) err = "falha ao executar adb";
    }
    {
      std::lock_guard<std::mutex> l(mu_);
      adb_devs_ = std::move(devs);
      adb_err_ = err;
    }
    std::unique_lock<std::mutex> lk(mu_);
    adb_cv_.wait_for(lk, std::chrono::seconds(2), [this] { return stop_.load(); });
  }
}

std::vector<owc::AdbDevice> App::adbDevices() {
  std::lock_guard<std::mutex> l(mu_);
  return adb_devs_;
}

std::string App::adbError() {
  std::lock_guard<std::mutex> l(mu_);
  return adb_err_;
}

// ---------------------------------------------------------------- settings
void App::saveNow() {
  if (!persist) return;
  AppSettings copy;
  {
    std::lock_guard<std::mutex> l(mu_);
    copy = settings;
  }
  saveSettings(copy);
  dirty_ = false;
  dirty_since_ = 0;
}

void App::tick() {
  const ULONGLONG now = GetTickCount64();
  {
    std::lock_guard<std::mutex> l(mu_);
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (now - it->second.sent_ms > 1500) it = pending_.erase(it);
      else ++it;
    }
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return t.until < now; }),
                  toasts_.end());
  }
  if (dirty_ && persist) {
    if (dirty_since_ == 0) dirty_since_ = now;
    else if (now - dirty_since_ > 800) saveNow();
  }
}

// ---------------------------------------------------------------- frame snapshot
void App::beginFrame() {
  stats = client.stats();
  caps_valid = client.capabilities(caps);
  dev_valid = client.deviceInfo(dev);
  const ULONGLONG now = GetTickCount64();
  static ULONGLONG lastDisc = 0;
  if (now - lastDisc > 250) {
    wifi_devices = discovery.devices();
    lastDisc = now;
  }
  if (now - probe_t_ > 2000) {
    probe_t_ = now;
    vcam_registered = virtualCamRegistered(&vcam_per_user);
    obs_installed = obsPluginInstalled();
  }
  const double t = static_cast<double>(now) / 1000.0;
  if (t - last_graph_t_ >= 0.1) {
    last_graph_t_ = t;
    GraphPoint p;
    p.t = t;
    p.e2e = static_cast<float>(stats.capture_to_publish_ms_avg);
    p.shown = static_cast<float>(shown_lat_avg);
    graph.push_back(p);
    while (!graph.empty() && t - graph.front().t > 10.0) graph.pop_front();
  }
}

bool App::takePreview(PreviewFrame& out) {
  std::lock_guard<std::mutex> l(frame_mu_);
  if (!frame_new_) return false;
  std::swap(out, pending_frame_);
  frame_new_ = false;
  return true;
}

void App::noteDisplayed(const PreviewFrame& f) {
  ++frames_shown;
  last_rotation = f.rotation;
  last_mirrored = f.mirrored;
  if (f.capture_qpc > 0) {
    LARGE_INTEGER q, fr;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&fr);
    const double ms = static_cast<double>(q.QuadPart - f.capture_qpc) * 1000.0 / static_cast<double>(fr.QuadPart);
    if (ms > -50 && ms < 10000) {
      lat_ring_[lat_pos_++ % lat_ring_.size()] = ms;
      const size_t n = std::min<size_t>(lat_pos_, lat_ring_.size());
      std::vector<double> v(lat_ring_.begin(), lat_ring_.begin() + static_cast<std::ptrdiff_t>(n));
      double sum = 0;
      for (double d : v) sum += d;
      shown_lat_avg = sum / static_cast<double>(n);
      std::sort(v.begin(), v.end());
      shown_lat_p95 = v[std::min(n - 1, static_cast<size_t>(static_cast<double>(n) * 0.95))];
    }
  }
  const ULONGLONG now = GetTickCount64();
  if (fps_t0_ == 0) fps_t0_ = now;
  ++fps_count_;
  if (now - fps_t0_ >= 1000) {
    shown_fps = fps_count_ * 1000.0 / static_cast<double>(now - fps_t0_);
    fps_count_ = 0;
    fps_t0_ = now;
  }
}

// ---------------------------------------------------------------- connection
void App::connectNet(const std::string& host, int port, owc::VideoTransport t, const std::string& label,
                     bool fromUsbFlow) {
  if (!fromUsbFlow) usbCancel();
  owc::ConnectParams cp;
  cp.host = host;
  cp.port = static_cast<uint16_t>(port);
  cp.transport = t;
  char name[64] = "OWC PC";
  DWORD n = sizeof(name);
  if (GetComputerNameA(name, &n)) cp.client_name = std::string("OWC PC (") + name + ")";
  {
    std::lock_guard<std::mutex> l(mu_);
    settings.last_host = host;
    settings.last_port = port;
    settings.last_transport = t == owc::VideoTransport::Udp ? 1 : 0;
    settings.last_device_label = label;
    pending_.clear();
  }
  markDirty();
  if (!client.connect(cp)) toast("Endereço inválido: " + host + ":" + std::to_string(port), true);
}

void App::disconnect() {
  usbCancel();
  client.disconnect();
}

bool App::sessionUp() const {
  auto s = client.state();
  return s == owc::ClientState::Connected || s == owc::ClientState::Streaming;
}

bool App::live() const {
  auto s = client.state();
  return s == owc::ClientState::Connecting || s == owc::ClientState::Handshaking ||
         s == owc::ClientState::Connected || s == owc::ClientState::Streaming ||
         s == owc::ClientState::Reconnecting;
}

std::string App::stateMessage() {
  std::lock_guard<std::mutex> l(mu_);
  return state_msg_;
}

// ---------------------------------------------------------------- events
void App::onStateChanged(owc::ClientState s, owc::DisconnectReason r, const std::string& msg) {
  std::lock_guard<std::mutex> l(mu_);
  state_msg_ = msg;
  if (s == owc::ClientState::Failed || (s == owc::ClientState::Reconnecting && r != owc::DisconnectReason::None)) {
    Toast t;
    t.text = std::string(stateText(s)) + ": " + reasonText(r);
    t.error = true;
    t.until = GetTickCount64() + 6000;
    toasts_.push_back(t);
  }
}

void App::onConnected(const owc::DeviceInfo& info) {
  std::lock_guard<std::mutex> l(mu_);
  dev_key_ = deviceKey(info.model, info.device_name);
  applied_ = false;
  pending_.clear();
}

void App::onCapabilities(const owc::Capabilities& c) {
  ParamList mem;
  int queued = -1;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (applied_) return;
    applied_ = true;
    queued = queued_preset_;
    queued_preset_ = -1;
    auto it = settings.device_params.find(dev_key_);
    if (it != settings.device_params.end()) mem = it->second;
  }
  if (queued >= 0) {
    const owc::Preset& p = owc::builtinPreset(static_cast<owc::PresetId>(queued));
    owc::applyPreset(client, p);
    for (const auto& kv : p.phone) rememberParam(kv.first, kv.second);
    return;
  }
  std::sort(mem.begin(), mem.end(), [](const auto& a, const auto& b) {
    return static_cast<uint16_t>(a.first) < static_cast<uint16_t>(b.first);
  });
  int sent = 0;
  for (const auto& kv : mem) {
    const owc::ParamDesc* d = nullptr;
    for (const auto& pd : c.params)
      if (pd.id == kv.first) d = &pd;
    if (!d || !(d->flags & owc::kParamSupported) || (d->flags & owc::kParamReadOnly)) continue;
    if (d->current == kv.second) continue;
    if (client.setParam(kv.first, kv.second)) ++sent;
  }
  if (sent > 0) toast("Parâmetros salvos deste aparelho reaplicados (" + std::to_string(sent) + ")", false, 3500);
}

void App::onParamState(const owc::ParamState& ps) {
  const ULONGLONG now = GetTickCount64();
  const uint16_t key = static_cast<uint16_t>(ps.id);
  bool rejected = ps.status != owc::ParamStatus::Ok;
  std::lock_guard<std::mutex> l(mu_);
  auto it = pending_.find(key);
  if (rejected) {
    if (it != pending_.end()) pending_.erase(it);
    auto dm = settings.device_params.find(dev_key_);
    if (dm != settings.device_params.end()) {
      auto& v = dm->second;
      v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& e) { return e.first == ps.id; }), v.end());
      dirty_ = true;
    }
    const char* why = ps.status == owc::ParamStatus::Unsupported ? "não suportado"
                      : ps.status == owc::ParamStatus::OutOfRange ? "valor fora da faixa"
                                                                  : "falhou";
    Toast t;
    t.text = "O aparelho recusou \"" + paramLabel(ps.id) + "\" (" + why + "); valor revertido.";
    t.error = true;
    t.until = now + 5000;
    toasts_.push_back(t);
    return;
  }
  if (it == pending_.end()) return;
  if (it->second.value == ps.value) {
    pending_.erase(it);
  } else if (drag_id_.load() != static_cast<int>(key) && now - it->second.sent_ms >= 300) {
    pending_.erase(it);  // phone clamped/adjusted the value: show what it confirmed
  }
}

void App::onFrame(const owc::DecodedFrame& f) {
  if (!f.y || !f.uv || f.width < 2 || f.height < 2) return;
  {
    std::lock_guard<std::mutex> l(frame_mu_);
    PreviewFrame& p = pending_frame_;
    p.w = f.width & ~1u;
    p.h = f.height & ~1u;
    p.y.resize(static_cast<size_t>(p.w) * p.h);
    p.uv.resize(static_cast<size_t>(p.w) * p.h / 2);
    for (uint32_t r = 0; r < p.h; ++r)
      std::memcpy(p.y.data() + static_cast<size_t>(r) * p.w, f.y + static_cast<size_t>(r) * f.y_stride, p.w);
    for (uint32_t r = 0; r < p.h / 2; ++r)
      std::memcpy(p.uv.data() + static_cast<size_t>(r) * p.w, f.uv + static_cast<size_t>(r) * f.uv_stride, p.w);
    p.rotation = f.rotation_applied;
    p.mirrored = f.mirrored;
    p.capture_qpc = f.capture_qpc;
    p.seq = ++frame_seq_;
    frame_new_ = true;
  }
  ++frames_in;
  SetEvent(frame_event_);
}

// ---------------------------------------------------------------- params
std::string App::deviceKeyNow() {
  std::lock_guard<std::mutex> l(mu_);
  return dev_key_;
}

void App::rememberParam(owc::ParamId id, const owc::ParamValue& v) {
  std::lock_guard<std::mutex> l(mu_);
  if (dev_key_.empty()) return;
  auto& list = settings.device_params[dev_key_];
  for (auto& e : list) {
    if (e.first == id) { e.second = v; dirty_ = true; return; }
  }
  list.emplace_back(id, v);
  dirty_ = true;
}

void App::setParam(owc::ParamId id, const owc::ParamValue& v, bool final) {
  const ULONGLONG now = GetTickCount64();
  const uint16_t key = static_cast<uint16_t>(id);
  {
    std::lock_guard<std::mutex> l(mu_);
    pending_[key] = PendingParam{v, now};
    if (!final) {
      auto it = last_sent_.find(key);
      if (it != last_sent_.end() && now - it->second < 70) return;
    }
    last_sent_[key] = now;
  }
  if (!client.setParam(id, v)) {
    std::lock_guard<std::mutex> l(mu_);
    pending_.erase(key);
    Toast t;
    t.text = "Sem conexão com o celular.";
    t.error = true;
    t.until = now + 2500;
    toasts_.push_back(t);
    return;
  }
  rememberParam(id, v);
}

bool App::pending(owc::ParamId id, owc::ParamValue& out) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = pending_.find(static_cast<uint16_t>(id));
  if (it == pending_.end()) return false;
  out = it->second.value;
  return true;
}

void App::setDragging(owc::ParamId id, bool dragging) {
  if (dragging) drag_id_ = static_cast<int>(static_cast<uint16_t>(id));
  else if (drag_id_.load() == static_cast<int>(static_cast<uint16_t>(id))) drag_id_ = -1;
}

void App::resetParams() {
  if (!client.resetParams()) return;
  {
    std::lock_guard<std::mutex> l(mu_);
    auto it = settings.device_params.find(dev_key_);
    if (it != settings.device_params.end()) {
      auto& v = it->second;
      v.erase(std::remove_if(v.begin(), v.end(),
                             [](const auto& e) {
                               const unsigned id = static_cast<unsigned>(e.first);
                               return id >= 20 && id < 60;
                             }),
              v.end());
    }
    pending_.clear();
    dirty_ = true;
  }
  toast("Controles da câmera restaurados para o padrão.");
}

void App::applyOptions(const owc::ClientOptions& o) {
  client.setOptions(o);
  {
    std::lock_guard<std::mutex> l(mu_);
    settings.opts = o;
  }
  markDirty();
}

void App::applyPresetBuiltin(const owc::Preset& p) {
  const bool up = sessionUp();
  int n = owc::applyPreset(client, p);
  {
    std::lock_guard<std::mutex> l(mu_);
    settings.opts = client.options();
    settings.last_preset = p.key;
    if (!up) queued_preset_ = static_cast<int>(p.id);
  }
  if (up) {
    for (const auto& kv : p.phone) rememberParam(kv.first, kv.second);
    toast(std::string("Preset \"") + p.name + "\" aplicado (" + std::to_string(n) + " parâmetros no celular).");
  } else {
    toast(std::string("Preset \"") + p.name + "\": opções do PC aplicadas; o celular receberá ao conectar.");
  }
  preset_active = p.name;
  markDirty();
}

void App::applyUserPreset(const UserPreset& p) {
  owc::ClientOptions o = client.options();
  o.jitter_buffer_ms = p.jitter_buffer_ms;
  o.low_latency_decode = p.low_latency_decode;
  o.keyframe_request_min_interval_ms = p.keyframe_request_min_interval_ms;
  o.udp_max_pending_frames = p.udp_max_pending_frames;
  applyOptions(o);
  int n = 0;
  if (sessionUp()) {
    for (const auto& kv : p.phone) {
      setParam(kv.first, kv.second);
      ++n;
    }
  }
  preset_active = p.name;
  settings.last_preset = "user:" + p.name;
  toast("Preset \"" + p.name + "\" aplicado (" + std::to_string(n) + " parâmetros no celular).");
}

void App::saveUserPreset(const std::string& name) {
  if (name.empty()) return;
  UserPreset up;
  up.name = name;
  owc::ClientOptions o = client.options();
  up.jitter_buffer_ms = o.jitter_buffer_ms;
  up.low_latency_decode = o.low_latency_decode;
  up.keyframe_request_min_interval_ms = o.keyframe_request_min_interval_ms;
  up.udp_max_pending_frames = o.udp_max_pending_frames;
  owc::Capabilities c;
  if (client.capabilities(c)) {
    for (const auto& d : c.params) {
      if (!(d.flags & owc::kParamSupported) || (d.flags & owc::kParamReadOnly)) continue;
      switch (d.id) {
        case owc::ParamId::CameraIndex: case owc::ParamId::VideoWidth: case owc::ParamId::VideoHeight:
        case owc::ParamId::Fps: case owc::ParamId::ScreenMode:
          continue;
        default: break;
      }
      owc::ParamValue v = d.current;
      client.paramValue(d.id, v);
      up.phone.emplace_back(d.id, v);
    }
  }
  bool replaced = false;
  for (auto& e : user_presets)
    if (e.name == name) { e = up; replaced = true; }
  if (!replaced) user_presets.push_back(up);
  if (persist) saveUserPresets(user_presets);
  toast("Preset \"" + name + "\" salvo.");
}

void App::deleteUserPreset(size_t idx) {
  if (idx >= user_presets.size()) return;
  user_presets.erase(user_presets.begin() + static_cast<std::ptrdiff_t>(idx));
  if (persist) saveUserPresets(user_presets);
}

// ---------------------------------------------------------------- toasts / logs
void App::toast(const std::string& text, bool error, unsigned ms) {
  std::lock_guard<std::mutex> l(mu_);
  Toast t;
  t.text = text;
  t.error = error;
  t.until = GetTickCount64() + ms;
  toasts_.push_back(t);
  if (toasts_.size() > 6) toasts_.erase(toasts_.begin());
}

std::vector<Toast> App::toasts() {
  std::lock_guard<std::mutex> l(mu_);
  return toasts_;
}

void App::pollLogs() {
  auto v = client.logs(last_log_id_);
  for (const auto& e : v) {
    LogLine l;
    l.id = e.id;
    l.time = formatClock(e.pc_time_ms);
    l.source = e.source;
    l.level = e.level;
    l.text = e.text;
    log_lines.push_back(std::move(l));
    last_log_id_ = std::max(last_log_id_, e.id);
  }
  if (log_lines.size() > 3000) log_lines.erase(log_lines.begin(), log_lines.begin() + 500);
}

void App::clearLogs() {
  client.clearLogs();
  log_lines.clear();
}

// ---------------------------------------------------------------- USB flow
UsbPhase App::usbPhase() {
  std::lock_guard<std::mutex> l(mu_);
  return usb_phase_;
}

std::vector<std::string> App::usbLines() {
  std::lock_guard<std::mutex> l(mu_);
  return usb_lines_;
}

std::string App::usbApkPath() {
  std::lock_guard<std::mutex> l(mu_);
  return usb_apk_;
}

void App::usbLog(const std::string& s) {
  std::lock_guard<std::mutex> l(mu_);
  usb_lines_.push_back(s);
  if (usb_lines_.size() > 40) usb_lines_.erase(usb_lines_.begin());
}

void App::usbAnswerInstall(bool yes) {
  {
    std::lock_guard<std::mutex> l(mu_);
    usb_answer_ = yes ? 1 : 2;
  }
  usb_cv_.notify_all();
}

void App::usbCancel() {
  usb_cancel_ = true;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (usb_phase_ == UsbPhase::AwaitInstall && usb_answer_ == 0) usb_answer_ = 2;
  }
  usb_cv_.notify_all();
}

void App::connectUsb(const std::string& serial, const std::string& label) {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (usb_phase_ == UsbPhase::Running || usb_phase_ == UsbPhase::AwaitInstall) return;
  }
  if (usb_th_.joinable()) usb_th_.join();
  usb_cancel_ = false;
  {
    std::lock_guard<std::mutex> l(mu_);
    usb_phase_ = UsbPhase::Running;
    usb_lines_.clear();
    usb_answer_ = 0;
  }
  usb_th_ = std::thread([this, serial, label] { usbRun(serial, label); });
}

bool App::usbWaitState(uint32_t timeoutMs) {
  const ULONGLONG t0 = GetTickCount64();
  while (!usb_cancel_ && GetTickCount64() - t0 < timeoutMs) {
    auto s = client.state();
    if (s == owc::ClientState::Connected || s == owc::ClientState::Streaming) return true;
    if ((s == owc::ClientState::Failed || s == owc::ClientState::Idle) && GetTickCount64() - t0 > 1500) return false;
    Sleep(100);
  }
  return false;
}

void App::usbRun(std::string serial, std::string label) {
  auto fail = [&](const std::string& msg) {
    usbLog("ERRO: " + msg);
    std::lock_guard<std::mutex> l(mu_);
    usb_phase_ = UsbPhase::Failed;
  };
  auto step = [&](const std::string& s) { usbLog(s); };
  const std::string pkg = owc::kPhonePackage;

  step("Verificando adb...");
  if (!adb.available()) return fail("adb.exe não encontrado.");
  std::string err;

  step("Verificando se o app OWC está instalado no celular...");
  bool installed = adb.isAppInstalled(serial, &err);
  if (usb_cancel_) return fail("cancelado.");
  if (!installed) {
    if (!err.empty()) usbLog("adb: " + err);
    std::wstring apk = findNearExe(L"OWC.apk");
    if (apk.empty()) apk = findNearExe(L"android\\app\\build\\outputs\\apk\\debug\\app-debug.apk");
    if (apk.empty()) return fail("o app OWC não está instalado e nenhum APK foi encontrado (OWC.apk ao lado do exe).");
    {
      std::lock_guard<std::mutex> l(mu_);
      usb_apk_ = toUtf8(apk);
      usb_phase_ = UsbPhase::AwaitInstall;
      usb_answer_ = 0;
    }
    step("O app OWC não está instalado. Aguardando sua confirmação para instalar o APK...");
    int ans;
    {
      std::unique_lock<std::mutex> lk(mu_);
      usb_cv_.wait(lk, [this] { return usb_answer_ != 0; });
      ans = usb_answer_;
    }
    if (ans != 1 || usb_cancel_) return fail("instalação do APK não autorizada.");
    {
      std::lock_guard<std::mutex> l(mu_);
      usb_phase_ = UsbPhase::Running;
    }
    step("Instalando APK (pode levar até um minuto)...");
    err.clear();
    if (!adb.installApk(serial, toUtf8(apk), &err)) return fail("falha ao instalar o APK: " + err);
    step("APK instalado.");
  } else {
    step("App OWC encontrado.");
  }
  if (usb_cancel_) return fail("cancelado.");

  step("Concedendo permissão de câmera...");
  auto r = adb.runFor(serial, {"shell", "pm", "grant", pkg, "android.permission.CAMERA"}, 15000);
  if (!r.ok()) usbLog("aviso: pm grant CAMERA: " + (r.err.empty() ? r.out : r.err));
  step("Permitindo sobreposição de tela (SYSTEM_ALERT_WINDOW)...");
  r = adb.runFor(serial, {"shell", "appops", "set", pkg, "SYSTEM_ALERT_WINDOW", "allow"}, 15000);
  if (!r.ok()) usbLog("aviso: appops: " + (r.err.empty() ? r.out : r.err));

  step("Abrindo o app OWC no celular...");
  err.clear();
  if (!adb.launchApp(serial, true, &err)) return fail("não foi possível abrir o app: " + err);
  for (int i = 0; i < 12 && !usb_cancel_; ++i) Sleep(100);

  step("Criando redirecionamento USB (adb forward)...");
  err.clear();
  uint16_t port = adb.forward(serial, owc::kDefaultControlPort, &err);
  if (port == 0) return fail("adb forward falhou: " + err);
  usbLog("Porta local " + std::to_string(port) + " -> celular:" + std::to_string(owc::kDefaultControlPort));
  if (usb_cancel_) return fail("cancelado.");

  step("Conectando...");
  connectNet("127.0.0.1", port, owc::VideoTransport::Tcp, label, true);
  if (!usbWaitState(25000)) {
    if (usb_cancel_) return fail("cancelado.");
    return fail(std::string("não foi possível conectar: ") + reasonText(client.lastDisconnectReason()));
  }
  step("Conectado. Iniciando o stream...");
  client.startStream();
  step("Pronto.");
  std::lock_guard<std::mutex> l(mu_);
  usb_phase_ = UsbPhase::Done;
}

// ---------------------------------------------------------------- scripts
std::wstring App::findScript(const wchar_t* name) { return findNearExe(std::wstring(L"scripts\\") + name); }

bool App::scriptBusy() {
  std::lock_guard<std::mutex> l(mu_);
  return script_busy_;
}

std::string App::scriptOutput() {
  std::lock_guard<std::mutex> l(mu_);
  return script_out_;
}

void App::runScript(const std::string& what, bool install) {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (script_busy_) return;
    script_busy_ = true;
    script_out_ = "Executando...\n";
  }
  if (script_th_.joinable()) script_th_.join();
  const bool vcam = what == "vcam";
  const wchar_t* name = vcam ? (install ? L"install-virtualcam.ps1" : L"uninstall-virtualcam.ps1")
                             : L"install-obs-plugin.ps1";
  std::wstring path = findScript(name);
  script_th_ = std::thread([this, vcam, install, path, name] {
    std::string out;
    if (path.empty()) {
      out = "Script não encontrado: scripts\\" + toUtf8(name) +
            "\n(procurado ao lado do OWC.exe e nas pastas acima).";
    } else {
      wchar_t sys[MAX_PATH];
      UINT n = GetSystemDirectoryW(sys, MAX_PATH);
      std::wstring ps = std::wstring(sys, n) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
      std::vector<std::string> args = {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", toUtf8(path)};
      if (vcam) args.push_back("-PerUser");
      else if (!install) args.push_back("-Uninstall");
      owc::ProcessResult r = owc::runProcess(ps, args, 120000);
      if (!r.started) out = "Não foi possível iniciar o PowerShell.";
      else {
        out = toUtf8Smart(r.out);
        if (!r.err.empty()) out += "\n" + toUtf8Smart(r.err);
        if (r.timed_out) out += "\n(tempo esgotado)";
        out += "\n[código de saída " + std::to_string(r.exit_code) + "]";
      }
    }
    std::lock_guard<std::mutex> l(mu_);
    script_out_ = out;
    script_busy_ = false;
    probe_t_ = 0;
  });
}

}  // namespace owcapp
