// "Gravação" tab and the header record button.
#include <imgui.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <set>

#include "platform.h"
#include "ui_internal.h"

namespace owcapp {
namespace ui {

std::string fmtBytes(uint64_t b) {
  char buf[48];
  if (b >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(b) / static_cast<double>(1ull << 30));
  else if (b >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / static_cast<double>(1ull << 20));
  else std::snprintf(buf, sizeof buf, "%.0f KB", static_cast<double>(b) / 1024.0);
  return buf;
}

std::string fmtClock(double seconds) {
  const long long t = static_cast<long long>(std::max(0.0, seconds));
  char buf[32];
  if (t >= 3600) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
  else std::snprintf(buf, sizeof buf, "%02lld:%02lld", t / 60, t % 60);
  return buf;
}

namespace {

const ImVec4 kRed(0.86f, 0.18f, 0.20f, 1.0f);

double wallElapsed(const App& app) {
  return app.rec_phase == RecPhase::Recording ? static_cast<double>(GetTickCount64() - app.rec_t0) / 1000.0 : 0.0;
}

bool pickFolder(HWND owner, std::string& out) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  dlg->SetTitle(L"Pasta de destino das gravações");
  bool ok = false;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item)) && item) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        out = toUtf8(p);
        ok = true;
        CoTaskMemFree(p);
      }
      item->Release();
    }
  }
  dlg->Release();
  return ok;
}

void shellOpen(const std::string& path) {
  ShellExecuteW(nullptr, L"open", fromUtf8(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void showInFolder(const std::string& file) {
  const std::wstring args = L"/select,\"" + fromUtf8(file) + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

const char* stateName(owc::RecordState s) {
  switch (s) {
    case owc::RecordState::Idle: return "Parada";
    case owc::RecordState::WaitingForKeyframe: return "Aguardando quadro-chave";
    case owc::RecordState::Recording: return "Gravando";
    case owc::RecordState::Error: return "Erro";
  }
  return "?";
}

void row(const char* label, const std::string& value, const ImVec4* c = nullptr) {
  ImGui::TextColored(col::dim(), "%s", label);
  ImGui::SameLine(sc(170));
  if (c) ImGui::TextColored(*c, "%s", value.c_str());
  else ImGui::TextUnformatted(value.c_str());
}

// Distinct sizes of the active camera, largest first.
std::vector<owc::VideoSize> cameraSizes(const App& app) {
  std::vector<owc::VideoSize> v;
  if (!app.caps_valid || app.caps.active_camera >= app.caps.cameras.size()) return v;
  std::set<std::pair<int, int>> seen;
  for (const auto& s : app.caps.cameras[app.caps.active_camera].sizes)
    if (seen.insert({s.width, s.height}).second) v.push_back(s);
  std::sort(v.begin(), v.end(), [](const owc::VideoSize& a, const owc::VideoSize& b) {
    return static_cast<int>(a.width) * a.height > static_cast<int>(b.width) * b.height;
  });
  return v;
}

bool slowmoAdvertised(const App& app, int w, int h, uint16_t* fps = nullptr) {
  if (!app.caps_valid || app.caps.active_camera >= app.caps.cameras.size()) return false;
  bool any = false;
  for (const auto& s : app.caps.cameras[app.caps.active_camera].sizes)
    if (s.width == w && s.height == h && s.max_fps >= 120) {
      any = true;
      if (fps) *fps = std::max(*fps, s.max_fps);
    }
  return any;
}

}  // namespace

void recordButton(App& app, float width) {
  const bool rec = app.rec_phase != RecPhase::Idle;
  const bool streaming = app.stats.state == owc::ClientState::Streaming;
  ImGui::BeginDisabled(!rec && !streaming);
  std::string label;
  if (app.rec_phase == RecPhase::Preparing) label = "Preparando...";
  else if (rec) label = "\xE2\x96\xA0 " + fmtClock(wallElapsed(app)) + "  " + fmtBytes(app.rec_status.bytes);
  else label = "\xE2\x97\x8F Gravar";
  ImGui::PushStyleColor(ImGuiCol_Button, rec ? kRed : ImVec4(0.20f, 0.22f, 0.27f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rec ? ImVec4(0.95f, 0.28f, 0.28f, 1) : ImVec4(0.28f, 0.31f, 0.38f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.12f, 0.14f, 1));
  if (!rec) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1));
  if (ImGui::Button(label.c_str(), ImVec2(width, 0))) {
    if (rec) {
      app.recStop();
    } else if (!app.recStart() && !app.rec_error.empty()) {
      app.toast(app.rec_error, true, 6000);
    }
  }
  if (!rec) ImGui::PopStyleColor();
  ImGui::PopStyleColor(3);
  ImGui::EndDisabled();
  tip(rec ? "Para a gravação, finaliza o MP4 e restaura a configuração anterior do celular."
          : "Grava o vídeo do celular em MP4 usando as opções da aba Gravação. Precisa do stream ativo.");
}

void tabRecord(App& app) {
  AppSettings& st = app.settings;
  const bool rec = app.rec_phase != RecPhase::Idle;
  auto dirty = [&] { app.markDirty(); };

  ImGui::PushStyleColor(ImGuiCol_Text, col::dim());
  ImGui::TextWrapped("A gravação copia o vídeo comprimido do celular direto para um MP4 (sem recodificar), com a qualidade do encoder do celular e quase nenhum uso de CPU.");
  ImGui::PopStyleColor();

  ImGui::BeginDisabled(rec);
  sectionTitle("Modo");
  int mode = st.rec_mode;
  if (ImGui::RadioButton("Tempo real", &mode, 0)) { st.rec_mode = 0; dirty(); }
  tip("Grava na velocidade normal, com os horários reais de captura do celular.");
  ImGui::SameLine();
  if (ImGui::RadioButton("Câmera lenta", &mode, 1)) { st.rec_mode = 1; dirty(); }
  tip("Captura em alta velocidade (240 fps) e grava com 30 ou 60 fps de reprodução: o vídeo toca 8x ou 4x mais devagar em qualquer player.");

  if (st.rec_mode == 1) {
    sectionTitle("Câmera lenta");
    const bool a720 = slowmoAdvertised(app, 1280, 720), a1080 = slowmoAdvertised(app, 1920, 1080);
    const bool known = app.caps_valid && (a720 || a1080);
    if (known && st.rec_slowmo_h == 1080 && !a1080) st.rec_slowmo_h = 720;
    if (known && st.rec_slowmo_h == 720 && !a720) st.rec_slowmo_h = 1080;
    int h = st.rec_slowmo_h;
    ImGui::TextUnformatted("Resolução");
    ImGui::SameLine(sc(170));
    ImGui::BeginDisabled(known && !a720);
    if (ImGui::RadioButton("1280x720", &h, 720)) { st.rec_slowmo_h = 720; dirty(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(known && !a1080);
    if (ImGui::RadioButton("1920x1080", &h, 1080)) { st.rec_slowmo_h = 1080; dirty(); }
    ImGui::EndDisabled();
    tip(known ? "Só ficam habilitados os tamanhos de alta velocidade que o celular anuncia."
              : "O celular não anunciou tamanhos de alta velocidade; o app tenta 240 fps no tamanho escolhido.");
    uint16_t hsfps = 0;
    slowmoAdvertised(app, st.rec_slowmo_h == 1080 ? 1920 : 1280, st.rec_slowmo_h == 1080 ? 1080 : 720, &hsfps);
    if (hsfps < 120) hsfps = 240;
    row("Captura", std::to_string(hsfps) + " fps");
    int of = st.rec_slowmo_out_fps;
    ImGui::TextUnformatted("Saída");
    ImGui::SameLine(sc(170));
    if (ImGui::RadioButton("30 fps (8x)", &of, 30)) { st.rec_slowmo_out_fps = 30; dirty(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("60 fps (4x)", &of, 60)) { st.rec_slowmo_out_fps = 60; dirty(); }
    tip("Quanto menor o fps de saída, mais lento fica o vídeo e mais longa a reprodução.");
    const double factor = static_cast<double>(hsfps) / st.rec_slowmo_out_fps;
    ImGui::TextColored(col::good(), "Fator %.0fx: 5 s gravados \xE2\x89\x88 %.0f s de vídeo", factor, 5.0 * factor);
  } else {
    sectionTitle("Tempo real");
    bool keep = st.rec_keep_current;
    if (ImGui::Checkbox("Manter resolução/fps atuais", &keep)) { st.rec_keep_current = keep; dirty(); }
    tip("Grava exatamente o que o stream já está enviando. Desmarque para escolher outro tamanho/fps da câmera.");
    if (!st.rec_keep_current) {
      const auto sizes = cameraSizes(app);
      if (sizes.empty()) {
        ImGui::TextDisabled("Conecte e inicie o stream para listar os tamanhos da câmera.");
      } else {
        char cur[48];
        if (st.rec_rt_w > 0) std::snprintf(cur, sizeof cur, "%dx%d", st.rec_rt_w, st.rec_rt_h);
        else std::snprintf(cur, sizeof cur, "(escolha)");
        ImGui::SetNextItemWidth(sc(190));
        if (ImGui::BeginCombo("Resolução", cur)) {
          for (const auto& s : sizes) {
            char b[48];
            std::snprintf(b, sizeof b, "%ux%u (até %u fps)", s.width, s.height, s.max_fps);
            if (ImGui::Selectable(b, st.rec_rt_w == s.width && st.rec_rt_h == s.height)) {
              st.rec_rt_w = s.width;
              st.rec_rt_h = s.height;
              if (st.rec_rt_fps > s.max_fps) st.rec_rt_fps = s.max_fps;
              dirty();
            }
          }
          ImGui::EndCombo();
        }
        uint16_t maxf = 30;
        for (const auto& s : sizes)
          if (s.width == st.rec_rt_w && s.height == st.rec_rt_h) maxf = s.max_fps;
        static const int fpsOpts[] = {24, 30, 60, 120};
        char fcur[24];
        if (st.rec_rt_fps > 0) std::snprintf(fcur, sizeof fcur, "%d fps", st.rec_rt_fps);
        else std::snprintf(fcur, sizeof fcur, "(atual)");
        ImGui::SetNextItemWidth(sc(190));
        if (ImGui::BeginCombo("Quadros/s", fcur)) {
          if (ImGui::Selectable("(atual)", st.rec_rt_fps == 0)) { st.rec_rt_fps = 0; dirty(); }
          for (int f : fpsOpts) {
            if (f > maxf) continue;
            char b[24];
            std::snprintf(b, sizeof b, "%d fps", f);
            if (ImGui::Selectable(b, st.rec_rt_fps == f)) { st.rec_rt_fps = f; dirty(); }
          }
          ImGui::EndCombo();
        }
      }
    }
  }

  sectionTitle("Prioridade");
  int pr = st.rec_priority;
  if (ImGui::RadioButton("Qualidade (recomendado para gravar)", &pr, 0)) { st.rec_priority = 0; dirty(); }
  tip("Usa o codec mais eficiente do celular (HEVC) mesmo que este PC não tenha decoder, com bitrate alto fixo e sem descartar quadros. O preview pode ficar desligado durante a gravação, mas o arquivo sai com a melhor qualidade.");
  if (ImGui::RadioButton("Latência", &pr, 1)) { st.rec_priority = 1; dirty(); }
  tip("Mantém o stream atual (codec, bitrate e política de descarte). O preview continua fluido, mas quadros podem ser descartados se a rede oscilar.");
  ImGui::EndDisabled();

  sectionTitle("Pasta de destino");
  const std::string dir = app.recDir();
  ImGui::TextWrapped("%s", dir.c_str());
  ImGui::BeginDisabled(rec);
  if (ImGui::Button("Alterar...")) {
    std::string d;
    if (pickFolder(nullptr, d)) { st.rec_dir = d; dirty(); }
  }
  ImGui::SameLine();
  if (ImGui::Button("Padrão")) { st.rec_dir.clear(); dirty(); }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Abrir pasta")) {
    SHCreateDirectoryExW(nullptr, fromUtf8(dir).c_str(), nullptr);
    shellOpen(dir);
  }
  ImGui::PushStyleColor(ImGuiCol_Text, col::dim());
  ImGui::TextWrapped("Nome do arquivo: OWC_AAAAMMDD_HHMMSS.mp4 (um novo arquivo _2, _3... se o codec ou a resolução mudar durante a gravação).");
  ImGui::PopStyleColor();

  sectionTitle("Status");
  if (!app.rec_error.empty()) ImGui::TextColored(col::bad(), "%s", app.rec_error.c_str());
  if (rec) {
    const owc::RecordStatus& s = app.rec_status;
    if (app.rec_phase == RecPhase::Preparing) {
      ImGui::TextColored(col::warn(), "Preparando o celular para gravar...");
    } else {
      const ImVec4 stc = s.state == owc::RecordState::Recording ? col::good()
                         : s.state == owc::RecordState::Error   ? col::bad()
                                                                : col::warn();
      row("Estado", stateName(s.state), &stc);
      ImGui::TextColored(col::dim(), "Arquivo atual");
      ImGui::TextWrapped("%s", s.path.c_str());
      row("Duração da mídia", fmtClock(s.media_duration_s) + " (decorrido " + fmtClock(wallElapsed(app)) + ")");
      char b[128];
      std::snprintf(b, sizeof b, "%llu gravados / %llu descartados / %llu faltando",
                    static_cast<unsigned long long>(s.frames_written), static_cast<unsigned long long>(s.frames_dropped),
                    static_cast<unsigned long long>(s.frames_missing));
      const ImVec4 dc = (s.frames_dropped || s.frames_missing) ? col::warn() : col::good();
      row("Quadros", b, &dc);
      row("Lacunas", std::to_string(s.gaps));
      row("Tamanho", fmtBytes(s.bytes));
      std::snprintf(b, sizeof b, "%.1f fps", s.capture_fps);
      row("Captura real", b);
      if (s.mode == owc::RecordMode::SlowMotion) {
        std::snprintf(b, sizeof b, "%.1fx", s.slowmo_factor);
        row("Fator câmera lenta", b);
      }
      if (!s.last_error.empty()) {
        const ImVec4 bad = col::bad();
        row("Último erro", s.last_error, &bad);
      }
    }
    if (!app.rec_setup.summary.empty()) ImGui::TextColored(col::accent(), "%s", app.rec_setup.summary.c_str());
  } else if (app.rec_has_final) {
    const owc::RecordStatus& s = app.rec_final;
    ImGui::TextColored(col::dim(), "Última gravação: %llu gravados / %llu descartados / %llu faltando, %s, %s",
                       static_cast<unsigned long long>(s.frames_written), static_cast<unsigned long long>(s.frames_dropped),
                       static_cast<unsigned long long>(s.frames_missing), fmtClock(s.media_duration_s).c_str(),
                       fmtBytes(s.bytes).c_str());
  } else {
    ImGui::TextDisabled("Nenhuma gravação em andamento.");
  }

  if (!rec && !app.rec_files.empty()) {
    sectionTitle("Arquivos gravados");
    for (size_t i = 0; i < app.rec_files.size(); ++i) {
      const std::string& f = app.rec_files[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::TextWrapped("%s", f.c_str());
      if (ImGui::Button("Abrir")) shellOpen(f);
      ImGui::SameLine();
      if (ImGui::Button("Mostrar na pasta")) showInFolder(f);
      ImGui::PopID();
    }
  }
}

}  // namespace ui
}  // namespace owcapp
