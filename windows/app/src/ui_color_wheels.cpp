// Colour page: primaries (Lift / Gamma / Gain / Offset colour wheels with
// master jog strips) and the row of numeric adjustments under them.
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

constexpr float kPi = 3.14159265358979f;
constexpr float kPuckReach = 0.86f;  // puck travel radius / wheel radius (stays inside the hue ring)

// Hue-ring colour at angle `a` (radians, CCW from +x, y up): the colour the
// puck pushes toward when moved in that direction (owc::wheelToRgb placement).
ImVec4 ringColor(float a, float sat) {
  float r, g, b;
  owc::wheelToRgb(std::cos(a), std::sin(a), r, g, b);
  const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
  const float span = mx - mn > 1e-6f ? mx - mn : 1.0f;
  float c[3] = {(r - mn) / span, (g - mn) / span, (b - mn) / span};
  for (float& v : c) v = 0.30f + (0.20f + 0.70f * v - 0.30f) * sat;
  return ImVec4(c[0], c[1], c[2], 1.0f);
}

void wheelDisk(ImDrawList* dl, ImVec2 c, float R) {
  const int N = 96;
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  const ImU32 centre = IM_COL32(44, 44, 46, 255);
  const float ring0 = R * 0.90f;
  // inner disk: fan from a grey centre to a faintly tinted edge
  dl->PrimReserve(N * 3, N + 1);
  const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
  dl->PrimWriteVtx(c, uv, centre);
  for (int i = 0; i < N; ++i) {
    const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(N);
    ImVec4 t = ringColor(a, 0.22f);
    t = ImVec4(t.x * 0.42f + 0.02f, t.y * 0.42f + 0.02f, t.z * 0.42f + 0.02f, 1.0f);
    dl->PrimWriteVtx(ImVec2(c.x + std::cos(a) * ring0, c.y - std::sin(a) * ring0), uv,
                     ImGui::ColorConvertFloat4ToU32(t));
  }
  for (int i = 0; i < N; ++i) {
    dl->PrimWriteIdx(base);
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + (i + 1) % N));
  }
  // hue ring
  dl->PrimReserve(N * 6, N * 2);
  const ImDrawIdx b2 = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
  for (int i = 0; i < N; ++i) {
    const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(N);
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(ringColor(a, 0.85f));
    dl->PrimWriteVtx(ImVec2(c.x + std::cos(a) * ring0, c.y - std::sin(a) * ring0), uv, col);
    dl->PrimWriteVtx(ImVec2(c.x + std::cos(a) * R, c.y - std::sin(a) * R), uv, col);
  }
  for (int i = 0; i < N; ++i) {
    const int j = (i + 1) % N;
    const ImDrawIdx i0 = static_cast<ImDrawIdx>(b2 + i * 2), i1 = static_cast<ImDrawIdx>(b2 + i * 2 + 1);
    const ImDrawIdx j0 = static_cast<ImDrawIdx>(b2 + j * 2), j1 = static_cast<ImDrawIdx>(b2 + j * 2 + 1);
    dl->PrimWriteIdx(i0); dl->PrimWriteIdx(i1); dl->PrimWriteIdx(j1);
    dl->PrimWriteIdx(i0); dl->PrimWriteIdx(j1); dl->PrimWriteIdx(j0);
  }
  dl->AddCircle(c, R, IM_COL32(20, 20, 22, 255), N, sc(1.5f));
  dl->AddCircle(c, ring0, IM_COL32(20, 20, 22, 200), N, 1.0f);
  // crosshair + 50 % reference
  const ImU32 cross = IM_COL32(255, 255, 255, 26);
  dl->AddLine(ImVec2(c.x - ring0, c.y), ImVec2(c.x + ring0, c.y), cross);
  dl->AddLine(ImVec2(c.x, c.y - ring0), ImVec2(c.x, c.y + ring0), cross);
  dl->AddCircle(c, ring0 * 0.5f, cross, 64);
}

struct DragState {
  ImGuiID id = 0;
  float px = 0, py = 0;
};
DragState g_drag;

// One wheel: disk + puck. Returns true when r/g/b changed.
bool wheelPuck(const char* id, owc::ColorWheel& w, float diam) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float R = diam * 0.5f;
  const ImVec2 c(p0.x + R, p0.y + R);
  wheelDisk(dl, c, R);
  ImGui::InvisibleButton(id, ImVec2(diam, diam));
  const ImGuiID gid = ImGui::GetItemID();
  bool changed = false;
  const ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    if (w.r != 0 || w.g != 0 || w.b != 0) {
      w.r = w.g = w.b = 0;
      changed = true;
    }
    g_drag.id = 0;
  } else if (ImGui::IsItemActivated()) {
    g_drag.id = gid;
    owc::rgbToWheel(w.r, w.g, w.b, g_drag.px, g_drag.py);
  }
  if (ImGui::IsItemActive() && g_drag.id == gid) {
    g_customDrag = true;
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
      const float k = (io.KeyShift ? 0.12f : 0.6f) / (R * kPuckReach);
      g_drag.px += io.MouseDelta.x * k;
      g_drag.py -= io.MouseDelta.y * k;
      const float d = std::sqrt(g_drag.px * g_drag.px + g_drag.py * g_drag.py);
      if (d > 1.0f) { g_drag.px /= d; g_drag.py /= d; }
      float r, g, b;
      owc::wheelToRgb(g_drag.px, g_drag.py, r, g, b);
      w.r = std::max(-1.0f, std::min(1.0f, r));
      w.g = std::max(-1.0f, std::min(1.0f, g));
      w.b = std::max(-1.0f, std::min(1.0f, b));
      changed = true;
    }
  } else if (g_drag.id == gid) {
    g_drag.id = 0;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
    ImGui::SetTooltip("Arraste para empurrar a cor (relativo; Shift = ajuste fino).\nDuplo clique: zera a cor desta roda.");

  // puck
  float px, py;
  if (g_drag.id == gid) { px = g_drag.px; py = g_drag.py; }
  else owc::rgbToWheel(w.r, w.g, w.b, px, py);
  const float reach = R * kPuckReach;
  const ImVec2 pk(c.x + px * reach, c.y - py * reach);
  if (px != 0 || py != 0) dl->AddLine(c, pk, IM_COL32(255, 255, 255, 70), 1.0f);
  const float pr = std::max(sc(4.5f), diam * 0.035f);
  dl->AddCircleFilled(pk, pr + sc(1.5f), IM_COL32(0, 0, 0, 160), 24);
  dl->AddCircle(pk, pr, IM_COL32(240, 240, 240, 255), 24, sc(1.6f));
  dl->AddCircleFilled(c, sc(1.6f), IM_COL32(255, 255, 255, 90), 12);
  return changed;
}

// Horizontal "jog" strip for the master value y (-1..1). Returns true when changed.
bool jogStrip(const char* id, float& y, float width) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float h = sc(14);
  const ImVec2 p1(p0.x + width, p0.y + h);
  ImGui::InvisibleButton(id, ImVec2(width, h));
  bool changed = false;
  const ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    if (y != 0) { y = 0; changed = true; }
  } else if (ImGui::IsItemActive()) {
    g_customDrag = true;
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (io.MouseDelta.x != 0) {
      y += io.MouseDelta.x / width * (io.KeyShift ? 0.08f : 0.5f);
      y = std::max(-1.0f, std::min(1.0f, y));
      changed = true;
    }
  }
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  dl->AddRectFilled(p0, p1, hot ? IM_COL32(40, 40, 42, 255) : IM_COL32(30, 30, 32, 255), sc(3));
  // ticks moving with the value (like a physical jog wheel)
  const float step = sc(7);
  float off = std::fmod(y * width * 2.0f, step);
  if (off < 0) off += step;
  dl->PushClipRect(p0, p1, true);
  for (float x = p0.x + off; x < p1.x; x += step) {
    const float t = std::fabs((x - (p0.x + p1.x) * 0.5f) / (width * 0.5f));
    const int a = static_cast<int>(110 * (1.0f - t * t));
    dl->AddLine(ImVec2(x, p0.y + sc(3)), ImVec2(x, p1.y - sc(3)), IM_COL32(200, 200, 205, std::max(0, a)));
  }
  // value bar from the centre
  const float cx = (p0.x + p1.x) * 0.5f;
  const float vx = cx + y * width * 0.5f;
  if (y != 0)
    dl->AddRectFilled(ImVec2(std::min(cx, vx), p1.y - sc(3)), ImVec2(std::max(cx, vx), p1.y),
                      pal::u32(pal::accent()));
  dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p1.y), IM_COL32(255, 255, 255, 60));
  dl->PopClipRect();
  dl->AddRect(p0, p1, pal::u32(pal::border()), sc(3));
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
    ImGui::SetTooltip("Mestre (Y): arraste para os lados (Shift = ajuste fino).\nDuplo clique: zera.");
  return changed;
}

// Y R G B readout: four compact drag fields in the channel colours.
bool readout(const char* id, owc::ColorWheel& w, float width) {
  bool changed = false;
  float* v[4] = {&w.y, &w.r, &w.g, &w.b};
  const ImVec4 cols[4] = {ImVec4(0.86f, 0.86f, 0.86f, 1), ImVec4(0.95f, 0.45f, 0.45f, 1), ImVec4(0.45f, 0.85f, 0.50f, 1),
                          ImVec4(0.50f, 0.65f, 1.0f, 1)};
  const char* names[4] = {"Y (mestre)", "R", "G", "B"};
  const float sp = sc(3);
  const float fw = (width - sp * 3) / 4.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(sc(2), sc(2)));
  for (int i = 0; i < 4; ++i) {
    if (i) ImGui::SameLine();
    ImGui::PushID(i);
    ImGui::PushStyleColor(ImGuiCol_Text, cols[i]);
    ImGui::SetNextItemWidth(fw);
    if (ImGui::DragFloat(id, v[i], 0.002f, -1.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) changed = true;
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !ImGui::IsItemActive())
      ImGui::SetTooltip("%s: %.3f\nArraste ou Ctrl+clique para digitar.", names[i], static_cast<double>(*v[i]));
    ImGui::PopID();
  }
  ImGui::PopStyleVar(2);
  return changed;
}

struct WheelInfo {
  const char* name;
  const char* tipText;
};
const WheelInfo kWheels[4] = {
    {"Lift", "Lift: sombras. Empurra a cor e o nível dos pretos, mantendo o branco fixo."},
    {"Gamma", "Gamma: meios-tons. Curva a imagem mantendo o preto e o branco fixos."},
    {"Gain", "Gain: realces. Multiplica a imagem (o preto fica fixo)."},
    {"Offset", "Offset: desloca a imagem inteira (pretos, meios-tons e brancos)."},
};

// Compact labelled drag field of the numeric row. `pct` shows -1..1 as -100..100.
bool numField(const char* id, const char* label, const char* tipText, float& v, float lo, float hi, float def,
              bool pct, const char* fmt, float width) {
  ImGui::PushID(id);
  ImGui::BeginGroup();
  ImGui::PushStyleColor(ImGuiCol_Text, v != def ? pal::text() : pal::dim());
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  bool changed = false;
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && v != def) {
    v = def;
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted(tipText);
    ImGui::TextColored(pal::dim(), "Duplo clique no nome: volta ao padrão.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  ImGui::SetNextItemWidth(width);
  if (pct) {
    float p = v * 100.0f;
    if (ImGui::DragFloat("##v", &p, 0.25f, lo * 100.0f, hi * 100.0f, fmt, ImGuiSliderFlags_AlwaysClamp)) {
      v = p / 100.0f;
      changed = true;
    }
  } else {
    if (ImGui::DragFloat("##v", &v, (hi - lo) / 600.0f, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) changed = true;
  }
  ImGui::EndGroup();
  ImGui::PopID();
  return changed;
}

}  // namespace

bool drawWheels(owc::ColorGrade& g, ImVec2 size) {
  bool changed = false;
  owc::ColorWheel* wheels[4] = {&g.lift, &g.gamma, &g.gain, &g.offset};
  struct Num {
    const char* id;
    const char* label;
    const char* tipText;
    float* v;
    float lo, hi, def;
    bool pct;
    const char* fmt;
  };
  const Num nums[] = {
      {"con", "Contraste", "Aumenta ou reduz a diferença entre claros e escuros (curva em S em torno do pivot).",
       &g.contrast, -1, 1, 0, true, "%+.0f"},
      {"piv", "Pivot", "Ponto central do contraste (0,435 = cinza médio, como no Resolve). Valores menores protegem as sombras.",
       &g.pivot, 0.1f, 0.9f, 0.435f, false, "%.3f"},
      {"sat", "Saturação", "Intensidade de todas as cores. -100 deixa a imagem em preto e branco.", &g.saturation, -1, 1,
       0, true, "%+.0f"},
      {"vib", "Vibração", "Como a saturação, mas atua mais nas cores apagadas e preserva tons de pele.", &g.vibrance, -1,
       1, 0, true, "%+.0f"},
      {"tmp", "Temperatura", "Balanço de branco: positivo deixa a imagem mais quente (amarelada), negativo mais fria (azulada).",
       &g.temperature, -1, 1, 0, true, "%+.0f"},
      {"tnt", "Matiz (tint)", "Corrige o desvio verde/magenta: positivo vai para magenta, negativo para verde.", &g.tint,
       -1, 1, 0, true, "%+.0f"},
      {"exp", "Exposição", "Clareia ou escurece a imagem em stops (EV). +1 EV dobra a luz.", &g.exposure_ev, -3, 3, 0,
       false, "%+.2f EV"},
      {"hil", "Realces", "Negativo recupera áreas muito claras (céu, janelas); positivo as intensifica.", &g.highlights,
       -1, 1, 0, true, "%+.0f"},
      {"shd", "Sombras", "Positivo clareia as áreas escuras sem estourar o resto; negativo as escurece.", &g.shadows, -1,
       1, 0, true, "%+.0f"},
  };
  const int n = static_cast<int>(sizeof(nums) / sizeof(nums[0]));
  const float numGap = sc(10);
  int perRow = std::max(1, std::min(n, static_cast<int>((size.x + numGap) / (sc(84) + numGap))));
  if (perRow < n && perRow > (n + 1) / 2) perRow = (n + 1) / 2;  // balance two rows
  const int rows = (n + perRow - 1) / perRow;

  const ImGuiStyle& style = ImGui::GetStyle();
  const float fh = ImGui::GetFrameHeight();
  const float th = ImGui::GetTextLineHeight();
  const float jogH = sc(14), gapS = sc(6);
  const float numRowH = static_cast<float>(rows) * (th + style.ItemSpacing.y + fh) +
                        static_cast<float>(rows - 1) * style.ItemSpacing.y + sc(12);
  const float overhead = th + gapS + gapS + jogH + gapS + fh;
  const float colGap = sc(14);
  float colW = (size.x - colGap * 3) / 4.0f;
  float diam = std::min(colW - sc(16), size.y - numRowH - overhead);
  diam = std::max(sc(64), std::min(diam, sc(250)));
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  // keep the four wheels together (centred) on wide panels, like Resolve
  colW = std::min(colW, diam + sc(110));
  const float wheelsX = origin.x + std::max(0.0f, (size.x - (colW * 4 + colGap * 3)) * 0.5f);
  const float yDisk = origin.y + th + gapS;
  const float yJog = yDisk + diam + gapS;
  const float yRo = yJog + jogH + gapS;
  const float yEnd = yRo + fh;
  for (int i = 0; i < 4; ++i) {
    owc::ColorWheel& w = *wheels[i];
    ImGui::PushID(i);
    const float x0 = wheelsX + (colW + colGap) * static_cast<float>(i);
    const float cx = x0 + colW * 0.5f;
    const float bw = std::min(colW, std::max(diam + sc(24), sc(170)));  // readout needs room for "-0.05"
    // title + reset
    ImGui::PushFont(fontBold);
    const ImVec2 ts = ImGui::CalcTextSize(kWheels[i].name);
    ImGui::SetCursorScreenPos(ImVec2(cx - ts.x * 0.5f, origin.y));
    ImGui::PushStyleColor(ImGuiCol_Text, w.isNeutral() ? pal::text() : pal::accent());
    ImGui::TextUnformatted(kWheels[i].name);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    tip(kWheels[i].tipText);
    ImGui::SetCursorScreenPos(ImVec2(cx + bw * 0.5f - th, origin.y));
    if (resetIcon("##rst", th, !w.isNeutral(), "Zera esta roda (cor e mestre).")) {
      w = owc::ColorWheel();
      changed = true;
    }
    ImGui::SetCursorScreenPos(ImVec2(cx - diam * 0.5f, yDisk));
    changed |= wheelPuck("##wheel", w, diam);
    ImGui::SetCursorScreenPos(ImVec2(cx - bw * 0.5f, yJog));
    changed |= jogStrip("##jog", w.y, bw);
    ImGui::SetCursorScreenPos(ImVec2(cx - bw * 0.5f, yRo));
    changed |= readout("##ro", w, bw);
    ImGui::PopID();
  }
  // numeric row under the wheels
  const float rowY = yEnd + sc(8);
  ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x, rowY), ImVec2(origin.x + size.x, rowY), pal::u32(pal::border()));
  const float fw = (size.x - numGap * static_cast<float>(perRow - 1)) / static_cast<float>(perRow);
  const float rowStep = th + style.ItemSpacing.y + fh + style.ItemSpacing.y;
  for (int i = 0; i < n; ++i) {
    const int rr = i / perRow, cc = i % perRow;
    ImGui::SetCursorScreenPos(
        ImVec2(origin.x + (fw + numGap) * static_cast<float>(cc), rowY + sc(5) + rowStep * static_cast<float>(rr)));
    const Num& m = nums[i];
    changed |= numField(m.id, m.label, m.tipText, *m.v, m.lo, m.hi, m.def, m.pct, m.fmt, fw);
  }
  return changed;
}

}  // namespace color
}  // namespace ui
}  // namespace owcapp
