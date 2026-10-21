// Parameter-driven tabs: Câmera, Encoder/Stream, PC, Dispositivo.
#include <imgui.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include "ui_internal.h"

namespace owcapp {
namespace ui {

namespace {

using owc::ParamDesc;
using owc::ParamId;
using owc::ParamValue;
using owc::ValueType;

const ParamDesc* findDesc(const owc::Capabilities& c, ParamId id) {
  for (const auto& d : c.params)
    if (d.id == id) return &d;
  return nullptr;
}

ParamValue shownValue(App& app, const ParamDesc& d) {
  ParamValue pv;
  if (app.pending(d.id, pv)) return pv;
  ParamValue v = d.current;
  app.client.paramValue(d.id, v);
  return v;
}

double rawOf(const ParamValue& v) { return v.type == ValueType::Float ? static_cast<double>(v.f) : static_cast<double>(v.i); }

ParamValue makeVal(ValueType t, double v) {
  switch (t) {
    case ValueType::Bool: return ParamValue::Bool(v != 0);
    case ValueType::Int32: return ParamValue::Int(static_cast<int32_t>(std::llround(v)));
    case ValueType::Int64: return ParamValue::Long(static_cast<int64_t>(std::llround(v)));
    case ValueType::Float: return ParamValue::Real(static_cast<float>(v));
    case ValueType::Enum: return ParamValue::EnumV(static_cast<int32_t>(std::llround(v)));
  }
  return ParamValue::Int(0);
}

double toDisp(PFmt f, double v) {
  switch (f) {
    case PFmt::Mbps:
    case PFmt::MsFromNs: return v / 1e6;
    default: return v;
  }
}
double fromDisp(PFmt f, double v) {
  switch (f) {
    case PFmt::Mbps:
    case PFmt::MsFromNs: return v * 1e6;
    default: return v;
  }
}

std::string fmtd(const char* f, double a) {
  char b[96];
  std::snprintf(b, sizeof b, f, a);
  return b;
}

std::string valueText(ParamId id, PFmt f, ValueType t, double disp) {
  const double raw = fromDisp(f, disp);
  char b[96];
  switch (id) {
    case ParamId::FecPercent:
      if (raw < 0) return "Auto";
      if (raw == 0) return "Desligado";
      std::snprintf(b, sizeof b, "%d %%", static_cast<int>(std::lround(raw)));
      return b;
    case ParamId::QpMin:
    case ParamId::QpMax:
      if (raw == 0) return "Auto";
      return std::to_string(static_cast<int>(std::lround(raw)));
    case ParamId::KeyframeIntervalMs:
      if (raw == 0) return "Só sob demanda";
      std::snprintf(b, sizeof b, "%d ms", static_cast<int>(std::lround(raw)));
      return b;
    case ParamId::IntraRefreshFrames:
      if (raw == 0) return "Desligado";
      std::snprintf(b, sizeof b, "%d quadros", static_cast<int>(std::lround(raw)));
      return b;
    case ParamId::MaxSendQueueFrames:
      std::snprintf(b, sizeof b, "%d quadros", static_cast<int>(std::lround(raw)));
      return b;
    case ParamId::FocusDistance:
      if (raw <= 0.001) return "\xE2\x88\x9E  (0 D)";
      std::snprintf(b, sizeof b, "%.2f D  (%.0f cm)", raw, 100.0 / raw);
      return b;
    default: break;
  }
  switch (f) {
    case PFmt::Mbps: return fmtd("%.2f Mbps", disp);
    case PFmt::MsFromNs: return fmtd(disp < 10 ? "%.2f ms" : "%.1f ms", disp);
    case PFmt::Diopter: return fmtd("%.2f D", disp);
    case PFmt::ZoomX: return fmtd("%.2f\xC3\x97", disp);
    case PFmt::Percent: return fmtd("%.0f %%", disp);
    case PFmt::Ms: return fmtd("%.0f ms", disp);
    case PFmt::Frames: return fmtd("%.0f quadros", disp);
    case PFmt::Signed: return fmtd("%+.0f", disp);
    default: break;
  }
  if (t == ValueType::Float) return fmtd("%.2f", disp);
  return fmtd("%.0f", disp);
}

// Generic widget for one ParamId, driven by its ParamDesc. asToggle: render a
// two-option enum as a checkbox (value != 0 = on).
void paramControl(App& app, ParamId id, bool asToggle = false, const char* labelOverride = nullptr) {
  const PMeta* m = findMeta(id);
  const std::string label = labelOverride ? labelOverride : paramLabel(id);
  const std::string baseTip = m ? m->tip : "Parâmetro novo (de uma versão mais recente do app do celular); controle genérico.";
  const PFmt fmt = m ? m->fmt : PFmt::Plain;
  const ParamDesc* d = findDesc(app.caps, id);
  const bool connected = app.sessionUp();
  const bool supported = d && (d->flags & owc::kParamSupported);
  const bool ro = d && (d->flags & owc::kParamReadOnly);
  const bool reconf = d && (d->flags & owc::kParamReconfigures);
  const bool enabled = connected && supported && !ro;

  std::string tipText = baseTip;
  if (!supported) tipText = std::string(connected ? "Não suportado por este aparelho.\n\n" : "") + baseTip;
  else if (ro) tipText = "Somente leitura.\n\n" + baseTip;
  if (!supported && connected) tipText = "não suportado por este aparelho\n\n" + baseTip;

  ImGui::PushID(static_cast<int>(id));
  ImGui::BeginGroup();
  ImGui::BeginDisabled(!enabled);

  if (!d) {
    // Unknown to this phone (or not connected): greyed placeholder.
    ImGui::TextUnformatted(label.c_str());
    if (connected) {
      ImGui::SameLine();
      ImGui::TextDisabled("(não suportado)");
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    tip(tipText.c_str());
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, sc(2)));
    return;
  }

  const ParamValue cur = shownValue(app, *d);
  const bool isToggle = d->type == ValueType::Bool || (asToggle && d->type == ValueType::Enum);

  if (isToggle) {
    bool b = cur.i != 0;
    if (ImGui::Checkbox(label.c_str(), &b)) app.setParam(id, makeVal(d->type, b ? 1 : 0));
    if (!supported && connected) {
      ImGui::SameLine();
      ImGui::TextDisabled("(não suportado)");
    }
    if (reconf) {
      ImGui::SameLine();
      ImGui::TextDisabled("(reinicia o stream)");
    }
  } else {
    ImGui::TextUnformatted(label.c_str());
    if (!supported && connected) {
      ImGui::SameLine();
      ImGui::TextDisabled("(não suportado)");
    }
    if (reconf) {
      ImGui::SameLine();
      ImGui::TextDisabled("(reinicia o stream)");
    }
    if (id == ParamId::ExposureTimeNs && rawOf(cur) > 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("  ~ 1/%.0f s", 1e9 / rawOf(cur));
    }
    if (d->type == ValueType::Enum && !d->options.empty()) {
      const int curV = static_cast<int>(cur.i);
      std::string preview = "valor " + std::to_string(curV);
      for (const auto& o : d->options)
        if (o.value == curV) preview = o.label;
      ImGui::SetNextItemWidth(-1);
      if (ImGui::BeginCombo("##v", preview.c_str())) {
        for (const auto& o : d->options) {
          if (ImGui::Selectable(o.label.c_str(), o.value == curV)) app.setParam(id, ParamValue::EnumV(o.value));
          if (o.value == curV) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
    } else {
      // numeric
      const bool isInt = d->type != ValueType::Float;
      double lo = d->min, hi = d->max;
      double dcur = toDisp(fmt, rawOf(cur));
      double dlo = toDisp(fmt, lo), dhi = toDisp(fmt, hi);
      const bool hasRange = hi > lo;
      const bool logScale = hasRange && (id == ParamId::ExposureTimeNs || id == ParamId::Iso) && lo >= 0 && hi / std::max(lo, 1.0) > 20;
      if (logScale) dlo = std::max(dlo, id == ParamId::ExposureTimeNs ? 0.01 : 1.0);
      const float inputW = sc(78);
      ParamValue send;
      bool have = false, deact = false, active = false;
      if (hasRange) {
        std::string txt = escapePct(valueText(id, fmt, d->type, dcur));
        ImGui::SetNextItemWidth(-(inputW + ImGui::GetStyle().ItemSpacing.x));
        double edit = std::min(std::max(dcur, dlo), dhi);
        ImGuiSliderFlags sf = ImGuiSliderFlags_AlwaysClamp | (logScale ? ImGuiSliderFlags_Logarithmic : 0);
        if (ImGui::SliderScalar("##s", ImGuiDataType_Double, &edit, &dlo, &dhi, txt.c_str(), sf)) {
          double raw = fromDisp(fmt, edit);
          if (isInt) {
            raw = std::round(raw);
            if (d->step > 1.0 && (d->type == ValueType::Int32 || d->type == ValueType::Int64)) {
              if (id != ParamId::ExposureTimeNs) raw = lo + std::round((raw - lo) / d->step) * d->step;
            }
          }
          raw = std::min(std::max(raw, lo), hi);
          send = makeVal(d->type, raw);
          have = true;
        }
        active = ImGui::IsItemActive();
        deact = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
      } else {
        ImGui::SetNextItemWidth(-1);
      }
      double inp = dcur;
      const char* ifmt = isInt && fmt != PFmt::Mbps && fmt != PFmt::MsFromNs ? "%.0f" : fmt == PFmt::MsFromNs ? "%.3f" : "%.2f";
      ImGui::SetNextItemWidth(hasRange ? inputW : -1);
      bool entered = ImGui::InputDouble("##i", &inp, 0, 0, ifmt, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
        double raw = fromDisp(fmt, inp);
        if (hasRange) raw = std::min(std::max(raw, lo), hi);
        send = makeVal(d->type, raw);
        app.setParam(id, send, true);
        have = false;
        deact = false;
      }
      app.setDragging(id, active);
      if (deact) {
        if (!have) {
          ParamValue pv;
          if (app.pending(id, pv)) { send = pv; have = true; }
        }
        if (have) app.setParam(id, send, true);
      } else if (have) {
        app.setParam(id, send, false);
      }
    }
  }
  ImGui::EndDisabled();
  ImGui::EndGroup();
  tip(tipText.c_str());
  ImGui::PopID();
  ImGui::Dummy(ImVec2(0, sc(3)));
}

const char* facingName(owc::Facing f) {
  switch (f) {
    case owc::Facing::Back: return "Traseira";
    case owc::Facing::Front: return "Frontal";
    case owc::Facing::External: return "Externa";
  }
  return "?";
}

void notConnectedNote(App& app) {
  if (app.sessionUp() && app.caps_valid) return;
  ImGui::PushStyleColor(ImGuiCol_Text, col::dim());
  ImGui::TextWrapped("Conecte um aparelho para habilitar estes controles. Eles são montados a partir das capacidades informadas pelo celular.");
  ImGui::PopStyleColor();
  ImGui::Spacing();
}

struct Res { int w, h, maxfps; };

void streamSelectors(App& app) {
  const owc::Capabilities& c = app.caps;
  const bool en = app.sessionUp() && app.caps_valid && !c.cameras.empty();
  ImGui::BeginDisabled(!en);

  // ---- camera
  ImGui::TextUnformatted("Câmera");
  ImGui::SameLine();
  ImGui::TextDisabled("(reinicia o stream)");
  int active = c.active_camera;
  {
    ParamValue pv;
    if (app.pending(ParamId::CameraIndex, pv)) active = static_cast<int>(pv.i);
  }
  std::string preview = "-";
  if (active >= 0 && active < static_cast<int>(c.cameras.size())) {
    const auto& ci = c.cameras[static_cast<size_t>(active)];
    preview = std::string(facingName(ci.facing)) + "  (id " + ci.id + ")";
  }
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##cam", preview.c_str())) {
    for (size_t i = 0; i < c.cameras.size(); ++i) {
      const auto& ci = c.cameras[i];
      char b[160];
      std::snprintf(b, sizeof b, "%zu - %s  (id %s, zoom até %.1f\xC3\x97%s)", i, facingName(ci.facing), ci.id.c_str(),
                    static_cast<double>(ci.max_zoom), ci.has_flash ? ", flash" : "");
      if (ImGui::Selectable(b, static_cast<int>(i) == active)) app.setParam(ParamId::CameraIndex, ParamValue::Int(static_cast<int32_t>(i)));
    }
    ImGui::EndCombo();
  }
  tip("Escolhe a câmera do aparelho (traseira, frontal, lentes extras).");

  // ---- resolution / fps
  const owc::CameraInfo* cam = (active >= 0 && active < static_cast<int>(c.cameras.size())) ? &c.cameras[static_cast<size_t>(active)] : nullptr;
  std::vector<Res> res;
  if (cam) {
    for (const auto& s : cam->sizes) {
      bool found = false;
      for (auto& r : res)
        if (r.w == s.width && r.h == s.height) { r.maxfps = std::max<int>(r.maxfps, s.max_fps); found = true; }
      if (!found) res.push_back({s.width, s.height, s.max_fps});
    }
    std::sort(res.begin(), res.end(), [](const Res& a, const Res& b) { return a.w * a.h > b.w * b.h; });
  }
  ParamValue vw = ParamValue::Int(0), vh = ParamValue::Int(0), vf = ParamValue::Int(0);
  {
    const ParamDesc* dw = findDesc(c, ParamId::VideoWidth);
    const ParamDesc* dh = findDesc(c, ParamId::VideoHeight);
    const ParamDesc* df = findDesc(c, ParamId::Fps);
    if (dw) vw = shownValue(app, *dw);
    if (dh) vh = shownValue(app, *dh);
    if (df) vf = shownValue(app, *df);
  }
  ImGui::Spacing();
  ImGui::TextUnformatted("Resolução");
  ImGui::SameLine();
  ImGui::TextDisabled("(reinicia o stream)");
  char rp[64];
  std::snprintf(rp, sizeof rp, "%d\xC3\x97%d", static_cast<int>(vw.i), static_cast<int>(vh.i));
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##res", rp)) {
    for (const auto& r : res) {
      char b[96];
      if (r.maxfps > 0) std::snprintf(b, sizeof b, "%d\xC3\x97%d   (até %d fps)", r.w, r.h, r.maxfps);
      else std::snprintf(b, sizeof b, "%d\xC3\x97%d", r.w, r.h);
      const bool sel = r.w == vw.i && r.h == vh.i;
      if (ImGui::Selectable(b, sel)) {
        app.setParam(ParamId::VideoWidth, ParamValue::Int(r.w));
        app.setParam(ParamId::VideoHeight, ParamValue::Int(r.h));
        if (r.maxfps > 0 && vf.i > r.maxfps) app.setParam(ParamId::Fps, ParamValue::Int(r.maxfps));
      }
    }
    ImGui::EndCombo();
  }
  tip("Resolução do vídeo. Maior = mais detalhe, porém mais bitrate e mais latência de codificação. Ao trocar, largura, altura e fps são enviados juntos.");

  int maxfps = 0;
  for (const auto& r : res)
    if (r.w == vw.i && r.h == vh.i) maxfps = r.maxfps;
  std::set<int> fpsSet;
  static const int common[] = {5, 10, 15, 24, 25, 30, 48, 50, 60, 90, 120, 240};
  for (int f : common)
    if (maxfps == 0 || f <= maxfps) fpsSet.insert(f);
  if (cam)
    for (const auto& fr : cam->fps_ranges)
      if (maxfps == 0 || fr.second <= maxfps) fpsSet.insert(fr.second);
  fpsSet.insert(static_cast<int>(vf.i));
  ImGui::Spacing();
  ImGui::TextUnformatted("Quadros por segundo");
  ImGui::SameLine();
  ImGui::TextDisabled("(reinicia o stream)");
  char fp[32];
  std::snprintf(fp, sizeof fp, "%d fps", static_cast<int>(vf.i));
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##fps", fp)) {
    for (int f : fpsSet) {
      if (f <= 0) continue;
      char b[32];
      std::snprintf(b, sizeof b, "%d fps", f);
      if (ImGui::Selectable(b, f == vf.i)) app.setParam(ParamId::Fps, ParamValue::Int(f));
    }
    ImGui::EndCombo();
  }
  tip(findMeta(ParamId::Fps)->tip);
  ImGui::EndDisabled();
}

}  // namespace

// ============================================================================
void tabCamera(App& app) {
  notConnectedNote(app);
  sectionTitle("Câmera e resolução");
  streamSelectors(app);

  sectionTitle("Controles da câmera");
  for (ParamId id : cameraGroup()) paramControl(app, id);

  // Unknown (future) params: render generically.
  std::vector<ParamId> extra;
  if (app.caps_valid && app.sessionUp()) {
    for (const auto& d : app.caps.params)
      if (!isSpecialParam(d.id)) extra.push_back(d.id);
  }
  if (!extra.empty()) {
    sectionTitle("Outros parâmetros do aparelho");
    for (ParamId id : extra) paramControl(app, id);
  }

  ImGui::Dummy(ImVec2(0, sc(6)));
  ImGui::BeginDisabled(!app.sessionUp());
  if (ImGui::Button("Resetar controles da câmera", ImVec2(-1, 0))) app.resetParams();
  ImGui::EndDisabled();
  tip("Volta os controles da câmera aos padrões do aparelho e esquece os valores salvos para ele.");
}

void tabEncoder(App& app) {
  notConnectedNote(app);
  ImGui::PushStyleColor(ImGuiCol_Text, col::dim());
  ImGui::TextWrapped("Tudo aqui troca latência por qualidade. Passe o mouse sobre cada item para ver o efeito. Os presets (aba Presets) ajustam vários itens de uma vez.");
  ImGui::PopStyleColor();
  sectionTitle("Codificação");
  for (ParamId id : {ParamId::VideoCodec, ParamId::Bitrate, ParamId::BitrateMode, ParamId::EncoderProfile,
                     ParamId::BFrames, ParamId::KeyframeIntervalMs})
    paramControl(app, id);
  sectionTitle("Qualidade (QP) e recuperação");
  for (ParamId id : {ParamId::QpMin, ParamId::QpMax, ParamId::IntraRefreshFrames}) paramControl(app, id);
  sectionTitle("Fila e rede");
  for (ParamId id : {ParamId::MaxSendQueueFrames, ParamId::LatencyDropPolicy, ParamId::AdaptiveBitrate, ParamId::FecPercent})
    paramControl(app, id);
  sectionTitle("Experimental");
  paramControl(app, ParamId::HighSpeedMode, true);
}

// ============================================================================
namespace {

bool sliderU32(const char* label, uint32_t& v, uint32_t lo, uint32_t hi, const char* fmt, bool commitOnRelease, bool& commit) {
  int iv = static_cast<int>(v);
  ImGui::SetNextItemWidth(-1);
  bool ch = ImGui::SliderInt(label, &iv, static_cast<int>(lo), static_cast<int>(hi), fmt, ImGuiSliderFlags_AlwaysClamp);
  if (ch) v = static_cast<uint32_t>(iv);
  if (commitOnRelease) { if (ImGui::IsItemDeactivatedAfterEdit()) commit = true; }
  else if (ch) commit = true;
  return ch;
}

void optLabel(const char* label, const char* tipText) {
  ImGui::TextUnformatted(label);
  tip(tipText);
}

}  // namespace

void tabPc(App& app) {
  owc::ClientOptions o = app.client.options();
  bool commit = false;
  const owc::ClientStats& st = app.stats;

  sectionTitle("Latência de apresentação");
  optLabel("Jitter buffer", "Buffer de apresentação. 0 = desligado: cada quadro é publicado assim que decodificado (menor latência, movimento pode engasgar em Wi-Fi ruim). Acima de 0 os quadros são segurados e liberados no ritmo da captura: latência + exatamente esse valor, mas absorve a variação da rede. Típico 30 a 150 ms. Efeito imediato.");
  {
    bool c2 = false;
    sliderU32("##jit", o.jitter_buffer_ms, 0, 400, o.jitter_buffer_ms == 0 ? "Desligado" : "%d ms", false, c2);
    commit |= c2;
  }
  if (o.jitter_buffer_ms > 0 && st.state == owc::ClientState::Streaming)
    ImGui::TextDisabled("Quadros retidos agora: %u", st.jitter_buffer_frames);

  sectionTitle("Decodificação");
  optLabel("Decoder", "Automático: começa em hardware (D3D11/DXVA), mede ms/quadro no stream ao vivo, testa o software e fica com o mais rápido. Hardware: só GPU. Software: CPU, mais carga porém muitas vezes 2 a 10 ms mais rápido em 1080p. Aplicado no próximo keyframe (pedido automaticamente).");
  {
    static const char* items[] = {"Automático (mede e escolhe)", "Hardware (GPU/DXVA)", "Software (CPU)"};
    int idx = static_cast<int>(o.decoder_mode);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##dec", &idx, items, 3)) { o.decoder_mode = static_cast<owc::DecoderMode>(idx); commit = true; }
  }
  if (st.has_format) {
    ImGui::TextColored(col::dim(), "Em uso:");
    ImGui::SameLine();
    ImGui::TextColored(st.decoder_hardware ? col::good() : col::accent(), "%s  [%s]",
                       st.decoder_name.empty() ? "-" : st.decoder_name.c_str(), st.decoder_hardware ? "HW" : "SW");
    if (!st.decoder_selection.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, col::dim());
      ImGui::TextWrapped("%s", st.decoder_selection.c_str());
      ImGui::PopStyleColor();
    }
    if (!st.decoder_error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, col::bad());
      ImGui::TextWrapped("%s", st.decoder_error.c_str());
      ImGui::PopStyleColor();
    }
  }
  if (ImGui::Checkbox("Decodificação de baixa latência", &o.low_latency_decode)) commit = true;
  tip("CODECAPI_AVLowLatencyMode / MF_LOW_LATENCY: o decoder entrega cada quadro assim que possível. Desligado, o decoder enche seu DPB inteiro (14-19 quadros, ~500 ms a 30 fps) sem ganho de qualidade. Mantenha ligado. O decoder é recriado no próximo keyframe.");
  if (ImGui::Checkbox("Corrigir SPS para baixa latência", &o.patch_sps)) commit = true;
  tip("Reescreve o SPS (H.264/HEVC) declarando max_num_reorder_frames=0 para que decoders de hardware não segurem quadros esperando um DPB que nunca enche. Só se aplica sem B-frames. Reduz de 0 a N quadros de latência; sem custo. Efeito no próximo keyframe.");

  sectionTitle("Rede e recuperação");
  optLabel("Intervalo mínimo entre pedidos de keyframe", "Intervalo mínimo entre pedidos de keyframe enviados após perdas ou erros do decoder. Menor = recuperação mais rápida, porém mais picos de bitrate (um keyframe é 5-20x maior que um quadro P). Efeito imediato.");
  {
    bool c2 = false;
    sliderU32("##kfi", o.keyframe_request_min_interval_ms, 50, 3000, "%d ms", false, c2);
    commit |= c2;
  }
  optLabel("Buffer de recepção UDP", "Tamanho do SO_RCVBUF do socket UDP. Muito pequeno: rajadas (keyframes) estouram o buffer e fragmentos são perdidos. Não adiciona latência. Efeito imediato.");
  {
    float mb = static_cast<float>(o.udp_recv_buffer_bytes) / (1024.0f * 1024.0f);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##rcv", &mb, 0.25f, 32.0f, "%.2f MB", ImGuiSliderFlags_AlwaysClamp)) {
      o.udp_recv_buffer_bytes = static_cast<uint32_t>(mb * 1024.0f * 1024.0f);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) commit = true;
  }
  optLabel("Quadros UDP pendentes (máx.)", "Quantos quadros UDP incompletos podem estar em voo antes de o mais antigo ser dado como perdido (1 a 32). Maior tolera reordenação na rede, porém demora mais para declarar perda e pedir keyframe. Reinicia o montador de quadros.");
  sliderU32("##pend", o.udp_max_pending_frames, 1, 32, "%d", true, commit);
  optLabel("Endereço local UDP", "Endereço local onde o socket de vídeo UDP é aberto (0.0.0.0 = todas as interfaces). Vale na próxima conexão.");
  {
    static char bind[64];
    static std::string last;
    if (last != o.udp_bind_address) { std::snprintf(bind, sizeof bind, "%s", o.udp_bind_address.c_str()); last = o.udp_bind_address; }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##bind", bind, sizeof bind, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
      o.udp_bind_address = bind;
      commit = true;
    }
  }

  sectionTitle("Imagem (rotação e espelho)");
  optLabel("Rotação", "Rotação horária aplicada no PC ao quadro decodificado, antes de publicar (webcam virtual, OBS e preview). Custa ~1-3 ms em 1080p para 90/270; 180 e espelho < 1 ms.");
  {
    static const char* items[] = {"0°", "90°", "180°", "270°"};
    int idx = ((o.transform.rotation_deg % 360 + 360) % 360) / 90;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##rot", &idx, items, 4)) { o.transform.rotation_deg = idx * 90; commit = true; }
  }
  if (ImGui::Checkbox("Espelhar horizontalmente", &o.transform.mirror)) commit = true;
  tip("Espelha a imagem final (já rotacionada) na horizontal.");
  if (ImGui::Checkbox("Combinar com a dica de rotação do celular", &o.transform.apply_phone_hints)) commit = true;
  tip("Soma a rotação/espelho informados pelo celular (orientação do sensor, câmera frontal) à sua escolha. Desmarque para ver o quadro cru do sensor.");

  sectionTitle("Conexão");
  if (ImGui::Checkbox("Reconectar automaticamente", &o.auto_reconnect)) commit = true;
  tip("Reconecta sozinho após queda ou falha (e reenvia o início do stream se estava transmitindo). Efeito imediato.");
  optLabel("Atraso para reconectar", "Espera entre tentativas de reconexão.");
  { bool c2 = false; sliderU32("##rdelay", o.reconnect_delay_ms, 200, 10000, "%d ms", true, c2); commit |= c2; }
  if (ImGui::Checkbox("Iniciar o stream ao conectar", &o.auto_start_stream)) commit = true;
  tip("Envia StartStream assim que a sessão sobe. Vale na próxima conexão.");
  optLabel("Timeout de conexão TCP", "Tempo máximo para o TCP conectar. Vale na próxima conexão.");
  { bool c2 = false; sliderU32("##ctmo", o.connect_timeout_ms, 500, 20000, "%d ms", true, c2); commit |= c2; }
  optLabel("Timeout de handshake", "Espera pelo HelloAck por tentativa. Vale na próxima conexão.");
  { bool c2 = false; sliderU32("##htmo", o.handshake_timeout_ms, 300, 10000, "%d ms", true, c2); commit |= c2; }
  optLabel("Tentativas de handshake", "Quantas vezes reconectar e reenviar o Hello antes de desistir. Por USB, o adb aceita a conexão mesmo que o app não esteja escutando.");
  { bool c2 = false; sliderU32("##hatt", o.handshake_attempts, 1, 10, "%d", true, c2); commit |= c2; }
  optLabel("Timeout de inatividade", "Desconecta se o celular ficar mudo por este tempo. Efeito imediato.");
  { bool c2 = false; sliderU32("##idle", o.idle_timeout_ms, 1000, 30000, "%d ms", true, c2); commit |= c2; }

  sectionTitle("Saída");
  if (ImGui::Checkbox("Publicar na webcam virtual / OBS", &o.publish_to_shm)) commit = true;
  tip("Publica os quadros decodificados no barramento de memória compartilhada lido pela webcam virtual \"OWC Webcam\" e pelo plugin do OBS. Desligado, só o preview deste app funciona.");
  ImGui::BeginDisabled();
  bool keep = o.keep_latest_frame;
  ImGui::Checkbox("Manter cópia do último quadro (interno)", &keep);
  ImGui::EndDisabled();
  tip("Cópia extra do último quadro para copyLatestFrame(). Este app recebe os quadros por callback, então fica desligado (economiza ~0,3 ms por quadro).");

  if (commit) app.applyOptions(o);
}

// ============================================================================
void tabDevice(App& app) {
  notConnectedNote(app);
  const owc::ClientStats& st = app.stats;
  const bool up = app.sessionUp();

  sectionTitle("Identificação");
  static char name[64];
  static std::string lastName;
  if (app.dev_valid && lastName != app.dev.device_name) {
    std::snprintf(name, sizeof name, "%s", app.dev.device_name.c_str());
    lastName = app.dev.device_name;
  }
  ImGui::TextUnformatted("Nome do aparelho");
  ImGui::SetNextItemWidth(-(sc(90) + ImGui::GetStyle().ItemSpacing.x));
  ImGui::BeginDisabled(!up);
  ImGui::InputText("##devname", name, sizeof name);
  ImGui::SameLine();
  if (ImGui::Button("Aplicar", ImVec2(sc(90), 0)) && name[0]) app.client.setDeviceName(name);
  ImGui::EndDisabled();
  tip("Nome exibido na descoberta Wi-Fi e na lista de dispositivos. Salvo no celular.");
  if (app.dev_valid && up) {
    ImGui::TextColored(col::dim(), "%s  -  Android API %u  -  sessão %u", app.dev.model.c_str(), app.dev.android_api,
                       app.dev.session_id);
  }

  sectionTitle("Controles rápidos");
  paramControl(app, ParamId::ScreenMode, true, "Tela preta no celular");
  paramControl(app, ParamId::Torch, false, "Lanterna (flash)");
  ImGui::BeginDisabled(!up);
  if (ImGui::Button("Reiniciar stream")) {
    app.client.stopStream();
    app.client.startStream();
  }
  ImGui::SameLine();
  if (ImGui::Button("Pedir keyframe")) app.client.requestKeyframe();
  ImGui::EndDisabled();

  sectionTitle("Bateria e temperatura");
  if (up && st.has_phone_stats) {
    const owc::Stats& p = st.phone;
    ImGui::Text("Bateria:");
    ImGui::SameLine();
    ImGui::TextColored(p.battery_pct >= 0 && p.battery_pct < 15 && !p.charging ? col::bad() : col::good(), "%d%% %s",
                       p.battery_pct, p.charging ? "(carregando)" : "");
    ImGui::Text("Temperatura da bateria:");
    ImGui::SameLine();
    ImGui::TextColored(p.battery_temp_c >= 45 ? col::bad() : p.battery_temp_c >= 40 ? col::warn() : col::good(), "%.1f C",
                       static_cast<double>(p.battery_temp_c));
    ImGui::Text("Estado térmico:");
    ImGui::SameLine();
    ImGui::TextColored(thermalColor(p.thermal_status), "%s (%d)", thermalName(p.thermal_status), p.thermal_status);
    if (p.thermal_status >= 3) {
      ImGui::PushStyleColor(ImGuiCol_Text, col::warn());
      ImGui::TextWrapped("O celular está superaquecendo e pode reduzir o desempenho. Reduza resolução/bitrate, ligue a tela preta e ventile o aparelho.");
      ImGui::PopStyleColor();
    }
  } else {
    ImGui::TextDisabled("Sem dados do celular.");
  }

  sectionTitle("Estatísticas do celular");
  if (up && st.has_phone_stats) {
    const owc::Stats& p = st.phone;
    if (ImGui::BeginTable("phonestats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      auto row = [](const char* k, const char* fmt, double v) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(col::dim(), "%s", k);
        ImGui::TableNextColumn();
        ImGui::Text(fmt, v);
      };
      row("FPS da câmera", "%.1f", static_cast<double>(p.camera_fps));
      row("FPS de codificação", "%.1f", static_cast<double>(p.encode_fps));
      row("FPS de envio", "%.1f", static_cast<double>(p.send_fps));
      row("Latência de codificação", "%.1f ms", static_cast<double>(p.encode_latency_ms));
      row("Latência do ISP", "%.1f ms", static_cast<double>(p.isp_latency_ms));
      row("Bitrate medido", "%.2f Mbps", p.bitrate_bps / 1e6);
      row("Bitrate alvo", "%.2f Mbps", p.target_bitrate_bps / 1e6);
      row("Quadros descartados", "%.0f", static_cast<double>(p.dropped_frames));
      row("Fila de envio", "%.0f quadros", static_cast<double>(p.send_queue));
      row("FEC aplicado", "%.0f %%", static_cast<double>(p.fec_percent));
      ImGui::EndTable();
    }
  } else {
    ImGui::TextDisabled("Sem dados do celular.");
  }

  sectionTitle("Recepção no PC");
  if (up) {
    if (ImGui::BeginTable("pcstats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      auto row = [](const char* k, const char* fmt, double v) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(col::dim(), "%s", k);
        ImGui::TableNextColumn();
        ImGui::Text(fmt, v);
      };
      row("Quadros recebidos/s", "%.1f", st.rx_fps);
      row("Bitrate recebido", "%.2f Mbps", st.rx_bitrate_bps / 1e6);
      row("Quadros perdidos", "%.0f", static_cast<double>(st.frames_lost));
      row("Recuperados por FEC", "%.0f", static_cast<double>(st.frames_recovered_fec));
      row("Jitter", "%.1f ms", st.jitter_ms);
      row("Decodificação", "%.1f ms", st.decode_ms_avg);
      row("Pedidos de keyframe", "%.0f", static_cast<double>(st.keyframe_requests));
      row("Reconexões", "%.0f", static_cast<double>(st.reconnect_count));
      ImGui::EndTable();
    }
  }
}

}  // namespace ui
}  // namespace owcapp
