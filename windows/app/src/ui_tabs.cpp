// Tabs: Presets, Saídas, Logs.
#include <imgui.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "platform.h"
#include "ui_internal.h"

namespace owcapp {
namespace ui {

namespace {

const char* oneLiner(owc::PresetId id) {
  switch (id) {
    case owc::PresetId::UltraLowLatency:
      return "Menor atraso possível (cerca de 1 quadro). Pode travar ou mostrar artefatos em Wi-Fi ruim.";
    case owc::PresetId::Balanced:
      return "Atraso moderado (+40 ms) com imagem mais estável e recuperação suave de perdas.";
    case owc::PresetId::MaxQuality:
      return "Melhor imagem (perfil High, B-frames, VBR). Adiciona 200 a 300 ms: ideal para gravar ou transmitir.";
  }
  return "";
}

char g_presetName[48] = "";

bool primary(const char* label, const ImVec2& sz = ImVec2(0, 0)) {
  const ImVec4 a = col::accent();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(a.x * 0.85f, a.y * 0.85f, a.z * 0.85f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, a);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(a.x * 0.7f, a.y * 0.7f, a.z * 0.7f, 1));
  bool r = ImGui::Button(label, sz);
  ImGui::PopStyleColor(3);
  return r;
}

}  // namespace

void tabPresets(App& app) {
  ImGui::PushStyleColor(ImGuiCol_Text, col::dim());
  ImGui::TextWrapped("Um clique ajusta o PC e o celular juntos. Depois você pode refinar cada valor nas outras abas.");
  ImGui::PopStyleColor();
  sectionTitle("Presets prontos");
  for (const auto& p : owc::builtinPresets()) {
    std::string id = std::string("preset_") + p.key;
    if (!beginCard(id.c_str())) { endCard(); continue; }
    ImGui::PushFont(fontBold);
    ImGui::TextUnformatted(p.name);
    ImGui::PopFont();
    if (app.preset_active == p.name) {
      ImGui::SameLine();
      ImGui::TextColored(col::good(), "  ativo");
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim());
    ImGui::TextWrapped("%s", oneLiner(p.id));
    ImGui::PopStyleColor();
    if (primary("Aplicar")) app.applyPresetBuiltin(p);
    tip(p.description);
    ImGui::SameLine();
    ImGui::TextDisabled("jitter %u ms - %zu parâmetros no celular", p.jitter_buffer_ms, p.phone.size());
    endCard();
  }

  sectionTitle("Personalizado");
  ImGui::TextWrapped("Salve a configuração atual (opções do PC e parâmetros do celular) para reutilizar.");
  ImGui::SetNextItemWidth(-(sc(120) + ImGui::GetStyle().ItemSpacing.x));
  ImGui::InputTextWithHint("##pname", "Nome do preset", g_presetName, sizeof g_presetName);
  ImGui::SameLine();
  ImGui::BeginDisabled(g_presetName[0] == 0);
  if (primary("Salvar atual", ImVec2(sc(120), 0))) {
    app.saveUserPreset(g_presetName);
    g_presetName[0] = 0;
  }
  ImGui::EndDisabled();
  tip("Salva em %APPDATA%\\OWC\\presets.json.");

  int del = -1;
  for (size_t i = 0; i < app.user_presets.size(); ++i) {
    const auto& u = app.user_presets[i];
    std::string id = "upreset_" + std::to_string(i);
    if (!beginCard(id.c_str())) { endCard(); continue; }
    ImGui::PushFont(fontBold);
    ImGui::TextUnformatted(u.name.c_str());
    ImGui::PopFont();
    if (app.preset_active == u.name) {
      ImGui::SameLine();
      ImGui::TextColored(col::good(), "  ativo");
    }
    ImGui::TextColored(col::dim(), "jitter %u ms - %zu parâmetros do celular", u.jitter_buffer_ms, u.phone.size());
    if (primary("Aplicar")) app.applyUserPreset(u);
    ImGui::SameLine();
    if (ImGui::Button("Excluir")) del = static_cast<int>(i);
    endCard();
  }
  if (del >= 0) app.deleteUserPreset(static_cast<size_t>(del));
  if (app.user_presets.empty()) ImGui::TextDisabled("Nenhum preset salvo ainda.");
}

// ============================================================================
void tabOutputs(App& app) {
  ImGui::PushStyleColor(ImGuiCol_Text, col::dim());
  ImGui::TextWrapped("O OWC publica o vídeo em um barramento de memória compartilhada. A webcam virtual e o plugin do OBS leem dele, então este app precisa continuar aberto (ele vai para a bandeja ao fechar a janela).");
  ImGui::PopStyleColor();

  const bool busy = app.scriptBusy();

  sectionTitle("Webcam virtual (DirectShow)");
  if (app.vcam_registered) {
    ImGui::TextColored(col::good(), "Registrada");
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", app.vcam_per_user ? "usuário atual" : "máquina inteira");
  } else {
    ImGui::TextColored(col::warn(), "Não registrada");
  }
  ImGui::BeginDisabled(busy);
  if (primary(app.vcam_registered ? "Reinstalar" : "Instalar", ImVec2(sc(130), 0))) app.runScript("vcam", true);
  tip("Executa scripts\\install-virtualcam.ps1 -PerUser (sem precisar de administrador).");
  ImGui::SameLine();
  ImGui::BeginDisabled(!app.vcam_registered);
  if (ImGui::Button("Remover", ImVec2(sc(130), 0))) app.runScript("vcam", false);
  ImGui::EndDisabled();
  tip("Executa scripts\\uninstall-virtualcam.ps1 -PerUser.");
  ImGui::EndDisabled();
  if (app.findScript(L"install-virtualcam.ps1").empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::warn());
    ImGui::TextWrapped("Script install-virtualcam.ps1 não encontrado ao lado do exe nem na estrutura do repositório.");
    ImGui::PopStyleColor();
  }

  sectionTitle("Plugin do OBS Studio");
  if (app.obs_installed) ImGui::TextColored(col::good(), "Instalado (ProgramData\\obs-studio\\plugins\\owc-obs-source)");
  else ImGui::TextColored(col::warn(), "Não instalado");
  ImGui::BeginDisabled(busy);
  if (primary(app.obs_installed ? "Reinstalar##obs" : "Instalar##obs", ImVec2(sc(130), 0))) app.runScript("obs", true);
  tip("Executa scripts\\install-obs-plugin.ps1. A pasta ProgramData pode exigir direitos de administrador.");
  ImGui::SameLine();
  ImGui::BeginDisabled(!app.obs_installed);
  if (ImGui::Button("Remover##obs", ImVec2(sc(130), 0))) app.runScript("obs", false);
  ImGui::EndDisabled();
  ImGui::EndDisabled();

  sectionTitle("Firewall (descoberta de celulares)");
  if (app.firewallState() == 1) {
    ImGui::TextColored(col::good(), "Regra de entrada presente (UDP 47803)");
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, col::warn());
    ImGui::TextWrapped("O Windows Firewall pode bloquear os anúncios dos celulares (UDP 47803). Sem a regra, aparelhos podem não aparecer na lista Wi-Fi.");
    ImGui::PopStyleColor();
    ImGui::BeginDisabled(app.firewallBusy());
    if (primary("Liberar no firewall", ImVec2(sc(180), 0))) app.addFirewallRule();
    tip("Executa netsh elevado (pede confirmação do UAC) para criar a regra de entrada no perfil Privado.");
    ImGui::SameLine();
    if (ImGui::Button("Verificar", ImVec2(sc(90), 0))) app.refreshFirewall();
    ImGui::EndDisabled();
    ImGui::TextDisabled("Ou, em um prompt de administrador:");
    std::string cmd = app.firewallCommand();
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim());
    ImGui::TextWrapped("%s", cmd.c_str());
    ImGui::PopStyleColor();
  }

  sectionTitle("Como usar");
  ImGui::PushStyleColor(ImGuiCol_Text, col::dim());
  ImGui::TextWrapped("Zoom, Teams, Chrome, Discord e outros apps: escolha a câmera \"OWC Webcam\" nas configurações de vídeo.");
  ImGui::Spacing();
  ImGui::TextWrapped("OBS Studio: adicione a fonte \"OWC Phone Camera\" (menor latência que capturar a webcam virtual). Alternativamente use \"Dispositivo de captura de vídeo\" com \"OWC Webcam\".");
  ImGui::PopStyleColor();

  sectionTitle("Saída dos scripts");
  std::string out = app.scriptOutput();
  if (busy) ImGui::TextColored(col::warn(), "Executando...");
  if (out.empty()) out = "(nenhuma execução ainda)";
  static std::string shown;
  shown = out;
  shown.reserve(shown.size() + 1);
  ImGui::InputTextMultiline("##scriptout", shown.data(), shown.capacity() + 1, ImVec2(-1, sc(170)),
                            ImGuiInputTextFlags_ReadOnly);
}

// ============================================================================
void tabLogs(App& app) {
  static int minLevel = 1;
  static bool autoScroll = true;
  static const char* levels[] = {"Debug", "Info", "Aviso", "Erro"};
  ImGui::SetNextItemWidth(sc(110));
  if (ImGui::Combo("Nível", &minLevel, levels, 4)) {
    app.settings.log_level = minLevel;
    app.markDirty();
  }
  ImGui::SameLine();
  ImGui::Checkbox("Rolar", &autoScroll);
  tip("Rola automaticamente para a linha mais recente.");

  std::vector<const LogLine*> shown;
  shown.reserve(app.log_lines.size());
  for (const auto& l : app.log_lines)
    if (static_cast<int>(l.level) >= minLevel) shown.push_back(&l);

  if (ImGui::Button("Copiar")) {
    std::string all;
    for (const LogLine* l : shown) {
      static const char* lv[] = {"DBG", "INF", "WRN", "ERR"};
      all += "[" + l->time + "] [" + (l->source == owc::LogSource::Phone ? "CEL" : "PC ") + "] " +
             lv[static_cast<int>(l->level) & 3] + " " + l->text + "\n";
    }
    ImGui::SetClipboardText(all.c_str());
    app.toast("Logs copiados para a área de transferência.", false, 2000);
  }
  tip("Copia as linhas visíveis (com o filtro atual).");
  ImGui::SameLine();
  if (ImGui::Button("Limpar")) app.clearLogs();
  ImGui::SameLine();
  ImGui::TextDisabled("%zu linhas", shown.size());

  ImGui::Separator();
  ImGui::BeginChild("##logs", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);
  const size_t first = shown.size() > 600 ? shown.size() - 600 : 0;  // keep wrapped rendering cheap
  if (first > 0) ImGui::TextDisabled("(%zu linhas anteriores ocultas; use Copiar para ver tudo)", first);
  for (size_t i = first; i < shown.size(); ++i) {
    const LogLine& l = *shown[i];
    ImVec4 c = l.level == owc::LogLevel::Error ? col::bad()
               : l.level == owc::LogLevel::Warn ? col::warn()
               : l.level == owc::LogLevel::Debug ? col::dim() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::TextColored(col::dim(), "%s %s", l.time.c_str() + 0, l.source == owc::LogSource::Phone ? "CEL" : "PC");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(l.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  }
  if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
}

}  // namespace ui
}  // namespace owcapp
