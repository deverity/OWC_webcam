#include "settings.h"

#include <algorithm>

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

static Json wheelToJson(const owc::ColorWheel& w) {
  Json a = Json::array();
  a.push(Json::number(static_cast<double>(w.r)));
  a.push(Json::number(static_cast<double>(w.g)));
  a.push(Json::number(static_cast<double>(w.b)));
  a.push(Json::number(static_cast<double>(w.y)));
  return a;
}

static float clampNum(const Json& v, double lo, double hi, float def) {
  if (v.type != Json::Type::Num || v.n != v.n) return def;
  return static_cast<float>(v.n < lo ? lo : (v.n > hi ? hi : v.n));
}

// [r, g, b, y], each -1..1
static void wheelFromJson(const Json* j, owc::ColorWheel& w) {
  if (!j || !j->isArr() || j->a.size() != 4) return;
  w.r = clampNum(j->a[0], -1.0, 1.0, 0.0f);
  w.g = clampNum(j->a[1], -1.0, 1.0, 0.0f);
  w.b = clampNum(j->a[2], -1.0, 1.0, 0.0f);
  w.y = clampNum(j->a[3], -1.0, 1.0, 0.0f);
}

// [[x, y], ...]
static Json curveToJson(const std::vector<owc::CurvePoint>& c) {
  Json a = Json::array();
  for (const auto& p : c) {
    Json e = Json::array();
    e.push(Json::number(static_cast<double>(p.x)));
    e.push(Json::number(static_cast<double>(p.y)));
    a.push(std::move(e));
  }
  return a;
}

// Clamped to 0..1, NaN dropped, sorted by x, duplicates in x merged, at most 16 points.
static void curveFromJson(const Json* j, std::vector<owc::CurvePoint>& c) {
  if (!j || !j->isArr()) return;
  c.clear();
  for (const auto& e : j->a) {
    if (!e.isArr() || e.a.size() != 2 || e.a[0].type != Json::Type::Num || e.a[1].type != Json::Type::Num) continue;
    if (e.a[0].n != e.a[0].n || e.a[1].n != e.a[1].n) continue;
    owc::CurvePoint p;
    p.x = clampNum(e.a[0], 0.0, 1.0, 0.0f);
    p.y = clampNum(e.a[1], 0.0, 1.0, 0.0f);
    c.push_back(p);
  }
  std::stable_sort(c.begin(), c.end(), [](const owc::CurvePoint& a, const owc::CurvePoint& b) { return a.x < b.x; });
  c.erase(std::unique(c.begin(), c.end(), [](const owc::CurvePoint& a, const owc::CurvePoint& b) { return a.x == b.x; }),
          c.end());
  if (c.size() > 16) c.resize(16);
}

static Json stringsToJson(const std::vector<std::string>& v) {
  Json a = Json::array();
  for (const auto& s : v) a.push(Json::string(s));
  return a;
}

static void stringsFromJson(const Json* j, std::vector<std::string>& v, size_t cap) {
  if (!j || !j->isArr()) return;
  v.clear();
  for (const auto& e : j->a)
    if (e.type == Json::Type::Str && !e.s.empty() && v.size() < cap) v.push_back(e.s);
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
  j.set("color_enabled", Json::boolean(o.color.enabled));
  j.set("color_exposure_ev", Json::number(static_cast<double>(o.color.exposure_ev)));
  j.set("color_contrast", Json::number(static_cast<double>(o.color.contrast)));
  j.set("color_highlights", Json::number(static_cast<double>(o.color.highlights)));
  j.set("color_shadows", Json::number(static_cast<double>(o.color.shadows)));
  j.set("color_saturation", Json::number(static_cast<double>(o.color.saturation)));
  j.set("color_vibrance", Json::number(static_cast<double>(o.color.vibrance)));
  j.set("color_temperature", Json::number(static_cast<double>(o.color.temperature)));
  j.set("color_tint", Json::number(static_cast<double>(o.color.tint)));
  j.set("color_lut", Json::string(o.color.lut));
  j.set("color_lut_strength", Json::number(static_cast<double>(o.color.lut_strength)));
  j.set("color_compare_split", Json::boolean(o.color.compare_split));
  j.set("color_pivot", Json::number(static_cast<double>(o.color.pivot)));
  j.set("color_lift", wheelToJson(o.color.lift));
  j.set("color_gamma", wheelToJson(o.color.gamma));
  j.set("color_gain", wheelToJson(o.color.gain));
  j.set("color_offset", wheelToJson(o.color.offset));
  j.set("color_curve_y", curveToJson(o.color.curve_y));
  j.set("color_curve_r", curveToJson(o.color.curve_r));
  j.set("color_curve_g", curveToJson(o.color.curve_g));
  j.set("color_curve_b", curveToJson(o.color.curve_b));
  j.set("color_hue_vs_hue", curveToJson(o.color.hue_vs_hue));
  j.set("color_hue_vs_sat", curveToJson(o.color.hue_vs_sat));
  j.set("color_luma_vs_sat", curveToJson(o.color.luma_vs_sat));
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

  // colour grade: clamp to the documented ranges (a hand-edited file must not feed garbage to the core)
  auto cf = [&](const char* k, float def, double lo, double hi) {
    const double v = j.num(k, static_cast<double>(def));
    if (v != v) return def;  // NaN
    return static_cast<float>(v < lo ? lo : (v > hi ? hi : v));
  };
  owc::ColorGrade& g = o.color;
  g.enabled = j.flag("color_enabled", g.enabled);
  g.exposure_ev = cf("color_exposure_ev", g.exposure_ev, -3.0, 3.0);
  g.contrast = cf("color_contrast", g.contrast, -1.0, 1.0);
  g.highlights = cf("color_highlights", g.highlights, -1.0, 1.0);
  g.shadows = cf("color_shadows", g.shadows, -1.0, 1.0);
  g.saturation = cf("color_saturation", g.saturation, -1.0, 1.0);
  g.vibrance = cf("color_vibrance", g.vibrance, -1.0, 1.0);
  g.temperature = cf("color_temperature", g.temperature, -1.0, 1.0);
  g.tint = cf("color_tint", g.tint, -1.0, 1.0);
  g.lut = j.str("color_lut", g.lut);
  g.lut_strength = cf("color_lut_strength", g.lut_strength, 0.0, 1.0);
  g.compare_split = j.flag("color_compare_split", g.compare_split);
  g.pivot = cf("color_pivot", g.pivot, 0.1, 0.9);
  wheelFromJson(j.find("color_lift"), g.lift);
  wheelFromJson(j.find("color_gamma"), g.gamma);
  wheelFromJson(j.find("color_gain"), g.gain);
  wheelFromJson(j.find("color_offset"), g.offset);
  curveFromJson(j.find("color_curve_y"), g.curve_y);
  curveFromJson(j.find("color_curve_r"), g.curve_r);
  curveFromJson(j.find("color_curve_g"), g.curve_g);
  curveFromJson(j.find("color_curve_b"), g.curve_b);
  curveFromJson(j.find("color_hue_vs_hue"), g.hue_vs_hue);
  curveFromJson(j.find("color_hue_vs_sat"), g.hue_vs_sat);
  curveFromJson(j.find("color_luma_vs_sat"), g.luma_vs_sat);
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
  stringsFromJson(j.find("color_recent_luts"), s.color_recent_luts, 24);
  stringsFromJson(j.find("color_lut_folders"), s.color_lut_folders, 16);
  auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  s.color_palette = clampi(static_cast<int>(j.num("color_palette", s.color_palette)), 0, 4);
  s.color_scope_a = clampi(static_cast<int>(j.num("color_scope_a", s.color_scope_a)), 0, 3);
  s.color_scope_b = clampi(static_cast<int>(j.num("color_scope_b", s.color_scope_b)), 0, 3);
  s.color_scope_two = j.flag("color_scope_two", s.color_scope_two);
  {
    const double sp = j.num("color_split", static_cast<double>(s.color_split));
    s.color_split = (sp == sp) ? static_cast<float>(sp < 0.25 ? 0.25 : (sp > 0.75 ? 0.75 : sp)) : 0.48f;
  }
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
  j.set("color_recent_luts", stringsToJson(s.color_recent_luts));
  j.set("color_lut_folders", stringsToJson(s.color_lut_folders));
  j.set("color_palette", Json::number(s.color_palette));
  j.set("color_scope_a", Json::number(s.color_scope_a));
  j.set("color_scope_b", Json::number(s.color_scope_b));
  j.set("color_scope_two", Json::boolean(s.color_scope_two));
  j.set("color_split", Json::number(static_cast<double>(s.color_split)));
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
