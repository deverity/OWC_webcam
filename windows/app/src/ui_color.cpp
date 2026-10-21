// "Cor / LUT" tab: PC-side colour grading (adjustments + built-in looks + .cube LUT).
#include <imgui.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include <owc/color_grade.h>

#include "platform.h"
#include "ui_internal.h"

namespace owcapp {
namespace ui {

namespace {

ImVec4 mul(ImVec4 c, float k) { return ImVec4(c.x * k, c.y * k, c.z * k, c.w); }

const char kBuiltinPrefix[] = "builtin:";

bool isBuiltin(const std::string& lut) { return lut.rfind(kBuiltinPrefix, 0) == 0; }

// File name (UTF-8) of a path, for display.
std::string baseName(const std::string& path) {
  const size_t k = path.find_last_of("\\/");
  return k == std::string::npos ? path : path.substr(k + 1);
}

// Slider shown in percent (-100..+100) for a -1..+1 grade value, with a "Zerar" button.
bool pctSlider(const char* id, const char* label, const char* tipText, float& v) {
  ImGui::TextUnformatted(label);
  tip(tipText);
  bool changed = false;
  int pct = static_cast<int>(v * 100.0f + (v < 0 ? -0.5f : 0.5f));
  ImGui::SetNextItemWidth(-sc(64));
  const std::string sid = std::string("##") + id;
  if (ImGui::SliderInt(sid.c_str(), &pct, -100, 100, "%d", ImGuiSliderFlags_AlwaysClamp)) {
    v = static_cast<float>(pct) / 100.0f;
    changed = true;
  }
  tip("Arraste para ajustar. Ctrl+clique para digitar o valor.");
  ImGui::SameLine();
  ImGui::BeginDisabled(v == 0.0f);
  if (ImGui::SmallButton((std::string("Zerar##") + id).c_str())) { v = 0.0f; changed = true; }
  ImGui::EndDisabled();
  tip("Volta este ajuste para o valor neutro (0).");
  return changed;
}

}  // namespace

void tabColor(App& app) {
  owc::ClientOptions o = app.client.options();
  owc::ColorGrade& g = o.color;
  const owc::ClientStats& st = app.stats;
  bool commit = false;

  ImGui::PushStyleColor(ImGuiCol_Text, col::dim());
  ImGui::TextWrapped("O tratamento de cor é aplicado aqui no PC, na prévia, na webcam virtual e no OBS. As gravações não são afetadas: elas guardam o vídeo original do celular.");
  ImGui::PopStyleColor();

  sectionTitle("Geral");
  if (ImGui::Checkbox("Ativar tratamento de cor", &g.enabled)) commit = true;
  tip("Liga ou desliga todo o tratamento (ajustes e LUT). Desligado não custa nada de CPU.");
  if (ImGui::Checkbox("Comparar antes/depois (metade esquerda original)", &g.compare_split)) commit = true;
  tip("A metade esquerda da imagem fica sem tratamento, para você comparar. Atenção: enquanto estiver ligado, a saída da webcam virtual e do OBS também mostra a imagem dividida.");

  sectionTitle("Looks prontos");
  {
    const auto& looks = owc::builtinLooks();
    auto lookButton = [&](const char* name, const char* desc, bool active, const std::string& value) {
      const float w = ImGui::CalcTextSize(name).x + ImGui::GetStyle().FramePadding.x * 2 + sc(8);
      flowItem(w);
      if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, mul(col::accent(), 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::accent());
      }
      if (ImGui::Button(name)) {
        g.lut = value;
        if (!value.empty()) g.enabled = true;
        commit = true;
      }
      if (active) ImGui::PopStyleColor(2);
      tip(desc);
    };
    lookButton("Nenhum", "Sem LUT: só os ajustes manuais abaixo.", g.lut.empty(), std::string());
    for (const auto& l : looks)
      lookButton(l.name, l.description, g.lut == std::string(kBuiltinPrefix) + l.key, std::string(kBuiltinPrefix) + l.key);
    ImGui::NewLine();
  }

  sectionTitle("Arquivo LUT (.cube)");
  if (ImGui::Button("Carregar .cube...")) {
    std::string path;
    if (pickOpenFile(GetActiveWindow(), L"Carregar LUT", L"LUT 3D/1D (*.cube)", L"*.cube", path)) {
      g.lut = path;
      g.enabled = true;
      commit = true;
    }
  }
  tip("Carrega um LUT no formato .cube (3D ou 1D, padrão Adobe/Resolve). O arquivo é recarregado sozinho se for modificado.");
  const bool fileLut = !g.lut.empty() && !isBuiltin(g.lut);
  if (fileLut) {
    ImGui::SameLine();
    ImGui::TextColored(col::good(), "%s", baseName(g.lut).c_str());
    tip(g.lut.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Remover")) { g.lut.clear(); commit = true; }
    tip("Remove o LUT do arquivo (os ajustes manuais continuam).");
  }

  sectionTitle("Intensidade do LUT");
  {
    ImGui::BeginDisabled(g.lut.empty());
    int pct = static_cast<int>(g.lut_strength * 100.0f + 0.5f);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##lutstr", &pct, 0, 100, "%d %%", ImGuiSliderFlags_AlwaysClamp)) {
      g.lut_strength = static_cast<float>(pct) / 100.0f;
      commit = true;
    }
    ImGui::EndDisabled();
    tip("Mistura entre a imagem com os ajustes (0 %) e o resultado do LUT (100 %). Valores menores deixam o look mais sutil. Precisa de um look ou arquivo LUT selecionado.");
  }

  sectionTitle("Ajustes");
  {
    ImGui::TextUnformatted("Exposição");
    tip("Clareia ou escurece a imagem em stops (EV). +1 EV dobra a luz, -1 EV reduz pela metade.");
    ImGui::SetNextItemWidth(-sc(64));
    if (ImGui::SliderFloat("##exp", &g.exposure_ev, -3.0f, 3.0f, "%.2f EV", ImGuiSliderFlags_AlwaysClamp)) commit = true;
    tip("Arraste para ajustar. Ctrl+clique para digitar o valor.");
    ImGui::SameLine();
    ImGui::BeginDisabled(g.exposure_ev == 0.0f);
    if (ImGui::SmallButton("Zerar##exp")) { g.exposure_ev = 0.0f; commit = true; }
    ImGui::EndDisabled();
    tip("Volta este ajuste para o valor neutro (0).");
  }
  commit |= pctSlider("contrast", "Contraste", "Aumenta ou reduz a diferença entre claros e escuros (curva em S em torno do cinza médio).", g.contrast);
  commit |= pctSlider("highlights", "Realces", "Valores negativos recuperam áreas muito claras (céu, janelas); positivos as intensificam.", g.highlights);
  commit |= pctSlider("shadows", "Sombras", "Valores positivos clareiam as áreas escuras sem estourar o resto; negativos as escurecem.", g.shadows);
  commit |= pctSlider("saturation", "Saturação", "Intensidade de todas as cores. -100 deixa a imagem em preto e branco.", g.saturation);
  commit |= pctSlider("vibrance", "Vibração", "Como a saturação, mas atua mais nas cores apagadas e preserva tons de pele.", g.vibrance);
  commit |= pctSlider("temperature", "Temperatura", "Balanço de branco: positivo deixa a imagem mais quente (amarelada), negativo mais fria (azulada).", g.temperature);
  commit |= pctSlider("tint", "Matiz (tint)", "Corrige o desvio verde/magenta do balanço de branco: positivo vai para magenta, negativo para verde.", g.tint);

  ImGui::Dummy(ImVec2(0, sc(4)));
  const bool neutral = g.exposure_ev == 0 && g.contrast == 0 && g.highlights == 0 && g.shadows == 0 &&
                       g.saturation == 0 && g.vibrance == 0 && g.temperature == 0 && g.tint == 0;
  ImGui::BeginDisabled(neutral);
  if (ImGui::Button("Resetar ajustes")) {
    g.exposure_ev = g.contrast = g.highlights = g.shadows = 0.0f;
    g.saturation = g.vibrance = g.temperature = g.tint = 0.0f;
    commit = true;
  }
  ImGui::EndDisabled();
  tip("Zera todos os ajustes manuais. O LUT selecionado é mantido.");

  sectionTitle("Exportar");
  if (ImGui::Button("Exportar .cube...")) {
    std::string path;
    if (pickSaveFile(GetActiveWindow(), L"Exportar tratamento como LUT", L"LUT 3D (*.cube)", L"*.cube", L"cube",
                     L"owc-cor.cube", path)) {
      std::string err;
      if (owc::exportCube(g, path, 33, err)) app.toast("LUT exportado: " + baseName(path));
      else app.toast("Falha ao exportar o LUT: " + err, true, 6000);
    }
  }
  tip("Grava os ajustes atuais mais o LUT escolhido (na intensidade definida) em um arquivo .cube 3D de 33 pontos, que dá para usar no OBS, DaVinci Resolve, Premiere etc.");

  sectionTitle("Estado");
  if (st.grade_active) {
    ImGui::TextColored(col::good(), "Ativo");
    ImGui::SameLine();
    ImGui::TextColored(col::dim(), "%.1f ms por quadro", st.grade_ms_avg);
  } else {
    ImGui::TextColored(col::dim(), g.enabled ? "Inativo (nada a aplicar ou sem vídeo)" : "Inativo");
  }
  if (st.grade_lut_size > 0) {
    ImGui::TextColored(col::dim(), "LUT: %s (%u\xC2\xB3)", st.grade_lut_title.c_str(), st.grade_lut_size);
  }
  if (!st.grade_error.empty()) ImGui::TextColored(col::warn(), "%s", st.grade_error.c_str());

  if (commit) app.applyOptions(o);  // setOptions + markDirty (debounced save)
}

}  // namespace ui
}  // namespace owcapp
