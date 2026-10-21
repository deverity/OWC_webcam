// Colour page: video scopes (waveform, RGB parade, vectorscope, histogram).
// The images come from owc::computeScopes on the colour worker thread; this
// file uploads them and draws Resolve-style graticules and labels on top.
#include <imgui.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui_color_internal.h"

namespace owcapp {
namespace ui {
namespace color {

namespace {

enum ScopeKind { kWaveform = 0, kParade = 1, kVector = 2, kHistogram = 3 };
const char* const kScopeNames[] = {"Waveform", "Parade RGB", "Vectorscope", "Histograma"};

struct ScopeState {
  ImageTexture tex[4];
  owc::ScopeImages img;
  ULONGLONG updated = 0;
  uint32_t want_w = 512, want_h = 256, want_v = 256;
  uint32_t matrix = 2;
};
ScopeState g_s;

ImTextureID texId(const ImageTexture& t) { return reinterpret_cast<ImTextureID>(t.srv()); }

uint32_t roundUp(float v, uint32_t q, uint32_t lo, uint32_t hi) {
  uint32_t n = static_cast<uint32_t>(std::max(0.0f, v));
  n = (n + q - 1) / q * q;
  return std::min(hi, std::max(lo, n));
}

void hLineLabel(ImDrawList* dl, float x0, float x1, float y, const char* lbl, float labelRight, bool strong) {
  dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), strong ? pal::gridStrong() : pal::grid(), 1.0f);
  if (lbl) {
    const ImVec2 ts = ImGui::CalcTextSize(lbl);
    dl->AddText(ImVec2(labelRight - ts.x, y - ts.y * 0.5f), pal::label(), lbl);
  }
}

void placeholder(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text) {
  const ImVec2 ts = ImGui::CalcTextSize(text);
  dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f), pal::label(), text);
}

// Waveform or parade in [p0, p1]: 10-bit style labels 0..1023 on the left.
void drawWave(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool parade, bool have) {
  const float lm = ImGui::CalcTextSize("1023").x + sc(8);
  const ImVec2 a(p0.x + lm, p0.y + sc(8)), b(p1.x - sc(6), p1.y - sc(8));
  dl->AddRectFilled(a, b, pal::u32(pal::inset()));
  if (have) dl->AddImage(texId(g_s.tex[parade ? kParade : kWaveform]), a, b);
  static const char* lbl[] = {"0", "256", "512", "768", "1023"};
  for (int i = 0; i <= 4; ++i) {
    const float y = b.y - (b.y - a.y) * static_cast<float>(i) / 4.0f;
    hLineLabel(dl, a.x, b.x, y, lbl[i], a.x - sc(5), i == 0 || i == 4);
  }
  // 10% steps, faint
  for (int i = 1; i < 10; ++i) {
    if (i % 25 == 0) continue;
    const float y = b.y - (b.y - a.y) * static_cast<float>(i) / 10.0f;
    dl->AddLine(ImVec2(a.x, y), ImVec2(a.x + sc(4), y), pal::gridStrong());
  }
  if (parade) {
    const float w3 = (b.x - a.x) / 3.0f;
    const ImU32 cc[3] = {IM_COL32(235, 90, 90, 220), IM_COL32(90, 210, 110, 220), IM_COL32(100, 150, 255, 230)};
    const char* nm[3] = {"R", "G", "B"};
    for (int i = 0; i < 3; ++i) {
      const float x = a.x + w3 * static_cast<float>(i);
      if (i > 0) dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), pal::gridStrong());
      dl->AddText(ImVec2(x + sc(5), a.y + sc(2)), cc[i], nm[i]);
    }
  }
  if (!have) placeholder(dl, a, b, "Sem dados");
}

void drawVector(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool have) {
  const float side = std::max(sc(40), std::min(p1.x - p0.x, p1.y - p0.y) - sc(12));
  const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  const ImVec2 a(c.x - side * 0.5f, c.y - side * 0.5f), b(c.x + side * 0.5f, c.y + side * 0.5f);
  const float R = side * 0.5f;
  dl->AddCircleFilled(c, R, pal::u32(pal::inset()), 96);
  if (have) dl->AddImage(texId(g_s.tex[kVector]), a, b);
  // full Cb/Cr scale (chroma +-0.5) is a circle of radius 0.45 of the image (owc/scopes.h)
  const float F = side * 0.45f;
  dl->AddCircle(c, F, pal::gridStrong(), 96, 1.0f);
  dl->AddCircle(c, F * 0.5f, pal::grid(), 64, 1.0f);
  dl->AddLine(ImVec2(c.x - F, c.y), ImVec2(c.x + F, c.y), pal::grid());
  dl->AddLine(ImVec2(c.x, c.y - F), ImVec2(c.x, c.y + F), pal::grid());
  // skin tone line
  const float ang = owc::vectorscopeSkinLineAngle();
  dl->AddLine(c, ImVec2(c.x + std::cos(ang) * F, c.y - std::sin(ang) * F), IM_COL32(230, 180, 140, 120), 1.0f);
  // 75% / 100% targets
  const auto targets = owc::vectorscopeTargets(g_s.matrix);
  const float bx = std::max(sc(4), side * 0.022f);
  for (const auto& t : targets) {
    const ImVec2 q100(a.x + t.x100 * side, a.y + t.y100 * side);
    const ImVec2 q75(a.x + t.x75 * side, a.y + t.y75 * side);
    dl->AddRect(ImVec2(q100.x - bx, q100.y - bx), ImVec2(q100.x + bx, q100.y + bx), IM_COL32(200, 200, 205, 150));
    dl->AddRect(ImVec2(q75.x - bx * 0.7f, q75.y - bx * 0.7f), ImVec2(q75.x + bx * 0.7f, q75.y + bx * 0.7f),
                IM_COL32(200, 200, 205, 90));
    // label just outside the 100% box, away from the centre
    float dx = q100.x - c.x, dy = q100.y - c.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d > 1.0f) { dx /= d; dy /= d; }
    const ImVec2 ts = ImGui::CalcTextSize(t.label);
    const ImVec2 lp(q100.x + dx * (bx + sc(9)) - ts.x * 0.5f, q100.y + dy * (bx + sc(9)) - ts.y * 0.5f);
    dl->AddText(lp, pal::label(), t.label);
  }
  if (!have) placeholder(dl, a, b, "Sem dados");
}

void drawHistogram(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool have) {
  const float th = ImGui::GetFontSize();
  const ImVec2 a(p0.x + sc(10), p0.y + sc(8)), b(p1.x - sc(10), p1.y - th - sc(8));
  dl->AddRectFilled(a, b, pal::u32(pal::inset()));
  if (have) dl->AddImage(texId(g_s.tex[kHistogram]), a, b);
  static const char* lbl[] = {"0", "256", "512", "768", "1023"};
  for (int i = 0; i <= 4; ++i) {
    const float x = a.x + (b.x - a.x) * static_cast<float>(i) / 4.0f;
    dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), (i == 0 || i == 4) ? pal::gridStrong() : pal::grid());
    const ImVec2 ts = ImGui::CalcTextSize(lbl[i]);
    float lx = x - ts.x * 0.5f;
    lx = std::max(a.x, std::min(lx, b.x - ts.x));
    dl->AddText(ImVec2(lx, b.y + sc(3)), pal::label(), lbl[i]);
  }
  dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), pal::gridStrong());
  if (!have) placeholder(dl, a, b, "Sem dados");
}

void drawOne(int kind, ImVec2 p0, ImVec2 p1, bool have) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(p0, p1, true);
  switch (kind) {
    case kWaveform: drawWave(dl, p0, p1, false, have); break;
    case kParade: drawWave(dl, p0, p1, true, have); break;
    case kVector: drawVector(dl, p0, p1, have); break;
    default: drawHistogram(dl, p0, p1, have); break;
  }
  dl->PopClipRect();
}

void noteWanted(int kind, ImVec2 p0, ImVec2 p1, uint32_t& w, uint32_t& h, uint32_t& v) {
  const float pw = p1.x - p0.x, ph = p1.y - p0.y;
  if (kind == kVector) v = std::max(v, roundUp(std::min(pw, ph), 32, 128, 512));
  else {
    w = std::max(w, roundUp(pw, 64, 256, 1024));
    h = std::max(h, roundUp(ph, 32, 128, 512));
  }
}

}  // namespace

owc::ScopeOptions scopeOptions() {
  owc::ScopeOptions o;
  o.width = g_s.want_w;
  o.height = g_s.want_h;
  o.vector_size = g_s.want_v;
  o.step = 2;
  o.gain = 1.0f;
  return o;
}

ImTextureID histogramTexture() {
  return g_s.tex[kHistogram].valid() && g_s.updated ? texId(g_s.tex[kHistogram]) : ImTextureID(0);
}

void releaseScopeTextures() {
  for (auto& t : g_s.tex) t.reset();
  g_s.updated = 0;
}

void drawScopes(App& app, Renderer& r, ImVec2 size) {
  // newest images from the worker
  if (worker().takeScopes(g_s.img)) {
    const owc::ScopeImages& im = g_s.img;
    bool ok = true;
    if (im.w && im.h && im.waveform.size() >= static_cast<size_t>(im.w) * im.h * 4) {
      ok &= g_s.tex[kWaveform].update(r.device(), r.context(), im.waveform.data(), im.w, im.h);
      if (im.parade.size() >= im.waveform.size())
        ok &= g_s.tex[kParade].update(r.device(), r.context(), im.parade.data(), im.w, im.h);
      if (im.histogram.size() >= im.waveform.size())
        ok &= g_s.tex[kHistogram].update(r.device(), r.context(), im.histogram.data(), im.w, im.h);
    }
    if (im.vs && im.vectorscope.size() >= static_cast<size_t>(im.vs) * im.vs * 4)
      ok &= g_s.tex[kVector].update(r.device(), r.context(), im.vectorscope.data(), im.vs, im.vs);
    if (ok) g_s.updated = GetTickCount64();
  }
  const int m = app.color_matrix;
  g_s.matrix = m == 1 || m == 2 ? static_cast<uint32_t>(m) : (app.stats.output_height >= 720 || app.stats.output_height == 0 ? 2u : 1u);
  // No staleness timeout: without live video the test chart feeds the scopes (and is
  // re-submitted as soon as the video goes away), so the last images are always meaningful.
  const bool have = g_s.updated != 0;

  if (!beginPanel("##scopes", size)) { endPanel(); return; }
  AppSettings& st = app.settings;
  // selector row
  auto combo = [&](const char* id, int& v) {
    ImGui::SetNextItemWidth(std::min(sc(150), ImGui::GetContentRegionAvail().x * 0.45f));
    if (ImGui::Combo(id, &v, kScopeNames, 4)) app.markDirty();
  };
  combo("##scopeA", st.color_scope_a);
  tip("Escopo exibido. Waveform: brilho (eixo vertical) ao longo da largura da imagem. Parade: o mesmo para R, G e B lado a lado. Vectorscope: matiz (ângulo) e saturação (distância do centro); a linha clara é a linha de tons de pele. Histograma: quantidade de pixels por nível.");
  ImGui::SameLine();
  if (ImGui::Checkbox("2 escopos", &st.color_scope_two)) app.markDirty();
  tip("Mostra dois escopos ao mesmo tempo.");
  if (st.color_scope_two) {
    ImGui::SameLine();
    combo("##scopeB", st.color_scope_b);
  }
  const ImVec2 c0 = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 c1(c0.x + avail.x, c0.y + std::max(sc(40), avail.y));
  uint32_t ww = 0, wh = 0, wv = 0;
  if (!st.color_scope_two) {
    drawOne(st.color_scope_a, c0, c1, have);
    noteWanted(st.color_scope_a, c0, c1, ww, wh, wv);
  } else {
    const bool side = avail.x > avail.y * 1.5f;
    const float gap = sc(6);
    ImVec2 a1, b0;
    if (side) {
      const float hw = (avail.x - gap) * 0.5f;
      a1 = ImVec2(c0.x + hw, c1.y);
      b0 = ImVec2(c0.x + hw + gap, c0.y);
    } else {
      const float hh = (c1.y - c0.y - gap) * 0.5f;
      a1 = ImVec2(c1.x, c0.y + hh);
      b0 = ImVec2(c0.x, c0.y + hh + gap);
    }
    drawOne(st.color_scope_a, c0, a1, have);
    drawOne(st.color_scope_b, b0, c1, have);
    noteWanted(st.color_scope_a, c0, a1, ww, wh, wv);
    noteWanted(st.color_scope_b, b0, c1, ww, wh, wv);
  }
  if (ww) { g_s.want_w = ww; g_s.want_h = wh; }
  if (wv) g_s.want_v = wv;
  ImGui::Dummy(ImVec2(avail.x, std::max(0.0f, avail.y - sc(1))));
  endPanel();
}

}  // namespace color
}  // namespace ui
}  // namespace owcapp
