#include "settings.h"

#include "json.h"
#include "platform.h"

namespace owcapp {

std::wstring settingsPath() { return appDataDir() + L"\\settings.json"; }
std::wstring presetsPath() { return appDataDir() + L"\\presets.json"; }

std::string deviceKey(const std::string& model, const std::string& name) { return model + "|" + name; }

static Json paramToJson(owc::ParamId id, const owc::ParamValue& v) {
  Json j = Json::object();
  j.set("id", Json::number(static_cast<double>(static_cast<uint16_t>(id))));
  j.set("t", Json::number(static_cast<double>(static_cast<int>(v.type))));
  if (v.type == owc::ValueType::Float) j.set("f", Json::number(static_cast<double>(v.f)));
  else j.set("i", Json::number(static_cast<double>(v.i)));
  return j;
}

static bool paramFromJson(const Json& j, owc::ParamId& id, owc::ParamValue& v) {
  if (!j.isObj() || !j.find("id") || !j.find("t")) return false;
  id = static_cast<owc::ParamId>(static_cast<uint16_t>(j.num("id", 0)));
  int t = static_cast<int>(j.num("t", 1));
  if (t < 0 || t > 4) return false;
  v.type = static_cast<owc::ValueType>(t);
  v.f = static_cast<float>(j.num("f", 0));
  v.i = static_cast<int64_t>(j.num("i", 0));
  return true;
}

static Json paramsToJson(const ParamList& l) {
  Json a = Json::array();
  for (const auto& p : l) a.push(paramToJson(p.first, p.second));
  return a;
}

static void paramsFromJson(const Json* a, ParamList& out) {
  out.clear();
  if (!a || !a->isArr()) return;
  for (const auto& e : a->a) {
    owc::ParamId id;
    owc::ParamValue v;
    if (paramFromJson(e, id, v)) out.emplace_back(id, v);
  }
}

static Json optionsToJson(const owc::ClientOptions& o) {
  Json j = Json::object();
  j.set("jitter_buffer_ms", Json::number(o.jitter_buffer_ms));
  j.set("decoder_mode", Json::number(static_cast<double>(o.decoder_mode)));
  j.set("low_latency_decode", Json::boolean(o.low_latency_decode));
  j.set("keyframe_request_min_interval_ms", Json::number(o.keyframe_request_min_interval_ms));
  j.set("udp_recv_buffer_bytes", Json::number(o.udp_recv_buffer_bytes));
  j.set("udp_max_pending_frames", Json::number(o.udp_max_pending_frames));
  j.set("rotation_deg", Json::number(o.transform.rotation_deg));
  j.set("mirror", Json::boolean(o.transform.mirror));
  j.set("apply_phone_hints", Json::boolean(o.transform.apply_phone_hints));
  j.set("codec_preference", Json::number(static_cast<double>(o.codec_preference)));
  j.set("auto_reconnect", Json::boolean(o.auto_reconnect));
  j.set("reconnect_delay_ms", Json::number(o.reconnect_delay_ms));
  j.set("auto_start_stream", Json::boolean(o.auto_start_stream));
  j.set("publish_to_shm", Json::boolean(o.publish_to_shm));
  j.set("connect_timeout_ms", Json::number(o.connect_timeout_ms));
  j.set("handshake_timeout_ms", Json::number(o.handshake_timeout_ms));
  j.set("handshake_attempts", Json::number(o.handshake_attempts));
  j.set("idle_timeout_ms", Json::number(o.idle_timeout_ms));
  j.set("patch_sps", Json::boolean(o.patch_sps));
  j.set("udp_bind_address", Json::string(o.udp_bind_address));
  return j;
}

static void optionsFromJson(const Json& j, owc::ClientOptions& o) {
  if (!j.isObj()) return;
  auto u32 = [&](const char* k, uint32_t def) { return static_cast<uint32_t>(j.num(k, def)); };
  o.jitter_buffer_ms = u32("jitter_buffer_ms", o.jitter_buffer_ms);
  int dm = static_cast<int>(j.num("decoder_mode", static_cast<double>(o.decoder_mode)));
  if (dm >= 0 && dm <= 2) o.decoder_mode = static_cast<owc::DecoderMode>(dm);
  int cp = static_cast<int>(j.num("codec_preference", 0));
  if (cp >= 0 && cp <= 3) o.codec_preference = static_cast<owc::CodecPreference>(cp);
  o.low_latency_decode = j.flag("low_latency_decode", o.low_latency_decode);
  o.keyframe_request_min_interval_ms = u32("keyframe_request_min_interval_ms", o.keyframe_request_min_interval_ms);
  o.udp_recv_buffer_bytes = u32("udp_recv_buffer_bytes", o.udp_recv_buffer_bytes);
  o.udp_max_pending_frames = u32("udp_max_pending_frames", o.udp_max_pending_frames);
  o.transform.rotation_deg = static_cast<int>(j.num("rotation_deg", o.transform.rotation_deg));
  o.transform.mirror = j.flag("mirror", o.transform.mirror);
  o.transform.apply_phone_hints = j.flag("apply_phone_hints", o.transform.apply_phone_hints);
  o.auto_reconnect = j.flag("auto_reconnect", o.auto_reconnect);
  o.reconnect_delay_ms = u32("reconnect_delay_ms", o.reconnect_delay_ms);
  o.auto_start_stream = j.flag("auto_start_stream", o.auto_start_stream);
  o.publish_to_shm = j.flag("publish_to_shm", o.publish_to_shm);
  o.connect_timeout_ms = u32("connect_timeout_ms", o.connect_timeout_ms);
  o.handshake_timeout_ms = u32("handshake_timeout_ms", o.handshake_timeout_ms);
  o.handshake_attempts = u32("handshake_attempts", o.handshake_attempts);
  o.idle_timeout_ms = u32("idle_timeout_ms", o.idle_timeout_ms);
  o.patch_sps = j.flag("patch_sps", o.patch_sps);
  o.udp_bind_address = j.str("udp_bind_address", o.udp_bind_address);
}

bool loadSettings(AppSettings& s) {
  std::string text;
  Json j;
  if (!readTextFile(settingsPath(), text) || !Json::parse(text, j) || !j.isObj()) return false;
  s.last_host = j.str("last_host", s.last_host);
  s.last_port = static_cast<int>(j.num("last_port", s.last_port));
  s.last_transport = static_cast<int>(j.num("last_transport", s.last_transport));
  s.last_device_label = j.str("last_device_label", s.last_device_label);
  s.manual_addr = j.str("manual_addr", s.manual_addr);
  s.last_preset = j.str("last_preset", s.last_preset);
  s.window_w = static_cast<int>(j.num("window_w", s.window_w));
  s.window_h = static_cast<int>(j.num("window_h", s.window_h));
  s.hud = j.flag("hud", s.hud);
  s.log_level = static_cast<int>(j.num("log_level", s.log_level));
  s.rec_mode = static_cast<int>(j.num("rec_mode", s.rec_mode)) == 1 ? 1 : 0;
  s.rec_priority = static_cast<int>(j.num("rec_priority", s.rec_priority)) == 1 ? 1 : 0;
  s.rec_slowmo_h = static_cast<int>(j.num("rec_slowmo_h", s.rec_slowmo_h)) == 1080 ? 1080 : 720;
  s.rec_slowmo_out_fps = static_cast<int>(j.num("rec_slowmo_out_fps", s.rec_slowmo_out_fps)) == 60 ? 60 : 30;
  s.rec_keep_current = j.flag("rec_keep_current", s.rec_keep_current);
  s.rec_rt_w = static_cast<int>(j.num("rec_rt_w", s.rec_rt_w));
  s.rec_rt_h = static_cast<int>(j.num("rec_rt_h", s.rec_rt_h));
  s.rec_rt_fps = static_cast<int>(j.num("rec_rt_fps", s.rec_rt_fps));
  s.rec_dir = j.str("rec_dir", s.rec_dir);
  if (const Json* o = j.find("options")) optionsFromJson(*o, s.opts);
  if (const Json* d = j.find("devices"); d && d->isObj()) {
    for (const auto& kv : d->o) {
      ParamList l;
      paramsFromJson(&kv.second, l);
      if (!l.empty()) s.device_params[kv.first] = std::move(l);
    }
  }
  if (const Json* d = j.find("orient"); d && d->isObj()) {
    for (const auto& kv : d->o)
      s.device_orient[kv.first] = {static_cast<int>(kv.second.num("m", -1)), kv.second.flag("x", false)};
  }
  if (s.window_w < 900) s.window_w = 900;
  if (s.window_h < 600) s.window_h = 600;
  return true;
}

bool saveSettings(const AppSettings& s) {
  Json j = Json::object();
  j.set("last_host", Json::string(s.last_host));
  j.set("last_port", Json::number(s.last_port));
  j.set("last_transport", Json::number(s.last_transport));
  j.set("last_device_label", Json::string(s.last_device_label));
  j.set("manual_addr", Json::string(s.manual_addr));
  j.set("last_preset", Json::string(s.last_preset));
  j.set("window_w", Json::number(s.window_w));
  j.set("window_h", Json::number(s.window_h));
  j.set("hud", Json::boolean(s.hud));
  j.set("log_level", Json::number(s.log_level));
  j.set("rec_mode", Json::number(s.rec_mode));
  j.set("rec_priority", Json::number(s.rec_priority));
  j.set("rec_slowmo_h", Json::number(s.rec_slowmo_h));
  j.set("rec_slowmo_out_fps", Json::number(s.rec_slowmo_out_fps));
  j.set("rec_keep_current", Json::boolean(s.rec_keep_current));
  j.set("rec_rt_w", Json::number(s.rec_rt_w));
  j.set("rec_rt_h", Json::number(s.rec_rt_h));
  j.set("rec_rt_fps", Json::number(s.rec_rt_fps));
  j.set("rec_dir", Json::string(s.rec_dir));
  j.set("options", optionsToJson(s.opts));
  Json d = Json::object();
  for (const auto& kv : s.device_params) d.set(kv.first, paramsToJson(kv.second));
  j.set("devices", std::move(d));
  Json orient = Json::object();
  for (const auto& kv : s.device_orient) {
    Json e = Json::object();
    e.set("m", Json::number(kv.second.first));
    e.set("x", Json::boolean(kv.second.second));
    orient.set(kv.first, std::move(e));
  }
  j.set("orient", std::move(orient));
  return writeTextFileAtomic(settingsPath(), j.dump(2));
}

bool loadUserPresets(std::vector<UserPreset>& out) {
  out.clear();
  std::string text;
  Json j;
  if (!readTextFile(presetsPath(), text) || !Json::parse(text, j)) return false;
  const Json* arr = j.isObj() ? j.find("presets") : &j;
  if (!arr || !arr->isArr()) return false;
  for (const auto& e : arr->a) {
    if (!e.isObj()) continue;
    UserPreset p;
    p.name = e.str("name");
    if (p.name.empty()) continue;
    p.jitter_buffer_ms = static_cast<uint32_t>(e.num("jitter_buffer_ms", 0));
    p.low_latency_decode = e.flag("low_latency_decode", true);
    p.keyframe_request_min_interval_ms = static_cast<uint32_t>(e.num("keyframe_request_min_interval_ms", 250));
    p.udp_max_pending_frames = static_cast<uint32_t>(e.num("udp_max_pending_frames", 4));
    paramsFromJson(e.find("phone"), p.phone);
    out.push_back(std::move(p));
  }
  return true;
}

bool saveUserPresets(const std::vector<UserPreset>& in) {
  Json arr = Json::array();
  for (const auto& p : in) {
    Json e = Json::object();
    e.set("name", Json::string(p.name));
    e.set("jitter_buffer_ms", Json::number(p.jitter_buffer_ms));
    e.set("low_latency_decode", Json::boolean(p.low_latency_decode));
    e.set("keyframe_request_min_interval_ms", Json::number(p.keyframe_request_min_interval_ms));
    e.set("udp_max_pending_frames", Json::number(p.udp_max_pending_frames));
    e.set("phone", paramsToJson(p.phone));
    arr.push(std::move(e));
  }
  Json root = Json::object();
  root.set("presets", std::move(arr));
  return writeTextFileAtomic(presetsPath(), root.dump(2));
}

}  // namespace owcapp
