#include "ui.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "platform.h"
#include "renderer.h"
#include "ui_internal.h"

namespace owcapp {
namespace ui {

float S = 1.0f;
ImFont* fontBold = nullptr;
ImFont* fontTitle = nullptr;

void tip(const char* text) {
  if (!text || !*text) return;
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

void sectionTitle(const char* text) {
  ImGui::Dummy(ImVec2(0, sc(4)));
  ImGui::PushFont(fontBold);
  ImGui::TextColored(col::accent(), "%s", text);
  ImGui::PopFont();
  ImGui::Separator();
  ImGui::Dummy(ImVec2(0, sc(2)));
}

bool beginCard(const char* id, const ImVec4* bg) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, bg ? *bg : col::card());
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, sc(8));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sc(12), sc(10)));
  bool r = ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                             ImGuiWindowFlags_NoScrollbar);
  return r;
}

void endCard() {
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, sc(2)));
}

void flowItem(float width) {
  const float avail = ImGui::GetContentRegionAvail().x;
  const float x = ImGui::GetCursorPosX();
  (void)x;
  // Continue on the same line only if the item fits.
  if (ImGui::GetCursorPosX() > ImGui::GetStyle().WindowPadding.x + 1.0f) {
    if (width + ImGui::GetStyle().ItemSpacing.x * 2 < avail) ImGui::SameLine(0, sc(18));
  }
}

std::string escapePct(const std::string& s) {
  std::string o;
  for (char c : s) {
    o += c;
    if (c == '%') o += '%';
  }
  return o;
}

bool previewLive(App& app, Renderer& r) {
  return r.previewSrv() && r.previewWidth() > 0 && app.frames_shown > 0 && app.stats.state == owc::ClientState::Streaming &&
         !(app.rec_phase == RecPhase::Recording && !app.rec_preview_available) && uiPreviewHasVideo();
}

const char* thermalName(int st) {
  switch (st) {
    case 0: return "normal";
    case 1: return "leve";
    case 2: return "moderado";
    case 3: return "severo";
    case 4: return "crítico";
    case 5: return "emergência";
    case 6: return "desligando";
    default: return "?";
  }
}

ImVec4 thermalColor(int st) {
  if (st <= 0) return col::good();
  if (st == 1) return ImVec4(0.75f, 0.85f, 0.45f, 1.0f);
  if (st == 2) return col::warn();
  return col::bad();
}

}  // namespace ui

// ============================================================================
namespace {

using namespace ui;

struct TabDef {
  const char* key;
  const char* title;
};
const TabDef kTabs[] = {{"presets", "Presets"},   {"camera", "Câmera"}, {"color", "Cor / LUT"}, {"encoder", "Encoder/Stream"},
                        {"pc", "PC"},             {"device", "Dispositivo"}, {"record", "Gravação"}, {"outputs", "Saídas"},
                        {"logs", "Logs"}};
int g_tab = 0;
constexpr int kColorTab = 2;
bool g_openHelp = false;

char g_manual[96] = "";
bool g_manualInit = false;

struct FocusMark {
  ImVec2 pos{0, 0};  // screen pos
  ULONGLONG t = 0;
};
FocusMark g_focus;
uint64_t g_lastShown = 0;
ULONGLONG g_lastShownTick = 0;
bool g_hasVideo = false;

ImVec4 mul(ImVec4 c, float k) { return ImVec4(c.x * k, c.y * k, c.z * k, c.w); }

bool primaryButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
  ImGui::PushStyleColor(ImGuiCol_Button, mul(col::accent(), 0.85f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::accent());
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, mul(col::accent(), 0.7f));
  bool r = ImGui::Button(label, size);
  ImGui::PopStyleColor(3);
  return r;
}

// "ip:port" -> host/port. Returns false if empty.
bool parseAddr(const char* s, std::string& host, int& port) {
  std::string t = s;
  while (!t.empty() && t.back() == ' ') t.pop_back();
  while (!t.empty() && t.front() == ' ') t.erase(t.begin());
  if (t.empty()) return false;
  port = owc::kDefaultControlPort;
  size_t k = t.rfind(':');
  if (k != std::string::npos) {
    int p = std::atoi(t.c_str() + k + 1);
    if (p > 0 && p < 65536) port = p;
    t = t.substr(0, k);
  }
  host = t;
  return !host.empty();
}

bool connectedTo(App& app, const std::string& ip) {
  if (!app.live()) return false;
  const std::string& peer = app.stats.peer;
  return peer.rfind(ip + ":", 0) == 0;
}

void drawUsbFlow(App& app) {
  auto phase = app.usbPhase();
  if (phase == UsbPhase::Idle) return;
  auto lines = app.usbLines();
  ImGui::PushStyleColor(ImGuiCol_Text, col::dim());
  ImGui::TextUnformatted("Fluxo USB");
  ImGui::PopStyleColor();
  if (!beginCard("usbflow")) { endCard(); return; }
  size_t from = lines.size() > 9 ? lines.size() - 9 : 0;
  for (size_t i = from; i < lines.size(); ++i) {
    const bool err = lines[i].rfind("ERRO", 0) == 0;
    const bool warn = lines[i].rfind("aviso", 0) == 0;
    ImGui::PushStyleColor(ImGuiCol_Text, err ? col::bad() : warn ? col::warn() : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    ImGui::TextWrapped("%s", lines[i].c_str());
    ImGui::PopStyleColor();
  }
  if (phase == UsbPhase::AwaitInstall) {
    ImGui::Spacing();
    std::string apk = app.usbApkPath();
    ImGui::TextColored(col::dim(), "APK: %s", apk.c_str());
    if (primaryButton("Instalar APK")) app.usbAnswerInstall(true);
    ImGui::SameLine();
    if (ImGui::Button("Cancelar")) app.usbAnswerInstall(false);
  } else if (phase == UsbPhase::Running) {
    ImGui::Spacing();
    if (ImGui::Button("Cancelar")) app.usbCancel();
  } else if (phase == UsbPhase::Done) {
    ImGui::TextColored(col::good(), "Concluído.");
  }
  endCard();
}

void helpStep(const char* n, const char* text) {
  ImGui::TextColored(col::accent(), "%s", n);
  ImGui::SameLine();
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
}

void drawHelp(App& app) {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(sc(620), vp->Size.x - sc(40)), 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sc(18), sc(16)));
  if (ImGui::BeginPopupModal("Como conectar##help", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
    if (ImGui::BeginTabBar("##helptabs")) {
      if (ImGui::BeginTabItem("Cabo USB (recomendado, sem modo desenvolvedor)")) {
        ImGui::Spacing();
        helpStep("1.", "Ligue o cabo USB entre o celular e o PC.");
        helpStep("2.", "No app OWC do celular, toque em \"Abrir Ancoragem USB\" e ative a ancoragem (em Samsung: \"Ancoragem via USB\"; em outros Android: \"Ancoragem USB\").");
        helpStep("3.", "O celular aparece aqui em Dispositivos. Clique em Conectar.");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(col::dim(), "Não precisa de adb nem de modo desenvolvedor. O celular vira um adaptador de rede (RNDIS) com baixa latência.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::TextColored(col::warn(), "Não aparece?");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("O celular mostra \"Ancoragem ativa\" mas o PC não o encontra? Em computadores de empresa, políticas de segurança costumam bloquear adaptadores de rede USB. Use o Wi-Fi (mesma rede) ou, se tiver modo desenvolvedor, \"USB avançado (ADB)\".");
        ImGui::PopTextWrapPos();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Wi-Fi")) {
        ImGui::Spacing();
        helpStep("1.", "Conecte o celular e o PC na mesma rede Wi-Fi (de preferência 5 GHz).");
        helpStep("2.", "Abra o app OWC no celular: ele aparece em Dispositivos.");
        helpStep("3.", "Se não aparecer, libere o firewall do Windows (aba Saídas > Liberar no firewall) ou use o endereço manual.");
        if (ImGui::Button("Ir para Saídas")) { uiSelectTab("outputs"); ImGui::CloseCurrentPopup(); }
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("USB avançado (ADB)")) {
        ImGui::Spacing();
        helpStep("1.", "Para desenvolvedores: ative a depuração USB no celular e autorize este PC.");
        helpStep("2.", "Abra \"USB avançado (ADB)\" na barra lateral e clique em \"Conectar via USB\".");
        helpStep("3.", "O OWC instala o app (se houver APK), concede permissões, abre o app e conecta por TCP via adb forward.");
        ImGui::TextColored(col::dim(), "adb: %s", app.adbAvailable() ? "encontrado" : "não encontrado");
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::Dummy(ImVec2(0, sc(8)));
    if (ImGui::Button("Fechar", ImVec2(sc(110), 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
}

void drawSidebar(App& app) {
  ImGui::PushFont(fontTitle);
  ImGui::TextColored(col::accent(), "OWC");
  ImGui::PopFont();
  ImGui::TextColored(col::dim(), "Webcam de baixa latência");
  ImGui::Dummy(ImVec2(0, sc(6)));

  const bool live = app.live();
  if (live) {
    if (ImGui::Button("Desconectar", ImVec2(-1, 0))) app.disconnect();
    ImGui::Dummy(ImVec2(0, sc(4)));
  }

  sectionTitle("Dispositivos");

  // ---- Wi-Fi
  for (const auto& ta : app.tether_adapters) {
    if (!ta.may_carry_internet) continue;
    ImGui::PushStyleColor(ImGuiCol_Text, col::warn());
    ImGui::TextWrapped("O Windows pode estar usando a internet móvel do celular. Mantenha o Wi-Fi do celular ligado ou desligue os dados móveis.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    break;
  }
  ImGui::TextColored(col::dim(), "Cabo USB e Wi-Fi (descoberta automática)");
  if (app.wifi_devices.empty()) {
    ImGui::TextWrapped("Nenhum celular encontrado. Ligue o cabo e use \"Abrir Ancoragem USB\" no app OWC, ou abra o app na mesma rede Wi-Fi. Veja \"Como conectar\".");
  }
  for (const auto& d : app.wifi_devices) {
    std::string id = "wifi_" + d.key();
    if (!beginCard(id.c_str())) { endCard(); continue; }
    ImGui::PushFont(fontBold);
    ImGui::TextUnformatted(d.name.empty() ? "(sem nome)" : d.name.c_str());
    ImGui::PopFont();
    if (d.busy) {
      ImGui::SameLine();
      ImGui::TextColored(col::warn(), "ocupado");
      tip("Este celular já está conectado a outro PC.");
    }
    const bool tether = d.link == owc::LinkType::UsbTether;
    if (tether) ImGui::TextColored(col::accent(), "[USB] Cabo USB (ancoragem)");
    else if (d.link == owc::LinkType::Ethernet) ImGui::TextColored(col::dim(), "Rede cabeada (Ethernet)");
    else if (d.link == owc::LinkType::Wifi) ImGui::TextColored(col::dim(), "Wi-Fi");
    ImGui::TextColored(col::dim(), "%s  -  %s:%u", d.model.c_str(), d.ip.c_str(), d.control_port);
    const bool here = connectedTo(app, d.ip);
    if (here) ImGui::TextColored(col::good(), "Conectado (%s)", app.stats.transport == owc::VideoTransport::Udp ? "UDP" : "TCP");
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    const std::string label = d.name.empty() ? d.ip : d.name;
    const char* tcpTip = "TCP: entrega confiável, bom para Wi-Fi estável. Latência um pouco maior se houver perdas.";
    const char* udpTip = "UDP: menor latência, tolera perdas com FEC e keyframes. Recomendado em cabo USB (ancoragem) e Wi-Fi 5 GHz.";
    // Over the tether (a real network link) UDP is the default (primary) choice.
    auto tcpBtn = [&](bool prim) {
      if (prim ? primaryButton("Conectar TCP", ImVec2(w, 0)) : ImGui::Button("Conectar TCP", ImVec2(w, 0)))
        app.connectNet(d.ip, d.control_port, owc::VideoTransport::Tcp, label);
      tip(tcpTip);
    };
    auto udpBtn = [&](bool prim) {
      if (prim ? primaryButton("Conectar UDP", ImVec2(w, 0)) : ImGui::Button("Conectar UDP", ImVec2(w, 0)))
        app.connectNet(d.ip, d.control_port, owc::VideoTransport::Udp, label);
      tip(udpTip);
    };
    if (tether) { udpBtn(true); ImGui::SameLine(); tcpBtn(false); }
    else { tcpBtn(true); ImGui::SameLine(); udpBtn(false); }
    endCard();
  }

  // ---- USB
  ImGui::Dummy(ImVec2(0, sc(6)));
  auto devs = app.adbDevices();
  std::string aerr = app.adbError();
  static bool adbOpenedOnce = false;
  if (!devs.empty() && !adbOpenedOnce) {
    adbOpenedOnce = true;
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);  // first adb device seen: expand once
  } else {
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);   // collapsed by default without adb devices
  }
  const bool adbOpen = ImGui::CollapsingHeader("USB avançado (ADB)");
  if (adbOpen && !aerr.empty() && devs.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::warn());
    ImGui::TextWrapped("%s", aerr.c_str());
    ImGui::PopStyleColor();
  } else if (adbOpen && devs.empty()) {
    ImGui::TextWrapped("Nenhum aparelho USB. Para desenvolvedores: conecte o cabo e ative a depuração USB.");
  }
  const auto usbPhase = app.usbPhase();
  const bool usbBusy = usbPhase == UsbPhase::Running || usbPhase == UsbPhase::AwaitInstall;
  for (const auto& d : devs) {
    if (!adbOpen) break;
    std::string id = "usb_" + d.serial;
    if (!beginCard(id.c_str())) { endCard(); continue; }
    std::string model = d.model;
    std::replace(model.begin(), model.end(), '_', ' ');
    ImGui::PushFont(fontBold);
    ImGui::TextUnformatted(model.empty() ? d.serial.c_str() : model.c_str());
    ImGui::PopFont();
    ImGui::TextColored(col::dim(), "%s  -  %s", d.serial.c_str(), d.state.c_str());
    if (!d.online()) {
      ImGui::PushStyleColor(ImGuiCol_Text, col::warn());
      ImGui::TextWrapped("%s", d.state == "unauthorized" ? "Aceite a autorização de depuração USB na tela do celular."
                                                          : "Aparelho indisponível no momento.");
      ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(!d.online() || usbBusy);
    if (primaryButton("Conectar via USB", ImVec2(-1, 0))) app.connectUsb(d.serial, model.empty() ? d.serial : model);
    ImGui::EndDisabled();
    tip("Instala o app (se preciso), concede permissões, abre o app, cria o redirecionamento e conecta por TCP via cabo.");
    endCard();
  }
  drawUsbFlow(app);

  // ---- help
  ImGui::Dummy(ImVec2(0, sc(6)));
  if (ImGui::Button("Como conectar", ImVec2(-1, 0)) || g_openHelp) ImGui::OpenPopup("Como conectar##help");
  g_openHelp = false;
  drawHelp(app);

  // ---- manual
  ImGui::Dummy(ImVec2(0, sc(6)));
  ImGui::TextColored(col::dim(), "Endereço manual");
  if (!g_manualInit) {
    std::snprintf(g_manual, sizeof g_manual, "%s", app.settings.manual_addr.c_str());
    g_manualInit = true;
  }
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputTextWithHint("##manual", "IP:porta  (ex.: 192.168.0.20:47800)", g_manual, sizeof g_manual)) {
    app.settings.manual_addr = g_manual;
    app.markDirty();
  }
  {
    std::string host;
    int port = 0;
    const bool ok = parseAddr(g_manual, host, port);
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::BeginDisabled(!ok);
    if (ImGui::Button("TCP", ImVec2(w, 0))) app.connectNet(host, port, owc::VideoTransport::Tcp, host);
    ImGui::SameLine();
    if (ImGui::Button("UDP", ImVec2(w, 0))) app.connectNet(host, port, owc::VideoTransport::Udp, host);
    ImGui::EndDisabled();
  }
}

// ---------------------------------------------------------------------------
// Tap-to-focus mapping. The preview shows the frame AFTER the PC transform:
//   shown = mirror_h( rotate_cw( sensorFrame, rot ) )
// The phone expects coordinates in the sensor-oriented frame, so we invert:
//   1. undo the horizontal mirror:  u = 1 - u
//   2. undo the clockwise rotation (rot = frame.rotation_applied):
//        0  : (x, y) = (u, v)
//        90 : (x, y) = (v, 1 - u)
//        180: (x, y) = (1 - u, 1 - v)
//        270: (x, y) = (1 - v, u)
// where (u, v) is the tap normalised in the displayed image (origin top-left).
void mapTapToSensor(float u, float v, int rot, bool mirrored, float& x, float& y) {
  if (mirrored) u = 1.0f - u;
  rot = ((rot % 360) + 360) % 360;
  switch (rot) {
    case 90: x = v; y = 1.0f - u; break;
    case 180: x = 1.0f - u; y = 1.0f - v; break;
    case 270: x = 1.0f - v; y = u; break;
    default: x = u; y = v; break;
  }
  x = std::min(1.0f, std::max(0.0f, x));
  y = std::min(1.0f, std::max(0.0f, y));
}

void drawHud(App& app, ImDrawList* dl, ImVec2 imgMin, ImVec2 imgMax) {
  const owc::ClientStats& st = app.stats;
  char l[6][160];
  int n = 0;
  std::snprintf(l[n++], 160, "%ux%u  %.0f fps rx  %.0f fps tela  %s", st.output_width, st.output_height, st.rx_fps,
                app.shown_fps, st.transport == owc::VideoTransport::Udp ? "UDP" : "TCP");
  if (st.clock_synced)
    std::snprintf(l[n++], 160, "Latência %.0f ms (p95 %.0f)  tela %.0f ms", st.capture_to_publish_ms_avg,
                  st.capture_to_publish_ms_p95, app.shown_lat_avg);
  else
    std::snprintf(l[n++], 160, "Latência: sincronizando relógio...");
  std::snprintf(l[n++], 160, "%.2f Mbps  perdidos %llu  FEC %llu", st.rx_bitrate_bps / 1e6,
                static_cast<unsigned long long>(st.frames_lost), static_cast<unsigned long long>(st.frames_recovered_fec));
  std::snprintf(l[n++], 160, "Decoder: %s", st.decoder_hardware ? "hardware (DXVA)" : "software (CPU)");
  ImFont* font = ImGui::GetFont();
  const float fs = ImGui::GetFontSize() * 0.9f;
  float wmax = 0;
  for (int i = 0; i < n; ++i) wmax = std::max(wmax, font->CalcTextSizeA(fs, 1e9f, 0.0f, l[i]).x);
  const float pad = sc(8);
  const float lh = fs + sc(4);
  ImVec2 p0(imgMin.x + sc(10), imgMin.y + sc(10));
  ImVec2 p1(p0.x + wmax + pad * 2, p0.y + lh * static_cast<float>(n) + pad * 2 - sc(2));
  dl->AddRectFilled(p0, p1, IM_COL32(8, 10, 14, 175), sc(6));
  for (int i = 0; i < n; ++i)
    dl->AddText(font, fs, ImVec2(p0.x + pad, p0.y + pad + lh * static_cast<float>(i)), IM_COL32(230, 236, 245, 255), l[i]);

  // latency graph (last 10 s)
  const float gw = sc(230), gh = sc(64);
  ImVec2 g0(imgMin.x + sc(10), imgMax.y - gh - sc(26));
  if (g0.y < p1.y + sc(8)) return;
  ImVec2 g1(g0.x + gw, g0.y + gh);
  dl->AddRectFilled(ImVec2(g0.x - sc(6), g0.y - sc(18)), ImVec2(g1.x + sc(6), g1.y + sc(6)), IM_COL32(8, 10, 14, 175),
                    sc(6));
  dl->AddText(ImVec2(g0.x, g0.y - sc(16)), IM_COL32(170, 180, 195, 255), "Latência (10 s)");
  float peak = 20;
  {
    std::vector<float> vals;
    for (const auto& p : app.graph) {
      if (p.e2e > 0.01f) vals.push_back(p.e2e);
      if (p.shown > 0.01f) vals.push_back(p.shown);
    }
    if (!vals.empty()) {
      std::sort(vals.begin(), vals.end());
      peak = std::max(peak, vals[std::min(vals.size() - 1, static_cast<size_t>(static_cast<double>(vals.size()) * 0.98))]);
    }
    peak *= 1.15f;
  }
  auto plot = [&](bool shown, ImU32 color) {
    ImVec2 prev(0, 0);
    bool have = false;
    const double tn = static_cast<double>(GetTickCount64()) / 1000.0;
    for (const auto& p : app.graph) {
      const float v = shown ? p.shown : p.e2e;
      if (v <= 0.01f) { have = false; continue; }
      ImVec2 q(g1.x - static_cast<float>((tn - p.t) / 10.0) * gw, g1.y - std::min(v / peak, 1.0f) * gh);
      if (q.x < g0.x) { have = false; continue; }
      if (have) dl->AddLine(prev, q, color, sc(1.6f));
      prev = q;
      have = true;
    }
  };
  plot(false, IM_COL32(80, 160, 255, 255));
  plot(true, IM_COL32(255, 170, 60, 255));
  char pk[32];
  std::snprintf(pk, sizeof pk, "%.0f ms", peak / 1.15f);
  dl->AddText(ImVec2(g1.x - ImGui::CalcTextSize(pk).x, g0.y - sc(16)), IM_COL32(170, 180, 195, 255), pk);
  dl->AddRect(g0, g1, IM_COL32(90, 100, 115, 120));
}

// Once per UI frame (whatever the layout): is real video being shown?
void updateVideoState(App& app, Renderer& r) {
  if (app.frames_shown != g_lastShown) {
    g_lastShown = app.frames_shown;
    g_lastShownTick = GetTickCount64();
  }
  const owc::ClientState cs = app.stats.state;
  g_hasVideo = r.previewSrv() && r.previewWidth() > 0 && g_lastShown > 0 &&
               GetTickCount64() - g_lastShownTick < 2500 && (cs == owc::ClientState::Streaming);
}

void drawPreview(App& app, Renderer& r, ImVec2 size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 base = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(base, ImVec2(base.x + size.x, base.y + size.y), IM_COL32(10, 11, 14, 255), sc(10));

  if (app.rec_phase == RecPhase::Recording && !app.rec_preview_available) {
    const char* l1 = "Gravando em HEVC";
    const char* l2 = "preview indisponível neste PC (instale HEVC Video Extensions para ver)";
    ImGui::PushFont(fontTitle);
    ImVec2 s1 = ImGui::CalcTextSize(l1);
    dl->AddText(ImVec2(base.x + (size.x - s1.x) / 2, base.y + size.y / 2 - s1.y - sc(4)), IM_COL32(235, 90, 90, 255), l1);
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    ImVec2 s2 = ImGui::CalcTextSize(l2, nullptr, false, size.x - sc(20));
    dl->AddText(nullptr, 0.0f, ImVec2(base.x + std::max(sc(10), (size.x - s2.x) / 2), base.y + size.y / 2 + sc(8)),
                IM_COL32(170, 178, 190, 255), l2, nullptr, size.x - sc(20));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(size);
    return;
  }

  if (!g_hasVideo) {
    const char* l1 = "Sem vídeo";
    const char* l2 = app.stats.state == owc::ClientState::Pairing
                         ? "Pareamento: digite o código de 6 dígitos mostrado no celular"
                     : app.live() ? "Aguardando o stream do celular..."
                                  : "Selecione um dispositivo na barra lateral para conectar";
    ImGui::PushFont(fontTitle);
    ImVec2 s1 = ImGui::CalcTextSize(l1);
    dl->AddText(ImVec2(base.x + (size.x - s1.x) / 2, base.y + size.y / 2 - s1.y - sc(4)), IM_COL32(120, 130, 145, 255), l1);
    ImGui::PopFont();
    ImVec2 s2 = ImGui::CalcTextSize(l2);
    dl->AddText(ImVec2(base.x + (size.x - s2.x) / 2, base.y + size.y / 2 + sc(8)), IM_COL32(120, 130, 145, 255), l2);
    ImGui::Dummy(size);
    return;
  }

  const float ar = static_cast<float>(r.previewWidth()) / static_cast<float>(r.previewHeight());
  const float pad = sc(6);
  float aw = size.x - pad * 2, ah = size.y - pad * 2;
  float iw = aw, ih = aw / ar;
  if (ih > ah) { ih = ah; iw = ah * ar; }
  ImVec2 imin(base.x + (size.x - iw) / 2, base.y + (size.y - ih) / 2);
  ImVec2 imax(imin.x + iw, imin.y + ih);
  dl->AddImageRounded(reinterpret_cast<ImTextureID>(r.previewSrv()), imin, imax, ImVec2(0, 0), ImVec2(1, 1),
                      IM_COL32_WHITE, sc(6));

  ImGui::SetCursorScreenPos(imin);
  ImGui::InvisibleButton("##preview", ImVec2(iw, ih));
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);  // click = tap-to-focus
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    ImVec2 m = ImGui::GetIO().MousePos;
    const float u = (m.x - imin.x) / iw, v = (m.y - imin.y) / ih;
    float x, y;
    mapTapToSensor(u, v, app.last_rotation, app.last_mirrored, x, y);
    app.client.focusAt(x, y);
    g_focus.pos = m;
    g_focus.t = GetTickCount64();
  }
  if (g_focus.t) {
    const float age = static_cast<float>(GetTickCount64() - g_focus.t) / 700.0f;
    if (age < 1.0f) {
      const float rad = sc(26) * (1.4f - 0.4f * age);
      dl->AddCircle(g_focus.pos, rad, IM_COL32(255, 255, 255, static_cast<int>(230 * (1.0f - age))), 0, sc(2));
    }
  }
  if (app.settings.hud) drawHud(app, dl, imin, imax);
  ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + size.y));
}

void statusCell(const char* label, const char* value, const ImVec4& color) {
  ImGui::TableNextColumn();
  ImGui::TextColored(col::dim(), "%s", label);
  ImGui::SameLine(0, sc(6));
  ImGui::TextColored(color, "%s", value);
}

void drawStatusBar(App& app) {
  const owc::ClientStats& st = app.stats;
  const ImVec4 txt = ImGui::GetStyleColorVec4(ImGuiCol_Text);
  ImVec4 stateCol = st.state == owc::ClientState::Streaming ? col::good()
                    : (st.state == owc::ClientState::Failed) ? col::bad()
                    : (st.state == owc::ClientState::Idle) ? col::dim() : col::warn();
  const bool up = app.sessionUp();
  char b[64];
  if (!ImGui::BeginTable("##st", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) return;
  statusCell("Estado", stateText(st.state), stateCol);
  statusCell("Transporte", up ? (st.transport == owc::VideoTransport::Udp ? "UDP" : "TCP") : "-", txt);
  if (up && st.clock_synced) std::snprintf(b, sizeof b, "%.1f ms", st.rtt_ms); else std::snprintf(b, sizeof b, "-");
  statusCell("RTT", b, txt);
  if (up && st.clock_synced) {
    // phone clock (uptime) - PC clock: usually huge, so scale the unit
    const double ms = static_cast<double>(st.clock_offset_us) / 1000.0;
    if (std::fabs(ms) < 1000.0) std::snprintf(b, sizeof b, "%+.1f ms", ms);
    else if (std::fabs(ms) < 3.6e6) std::snprintf(b, sizeof b, "%+.1f s", ms / 1000.0);
    else std::snprintf(b, sizeof b, "%+.2f h", ms / 3.6e6);
  } else {
    std::snprintf(b, sizeof b, "-");
  }
  statusCell("Offset", b, txt);
  if (up && st.has_phone_stats && st.phone.battery_pct >= 0) {
    std::snprintf(b, sizeof b, "%d%%%s", st.phone.battery_pct, st.phone.charging ? " (carga)" : "");
    statusCell("Bateria", b, st.phone.battery_pct < 15 && !st.phone.charging ? col::bad() : col::good());
  } else {
    statusCell("Bateria", "-", col::dim());
  }
  if (up && st.has_phone_stats) {
    std::snprintf(b, sizeof b, "%.1f C", static_cast<double>(st.phone.battery_temp_c));
    statusCell("Temp.", b, st.phone.battery_temp_c >= 45 ? col::bad() : st.phone.battery_temp_c >= 40 ? col::warn() : col::good());
    std::snprintf(b, sizeof b, "%s", thermalName(st.phone.thermal_status));
    statusCell("Térmica", b, thermalColor(st.phone.thermal_status));
  } else {
    statusCell("Temp.", "-", col::dim());
    statusCell("Térmica", "-", col::dim());
  }
  if (up) std::snprintf(b, sizeof b, "%.1f fps", st.rx_fps); else std::snprintf(b, sizeof b, "-");
  statusCell("Recepção", b, txt);
  ImGui::EndTable();
}

void drawOrientationRow(App& app) {
  const owc::ClientStats& st = app.stats;
  int mode;
  bool mirror;
  app.currentOrientation(mode, mirror);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(col::dim(), "Orientação");
  ImGui::SameLine();
  static const char* items[] = {"Auto (celular)", "0°", "90°", "180°", "270°"};
  int idx = mode < 0 ? 0 : mode / 90 + 1;
  bool changed = false;
  ImGui::SetNextItemWidth(sc(128));
  if (ImGui::Combo("##orient", &idx, items, 5)) {
    mode = idx == 0 ? -1 : (idx - 1) * 90;
    changed = true;
  }
  tip("Auto aplica a rotação informada pelo celular (a imagem sai em pé). Os outros valores ignoram a dica e giram o quadro cru do sensor. Lembrado por aparelho. Vale também para a webcam virtual e o OBS.");
  ImGui::SameLine();
  if (ImGui::Checkbox("Espelhar", &mirror)) changed = true;
  tip("Espelha a imagem na horizontal (útil com a câmera frontal).");
  if (changed) app.setOrientation(mode, mirror);
  if (st.output_width > 0 && app.live()) {
    ImGui::SameLine();
    ImGui::TextColored(col::dim(), "%ux%u", st.output_width, st.output_height);
  }
  // high-speed quick toggle
  const owc::ParamDesc* hs = nullptr;
  if (app.caps_valid)
    for (const auto& d : app.caps.params)
      if (d.id == owc::ParamId::HighSpeedMode) hs = &d;
  if (hs && (hs->flags & owc::kParamSupported) && app.sessionUp()) {
    owc::ParamValue cur = hs->current;
    app.client.paramValue(hs->id, cur);
    owc::ParamValue pv;
    if (app.pending(hs->id, pv)) cur = pv;
    bool on = cur.i != 0;
    ImGui::SameLine(0, sc(18));
    if (ImGui::Checkbox("Alta velocidade", &on))
      app.setParam(hs->id, hs->type == owc::ValueType::Bool ? owc::ParamValue::Bool(on) : owc::ParamValue::EnumV(on ? 1 : 0));
    tip("sensor a 240 fps: ~50 ms em vez de ~110 ms; desativa controles manuais. Reinicia o stream e troca a lista de resoluções/fps.");
  }
}

void drawCenter(App& app, Renderer& r) {
  const owc::ClientStats& st = app.stats;
  ImGui::PushFont(fontTitle);
  std::string title = "Nenhum dispositivo";
  if (st.state == owc::ClientState::Pairing) {
    const owc::PairingInfo pi = app.client.pairingInfo();
    title = pi.device_name.empty() ? pi.model : pi.device_name;
  } else if (app.dev_valid && app.live()) {
    title = app.dev.device_name.empty() ? app.dev.model : app.dev.device_name;
  }
  ImGui::TextUnformatted(title.c_str());
  ImGui::PopFont();
  {
    const float bw = sc(104);
    const float recW = sc(138);
    const float total = bw * 2 + recW + ImGui::GetStyle().ItemSpacing.x * 4 + sc(64);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + sc(8), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - total));
    ImGui::Checkbox("HUD", &app.settings.hud);
    if (ImGui::IsItemDeactivatedAfterEdit()) app.markDirty();
    tip("Mostra estatísticas e o gráfico de latência sobre a imagem.");
    ImGui::SameLine();
    recordButton(app, recW);
    ImGui::SameLine();
    ImGui::BeginDisabled(!app.sessionUp());
    if (st.state == owc::ClientState::Streaming) {
      if (ImGui::Button("Parar stream", ImVec2(bw, 0))) app.client.stopStream();
    } else {
      if (primaryButton("Iniciar stream", ImVec2(bw, 0))) app.client.startStream();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keyframe", ImVec2(bw, 0))) app.client.requestKeyframe();
    tip("Pede um quadro-chave (IDR) imediatamente, útil se a imagem estiver corrompida.");
    ImGui::EndDisabled();
  }
  drawOrientationRow(app);
  if (st.state == owc::ClientState::Pairing)
    ImGui::TextColored(col::warn(), "Pareamento: digite no OWC o código mostrado no celular.");
  else if (app.dev_valid && app.live())
    ImGui::TextColored(col::dim(), "%s  -  Android API %u  -  %s", app.dev.model.c_str(), app.dev.android_api,
                       app.stateMessage().empty() ? stateText(st.state) : app.stateMessage().c_str());
  else
    ImGui::TextColored(col::dim(), "Escolha um dispositivo na barra lateral para começar.");
  ImGui::Dummy(ImVec2(0, sc(2)));

  // status bar sized first (so the preview takes the rest)
  const float statusH = sc(80);
  ImVec2 avail = ImGui::GetContentRegionAvail();
  drawPreview(app, r, ImVec2(avail.x, std::max(sc(120), avail.y - statusH - sc(6))));
  ImGui::Dummy(ImVec2(0, sc(4)));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, col::card());
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, sc(8));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sc(12), sc(8)));
  if (ImGui::BeginChild("status", ImVec2(0, statusH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar))
    drawStatusBar(app);
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

// Pill tabs, wrapping.
void drawTabPills() {
  const float avail = ImGui::GetContentRegionAvail().x;
  float x = 0;
  for (int i = 0; i < static_cast<int>(sizeof(kTabs) / sizeof(kTabs[0])); ++i) {
    const float w = ImGui::CalcTextSize(kTabs[i].title).x + ImGui::GetStyle().FramePadding.x * 2;
    if (i > 0) {
      if (x + ImGui::GetStyle().ItemSpacing.x + w <= avail) { ImGui::SameLine(); x += ImGui::GetStyle().ItemSpacing.x; }
      else x = 0;
    }
    const bool act = g_tab == i;
    if (act) {
      ImGui::PushStyleColor(ImGuiCol_Button, mul(col::accent(), 0.85f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::accent());
    } else {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.18f, 0.22f, 1));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.25f, 0.31f, 1));
    }
    if (ImGui::Button(kTabs[i].title)) g_tab = i;
    ImGui::PopStyleColor(2);
    x += w;
  }
}

void drawRight(App& app) {
  drawTabPills();
  ImGui::Dummy(ImVec2(0, sc(4)));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, sc(8));
  ImGui::BeginChild("tabbody", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);
  switch (g_tab) {
    case 0: tabPresets(app); break;
    case 1: tabCamera(app); break;
    case 2: break;  // colour page: full window (colorPage)
    case 3: tabEncoder(app); break;
    case 4: tabPc(app); break;
    case 5: tabDevice(app); break;
    case 6: tabRecord(app); break;
    case 7: tabOutputs(app); break;
    case 8: tabLogs(app); break;
    default: break;
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Pairing (protocol v2): while the client is in state Pairing the phone shows a
// 6-digit code; this modal asks for it. It closes itself when the state
// changes (success: toast "Pareado com ..."; failure: toast from App::onStateChanged).
constexpr const char* kPairPopup = "###owc_pairing";
char g_pairCode[8] = "";
bool g_pairFocus = false;
bool g_pairWasOpen = false;
uint32_t g_pairWrongSeen = 0;  // attempts_left of the last wrong-code answer already shown

int digitsOnly(ImGuiInputTextCallbackData* d) {
  return (d->EventChar >= '0' && d->EventChar <= '9') ? 0 : 1;
}

void drawPairingModal(App& app) {
  const bool pairing = app.stats.state == owc::ClientState::Pairing;
  if (pairing && !g_pairWasOpen) {
    ImGui::OpenPopup(kPairPopup);
    g_pairCode[0] = 0;
    g_pairFocus = true;
    g_pairWrongSeen = 0;
  }
  g_pairWasOpen = pairing;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(sc(440), vp->Size.x - sc(40)), 0));
  const owc::PairingInfo pi = app.client.pairingInfo();
  const std::string who = pi.device_name.empty() ? (pi.model.empty() ? "o celular" : pi.model) : pi.device_name;
  const std::string title = "Parear com " + who + kPairPopup;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sc(18), sc(16)));
  if (ImGui::BeginPopupModal(title.c_str(), nullptr,
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    if (!pairing) {
      ImGui::CloseCurrentPopup();
    } else {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextUnformatted("Digite o código de 6 dígitos que apareceu no celular.");
      ImGui::PushStyleColor(ImGuiCol_Text, col::dim());
      ImGui::TextUnformatted("Só é preciso parear uma vez por PC. Para desfazer, remova este PC no app do celular.");
      ImGui::PopStyleColor();
      ImGui::PopTextWrapPos();
      ImGui::Dummy(ImVec2(0, sc(4)));

      if (pi.wrong_code && pi.attempts_left != g_pairWrongSeen) {  // new wrong answer: clear and retype
        g_pairWrongSeen = pi.attempts_left;
        g_pairCode[0] = 0;
        g_pairFocus = true;
      }
      const bool busy = pi.code_pending;
      auto submit = [&] {
        if (std::strlen(g_pairCode) == owc::kPairCodeDigits && !busy) app.client.submitPairingCode(g_pairCode);
      };
      ImGui::PushFont(fontTitle);
      ImGui::SetNextItemWidth(ImGui::CalcTextSize("000000").x + ImGui::GetStyle().FramePadding.x * 2 + sc(16));
      if (g_pairFocus) {
        ImGui::SetKeyboardFocusHere();
        g_pairFocus = false;
      }
      ImGui::BeginDisabled(busy);
      const bool enter = ImGui::InputText("##paircode", g_pairCode, owc::kPairCodeDigits + 1,
                                          ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue,
                                          digitsOnly);
      ImGui::EndDisabled();
      ImGui::PopFont();
      if (enter) {
        submit();
        g_pairFocus = std::strlen(g_pairCode) != owc::kPairCodeDigits;  // keep typing on an incomplete code
      }
      ImGui::SameLine(0, sc(14));
      {
        const unsigned s = (pi.ms_left + 999) / 1000;
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(s <= 15 ? col::warn() : col::dim(), "Expira em %u:%02u", s / 60, s % 60);
      }
      if (busy) {
        ImGui::TextColored(col::dim(), "Verificando...");
      } else if (pi.wrong_code) {
        ImGui::TextColored(col::bad(), "Código incorreto — %u tentativa%s restante%s", pi.attempts_left,
                           pi.attempts_left == 1 ? "" : "s", pi.attempts_left == 1 ? "" : "s");
      } else {
        ImGui::TextColored(col::dim(), "%u tentativas", pi.attempts_left);
      }
      ImGui::Dummy(ImVec2(0, sc(6)));
      const float bw = sc(120);
      ImGui::BeginDisabled(busy || std::strlen(g_pairCode) != owc::kPairCodeDigits);
      if (primaryButton("Parear", ImVec2(bw, 0))) submit();
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Cancelar", ImVec2(bw, 0))) {
        app.client.cancelPairing();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
}

void drawToasts(App& app) {
  auto t = app.toasts();
  if (t.empty()) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImVec2 ds = ImGui::GetIO().DisplaySize;
  float y = ds.y - sc(120);
  const ULONGLONG now = GetTickCount64();
  for (auto it = t.rbegin(); it != t.rend(); ++it) {
    const float maxw = std::min(ds.x - sc(40), sc(640));
    ImVec2 ts = ImGui::CalcTextSize(it->text.c_str(), nullptr, false, maxw - sc(28));
    ImVec2 bsz(ts.x + sc(28), ts.y + sc(20));
    float alpha = 1.0f;
    if (it->until > now && it->until - now < 500) alpha = static_cast<float>(it->until - now) / 500.0f;
    ImVec2 p0((ds.x - bsz.x) / 2, y - bsz.y), p1(p0.x + bsz.x, y);
    ImU32 bg = it->error ? IM_COL32(120, 36, 40, static_cast<int>(235 * alpha)) : IM_COL32(30, 40, 56, static_cast<int>(235 * alpha));
    ImU32 bd = it->error ? IM_COL32(240, 100, 100, static_cast<int>(255 * alpha)) : IM_COL32(70, 130, 220, static_cast<int>(255 * alpha));
    dl->AddRectFilled(p0, p1, bg, sc(8));
    dl->AddRect(p0, p1, bd, sc(8), 0, sc(1.2f));
    dl->AddText(nullptr, 0.0f, ImVec2(p0.x + sc(14), p0.y + sc(10)), IM_COL32(240, 244, 250, static_cast<int>(255 * alpha)),
                it->text.c_str(), nullptr, maxw - sc(28));
    y = p0.y - sc(8);
  }
}

}  // namespace

// ============================================================================
void uiSetup(float dpiScale) {
  S = dpiScale;
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2010, 0x2027, 0x2030, 0x203A, 0x2190,
                                   0x21FF, 0x2200, 0x22FF, 0x25A0, 0x25FF, 0};
  auto load = [&](const char* file, float px) -> ImFont* {
    char path[MAX_PATH];
    UINT n = GetWindowsDirectoryA(path, MAX_PATH);
    std::string p = std::string(path, n) + "\\Fonts\\" + file;
    if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    return io.Fonts->AddFontFromFileTTF(p.c_str(), px, &cfg, ranges);
  };
  ImFont* regular = load("segoeui.ttf", 16.0f * S);
  fontBold = load("segoeuib.ttf", 16.0f * S);
  fontTitle = load("segoeuib.ttf", 24.0f * S);
  if (!regular) {
    ImFontConfig cfg;
    cfg.SizePixels = 16.0f * S;
    regular = io.Fonts->AddFontDefault(&cfg);
  }
  if (!fontBold) fontBold = regular;
  if (!fontTitle) fontTitle = regular;
  io.FontDefault = regular;
  io.Fonts->Build();

  ImGuiStyle& s = ImGui::GetStyle();
  s = ImGuiStyle();
  ImGui::StyleColorsDark();
  s.WindowRounding = sc(8);
  s.ChildRounding = sc(8);
  s.FrameRounding = sc(6);
  s.PopupRounding = sc(6);
  s.GrabRounding = sc(6);
  s.ScrollbarRounding = sc(8);
  s.TabRounding = sc(6);
  s.FramePadding = ImVec2(sc(10), sc(6));
  s.ItemSpacing = ImVec2(sc(8), sc(7));
  s.ItemInnerSpacing = ImVec2(sc(6), sc(4));
  s.WindowPadding = ImVec2(sc(12), sc(12));
  s.ScrollbarSize = sc(11);
  s.GrabMinSize = sc(14);
  s.WindowBorderSize = 0;
  s.ChildBorderSize = 0;
  s.FrameBorderSize = 0;
  s.PopupBorderSize = 1;
  s.SeparatorTextBorderSize = 1;

  ImVec4* c = s.Colors;
  const ImVec4 acc = col::accent();
  c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.082f, 0.098f, 1.0f);
  c[ImGuiCol_ChildBg] = ImVec4(0.098f, 0.108f, 0.129f, 1.0f);
  c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.15f, 0.98f);
  c[ImGuiCol_Border] = ImVec4(0.20f, 0.22f, 0.27f, 1.0f);
  c[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.95f, 1.0f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.49f, 0.56f, 1.0f);
  c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.165f, 0.20f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.21f, 0.26f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.25f, 0.31f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.18f, 0.20f, 0.25f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.27f, 0.34f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.29f, 0.33f, 0.41f, 1.0f);
  c[ImGuiCol_Header] = ImVec4(acc.x, acc.y, acc.z, 0.30f);
  c[ImGuiCol_HeaderHovered] = ImVec4(acc.x, acc.y, acc.z, 0.45f);
  c[ImGuiCol_HeaderActive] = ImVec4(acc.x, acc.y, acc.z, 0.60f);
  c[ImGuiCol_SliderGrab] = acc;
  c[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.68f, 1.0f, 1.0f);
  c[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.72f, 1.0f, 1.0f);
  c[ImGuiCol_Separator] = ImVec4(0.20f, 0.22f, 0.27f, 1.0f);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.28f, 0.34f, 1.0f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.33f, 0.37f, 0.45f, 1.0f);
  c[ImGuiCol_TextSelectedBg] = ImVec4(acc.x, acc.y, acc.z, 0.40f);
  c[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.16f, 0.19f, 1.0f);
  c[ImGuiCol_TableBorderLight] = ImVec4(0.17f, 0.19f, 0.23f, 1.0f);
  c[ImGuiCol_TableBorderStrong] = ImVec4(0.20f, 0.22f, 0.27f, 1.0f);
}

bool uiSelectTab(const std::string& name) {
  std::string n;
  for (char ch : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  static const struct { const char* k; int i; } alias[] = {
      {"presets", 0}, {"preset", 0}, {"camera", 1}, {"câmera", 1}, {"color", 2}, {"cor", 2}, {"lut", 2}, {"cor / lut", 2},
      {"encoder", 3}, {"stream", 3}, {"encoder/stream", 3},
      {"pc", 4}, {"device", 5}, {"dispositivo", 5}, {"outputs", 7}, {"saidas", 7}, {"saídas", 7}, {"logs", 8},
      {"record", 6}, {"gravacao", 6}, {"gravação", 6}};
  if (n == "help") { g_openHelp = true; return true; }
  for (const auto& a : alias)
    if (n == a.k) { g_tab = a.i; return true; }
  return false;
}

bool uiPreviewHasVideo() { return g_hasVideo; }

void uiOnPreviewFrame(PreviewFrame& frame) { colorOnFrame(frame); }

void uiShutdown() { colorShutdown(); }

void uiDraw(App& app, Renderer& r) {
  app.beginFrame();
  app.pollLogs();
  app.tick();

  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sc(10), sc(10)));
  ImGui::Begin("##root", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
  ImGui::PopStyleVar();

  updateVideoState(app, r);
  if (g_tab == kColorTab) {
    // Colour page (Resolve style): the whole window, with the tab pills on top to leave it.
    ImGui::PushFont(fontTitle);
    ImGui::TextColored(col::accent(), "OWC");
    ImGui::PopFont();
    ImGui::SameLine(0, sc(14));
    drawTabPills();
    ImGui::Dummy(ImVec2(0, sc(2)));
    colorPage(app, r);
    drawPairingModal(app);
    ImGui::End();
    drawToasts(app);
    return;
  }

  const float total = ImGui::GetContentRegionAvail().x;
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  float leftW = sc(300), rightW = sc(450);
  if (total < sc(1300)) { leftW = sc(270); rightW = sc(410); }
  const float centerW = std::max(sc(300), total - leftW - rightW - gap * 2);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sc(12), sc(12)));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, sc(10));
  ImGui::BeginChild("##left", ImVec2(leftW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  drawSidebar(app);
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::BeginChild("##center", ImVec2(centerW, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
  drawCenter(app, r);
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::BeginChild("##right", ImVec2(rightW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  drawRight(app);
  ImGui::EndChild();
  ImGui::PopStyleVar(2);

  drawPairingModal(app);
  ImGui::End();
  drawToasts(app);
}

}  // namespace owcapp
