// "Cor / LUT" page, DaVinci Resolve "Color page" style: header (enable,
// before/after, undo/redo, reset, status), viewer + video scopes on top, and a
// palette area below (Rodas, Curvas, Hue/Sat, Ajustes, Galeria LUT).
// The page takes the whole window (sidebar hidden) while the tab is selected.
// Grade changes go to App::applyOptions at most once per UI frame.
#include <imgui.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <owc/color_grade.h>

#include "ui_color_internal.h"

namespace owcapp {
namespace ui {

// ============================================================================ shared helpers
namespace color {

bool g_customDrag = false;

ColorWorker& worker() {
  static ColorWorker w;
  return w;
}

bool beginPanel(const char* id, const ImVec2& size, bool scroll) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, pal::panel());
  ImGui::PushStyleColor(ImGuiCol_Border, pal::border());
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, sc(4));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sc(8), sc(6)));
  return ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                           scroll ? ImGuiWindowFlags_None : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse));
}

void endPanel() {
  ImGui::EndChild();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
}

void caption(const char* text) { ImGui::TextColored(pal::dim(), "%s", text); }

bool resetIcon(const char* id, float size, bool enabled, const char* tipText) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(!enabled);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool hot = ImGui::IsItemHovered();
  ImGui::EndDisabled();
  tip(tipText);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
  const float r = size * 0.32f;
  const ImU32 col = !enabled ? IM_COL32(90, 90, 95, 255) : hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(185, 185, 190, 255);
  dl->PathArcTo(c, r, 3.14159f * 0.35f, 3.14159f * 1.95f, 16);
  dl->PathStroke(col, ImDrawFlags_None, sc(1.6f));
  // arrow head at the arc start (pointing clockwise)
  const float a = 3.14159f * 0.35f;
  const ImVec2 e(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
  const float hs = size * 0.18f;
  dl->AddTriangleFilled(ImVec2(e.x - hs, e.y - hs * 0.2f), ImVec2(e.x + hs * 0.6f, e.y - hs * 0.9f),
                        ImVec2(e.x + hs * 0.4f, e.y + hs * 0.6f), col);
  return clicked && enabled;
}

bool toggleButton(const char* label, bool active, const ImVec2& size) {
  if (active) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(pal::accent().x * 0.75f, pal::accent().y * 0.75f, pal::accent().z * 0.75f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pal::accent());
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  }
  const bool r = ImGui::Button(label, size);
  if (active) ImGui::PopStyleColor(3);
  return r;
}

}  // namespace color

// ============================================================================ page
namespace {

using namespace color;

const char* const kPalettes[] = {"Rodas", "Curvas", "Hue/Sat", "Ajustes", "Galeria LUT"};
const char* const kPaletteTips[] = {
    "Rodas de cor primárias: Lift (sombras), Gamma (meios-tons), Gain (realces) e Offset (tudo), mais contraste, pivot, saturação etc.",
    "Curvas personalizadas Y, R, G e B.",
    "Curvas Hue vs Hue, Hue vs Sat e Luma vs Sat: mexem só numa faixa de cores ou de brilho.",
    "Ajustes básicos com controles deslizantes.",
    "Galeria de LUTs: looks prontos, arquivos .cube recentes e pastas de LUTs, com miniaturas."};

// ---- page / video state
ULONGLONG g_visibleTick = 0;
bool g_live = false;
bool g_snapEligible = false;
ULONGLONG g_ungradedSince = 0;
ULONGLONG g_lastScopeSubmit = 0, g_lastSnap = 0;

// ---- test chart (no video)
ImageTexture g_chartTex, g_chartOrigTex;
std::vector<uint8_t> g_chartBuf;
owc::ColorGrade g_chartGrade;
owc::ScopeOptions g_chartOpt;
bool g_chartSent = false;
ULONGLONG g_chartSentTick = 0;

// ---- undo / redo (snapshots of the grade; compare_split is a view toggle, not undone)
std::vector<owc::ColorGrade> g_undo, g_redo;
owc::ColorGrade g_base;
bool g_baseInit = false;
constexpr size_t kUndoMax = 100;

bool sameGrade(owc::ColorGrade a, const owc::ColorGrade& b) {
  a.compare_split = b.compare_split;
  return a == b;
}

bool sameOpts(const owc::ScopeOptions& a, const owc::ScopeOptions& b) {
  return a.width == b.width && a.height == b.height && a.vector_size == b.vector_size;
}

// ---- adjustments palette (the classic sliders)
bool pctSlider(const char* id, const char* label, const char* tipText, float& v, float def = 0.0f) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  tip(tipText);
  ImGui::SameLine(sc(104));
  bool changed = false;
  int pct = static_cast<int>(std::lround(v * 100.0f));
  ImGui::SetNextItemWidth(-sc(58));
  const std::string sid = std::string("##") + id;
  if (ImGui::SliderInt(sid.c_str(), &pct, -100, 100, "%d", ImGuiSliderFlags_AlwaysClamp)) {
    v = static_cast<float>(pct) / 100.0f;
    changed = true;
  }
  tip("Arraste para ajustar. Ctrl+clique para digitar o valor.");
  ImGui::SameLine();
  ImGui::BeginDisabled(v == def);
  if (ImGui::SmallButton((std::string("Zerar##") + id).c_str())) { v = def; changed = true; }
  ImGui::EndDisabled();
  tip("Volta este ajuste para o valor neutro.");
  return changed;
}

bool drawAdjustments(owc::ColorGrade& g) {
  bool c = false;
  if (!ImGui::BeginTable("##adj", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) return false;
  ImGui::TableNextColumn();
  {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Exposição");
    tip("Clareia ou escurece a imagem em stops (EV). +1 EV dobra a luz, -1 EV reduz pela metade.");
    ImGui::SameLine(sc(104));
    ImGui::SetNextItemWidth(-sc(58));
    if (ImGui::SliderFloat("##exp", &g.exposure_ev, -3.0f, 3.0f, "%.2f EV", ImGuiSliderFlags_AlwaysClamp)) c = true;
    tip("Arraste para ajustar. Ctrl+clique para digitar o valor.");
    ImGui::SameLine();
    ImGui::BeginDisabled(g.exposure_ev == 0.0f);
    if (ImGui::SmallButton("Zerar##exp")) { g.exposure_ev = 0.0f; c = true; }
    ImGui::EndDisabled();
  }
  c |= pctSlider("contrast", "Contraste", "Aumenta ou reduz a diferença entre claros e escuros (curva em S em torno do pivot).", g.contrast);
  {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Pivot");
    tip("Ponto central do contraste. 0,435 é o cinza médio (padrão do Resolve).");
    ImGui::SameLine(sc(104));
    ImGui::SetNextItemWidth(-sc(58));
    if (ImGui::SliderFloat("##pivot", &g.pivot, 0.1f, 0.9f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) c = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(g.pivot == 0.435f);
    if (ImGui::SmallButton("Zerar##pivot")) { g.pivot = 0.435f; c = true; }
    ImGui::EndDisabled();
  }
  c |= pctSlider("highlights", "Realces", "Valores negativos recuperam áreas muito claras (céu, janelas); positivos as intensificam.", g.highlights);
  c |= pctSlider("shadows", "Sombras", "Valores positivos clareiam as áreas escuras sem estourar o resto; negativos as escurecem.", g.shadows);
  ImGui::TableNextColumn();
  c |= pctSlider("saturation", "Saturação", "Intensidade de todas as cores. -100 deixa a imagem em preto e branco.", g.saturation);
  c |= pctSlider("vibrance", "Vibração", "Como a saturação, mas atua mais nas cores apagadas e preserva tons de pele.", g.vibrance);
  c |= pctSlider("temperature", "Temperatura", "Balanço de branco: positivo deixa a imagem mais quente (amarelada), negativo mais fria (azulada).", g.temperature);
  c |= pctSlider("tint", "Matiz (tint)", "Corrige o desvio verde/magenta do balanço de branco: positivo vai para magenta, negativo para verde.", g.tint);
  {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Intens. LUT");
    tip("Mistura entre a imagem com os ajustes (0 %) e o resultado do LUT (100 %). Precisa de um look ou arquivo LUT selecionado (Galeria LUT).");
    ImGui::SameLine(sc(104));
    ImGui::BeginDisabled(g.lut.empty());
    int pct = static_cast<int>(g.lut_strength * 100.0f + 0.5f);
    ImGui::SetNextItemWidth(-sc(58));
    if (ImGui::SliderInt("##lutstr2", &pct, 0, 100, "%d %%", ImGuiSliderFlags_AlwaysClamp)) {
      g.lut_strength = static_cast<float>(pct) / 100.0f;
      c = true;
    }
    ImGui::EndDisabled();
  }
  ImGui::EndTable();
  ImGui::Dummy(ImVec2(0, sc(4)));
  const bool neutral = g.exposure_ev == 0 && g.contrast == 0 && g.highlights == 0 && g.shadows == 0 &&
                       g.saturation == 0 && g.vibrance == 0 && g.temperature == 0 && g.tint == 0 && g.pivot == 0.435f;
  ImGui::BeginDisabled(neutral);
  if (ImGui::Button("Resetar ajustes")) {
    g.exposure_ev = g.contrast = g.highlights = g.shadows = 0.0f;
    g.saturation = g.vibrance = g.temperature = g.tint = 0.0f;
    g.pivot = 0.435f;
    c = true;
  }
  ImGui::EndDisabled();
  tip("Zera estes ajustes. Rodas, curvas e LUT são mantidos.");
  return c;
}

// ---- header bar
void drawHeader(App& app, owc::ColorGrade& g, bool& commit, bool& doUndo, bool& doRedo) {
  const owc::ClientStats& st = app.stats;
  const float rightEdge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;  // screen x
  if (ImGui::Checkbox("Ativar tratamento de cor", &g.enabled)) commit = true;
  tip("Liga ou desliga todo o tratamento (rodas, curvas, ajustes e LUT). Desligado não custa nada de CPU. Vale para a prévia, a webcam virtual e o OBS.");
  ImGui::SameLine(0, sc(14));
  ImGui::BeginDisabled(!g.enabled);
  if (ImGui::Checkbox("Antes/Depois", &g.compare_split)) commit = true;
  ImGui::EndDisabled();
  tip("Divide a imagem: a metade esquerda fica sem tratamento, para comparar. Atenção: enquanto estiver ligado, a webcam virtual e o OBS também recebem a imagem dividida.");
  ImGui::SameLine(0, sc(14));
  ImGui::BeginDisabled(g_undo.empty());
  if (ImGui::Button("Desfazer")) doUndo = true;
  ImGui::EndDisabled();
  tip("Desfaz a última alteração de cor (Ctrl+Z).");
  ImGui::SameLine(0, sc(4));
  ImGui::BeginDisabled(g_redo.empty());
  if (ImGui::Button("Refazer")) doRedo = true;
  ImGui::EndDisabled();
  tip("Refaz a alteração desfeita (Ctrl+Y ou Ctrl+Shift+Z).");
  ImGui::SameLine(0, sc(14));
  if (ImGui::Button("Resetar tudo...")) ImGui::OpenPopup("Resetar tudo?##color");
  const float leftEnd = ImGui::GetItemRectMax().x;  // screen x after the last button
  tip("Volta rodas, curvas, ajustes e LUT ao neutro (pede confirmação; dá para desfazer).");
  if (ImGui::BeginPopupModal("Resetar tudo?##color", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextUnformatted("Zerar rodas, curvas, ajustes e tirar o LUT?");
    ImGui::TextColored(pal::dim(), "O liga/desliga do tratamento é mantido. Ctrl+Z desfaz.");
    ImGui::Dummy(ImVec2(0, sc(4)));
    if (ImGui::Button("Resetar", ImVec2(sc(110), 0))) {
      owc::ColorGrade n;
      n.enabled = g.enabled;
      n.compare_split = g.compare_split;
      g = n;
      commit = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancelar", ImVec2(sc(110), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // status, right-aligned
  char b[256];
  ImVec4 c = pal::dim();
  if (st.grade_active) {
    std::snprintf(b, sizeof b, "Ativo  %.1f ms/quadro", st.grade_ms_avg);
    c = col::good();
  } else {
    std::snprintf(b, sizeof b, "%s", g.enabled ? (g_live ? "Inativo (nada a aplicar)" : "Sem vídeo") : "Desligado");
  }
  std::string lut;
  if (st.grade_lut_size > 0) {
    char l[200];
    std::snprintf(l, sizeof l, "LUT: %s (%u\xC2\xB3)", st.grade_lut_title.empty() ? lutDisplayName(g.lut).c_str() : st.grade_lut_title.c_str(),
                  st.grade_lut_size);
    lut = l;
  } else if (!g.lut.empty()) {
    lut = "LUT: " + lutDisplayName(g.lut);
  }
  const char* notice = "Gravações não recebem o tratamento";
  const float w1 = ImGui::CalcTextSize(b).x, w2 = lut.empty() ? 0 : ImGui::CalcTextSize(lut.c_str()).x + sc(14);
  const float w3 = ImGui::CalcTextSize(notice).x + sc(14);
  const float wi = ImGui::CalcTextSize("(i)").x + sc(14);
  const float avail = rightEdge - leftEnd - sc(16) - sc(14);
  // drop the least important pieces first when the window is narrow
  const bool showNotice = avail > w1 + w2 + w3 + wi;
  const bool showLut = !lut.empty() && avail > w1 + w2 + wi;
  const float need = w1 + (showLut ? w2 : 0) + (showNotice ? w3 : 0) + wi;
  ImGui::SameLine(0, sc(16));
  if (avail > need) ImGui::SetCursorScreenPos(ImVec2(rightEdge - need, ImGui::GetCursorScreenPos().y));
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(c, "%s", b);
  if (st.grade_active) tip("Tempo médio por quadro para aplicar o tratamento (CPU, multithread).");
  if (showLut) {
    ImGui::SameLine(0, sc(14));
    ImGui::TextColored(pal::dim(), "%s", lut.c_str());
    tip(g.lut.c_str());
  }
  const char* infoTip = "O tratamento de cor é aplicado aqui no PC: na prévia, na webcam virtual e no OBS. As gravações NÃO são afetadas: elas guardam o vídeo original do celular. Ctrl+Z / Ctrl+Y desfazem e refazem.";
  if (showNotice) {
    ImGui::SameLine(0, sc(14));
    ImGui::TextColored(pal::dim(), "%s", notice);
    tip(infoTip);
  }
  ImGui::SameLine(0, sc(14));
  ImGui::TextColored(pal::dim(), "(i)");
  tip(infoTip);
  if (!st.grade_error.empty()) ImGui::TextColored(col::warn(), "%s", st.grade_error.c_str());
}

// ---- viewer
void drawViewer(App& app, Renderer& r, ImVec2 size, const owc::ColorGrade& g) {
  if (!beginPanel("##viewer", size)) { endPanel(); return; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 base = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  dl->AddRectFilled(base, ImVec2(base.x + avail.x, base.y + avail.y), pal::u32(pal::inset()), sc(3));
  const bool split = g.enabled && g.compare_split;

  if (app.rec_phase == RecPhase::Recording && !app.rec_preview_available) {
    const char* l1 = "Gravando em HEVC: prévia indisponível neste PC";
    const ImVec2 ts = ImGui::CalcTextSize(l1);
    dl->AddText(ImVec2(base.x + (avail.x - ts.x) * 0.5f, base.y + (avail.y - ts.y) * 0.5f), IM_COL32(235, 110, 110, 255), l1);
    ImGui::Dummy(avail);
    endPanel();
    return;
  }

  ImTextureID tex = 0, orig = 0;
  float iwSrc = 16, ihSrc = 9;
  if (g_live) {
    tex = reinterpret_cast<ImTextureID>(r.previewSrv());
    iwSrc = static_cast<float>(r.previewWidth());
    ihSrc = static_cast<float>(r.previewHeight());
  } else {
    if (!g_chartOrigTex.valid()) {
      const auto& px = ColorWorker::chartRgba();
      g_chartOrigTex.update(r.device(), r.context(), px.data(), ColorWorker::kChartW, ColorWorker::kChartH);
    }
    orig = reinterpret_cast<ImTextureID>(g_chartOrigTex.srv());
    tex = g_chartTex.valid() && g.enabled ? reinterpret_cast<ImTextureID>(g_chartTex.srv()) : orig;
    iwSrc = static_cast<float>(ColorWorker::kChartW);
    ihSrc = static_cast<float>(ColorWorker::kChartH);
  }
  const float ar = iwSrc / std::max(1.0f, ihSrc);
  const float pad = sc(4);
  float iw = avail.x - pad * 2, ih = iw / ar;
  if (ih > avail.y - pad * 2) { ih = avail.y - pad * 2; iw = ih * ar; }
  const ImVec2 imin(base.x + (avail.x - iw) * 0.5f, base.y + (avail.y - ih) * 0.5f);
  const ImVec2 imax(imin.x + iw, imin.y + ih);
  if (tex) {
    if (!g_live && split && orig) {
      const float mx = (imin.x + imax.x) * 0.5f;
      dl->AddImage(orig, imin, ImVec2(mx, imax.y), ImVec2(0, 0), ImVec2(0.5f, 1));
      dl->AddImage(tex, ImVec2(mx, imin.y), imax, ImVec2(0.5f, 0), ImVec2(1, 1));
    } else {
      dl->AddImage(tex, imin, imax);
    }
  }
  if (split) {
    const float mx = (imin.x + imax.x) * 0.5f;
    dl->AddLine(ImVec2(mx, imin.y), ImVec2(mx, imax.y), IM_COL32(255, 255, 255, 200), sc(1.5f));
    auto tag = [&](const char* t, float x, bool right) {
      const ImVec2 ts = ImGui::CalcTextSize(t);
      const ImVec2 p(right ? x - ts.x - sc(14) : x + sc(6), imin.y + sc(6));
      dl->AddRectFilled(ImVec2(p.x - sc(5), p.y - sc(2)), ImVec2(p.x + ts.x + sc(5), p.y + ts.y + sc(2)), IM_COL32(0, 0, 0, 150), sc(3));
      dl->AddText(p, IM_COL32(235, 235, 235, 255), t);
    };
    tag("Antes", mx, true);
    tag("Depois", mx + sc(8), false);
  }
  // bottom-left caption
  {
    char t[128];
    if (g_live) std::snprintf(t, sizeof t, "%ux%u", r.previewWidth(), r.previewHeight());
    else std::snprintf(t, sizeof t, "%s", app.live() ? "Aguardando vídeo: imagem de teste" : "Sem vídeo: imagem de teste");
    const ImVec2 ts = ImGui::CalcTextSize(t);
    const ImVec2 p(imin.x + sc(8), imax.y - ts.y - sc(8));
    dl->AddRectFilled(ImVec2(p.x - sc(5), p.y - sc(2)), ImVec2(p.x + ts.x + sc(5), p.y + ts.y + sc(2)), IM_COL32(0, 0, 0, 140), sc(3));
    dl->AddText(p, IM_COL32(220, 220, 220, 255), t);
    if (!g.enabled) {
      const char* off = "Tratamento desligado";
      const ImVec2 os = ImGui::CalcTextSize(off);
      const ImVec2 q(imax.x - os.x - sc(8), imax.y - os.y - sc(8));
      dl->AddRectFilled(ImVec2(q.x - sc(5), q.y - sc(2)), ImVec2(q.x + os.x + sc(5), q.y + os.y + sc(2)), IM_COL32(0, 0, 0, 140), sc(3));
      dl->AddText(q, IM_COL32(240, 190, 90, 255), off);
    }
  }
  ImGui::Dummy(avail);
  endPanel();
}

// Horizontal splitter between the viewer row and the palettes.
void splitter(App& app, float pageH, float minTop, float maxTop, float& topH) {
  const float h = sc(8);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  ImGui::InvisibleButton("##split", ImVec2(w, h));
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
  if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y != 0) {
    topH = std::max(minTop, std::min(maxTop, topH + ImGui::GetIO().MouseDelta.y));
    app.settings.color_split = topH / std::max(1.0f, pageH);
  }
  if (ImGui::IsItemDeactivated()) app.markDirty();
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    app.settings.color_split = 0.48f;
    app.markDirty();
  }
  const float cy = p.y + h * 0.5f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cx = p.x + w * 0.5f;
  dl->AddLine(ImVec2(cx - sc(20), cy), ImVec2(cx + sc(20), cy), hot ? IM_COL32(200, 200, 205, 255) : IM_COL32(90, 90, 95, 255), sc(2));
}

void pushPageStyle() {
  ImGui::PushStyleColor(ImGuiCol_Text, pal::text());
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.17f, 0.17f, 0.175f, 1));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.21f, 0.21f, 0.215f, 1));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.25f, 0.25f, 0.26f, 1));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.205f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.26f, 0.27f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.31f, 0.31f, 0.32f, 1));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.13f, 0.13f, 0.135f, 0.98f));
  ImGui::PushStyleColor(ImGuiCol_Border, pal::border());
  ImGui::PushStyleColor(ImGuiCol_Separator, pal::border());
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.28f, 0.28f, 0.29f, 1));
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.25f, 0.25f, 0.26f, 1));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.30f, 0.30f, 0.31f, 1));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, sc(3));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(sc(8), sc(4)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sc(6), sc(6)));
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, sc(3));
}
void popPageStyle() {
  ImGui::PopStyleVar(4);
  ImGui::PopStyleColor(13);
}

}  // namespace

// ============================================================================ entry points
void colorPage(App& app, Renderer& r) {
  owc::ClientOptions o = app.client.options();
  owc::ColorGrade& g = o.color;
  const ULONGLONG now = GetTickCount64();
  g_visibleTick = now;
  g_live = previewLive(app, r);
  g_customDrag = false;
  bool commit = false, doUndo = false, doRedo = false;

  // snapshot eligibility for the gallery: the frames are ungraded only when the grade is
  // identity AND the engine reports no table applied, stable for a moment (bake latency)
  {
    const bool ungraded = !app.stats.grade_active && owc::isIdentity(g);
    if (!ungraded) g_ungradedSince = 0;
    else if (!g_ungradedSince) g_ungradedSince = now;
    g_snapEligible = g_live && g_ungradedSince != 0 && now - g_ungradedSince >= 600;
  }
  // keyboard: undo / redo
  {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && !ImGui::IsAnyItemActive()) {
      if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) doUndo = true;
      if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
        doRedo = true;
    }
  }

  pushPageStyle();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, pal::page());
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, sc(6));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sc(8), sc(8)));
  ImGui::BeginChild("##colorpage", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar(2);

  drawHeader(app, g, commit, doUndo, doRedo);

  // ---- layout: viewer + scopes | splitter | palettes
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float splitH = sc(8);
  const float pageH = avail.y - splitH - style.ItemSpacing.y * 2;
  const float minTop = sc(150), minBottom = sc(250);
  const float maxTop = std::max(minTop, pageH - minBottom);
  float topH = std::max(minTop, std::min(maxTop, pageH * app.settings.color_split));
  {
    const float gap = sc(6);
    const float vw = std::max(avail.x * 0.42f, std::min(avail.x - sc(280) - gap, (topH - sc(20)) * 16.0f / 9.0f + sc(20)));
    drawViewer(app, r, ImVec2(vw, topH), g);
    ImGui::SameLine(0, gap);
    drawScopes(app, r, ImVec2(avail.x - vw - gap, topH));
  }
  splitter(app, pageH, minTop, maxTop, topH);
  {
    const float bottomH = std::max(sc(120), ImGui::GetContentRegionAvail().y);
    if (beginPanel("##palette", ImVec2(0, bottomH))) {
      int& pi = app.settings.color_palette;
      const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
      for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine(0, sc(2));
        if (toggleButton(kPalettes[i], pi == i, ImVec2(sc(104), 0))) {
          pi = i;
          app.markDirty();
        }
        tip(kPaletteTips[i]);
      }
      if (pi == 0) {
        const bool any = !g.lift.isNeutral() || !g.gamma.isNeutral() || !g.gain.isNeutral() || !g.offset.isNeutral();
        const char* lbl = "Resetar rodas";
        const float bw = ImGui::CalcTextSize(lbl).x + style.FramePadding.x * 2;
        ImGui::SameLine();
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + sc(10), rightEdge - bw));
        ImGui::BeginDisabled(!any);
        if (ImGui::Button(lbl)) {
          g.lift = g.gamma = g.gain = g.offset = owc::ColorWheel();
          commit = true;
        }
        ImGui::EndDisabled();
        tip("Zera as quatro rodas (Lift, Gamma, Gain e Offset).");
      }
      ImGui::Separator();
      const ImVec2 pa = ImGui::GetContentRegionAvail();
      switch (pi) {
        case 0: commit |= drawWheels(g, pa); break;
        case 1: commit |= drawCurves(g, pa, histogramTexture()); break;
        case 2: commit |= drawHueSat(g, pa); break;
        case 3: {
          ImGui::BeginChild("##adjscroll", pa, ImGuiChildFlags_None);
          commit |= drawAdjustments(g);
          ImGui::EndChild();
          break;
        }
        default: commit |= drawGallery(app, r, g, pa); break;
      }
    }
    endPanel();
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  popPageStyle();

  // ---- undo bookkeeping: one snapshot per finished edit (drags are grouped)
  if (!g_baseInit) {
    g_base = g;
    g_baseInit = true;
  }
  const bool editing = ImGui::IsAnyItemActive() || g_customDrag;
  if (!editing && !sameGrade(g, g_base)) {
    g_undo.push_back(g_base);
    if (g_undo.size() > kUndoMax) g_undo.erase(g_undo.begin());
    g_redo.clear();
    g_base = g;
  }
  if (doUndo && !g_undo.empty() && !editing) {
    owc::ColorGrade prev = g_undo.back();
    g_undo.pop_back();
    g_redo.push_back(g);
    prev.compare_split = g.compare_split;
    g = prev;
    g_base = g;
    commit = true;
  } else if (doRedo && !g_redo.empty() && !editing) {
    owc::ColorGrade next = g_redo.back();
    g_redo.pop_back();
    g_undo.push_back(g);
    next.compare_split = g.compare_split;
    g = next;
    g_base = g;
    commit = true;
  }

  // ---- test chart when there is no video (graded on the worker, also feeds the scopes)
  if (!g_live) {
    if (worker().takeChart(g_chartBuf) && g_chartBuf.size() == static_cast<size_t>(ColorWorker::kChartW) * ColorWorker::kChartH * 4)
      g_chartTex.update(r.device(), r.context(), g_chartBuf.data(), ColorWorker::kChartW, ColorWorker::kChartH);
    const owc::ScopeOptions so = scopeOptions();
    if ((!g_chartSent || g != g_chartGrade || !sameOpts(so, g_chartOpt)) && now - g_chartSentTick >= 60) {
      worker().submitChart(g, so);
      g_chartGrade = g;
      g_chartOpt = so;
      g_chartSent = true;
      g_chartSentTick = now;
    }
  } else {
    g_chartSent = false;  // re-render when the video goes away
  }

  if (commit) app.applyOptions(o);  // setOptions + markDirty (debounced save)
}

void colorOnFrame(PreviewFrame& f) {
  const ULONGLONG now = GetTickCount64();
  if (now - g_visibleTick > 300 || f.w < 2 || f.h < 2) return;  // page not visible
  if (now - g_lastScopeSubmit < 66) return;                       // scopes <= 15 Hz
  owc::ScopeOptions o = scopeOptions();
  o.step = std::max(1u, std::max(f.w, f.h) / 640u);
  const bool snap = g_snapEligible && now - g_lastSnap >= 2000;
  if (worker().submitFrame(f, o, snap)) {
    g_lastScopeSubmit = now;
    if (snap) g_lastSnap = now;
  }
}

void colorShutdown() {
  worker().stop();
  g_chartTex.reset();
  g_chartOrigTex.reset();
  releaseScopeTextures();
  releaseGalleryTextures();
}

}  // namespace ui
}  // namespace owcapp
