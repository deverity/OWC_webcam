// Colour page: LUT gallery. Thumbnails of "Nenhum", every built-in look, the
// recently loaded .cube files and the .cube files of the user's LUT folders,
// rendered on the colour worker with owc::renderGradedRgba (LUT only, strength
// 1) from an UNGRADED source:
//   * the last ungraded video snapshot (taken by ui_color.cpp only while the
//     grade is identity and the engine reports no table applied), or
//   * the built-in test chart when no such snapshot exists yet.
// The preview frames themselves are already graded, so they are never used
// directly while grading is on (that would apply the grade twice).
#include <imgui.h>
#include <imgui_internal.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <owc/color_grade.h>

#include "platform.h"
#include "ui_color_internal.h"

namespace owcapp {
namespace ui {
namespace color {

namespace {

const char kBuiltinPrefix[] = "builtin:";
constexpr uint64_t kChartGen = ~0ull;

struct Thumb {
  ImageTexture tex;
  uint64_t gen = 0;
  bool rendered = false;
  bool pending = false;
  ULONGLONG done = 0;
  std::string err;
};
std::map<std::string, Thumb> g_thumbs;

struct Folder {
  std::string path;
  std::vector<std::string> files;
};
std::vector<Folder> g_folders;
std::vector<std::string> g_scannedFor;  // folder list the scan reflects

uint64_t g_snapGenSeen = 0;
ULONGLONG g_snapTick = 0;

struct Entry {
  std::string value;  // ColorGrade::lut value
  std::string title;
  std::string desc;
  bool recent = false;
};

void rescanFolders(const std::vector<std::string>& dirs) {
  g_folders.clear();
  for (const auto& d : dirs) g_folders.push_back(Folder{d, listFiles(d, L".cube")});
  g_scannedFor = dirs;
}

std::string stripExt(const std::string& name) {
  const size_t k = name.find_last_of('.');
  return k == std::string::npos || k == 0 ? name : name.substr(0, k);
}

// One gallery tile. Returns true when clicked.
bool tile(const Entry& e, bool active, float w, float imgH, const std::shared_ptr<const Nv12Image>& src,
          uint64_t srcGen, bool& removeRecent) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float th = ImGui::GetTextLineHeight();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::PushID(e.value.c_str());
  const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(w, imgH + th + sc(6)));
  const bool hot = ImGui::IsItemHovered();
  Thumb& t = g_thumbs[e.value];
  if (ImGui::IsItemVisible()) {
    const ULONGLONG now = GetTickCount64();
    if (!t.pending && (!t.rendered || (t.gen != srcGen && now - t.done >= 2000))) {
      owc::ColorGrade tg;
      tg.enabled = true;
      tg.lut = e.value;
      tg.lut_strength = 1.0f;
      worker().requestThumb(e.value, tg, src, srcGen);
      t.pending = true;
    }
  }
  const ImVec2 i0 = p0, i1(p0.x + w, p0.y + imgH);
  dl->AddRectFilled(i0, i1, pal::u32(pal::inset()), sc(3));
  if (t.rendered && t.tex.valid()) {
    dl->AddImageRounded(reinterpret_cast<ImTextureID>(t.tex.srv()), i0, i1, ImVec2(0, 0), ImVec2(1, 1),
                        IM_COL32_WHITE, sc(3));
  } else {
    const char* dots = "...";
    const ImVec2 ts = ImGui::CalcTextSize(dots);
    dl->AddText(ImVec2((i0.x + i1.x - ts.x) * 0.5f, (i0.y + i1.y - ts.y) * 0.5f), pal::label(), dots);
  }
  if (!t.err.empty() && !e.value.empty()) {
    const ImVec2 c(i1.x - sc(10), i0.y + sc(10));
    dl->AddCircleFilled(c, sc(7), IM_COL32(200, 120, 30, 230));
    const ImVec2 ts = ImGui::CalcTextSize("!");
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(20, 20, 20, 255), "!");
  }
  const ImU32 bd = active ? pal::u32(pal::accent()) : hot ? IM_COL32(150, 150, 155, 255) : pal::u32(pal::border());
  dl->AddRect(ImVec2(i0.x - 1, i0.y - 1), ImVec2(i1.x + 1, i1.y + 1), bd, sc(3), 0, active ? sc(2.5f) : 1.0f);
  // title (ellipsis)
  const ImVec2 tp(p0.x, i1.y + sc(3));
  ImGui::PushStyleColor(ImGuiCol_Text, active ? pal::accent() : (hot ? pal::text() : pal::dim()));
  const ImVec2 tsz = ImGui::CalcTextSize(e.title.c_str());
  const float tx = tsz.x < w ? p0.x + (w - tsz.x) * 0.5f : p0.x;
  ImGui::RenderTextEllipsis(dl, ImVec2(tx, tp.y), ImVec2(p0.x + w, tp.y + th), p0.x + w, p0.x + w, e.title.c_str(),
                            nullptr, &tsz);
  ImGui::PopStyleColor();

  if (hot) {
    ImGui::BeginTooltip();
    if (t.rendered && t.tex.valid())
      ImGui::Image(reinterpret_cast<ImTextureID>(t.tex.srv()), ImVec2(sc(384), sc(216)));
    ImGui::PushFont(fontBold);
    ImGui::TextUnformatted(e.title.c_str());
    ImGui::PopFont();
    ImGui::PushTextWrapPos(sc(384));
    if (!e.desc.empty()) ImGui::TextColored(pal::dim(), "%s", e.desc.c_str());
    if (!t.err.empty() && !e.value.empty()) ImGui::TextColored(col::warn(), "%s", t.err.c_str());
    ImGui::TextColored(pal::dim(), active ? "Em uso." : "Clique para aplicar.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  if (e.recent && ImGui::BeginPopupContextItem("##ctx")) {
    if (ImGui::MenuItem("Remover da lista de recentes")) removeRecent = true;
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return clicked;
}

void groupTitle(const char* text) {
  ImGui::Dummy(ImVec2(0, sc(2)));
  ImGui::PushFont(fontBold);
  ImGui::TextColored(pal::dim(), "%s", text);
  ImGui::PopFont();
}

}  // namespace

void releaseGalleryTextures() { g_thumbs.clear(); }

std::string baseName(const std::string& path) {
  const size_t k = path.find_last_of("\\/");
  return k == std::string::npos ? path : path.substr(k + 1);
}

std::string lutDisplayName(const std::string& lut) {
  if (lut.empty()) return "Nenhum";
  if (lut.rfind(kBuiltinPrefix, 0) == 0) {
    const std::string key = lut.substr(sizeof(kBuiltinPrefix) - 1);
    for (const auto& l : owc::builtinLooks())
      if (key == l.key) return l.name;
    return key;
  }
  return stripExt(baseName(lut));
}

void rememberLut(App& app, const std::string& path) {
  if (path.empty() || path.rfind(kBuiltinPrefix, 0) == 0) return;
  auto& v = app.settings.color_recent_luts;
  v.erase(std::remove(v.begin(), v.end(), path), v.end());
  v.insert(v.begin(), path);
  if (v.size() > 24) v.resize(24);
  app.markDirty();
}

bool loadCubeDialog(App& app, owc::ColorGrade& g) {
  std::string path;
  if (!pickOpenFile(GetActiveWindow(), L"Carregar LUT", L"LUT 3D/1D (*.cube)", L"*.cube", path)) return false;
  g.lut = path;
  g.enabled = true;
  rememberLut(app, path);
  return true;
}

void exportCubeDialog(App& app, const owc::ColorGrade& g) {
  std::string path;
  if (pickSaveFile(GetActiveWindow(), L"Exportar tratamento como LUT", L"LUT 3D (*.cube)", L"*.cube", L"cube",
                   L"owc-cor.cube", path)) {
    std::string err;
    if (owc::exportCube(g, path, 33, err)) app.toast("LUT exportado: " + baseName(path));
    else app.toast("Falha ao exportar o LUT: " + err, true, 6000);
  }
}

bool drawGallery(App& app, Renderer& r, owc::ColorGrade& g, ImVec2 size) {
  bool changed = false;
  AppSettings& st = app.settings;
  if (g_scannedFor != st.color_lut_folders) rescanFolders(st.color_lut_folders);

  // ---- finished thumbnails -> textures (a few per UI frame)
  {
    ThumbResult res;
    const ULONGLONG now = GetTickCount64();
    for (int n = 0; n < 6 && worker().takeThumb(res); ++n) {
      Thumb& t = g_thumbs[res.key];
      if (!res.rgba.empty()) t.tex.update(r.device(), r.context(), res.rgba.data(), res.w, res.h);
      t.gen = res.src_gen;
      t.rendered = true;
      t.pending = false;
      t.done = now;
      t.err = res.err;
    }
  }
  // ---- source of the thumbnails
  uint64_t snapGen = 0;
  std::shared_ptr<const Nv12Image> src = worker().snapshot(&snapGen);
  if (snapGen != g_snapGenSeen) {
    g_snapGenSeen = snapGen;
    g_snapTick = GetTickCount64();
  }
  uint64_t srcGen = snapGen;
  if (!src) {
    src = ColorWorker::chartThumbSource();
    srcGen = kChartGen;
  }

  // ---- toolbar
  if (ImGui::Button("Carregar .cube...")) changed |= loadCubeDialog(app, g);
  tip("Carrega um LUT no formato .cube (3D ou 1D, padrão Adobe/Resolve) e o aplica. Ele entra na lista de recentes. O arquivo é recarregado sozinho se for modificado.");
  ImGui::SameLine();
  if (ImGui::Button("Adicionar pasta...")) {
    std::string dir;
    if (pickFolder(GetActiveWindow(), L"Pasta com LUTs (.cube)", dir)) {
      auto& f = st.color_lut_folders;
      if (std::find(f.begin(), f.end(), dir) == f.end() && f.size() < 16) f.push_back(dir);
      app.markDirty();
      rescanFolders(f);
    }
  }
  tip("Mostra na galeria todos os arquivos .cube de uma pasta (sem subpastas).");
  ImGui::SameLine();
  if (ImGui::Button("Exportar .cube...")) exportCubeDialog(app, g);
  tip("Grava os ajustes atuais mais o LUT escolhido (na intensidade definida) em um .cube 3D de 33 pontos, para usar no OBS, DaVinci Resolve, Premiere etc.");
  ImGui::SameLine(0, sc(18));
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(pal::dim(), "Intensidade");
  ImGui::SameLine();
  ImGui::BeginDisabled(g.lut.empty());
  {
    int pct = static_cast<int>(g.lut_strength * 100.0f + 0.5f);
    ImGui::SetNextItemWidth(sc(150));
    if (ImGui::SliderInt("##lutstr", &pct, 0, 100, "%d %%", ImGuiSliderFlags_AlwaysClamp)) {
      g.lut_strength = static_cast<float>(pct) / 100.0f;
      changed = true;
    }
  }
  tip("Mistura entre a imagem só com os ajustes (0 %) e o resultado do LUT (100 %).");
  ImGui::SameLine();
  if (ImGui::Button("Remover LUT")) { g.lut.clear(); changed = true; }
  tip("Tira o LUT (os ajustes, rodas e curvas continuam).");
  ImGui::EndDisabled();
  {
    std::string srcText;
    if (srcGen == kChartGen) {
      srcText = "Miniaturas: imagem de teste (ainda sem quadro de vídeo sem tratamento)";
    } else {
      const ULONGLONG age = (GetTickCount64() - g_snapTick) / 1000;
      char b[160];
      if (age < 5) std::snprintf(b, sizeof b, "Miniaturas: vídeo ao vivo (sem tratamento)");
      else if (age < 120) std::snprintf(b, sizeof b, "Miniaturas: último quadro sem tratamento (há %llu s)", static_cast<unsigned long long>(age));
      else std::snprintf(b, sizeof b, "Miniaturas: último quadro sem tratamento (há %llu min)", static_cast<unsigned long long>(age / 60));
      srcText = b;
    }
    ImGui::TextColored(pal::dim(), "%s", srcText.c_str());
    tip("As miniaturas mostram só o LUT de cada item, aplicado a um quadro SEM tratamento (o vídeo da prévia já sai tratado). Com o tratamento ligado, é usado o último quadro capturado com ele desligado; sem nenhum, uma imagem de teste. Atualizadas a cada ~2 s.");
  }

  // ---- grid
  std::vector<Entry> looks, recent;
  looks.push_back(Entry{"", "Nenhum", "Sem LUT: só os ajustes, rodas e curvas.", false});
  for (const auto& l : owc::builtinLooks())
    looks.push_back(Entry{std::string(kBuiltinPrefix) + l.key, l.name, l.description, false});
  for (const auto& p : st.color_recent_luts) recent.push_back(Entry{p, stripExt(baseName(p)), p, true});

  const float gridH = std::max(sc(60), ImGui::GetContentRegionAvail().y);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::BeginChild("##grid", ImVec2(size.x, gridH), ImGuiChildFlags_None);
  const float gap = sc(10);
  const float availW = ImGui::GetContentRegionAvail().x;
  const int cols = std::max(1, static_cast<int>((availW + gap) / (sc(176) + gap)));
  const float tw = (availW - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
  const float ih = tw * 9.0f / 16.0f;
  std::string removeKey;
  auto grid = [&](const std::vector<Entry>& es) {
    for (size_t i = 0; i < es.size(); ++i) {
      if (i % static_cast<size_t>(cols) != 0) ImGui::SameLine(0, gap);
      bool rm = false;
      if (tile(es[i], g.lut == es[i].value, tw, ih, src, srcGen, rm)) {
        g.lut = es[i].value;
        if (!g.lut.empty()) g.enabled = true;
        changed = true;
      }
      if (rm) removeKey = es[i].value;
    }
  };
  groupTitle("Looks prontos");
  ImGui::PushID("looks");
  grid(looks);
  ImGui::PopID();
  if (!recent.empty()) {
    groupTitle("Arquivos recentes");
    ImGui::PushID("recent");
    grid(recent);
    ImGui::PopID();
  }
  std::string removeFolder;
  for (const auto& f : g_folders) {
    ImGui::PushID(f.path.c_str());
    char t[300];
    std::snprintf(t, sizeof t, "Pasta: %s (%zu)", baseName(f.path).c_str(), f.files.size());
    groupTitle(t);
    tip(f.path.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Atualizar")) rescanFolders(st.color_lut_folders);
    tip("Lê a pasta de novo.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Tirar da galeria")) removeFolder = f.path;
    tip("Para de mostrar esta pasta (os arquivos não são apagados).");
    std::vector<Entry> es;
    for (const auto& p : f.files) es.push_back(Entry{p, stripExt(baseName(p)), p, false});
    if (es.empty()) ImGui::TextColored(pal::dim(), "Nenhum arquivo .cube nesta pasta.");
    grid(es);
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  if (!removeKey.empty()) {
    auto& v = st.color_recent_luts;
    v.erase(std::remove(v.begin(), v.end(), removeKey), v.end());
    app.markDirty();
  }
  if (!removeFolder.empty()) {
    auto& v = st.color_lut_folders;
    v.erase(std::remove(v.begin(), v.end(), removeFolder), v.end());
    app.markDirty();
  }
  return changed;
}

}  // namespace color
}  // namespace ui
}  // namespace owcapp
