// Recording flow (App::rec*): prepareRecording -> wait for the stream to reflect the
// setup -> startRecording -> stopRecording + endRecordingSetup. Driven from App::tick()
// on the UI thread; it never blocks waiting for the phone.
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>

#include "app.h"
#include "platform.h"

namespace owcapp {

namespace {
constexpr ULONGLONG kPrepareTimeoutMs = 6000;

std::string defaultRecDir() {
  PWSTR p = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &p)) && p) dir = p;
  if (p) CoTaskMemFree(p);
  if (dir.empty()) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) + L"\\Videos" : L".";
  }
  return toUtf8(dir + L"\\OWC");
}
}  // namespace

std::string App::recDir() const { return settings.rec_dir.empty() ? defaultRecDir() : settings.rec_dir; }

owc::RecordPreset App::recPreset() const {
  owc::RecordPreset p;
  p.mode = settings.rec_mode == 1 ? owc::RecordMode::SlowMotion : owc::RecordMode::RealTime;
  p.priority = settings.rec_priority == 1 ? owc::RecordPriority::Latency : owc::RecordPriority::Quality;
  p.slowmo_output_fps = settings.rec_slowmo_out_fps == 60 ? 60u : 30u;
  p.allow_codec_without_pc_decoder = true;
  if (p.mode == owc::RecordMode::SlowMotion) {
    p.height = settings.rec_slowmo_h == 1080 ? 1080u : 720u;
    p.width = p.height == 1080 ? 1920u : 1280u;
    uint32_t hs = 240;  // the phone's high-speed fps for this size, when advertised
    if (caps_valid && caps.active_camera < caps.cameras.size()) {
      uint32_t best = 0;
      for (const auto& s : caps.cameras[caps.active_camera].sizes)
        if (s.width == p.width && s.height == p.height && s.max_fps >= 120) best = std::max<uint32_t>(best, s.max_fps);
      if (best) hs = best;
    }
    p.fps = hs;
  } else if (!settings.rec_keep_current && settings.rec_rt_w > 0 && settings.rec_rt_h > 0) {
    p.width = static_cast<uint32_t>(settings.rec_rt_w);
    p.height = static_cast<uint32_t>(settings.rec_rt_h);
    p.fps = static_cast<uint32_t>(std::max(0, settings.rec_rt_fps));
  }
  return p;
}


bool App::recStart() {
  rec_error.clear();
  if (rec_phase != RecPhase::Idle) return false;
  if (client.state() != owc::ClientState::Streaming) {
    rec_error = "Inicie o stream antes de gravar.";
    return false;
  }
  const std::string dir = recDir();
  const int dr = SHCreateDirectoryExW(nullptr, fromUtf8(dir).c_str(), nullptr);
  if (dr != ERROR_SUCCESS && dr != ERROR_ALREADY_EXISTS && dr != ERROR_FILE_EXISTS) {
    rec_error = "Não foi possível criar a pasta de destino: " + dir;
    return false;
  }
  SYSTEMTIME st;
  GetLocalTime(&st);
  char name[64];
  std::snprintf(name, sizeof name, "\\OWC_%04d%02d%02d_%02d%02d%02d.mp4", st.wYear, st.wMonth, st.wDay, st.wHour,
                st.wMinute, st.wSecond);
  rec_path_ = dir + name;

  rec_req_ = recPreset();
  std::string err;
  if (!client.prepareRecording(rec_req_, &err)) {
    rec_error = err.empty() ? "O celular recusou a configuração de gravação." : err;
    return false;
  }
  rec_files.clear();
  rec_has_final = false;
  rec_status = owc::RecordStatus();
  rec_setup = client.recordSetupStatus();
  rec_preview_available = rec_setup.preview_available;
  rec_prep_t_ = GetTickCount64();
  rec_phase = RecPhase::Preparing;
  return true;
}

void App::recStop() {
  if (rec_phase == RecPhase::Idle) return;
  const bool wasRecording = rec_phase == RecPhase::Recording;
  if (wasRecording) {
    rec_final = client.stopRecording();
    rec_has_final = true;
    rec_status = rec_final;
    rec_files = rec_final.files;
    if (rec_files.empty() && !rec_final.path.empty() && rec_final.frames_written > 0) rec_files.push_back(rec_final.path);
    if (!rec_final.last_error.empty()) rec_error = rec_final.last_error;
  }
  client.endRecordingSetup();  // also when the session is down: re-applied on reconnect
  rec_phase = RecPhase::Idle;
  rec_preview_available = true;
  if (wasRecording) toast(rec_files.empty() ? "Gravação encerrada sem arquivos." : "Gravação salva.", rec_files.empty());
}

void App::recTick() {
  if (rec_phase == RecPhase::Idle) return;
  const ULONGLONG now = GetTickCount64();
  const owc::ClientState cs = client.state();
  if (cs == owc::ClientState::Idle || cs == owc::ClientState::Failed) {
    if (rec_error.empty()) rec_error = "Conexão com o celular perdida; gravação encerrada.";
    recStop();
    return;
  }
  if (rec_phase == RecPhase::Preparing) {
    rec_setup = client.recordSetupStatus();
    owc::VideoFormat vf;
    bool ready = false;
    if (rec_setup.active && client.videoFormat(vf) && cs == owc::ClientState::Streaming) {
      ready = vf.codec == rec_setup.codec && (rec_setup.fps == 0 || vf.fps == rec_setup.fps) &&
              (rec_setup.width == 0 || (vf.width == rec_setup.width && vf.height == rec_setup.height) ||
               (vf.width == rec_setup.height && vf.height == rec_setup.width));
    }
    const bool timeout = now - rec_prep_t_ > kPrepareTimeoutMs;
    if (!ready && !timeout) return;
    rec_preview_available = rec_setup.preview_available;
    owc::RecordOptions o;
    o.path = rec_path_;
    o.mode = rec_req_.mode;
    o.slowmo_output_fps = rec_req_.slowmo_output_fps;
    o.present = rec_preview_available;
    std::string err;
    if (!client.startRecording(o, &err)) {
      rec_error = err.empty() ? "Não foi possível iniciar a gravação." : err;
      client.endRecordingSetup();
      rec_phase = RecPhase::Idle;
      rec_preview_available = true;
      return;
    }
    if (!ready) toast("O stream não confirmou o formato pedido em 6 s; gravando com o formato atual.", true, 6000);
    rec_t0 = now;
    rec_phase = RecPhase::Recording;
    return;
  }
  // Recording
  rec_setup = client.recordSetupStatus();
  rec_status = client.recordingStatus();
  if (rec_status.state == owc::RecordState::Error || rec_status.state == owc::RecordState::Idle) {
    if (rec_status.state == owc::RecordState::Error && !rec_status.last_error.empty()) rec_error = rec_status.last_error;
    else if (rec_error.empty()) rec_error = "A gravação parou inesperadamente.";
    recStop();
  }
}

}  // namespace owcapp
