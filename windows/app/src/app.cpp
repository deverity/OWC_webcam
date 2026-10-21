#include "app.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

#include <owc/firewall.h>

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
  // This PC's pairing identity: created once, saved by the constructor.
  // (Self-test / no-persist runs get a throwaway identity.)
  if (s.client_id == owc::ClientId{}) s.client_id = owc::newClientId();
  return s;
}

std::string hexOf(const owc::Id16& id) { return owc::toHex(id.data(), id.size()); }

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
    case R::PairingLocked: return "pareamento bloqueado no celular (muitas tentativas)";
    case R::PairingExpired: return "o código de pareamento expirou";
    case R::PairingRejected: return "pareamento recusado no celular";
    case R::NotAuthorized: return "o celular recusou a autenticação deste PC";
    case R::NotPaired: return "este PC foi removido no celular — conecte de novo para parear";
    case R::PhoneNotAuthentic: return "o celular não provou a identidade — possível interceptação";
  }
  return "";
}

const char* cipherText(owc::Cipher c) {
  switch (c) {
    case owc::Cipher::Aes128Gcm: return "AES-128-GCM";
    case owc::Cipher::ChaCha20Poly1305: return "ChaCha20-Poly1305";
    case owc::Cipher::None: return "Desligada";
  }
  return "?";
}

const char* stateText(owc::ClientState s) {
  using S = owc::ClientState;
  switch (s) {
    case S::Idle: return "Desconectado";
    case S::Connecting: return "Conectando...";
    case S::Handshaking: return "Negociando...";
    case S::Pairing: return "Pareando...";
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
  if (persist) save_asap_ = true;  // persists a newly created client_id (and drops undecryptable keys)
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
  if (fw_th_.joinable()) fw_th_.join();
  if (persist) saveNow();
  if (frame_event_) CloseHandle(frame_event_);
}

void App::startServices() {
  owc::Discovery::Options o;
  discovery.start(o);
  adb_th_ = std::thread([this] { adbLoop(); });
  refreshFirewall();
}

std::string App::firewallCommand() const { return owc::Firewall::addRuleCommandLine(); }

void App::refreshFirewall() {
  if (fw_busy_.exchange(true)) return;
  if (fw_th_.joinable()) fw_th_.join();
  fw_th_ = std::thread([this] {
    fw_state_ = owc::Firewall::rulesPresent() ? 1 : 0;
    fw_busy_ = false;
  });
}

void App::addFirewallRule() {
  if (fw_busy_.exchange(true)) return;
  if (fw_th_.joinable()) fw_th_.join();
  fw_th_ = std::thread([this] {
    const bool ok = owc::Firewall::addRulesElevated();
    fw_state_ = owc::Firewall::rulesPresent() ? 1 : 0;
    toast(ok ? "Regra de firewall criada." : "Regra de firewall não criada (UAC recusado ou falha).", !ok);
    fw_busy_ = false;
  });
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
  recTick();
  {
    std::lock_guard<std::mutex> l(mu_);
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (now - it->second.sent_ms > 1500) it = pending_.erase(it);
      else ++it;
    }
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return t.until < now; }),
                  toasts_.end());
  }
  if (save_asap_.exchange(false) && persist) saveNow();
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
    wifi_devices = owc::Discovery::preferBestLink(discovery.devices());
    std::stable_sort(wifi_devices.begin(), wifi_devices.end(), [](const auto& a, const auto& b) {
      return (a.link == owc::LinkType::UsbTether) > (b.link == owc::LinkType::UsbTether);
    });
    lastDisc = now;
  }
  if (now - probe_t_ > 2000) {
    probe_t_ = now;
    tether_adapters = owc::Discovery::usbTetherAdapters();
    vcam_registered = virtualCamRegistered(&vcam_per_user);
    obs_installed = obsPluginInstalled();
  }
  const double t = static_cast<double>(now) / 1000.0;
  if (stats.state != owc::ClientState::Streaming || stats.session_uptime_ms < 1500) {
    graph.clear();
  } else if (t - last_graph_t_ >= 0.1) {
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
  color_matrix = static_cast<int>(f.color_matrix);
  full_range = f.full_range;
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
    cp.client_id = settings.client_id;
    cp.paired = pairedPhoneList(settings);
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
  recStop();  // finalize the MP4 and restore the pre-recording setup
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
         s == owc::ClientState::Pairing || s == owc::ClientState::Connected ||
         s == owc::ClientState::Streaming || s == owc::ClientState::Reconnecting;
}

std::vector<App::PairedView> App::pairedPhones() {
  std::vector<PairedView> v;
  {
    std::lock_guard<std::mutex> l(mu_);
    for (const auto& kv : settings.paired_phones) v.push_back(PairedView{kv.first, kv.second});
  }
  std::sort(v.begin(), v.end(), [](const PairedView& a, const PairedView& b) { return a.entry.label < b.entry.label; });
  for (auto& e : v) e.entry.key = owc::Key32{};  // the UI never needs the secret
  return v;
}

void App::forgetPairedPhone(const std::string& phoneHex) {
  owc::PhoneId id;
  if (owc::fromHex(phoneHex, id.data(), id.size())) client.forgetPairing(id);
  std::string label;
  {
    std::lock_guard<std::mutex> l(mu_);
    auto it = settings.paired_phones.find(phoneHex);
    if (it == settings.paired_phones.end()) return;
    label = it->second.label;
    SecureZeroMemory(it->second.key.data(), it->second.key.size());
    settings.paired_phones.erase(it);
  }
  save_asap_ = true;
  toast("\"" + label + "\" esquecido neste PC. Para desparear de vez, remova este PC no app do celular.", false, 6000);
}

std::string App::clientIdHex() {
  std::lock_guard<std::mutex> l(mu_);
  return hexOf(settings.client_id);
}

std::string App::stateMessage() {
  std::lock_guard<std::mutex> l(mu_);
  return state_msg_;
}

// ---------------------------------------------------------------- events
void App::onStateChanged(owc::ClientState s, owc::DisconnectReason r, const std::string& msg) {
  using R = owc::DisconnectReason;
  std::string pairText;  // pairing failures get their own (actionable) message
  if (s == owc::ClientState::Failed) {
    const owc::PairingInfo pi = client.pairingInfo();
    const std::string who = pi.device_name.empty() ? (pi.model.empty() ? "o celular" : pi.model) : pi.device_name;
    if (r == R::PairingLocked)
      pairText = "Pareamento com " + who + " bloqueado (muitas tentativas). Tente de novo em " +
                 std::to_string(pi.retry_after_s ? pi.retry_after_s : owc::kPairLockoutS) + " s.";
    else if (r == R::PairingExpired)
      pairText = "O código de pareamento expirou. Conecte de novo para gerar outro código.";
    else if (r == R::PairingRejected)
      pairText = "Pareamento recusado no celular (" + who + ").";
    else if (r == R::NotAuthorized)
      pairText = "O celular " + who + " recusou a autenticação deste PC. Conecte de novo para parear.";
    else if (r == R::NotPaired)
      pairText = "Este PC foi removido no celular " + who + ". Conecte de novo para parear (o celular mostrará um código).";
    else if (r == R::PhoneNotAuthentic)
      pairText = "O celular " + who + " não provou a identidade: possível interceptação na rede. Conexão "
                 "recusada (o pareamento foi mantido). Verifique a rede e conecte de novo.";
  }
  std::lock_guard<std::mutex> l(mu_);
  state_msg_ = msg;
  if (s == owc::ClientState::Failed || (s == owc::ClientState::Reconnecting && r != owc::DisconnectReason::None)) {
    Toast t;
    t.text = !pairText.empty() ? pairText : std::string(stateText(s)) + ": " + reasonText(r);
    t.error = true;
    t.until = GetTickCount64() + (pairText.empty() ? 6000 : 8000);
    toasts_.push_back(t);
  }
}

void App::onPaired(const owc::PhoneId& id, const owc::Key32& key, const owc::DeviceInfo& info) {
  const std::string name = info.device_name.empty() ? info.model : info.device_name;
  {
    std::lock_guard<std::mutex> l(mu_);
    PairedPhoneEntry e;
    e.key = key;
    e.label = info.model.empty() || info.model == name ? name : name + " (" + info.model + ")";
    e.paired_at = static_cast<long long>(time(nullptr));
    settings.paired_phones[hexOf(id)] = e;
  }
  save_asap_ = true;  // saved by the UI thread (DPAPI + file), not on owc-net
  toast("Pareado com " + name + ".", false, 5000);
}

void App::onKeyRejected(const owc::PhoneId& id) {
  {
    std::lock_guard<std::mutex> l(mu_);
    auto it = settings.paired_phones.find(hexOf(id));
    if (it == settings.paired_phones.end()) return;
    SecureZeroMemory(it->second.key.data(), it->second.key.size());
    settings.paired_phones.erase(it);
  }
  save_asap_ = true;
  toast("O celular não reconhece mais este PC (foi removido lá): é preciso parear de novo.", false, 6000);
}

void App::onConnected(const owc::DeviceInfo& info) {
  std::lock_guard<std::mutex> l(mu_);
  dev_key_ = deviceKey(info.model, info.device_name);
  applied_ = false;
  applied_cam_ = -1;
  pending_.clear();
  auto it = settings.device_orient.find(dev_key_);
  if (it != settings.device_orient.end()) {
    owc::OutputTransform t = client.options().transform;
    t.apply_phone_hints = it->second.first < 0;
    t.rotation_deg = it->second.first < 0 ? 0 : it->second.first;
    t.mirror = it->second.second;
    client.setTransform(t);
  }
}

std::string App::memKey(owc::ParamId id) {
  const unsigned v = static_cast<unsigned>(id);
  std::lock_guard<std::mutex> l(mu_);
  if (v >= 20 && v < 60) return dev_key_ + "#cam" + std::to_string(cur_cam_);
  return dev_key_;
}

void App::currentOrientation(int& mode, bool& mirror) {
  owc::OutputTransform t = client.options().transform;
  mode = t.apply_phone_hints ? -1 : (((t.rotation_deg % 360) + 360) % 360);
  mirror = t.mirror;
}

void App::setOrientation(int mode, bool mirror) {
  owc::ClientOptions o = client.options();
  o.transform.apply_phone_hints = mode < 0;
  o.transform.rotation_deg = mode < 0 ? 0 : mode;
  o.transform.mirror = mirror;
  applyOptions(o);
  std::lock_guard<std::mutex> l(mu_);
  if (!dev_key_.empty()) settings.device_orient[dev_key_] = {mode, mirror};
  dirty_ = true;
}

void App::onCapabilities(const owc::Capabilities& c) {
  ParamList mem;
  int queued = -1;
  bool deviceLevel = false;
  {
    std::lock_guard<std::mutex> l(mu_);
    cur_cam_ = c.active_camera;
    if (!applied_) {
      applied_ = true;
      deviceLevel = true;
      queued = queued_preset_;
      queued_preset_ = -1;
      auto it = settings.device_params.find(dev_key_);
      if (it != settings.device_params.end()) mem = it->second;
    }
  }
  if (queued >= 0) {
    const owc::Preset& p = owc::builtinPreset(static_cast<owc::PresetId>(queued));
    owc::applyPreset(client, p);
    for (const auto& kv : p.phone) rememberParam(kv.first, kv.second);
    applied_cam_ = c.active_camera;
    return;
  }
  auto byId = [](const auto& a, const auto& b) { return static_cast<uint16_t>(a.first) < static_cast<uint16_t>(b.first); };
  std::sort(mem.begin(), mem.end(), byId);
  int sent = 0;
  bool camChanging = false;
  auto send = [&](const std::pair<owc::ParamId, owc::ParamValue>& kv) {
    const owc::ParamDesc* d = nullptr;
    for (const auto& pd : c.params)
      if (pd.id == kv.first) d = &pd;
    if (!d || !(d->flags & owc::kParamSupported) || (d->flags & owc::kParamReadOnly)) return;
    if (d->current == kv.second) return;
    if (client.setParam(kv.first, kv.second)) {
      ++sent;
      if (kv.first == owc::ParamId::CameraIndex) camChanging = true;
    }
  };
  if (deviceLevel)
    for (const auto& kv : mem) send(kv);
  // camera controls are remembered per camera index; skip while a camera switch is pending
  // (new Capabilities will arrive and the right camera's controls are applied then)
  if (!camChanging && applied_cam_ != c.active_camera) {
    applied_cam_ = c.active_camera;
    ParamList cam;
    {
      std::lock_guard<std::mutex> l(mu_);
      auto it = settings.device_params.find(dev_key_ + "#cam" + std::to_string(c.active_camera));
      if (it != settings.device_params.end()) cam = it->second;
    }
    std::sort(cam.begin(), cam.end(), byId);
    for (const auto& kv : cam) send(kv);
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
    const unsigned idv = static_cast<unsigned>(ps.id);
    auto dm = settings.device_params.find(idv >= 20 && idv < 60 ? dev_key_ + "#cam" + std::to_string(cur_cam_) : dev_key_);
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
    p.color_matrix = f.color_matrix;
    p.full_range = f.full_range;
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
  const unsigned idv = static_cast<unsigned>(id);
  auto& list = settings.device_params[idv >= 20 && idv < 60 ? dev_key_ + "#cam" + std::to_string(cur_cam_) : dev_key_];
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
    settings.device_params.erase(dev_key_ + "#cam" + std::to_string(cur_cam_));
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
  ULONGLONG t0 = GetTickCount64();
  while (!usb_cancel_ && GetTickCount64() - t0 < timeoutMs) {
    auto s = client.state();
    if (s == owc::ClientState::Connected || s == owc::ClientState::Streaming) return true;
    // The user is typing the pairing code: keep waiting (the client bounds it by the code lifetime).
    if (s == owc::ClientState::Pairing) t0 = GetTickCount64();
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
