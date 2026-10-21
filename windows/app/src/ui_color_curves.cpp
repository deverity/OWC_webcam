// Colour page: custom curves (Y / R / G / B) and the Hue vs Hue, Hue vs Sat
// and Luma vs Sat curves. The curves are drawn by sampling owc::evalCurve /
// evalHueCurve / evalLumaSatCurve, i.e. exactly what the engine applies.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <owc/color_grade.h>

#include "ui_color_internal.h"

namespace owcapp {
namespace ui {
namespace color {

namespace {

using Curve = std::vector<owc::CurvePoint>;
constexpr size_t kMaxPoints = 16;

enum class Kind { Custom, Hue, LumaSat };

float evalK(Kind k, const Curve& c, float x) {
  switch (k) {
    case Kind::Custom: return owc::evalCurve(c, x);
    case Kind::Hue: return owc::evalHueCurve(c, x);
    default: return owc::evalLumaSatCurve(c, x);
  }
}

ImU32 hueColor(float h, float s = 0.85f, float v = 0.95f, int a = 255) {
  h -= std::floor(h);
  float r, g, b;
  ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
  return IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255), a);
}

void sortCurve(Curve& c) {
  std::stable_sort(c.begin(), c.end(), [](const owc::CurvePoint& a, const owc::CurvePoint& b) { return a.x < b.x; });
}

// Inserts p keeping the order; returns its index (or -1 if too close to an existing point / full).
int insertPoint(Curve& c, owc::CurvePoint p, float minDx = 0.01f) {
  if (c.size() >= kMaxPoints) return -1;
  for (const auto& q : c)
    if (std::fabs(q.x - p.x) < minDx) return -1;
  auto it = std::upper_bound(c.begin(), c.end(), p.x, [](float x, const owc::CurvePoint& q) { return x < q.x; });
  it = c.insert(it, p);
  return static_cast<int>(it - c.begin());
}

struct EditorDrag {
  ImGuiID id = 0;
  int idx = -1;
};
EditorDrag g_edit;

struct View {
  ImVec2 p0, p1;   // graph rect (screen)
  float x0, x1;    // curve x shown at p0.x / p1.x
  ImVec2 toScreen(float x, float y) const {
    return ImVec2(p0.x + (x - x0) / (x1 - x0) * (p1.x - p0.x), p1.y - y * (p1.y - p0.y));
  }
  void toCurve(ImVec2 s, float& x, float& y) const {
    x = x0 + (s.x - p0.x) / (p1.x - p0.x) * (x1 - x0);
    y = (p1.y - s.y) / (p1.y - p0.y);
  }
};

void polyCurve(ImDrawList* dl, const View& v, Kind k, const Curve& c, ImU32 col, float thick) {
  const int N = 160;
  ImVec2 pts[N + 1];
  for (int i = 0; i <= N; ++i) {
    const float x = v.x0 + (v.x1 - v.x0) * static_cast<float>(i) / static_cast<float>(N);
    float y = evalK(k, c, k == Kind::Hue ? x - std::floor(x) : std::max(0.0f, std::min(1.0f, x)));
    y = std::max(0.0f, std::min(1.0f, y));
    pts[i] = v.toScreen(x, y);
  }
  dl->AddPolyline(pts, N + 1, col, ImDrawFlags_None, thick);
}

// Saturation gain of hue_vs_sat / luma_vs_sat (owc/client.h): 2^(4(y-0.5)), and below
// y = 0.5 minus 0.25*(1-2y)^2 so that y = 0 is exactly grey.
float satGain(float y) {
  float g = std::pow(2.0f, 4.0f * (y - 0.5f));
  if (y < 0.5f) g -= 0.25f * (1.0f - 2.0f * y) * (1.0f - 2.0f * y);
  return std::max(0.0f, g);
}

std::string valueText(Kind k, int mode, float x, float y) {
  char b[96];
  if (k == Kind::Custom) {
    std::snprintf(b, sizeof b, "Entrada %.0f  ->  Saída %.0f", static_cast<double>(x * 1023.0f),
                  static_cast<double>(y * 1023.0f));
  } else if (k == Kind::Hue && mode == 0) {
    std::snprintf(b, sizeof b, "Matiz %.0f°  ->  desloca %+.0f°", static_cast<double>(x * 360.0f),
                  static_cast<double>((y - 0.5f) * 360.0f));
  } else {
    std::snprintf(b, sizeof b, "%s %.0f%s  ->  saturação x%.2f", k == Kind::Hue ? "Matiz" : "Luma",
                  static_cast<double>(k == Kind::Hue ? x * 360.0f : x * 100.0f), k == Kind::Hue ? "°" : "%",
                  static_cast<double>(satGain(y)));
  }
  return b;
}

struct Ghost {
  const Curve* c;
  ImU32 col;
};

// Generic curve editor. `mode` only matters for Kind::Hue (0 = hue vs hue, 1 = hue vs sat).
// Interaction: click on empty space adds a point (on the curve) and drags it;
// drag points; right-click or double-click deletes; Shift = fine.
bool curveEditor(const char* id, Kind kind, int mode, Curve& pts, ImVec2 size, ImU32 color,
                 const std::vector<Ghost>& ghosts, ImTextureID bg) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 base = ImGui::GetCursorScreenPos();
  const float bandH = kind == Kind::Custom ? 0.0f : sc(12) * (kind == Kind::Hue && mode == 0 ? 2.0f : 1.0f);
  View v;
  v.p0 = base;
  v.p1 = ImVec2(base.x + size.x, base.y + size.y - (bandH > 0 ? bandH + sc(4) : 0.0f));
  const float margin = kind == Kind::Hue ? 1.0f / 12.0f : 0.0f;  // wrap-around context on both sides
  v.x0 = -margin;
  v.x1 = 1.0f + margin;
  const ImVec2 a = v.toScreen(0, 1), b = v.toScreen(1, 0);

  // ---- background
  dl->AddRectFilled(v.p0, v.p1, pal::u32(pal::inset()));
  if (bg) dl->AddImage(bg, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 60));
  if (margin > 0) {  // wrapped regions, darker
    dl->AddRectFilled(v.p0, ImVec2(a.x, v.p1.y), IM_COL32(0, 0, 0, 90));
    dl->AddRectFilled(ImVec2(b.x, v.p0.y), v.p1, IM_COL32(0, 0, 0, 90));
  }
  const int div = kind == Kind::Hue ? 6 : 4;
  for (int i = 0; i <= div; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(div);
    const ImVec2 x = v.toScreen(t, 0), y0 = v.toScreen(0, t), y1 = v.toScreen(1, t);
    dl->AddLine(ImVec2(x.x, v.p0.y), ImVec2(x.x, v.p1.y), (i == 0 || i == div) ? pal::gridStrong() : pal::grid());
    if (kind == Kind::Custom || kind == Kind::LumaSat || i % 3 == 0)
      dl->AddLine(ImVec2(margin > 0 ? v.p0.x : y0.x, y0.y), ImVec2(margin > 0 ? v.p1.x : y1.x, y1.y), pal::grid());
  }
  if (kind == Kind::Custom) dl->AddLine(v.toScreen(0, 0), v.toScreen(1, 1), IM_COL32(255, 255, 255, 40));
  else dl->AddLine(ImVec2(v.p0.x, v.toScreen(0, 0.5f).y), ImVec2(v.p1.x, v.toScreen(0, 0.5f).y), IM_COL32(255, 255, 255, 70));

  // ---- band(s) under the graph
  if (bandH > 0) {
    const float by0 = v.p1.y + sc(4);
    const int N = 72;
    const bool two = kind == Kind::Hue && mode == 0;
    const float h1 = two ? bandH * 0.5f : bandH;
    for (int i = 0; i < N; ++i) {
      const float xa = v.x0 + (v.x1 - v.x0) * static_cast<float>(i) / N;
      const float xb = v.x0 + (v.x1 - v.x0) * static_cast<float>(i + 1) / N;
      const float sa = v.toScreen(xa, 0).x, sb = v.toScreen(xb, 0).x;
      if (kind == Kind::LumaSat) {
        const auto ga = static_cast<int>(std::max(0.0f, std::min(1.0f, xa)) * 255),
                   gb = static_cast<int>(std::max(0.0f, std::min(1.0f, xb)) * 255);
        dl->AddRectFilledMultiColor(ImVec2(sa, by0), ImVec2(sb, by0 + h1), IM_COL32(ga, ga, ga, 255),
                                    IM_COL32(gb, gb, gb, 255), IM_COL32(gb, gb, gb, 255), IM_COL32(ga, ga, ga, 255));
      } else {
        const ImU32 ca = hueColor(xa), cb = hueColor(xb);
        dl->AddRectFilledMultiColor(ImVec2(sa, by0), ImVec2(sb, by0 + h1), ca, cb, cb, ca);
        if (two) {  // output hue after the hue-vs-hue shift
          const float oa = xa + evalK(kind, pts, xa - std::floor(xa)) - 0.5f;
          const float ob = xb + evalK(kind, pts, xb - std::floor(xb)) - 0.5f;
          dl->AddRectFilledMultiColor(ImVec2(sa, by0 + h1), ImVec2(sb, by0 + bandH), hueColor(oa), hueColor(ob),
                                      hueColor(ob), hueColor(oa));
        }
      }
    }
  }

  // ---- interaction
  ImGui::SetCursorScreenPos(v.p0);
  ImGui::InvisibleButton(id, ImVec2(v.p1.x - v.p0.x, v.p1.y - v.p0.y),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const ImGuiID gid = ImGui::GetItemID();
  const ImGuiIO& io = ImGui::GetIO();
  bool changed = false;
  const bool fixedEnds = kind != Kind::Hue;
  const float hitR = sc(7);
  auto pointScreen = [&](size_t i, float shift) { return v.toScreen(pts[i].x + shift, pts[i].y); };
  auto hitTest = [&](ImVec2 m) -> int {
    int best = -1;
    float bd = hitR * hitR;
    for (size_t i = 0; i < pts.size(); ++i)
      for (float sh : {0.0f, -1.0f, 1.0f}) {
        if (sh != 0 && margin == 0) continue;
        const ImVec2 p = pointScreen(i, sh);
        const float d = (p.x - m.x) * (p.x - m.x) + (p.y - m.y) * (p.y - m.y);
        if (d < bd) { bd = d; best = static_cast<int>(i); }
      }
    return best;
  };
  const bool hovered = ImGui::IsItemHovered();
  const int hov = hovered ? hitTest(io.MousePos) : -1;
  auto deletable = [&](int i) { return i >= 0 && (!fixedEnds || (i > 0 && i + 1 < static_cast<int>(pts.size()))); };

  if (hovered && hov >= 0 &&
      (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
    if (deletable(hov)) {
      pts.erase(pts.begin() + hov);
      if (kind == Kind::Custom && pts.size() == 2 && pts[0].x == 0 && pts[0].y == 0 && pts[1].x == 1 && pts[1].y == 1)
        pts.clear();
      changed = true;
    }
    g_edit = EditorDrag();
  } else if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    g_edit.id = gid;
    g_edit.idx = hov;
    if (hov < 0) {
      float mx, my;
      v.toCurve(io.MousePos, mx, my);
      if (kind == Kind::Hue) mx -= std::floor(mx);
      mx = std::max(0.0f, std::min(1.0f, mx));
      if (pts.empty()) {
        if (kind == Kind::Custom) pts = {{0, 0}, {1, 1}};
        else if (kind == Kind::LumaSat) pts = {{0, 0.5f}, {1, 0.5f}};
        else {  // hue: the point plus two neutral anchors, so the change stays local
          for (float d : {-1.0f / 10.0f, 1.0f / 10.0f}) {
            float x = mx + d;
            x -= std::floor(x);
            insertPoint(pts, {x, 0.5f});
          }
        }
      }
      const float y = std::max(0.0f, std::min(1.0f, evalK(kind, pts, mx)));
      g_edit.idx = insertPoint(pts, {mx, y}, 0.005f);
      if (g_edit.idx >= 0) changed = true;
    }
  }
  if (ImGui::IsItemActive() && g_edit.id == gid && g_edit.idx >= 0 && g_edit.idx < static_cast<int>(pts.size()) &&
      ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    g_customDrag = true;
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
      const int i = g_edit.idx;
      const float k = io.KeyShift ? 0.15f : 1.0f;
      float dx = io.MouseDelta.x * k * (v.x1 - v.x0) / (v.p1.x - v.p0.x);
      const float dy = -io.MouseDelta.y * k / (v.p1.y - v.p0.y);
      owc::CurvePoint& p = pts[static_cast<size_t>(i)];
      const bool lockX = fixedEnds && (i == 0 || i + 1 == static_cast<int>(pts.size()));
      if (!lockX) {
        const float lo = i > 0 ? pts[static_cast<size_t>(i - 1)].x + 0.005f : 0.0f;
        const float hi = i + 1 < static_cast<int>(pts.size()) ? pts[static_cast<size_t>(i + 1)].x - 0.005f : 1.0f;
        p.x = std::max(lo, std::min(hi, p.x + dx));
      } else {
        dx = 0;
      }
      p.y = std::max(0.0f, std::min(1.0f, p.y + dy));
      changed = true;
    }
  } else if (g_edit.id == gid && !ImGui::IsItemActive()) {
    g_edit = EditorDrag();
  }
  if (changed) sortCurve(pts);

  // ---- curves
  dl->PushClipRect(v.p0, v.p1, true);
  for (const auto& gh : ghosts) polyCurve(dl, v, kind, *gh.c, gh.col, sc(1.2f));
  polyCurve(dl, v, kind, pts, color, sc(2.0f));
  // points (wrapped copies for hue curves)
  const int active = (g_edit.id == gid) ? g_edit.idx : -1;
  for (size_t i = 0; i < pts.size(); ++i) {
    for (float sh : {0.0f, -1.0f, 1.0f}) {
      if (sh != 0 && margin == 0) continue;
      const ImVec2 p = pointScreen(i, sh);
      if (p.x < v.p0.x - hitR || p.x > v.p1.x + hitR) continue;
      const bool hot = static_cast<int>(i) == hov || static_cast<int>(i) == active;
      const float r = hot ? sc(5.5f) : sc(4.0f);
      if (sh != 0) {
        dl->AddCircle(p, r, IM_COL32(220, 220, 220, 110), 16, sc(1.2f));
        continue;
      }
      dl->AddCircleFilled(p, r + sc(1.5f), IM_COL32(0, 0, 0, 170), 16);
      if (hot) dl->AddCircleFilled(p, r, color, 16);
      dl->AddCircle(p, r, IM_COL32(240, 240, 240, 255), 16, sc(1.5f));
    }
  }
  dl->PopClipRect();
  dl->AddRect(v.p0, v.p1, pal::u32(pal::border()));

  // ---- tooltip with the value under the cursor / of the dragged point
  if (active >= 0 && active < static_cast<int>(pts.size())) {
    ImGui::SetTooltip("%s", valueText(kind, mode, pts[static_cast<size_t>(active)].x, pts[static_cast<size_t>(active)].y).c_str());
  } else if (hovered && hov >= 0) {
    ImGui::SetTooltip("%s\n%s", valueText(kind, mode, pts[static_cast<size_t>(hov)].x, pts[static_cast<size_t>(hov)].y).c_str(),
                      deletable(hov) ? "Clique direito ou duplo clique: apaga o ponto." : "Ponto da ponta: só se move na vertical.");
  }
  ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + size.y));
  ImGui::Dummy(ImVec2(size.x, 0));
  return changed;
}

// Small clickable preview of a curve (channel selector).
bool curveTile(const char* id, const char* label, const Curve& c, ImU32 col, bool selected, float s) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(s, s));
  const bool hot = ImGui::IsItemHovered();
  View v;
  v.p0 = p0;
  v.p1 = ImVec2(p0.x + s, p0.y + s);
  v.x0 = 0;
  v.x1 = 1;
  dl->AddRectFilled(v.p0, v.p1, pal::u32(pal::inset()), sc(3));
  dl->AddLine(v.toScreen(0, 0), v.toScreen(1, 1), IM_COL32(255, 255, 255, 30));
  polyCurve(dl, v, Kind::Custom, c, col, sc(1.5f));
  dl->AddText(ImVec2(p0.x + sc(4), p0.y + sc(2)), col, label);
  dl->AddRect(v.p0, v.p1, selected ? pal::u32(pal::accent()) : (hot ? IM_COL32(120, 120, 125, 255) : pal::u32(pal::border())),
              sc(3), 0, selected ? sc(2) : 1.0f);
  return clicked;
}

int g_channel = 0;   // 0 Y, 1 R, 2 G, 3 B
bool g_linked = true;
int g_hueMode = 0;   // 0 hue vs hue, 1 hue vs sat, 2 luma vs sat

const ImU32 kChanCol[4] = {IM_COL32(225, 225, 225, 255), IM_COL32(240, 85, 85, 255), IM_COL32(85, 215, 105, 255),
                           IM_COL32(95, 145, 255, 255)};
const char* const kChanName[4] = {"Y", "R", "G", "B"};

ImU32 withAlpha(ImU32 c, int a) { return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT); }

}  // namespace

bool drawCurves(owc::ColorGrade& g, ImVec2 size, ImTextureID histogram) {
  bool changed = false;
  Curve* ch[4] = {&g.curve_y, &g.curve_r, &g.curve_g, &g.curve_b};
  const float side = std::max(sc(120), std::min(size.y, size.x - sc(200)));
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  std::vector<Ghost> ghosts;
  for (int i = 0; i < 4; ++i)
    if (i != g_channel && !ch[i]->empty()) ghosts.push_back({ch[i], withAlpha(kChanCol[i], 90)});
  if (curveEditor("##curve", Kind::Custom, 0, *ch[g_channel], ImVec2(side, side), kChanCol[g_channel], ghosts, histogram)) {
    changed = true;
    if (g_linked && g_channel > 0)
      for (int i = 1; i < 4; ++i)
        if (i != g_channel) *ch[i] = *ch[g_channel];
  }

  // ---- side controls
  const float sx = origin.x + side + sc(14);
  ImGui::SetCursorScreenPos(ImVec2(sx, origin.y));
  ImGui::BeginGroup();
  caption("Canal");
  const float tile = std::max(sc(44), std::min(sc(76), (size.x - side - sc(14) - sc(18)) / 4.0f));
  for (int i = 0; i < 4; ++i) {
    if (i) ImGui::SameLine(0, sc(6));
    ImGui::PushID(i);
    if (curveTile("##tile", kChanName[i], *ch[i], kChanCol[i], g_channel == i, tile)) g_channel = i;
    tip(i == 0 ? "Curva Y (mestre): age igualmente nos três canais." : "Curva só deste canal de cor.");
    ImGui::PopID();
  }
  ImGui::Checkbox("Vincular R, G e B", &g_linked);
  tip("Com o vínculo ligado, editar R, G ou B aplica a mesma curva aos três canais (como o elo do Resolve). A curva Y é sempre independente.");
  char lbl[48];
  std::snprintf(lbl, sizeof lbl, "Resetar curva %s", kChanName[g_channel]);
  ImGui::BeginDisabled(ch[g_channel]->empty());
  if (ImGui::Button(lbl)) {
    ch[g_channel]->clear();
    if (g_linked && g_channel > 0)
      for (int i = 1; i < 4; ++i) ch[i]->clear();
    changed = true;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  const bool any = !g.curve_y.empty() || !g.curve_r.empty() || !g.curve_g.empty() || !g.curve_b.empty();
  ImGui::BeginDisabled(!any);
  if (ImGui::Button("Resetar todas")) {
    for (auto* c : ch) c->clear();
    changed = true;
  }
  ImGui::EndDisabled();
  ImGui::Dummy(ImVec2(0, sc(4)));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(sc(160), size.x - side - sc(20)));
  ImGui::TextColored(pal::dim(), "Clique na curva para criar um ponto e arraste. Clique direito ou duplo clique apaga. Shift = ajuste fino. Até 16 pontos.");
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
  ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + side));
  ImGui::Dummy(ImVec2(size.x, 0));
  return changed;
}

bool drawHueSat(owc::ColorGrade& g, ImVec2 size) {
  bool changed = false;
  static const char* const names[3] = {"Hue vs Hue", "Hue vs Sat", "Luma vs Sat"};
  static const char* const tips[3] = {
      "Hue vs Hue: desloca a matiz de uma faixa de cores (ex.: puxar o verde da grama para o amarelo).",
      "Hue vs Sat: aumenta ou reduz a saturação de uma faixa de cores (ex.: tirar o vermelho da pele).",
      "Luma vs Sat: saturação conforme o brilho (ex.: dessaturar sombras e altas luzes)."};
  Curve* curves[3] = {&g.hue_vs_hue, &g.hue_vs_sat, &g.luma_vs_sat};
  for (int i = 0; i < 3; ++i) {
    if (i) ImGui::SameLine(0, sc(4));
    if (toggleButton(names[i], g_hueMode == i)) g_hueMode = i;
    tip(tips[i]);
  }
  Curve& c = *curves[g_hueMode];
  ImGui::SameLine(0, sc(16));
  ImGui::BeginDisabled(c.empty());
  if (ImGui::Button("Resetar curva")) { c.clear(); changed = true; }
  ImGui::EndDisabled();
  ImGui::SameLine();
  const bool any = !g.hue_vs_hue.empty() || !g.hue_vs_sat.empty() || !g.luma_vs_sat.empty();
  ImGui::BeginDisabled(!any);
  if (ImGui::Button("Resetar as três")) {
    for (auto* k : curves) k->clear();
    changed = true;
  }
  ImGui::EndDisabled();

  // quick hue swatches (adds the hue plus two neutral anchors)
  const bool hue = g_hueMode < 2;
  if (hue) {
    ImGui::SameLine(0, sc(16));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(pal::dim(), "Amostras");
    static const float hues[6] = {0.0f, 1.0f / 6, 2.0f / 6, 3.0f / 6, 4.0f / 6, 5.0f / 6};
    static const char* const hn[6] = {"vermelho", "amarelo", "verde", "ciano", "azul", "magenta"};
    for (int i = 0; i < 6; ++i) {
      ImGui::SameLine(0, sc(4));
      ImGui::PushID(i);
      float r, gg, b;
      ImGui::ColorConvertHSVtoRGB(hues[i], 0.8f, 0.9f, r, gg, b);
      const float s = ImGui::GetFrameHeight();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(r, gg, b, 1));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(1.0f, r + 0.1f), std::min(1.0f, gg + 0.1f), std::min(1.0f, b + 0.1f), 1));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(r, gg, b, 1));
      if (ImGui::Button("##sw", ImVec2(s, s)) && c.size() + 3 <= kMaxPoints) {
        const float h = hues[i];
        for (float d : {-1.0f / 12.0f, 0.0f, 1.0f / 12.0f}) {
          float x = h + d;
          x -= std::floor(x);
          insertPoint(c, {x, std::max(0.0f, std::min(1.0f, owc::evalHueCurve(c, x)))}, 0.02f);
        }
        changed = true;
      }
      ImGui::PopStyleColor(3);
      char t[96];
      std::snprintf(t, sizeof t, "Cria três pontos em torno do %s (o do meio é o que você arrasta).", hn[i]);
      tip(t);
      ImGui::PopID();
    }
  }
  ImGui::Dummy(ImVec2(0, sc(2)));
  const ImVec2 avail(size.x, std::max(sc(80), size.y - ImGui::GetFrameHeightWithSpacing() - sc(4)));
  const ImU32 col = IM_COL32(230, 230, 230, 255);
  if (curveEditor("##hs", g_hueMode == 2 ? Kind::LumaSat : Kind::Hue, g_hueMode, c, avail, col, {}, ImTextureID(0)))
    changed = true;
  return changed;
}

}  // namespace color
}  // namespace ui
}  // namespace owcapp
