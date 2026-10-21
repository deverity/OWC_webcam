// OWC.exe - desktop app entry: window, D3D11 loop, tray, single instance,
// self-test mode.
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app.h"
#include "platform.h"
#include "renderer.h"
#include "ui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using namespace owcapp;

namespace {

constexpr UINT kWmTray = WM_APP + 1;
constexpr UINT kWmShowApp = WM_APP + 2;
constexpr UINT kTrayId = 1;
constexpr wchar_t kClassName[] = L"OWCMainWindow";

struct Args {
  bool selftest = false;
  std::string host = "127.0.0.1";
  int port = 47800;
  std::string transport = "tcp";
  double seconds = 5.0;
  std::wstring screenshot;
  std::wstring logfile;
  std::string tab;
  bool help = false;
  bool colortest = false;
  bool idle = false;  // self-test without a phone: only screenshot the idle UI
  double record = 0;          // --record N: record N seconds through the UI flow
  bool slowmo = false;        // --slowmo
  std::string rec_priority;   // --rec-priority quality|latency
  std::wstring rec_dir;       // --rec-dir
  std::vector<std::string> sets;  // --set id:value (self-test: send a param, report the outcome)
};

HWND g_hwnd = nullptr;
HINSTANCE g_inst = nullptr;
Renderer g_renderer;
App* g_app = nullptr;
Args g_args;
float g_dpiScale = 1.0f;
bool g_fontsDirty = false;
bool g_running = true;
bool g_inFrame = false;
bool g_trayActive = false;
bool g_hidden = false;
UINT g_taskbarCreated = 0;
std::wstring g_logPath;

// ------------------------------------------------------------------ output
void emit(const std::string& text) {
  static HANDLE out = nullptr;
  static bool init = false;
  if (!init) {
    init = true;
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
      if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (out == INVALID_HANDLE_VALUE) out = nullptr;
      } else {
        out = nullptr;
      }
    }
  }
  if (out) {
    DWORD w = 0;
    WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &w, nullptr);
  }
  if (!g_logPath.empty()) {
    HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      DWORD w = 0;
      WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &w, nullptr);
      CloseHandle(f);
    }
  }
}

void emitf(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  emit(buf);
}

// ------------------------------------------------------------------ args
bool parseArgs(Args& a) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return true;
  for (int i = 1; i < argc; ++i) {
    std::wstring s = argv[i];
    auto next = [&]() -> std::wstring { return (i + 1 < argc) ? std::wstring(argv[++i]) : std::wstring(); };
    if (s == L"--selftest") a.selftest = true;
    else if (s == L"--colortest") a.colortest = true;
    else if (s == L"--idle") a.idle = true;
    else if (s == L"--set") a.sets.push_back(toUtf8(next()));
    else if (s == L"--record") a.record = _wtof(next().c_str());
    else if (s == L"--slowmo") a.slowmo = true;
    else if (s == L"--rec-priority") a.rec_priority = toUtf8(next());
    else if (s == L"--rec-dir") a.rec_dir = next();
    else if (s == L"--host") a.host = toUtf8(next());
    else if (s == L"--port") a.port = _wtoi(next().c_str());
    else if (s == L"--transport") a.transport = toUtf8(next());
    else if (s == L"--seconds") a.seconds = _wtof(next().c_str());
    else if (s == L"--screenshot") a.screenshot = next();
    else if (s == L"--log") a.logfile = next();
    else if (s == L"--tab") a.tab = toUtf8(next());
    else if (s == L"--help" || s == L"-h" || s == L"/?") a.help = true;
  }
  LocalFree(argv);
  return true;
}

// ------------------------------------------------------------------ tray
void trayAdd() {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof nid;
  nid.hWnd = g_hwnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = kWmTray;
  nid.hIcon = static_cast<HICON>(LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                            GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
  lstrcpynW(nid.szTip, L"OWC - webcam do celular (em execução)", ARRAYSIZE(nid.szTip));
  g_trayActive = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

void trayRemove() {
  if (!g_trayActive) return;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof nid;
  nid.hWnd = g_hwnd;
  nid.uID = kTrayId;
  Shell_NotifyIconW(NIM_DELETE, &nid);
  g_trayActive = false;
}

void showMainWindow() {
  g_hidden = false;
  trayRemove();
  ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(g_hwnd);
}

void hideToTray() {
  g_hidden = true;
  ShowWindow(g_hwnd, SW_HIDE);
  trayAdd();
}

void trayMenu() {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, 1, L"Abrir");
  AppendMenuW(m, g_app && g_app->live() ? MF_STRING : MF_STRING | MF_GRAYED, 2, L"Desconectar");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, 3, L"Sair");
  POINT p;
  GetCursorPos(&p);
  SetForegroundWindow(g_hwnd);
  UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, nullptr);
  DestroyMenu(m);
  PostMessageW(g_hwnd, WM_NULL, 0, 0);
  if (cmd == 1) showMainWindow();
  else if (cmd == 2 && g_app) g_app->disconnect();
  else if (cmd == 3) {
    g_running = false;
    PostQuitMessage(0);
  }
}

// ------------------------------------------------------------------ rendering
void rebuildFonts() {
  ImGui_ImplDX11_InvalidateDeviceObjects();
  uiSetup(g_dpiScale);
  g_fontsDirty = false;
}

void renderFrame() {
  if (g_inFrame || !g_app) return;
  g_inFrame = true;
  if (g_fontsDirty) rebuildFonts();

  static PreviewFrame frame;
  if (g_app->takePreview(frame)) {
    YuvParams yp;
    // color_matrix: 0 unknown (709 for >= 720p, else 601), 1 = BT.601, 2 = BT.709
    yp.bt709 = frame.color_matrix == 2 || (frame.color_matrix == 0 && frame.h >= 720);
    yp.full_range = frame.full_range;
    if (g_renderer.uploadNv12(frame.y.data(), frame.uv.data(), frame.w, frame.h, yp)) g_app->noteDisplayed(frame);
  }

  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  uiDraw(*g_app, g_renderer);
  ImGui::Render();
  const float clear[4] = {0.075f, 0.082f, 0.098f, 1.0f};
  g_renderer.beginRender(clear);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  if (!g_renderer.present()) {
    emit("ERRO: dispositivo D3D11 perdido\n");
    g_running = false;
  }
  g_inFrame = false;
}

void saveWindowSize() {
  if (!g_app || !g_app->persist || !g_hwnd || IsIconic(g_hwnd) || IsZoomed(g_hwnd)) return;
  RECT r;
  if (GetWindowRect(g_hwnd, &r)) {
    g_app->settings.window_w = r.right - r.left;
    g_app->settings.window_h = r.bottom - r.top;
    g_app->markDirty();
  }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return TRUE;
  switch (msg) {
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize.x = static_cast<LONG>(1000 * g_dpiScale);
      mm->ptMinTrackSize.y = static_cast<LONG>(640 * g_dpiScale);
      return 0;
    }
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) {
        g_renderer.resize(static_cast<int>(LOWORD(lp)), static_cast<int>(HIWORD(lp)));
        if (ImGui::GetCurrentContext() && !g_inFrame) renderFrame();
      }
      return 0;
    case WM_EXITSIZEMOVE:
      saveWindowSize();
      return 0;
    case WM_DPICHANGED: {
      g_dpiScale = static_cast<float>(HIWORD(wp)) / 96.0f;
      g_fontsDirty = true;
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // no Alt menu beep
      break;
    case kWmTray:
      if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP) showMainWindow();
      else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) trayMenu();
      return 0;
    case kWmShowApp:
      showMainWindow();
      return 0;
    case WM_CLOSE:
      if (!g_args.selftest && g_app && g_app->live()) {
        hideToTray();  // keep streaming for the virtual camera / OBS
        return 0;
      }
      saveWindowSize();
      g_running = false;
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      trayRemove();
      PostQuitMessage(0);
      return 0;
    default:
      if (msg == g_taskbarCreated && g_taskbarCreated != 0 && g_hidden) trayAdd();
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ colour test
// Uploads SMPTE-like 100% colour bars encoded as NV12 with an independent CPU
// RGB->YCbCr matrix and checks the GPU YUV->RGB conversion read back from the preview texture.
int runColorTest() {
  struct Bar { float r, g, b; const char* name; };
  const Bar bars[8] = {{1, 1, 1, "branco"}, {1, 1, 0, "amarelo"}, {0, 1, 1, "ciano"}, {0, 1, 0, "verde"},
                       {1, 0, 1, "magenta"}, {1, 0, 0, "vermelho"}, {0, 0, 1, "azul"}, {0, 0, 0, "preto"}};
  const uint32_t w = 640, h = 360;
  int worst = 0;
  bool allOk = true;
  for (int mode = 0; mode < 4; ++mode) {
    YuvParams yp;
    yp.bt709 = (mode & 1) == 0;
    yp.full_range = (mode & 2) != 0;
    const float kr = yp.bt709 ? 0.2126f : 0.299f, kb = yp.bt709 ? 0.0722f : 0.114f, kg = 1 - kr - kb;
    std::vector<uint8_t> y(w * h), uv(w * h / 2);
    auto enc = [&](const Bar& b, uint8_t& Y, uint8_t& U, uint8_t& V) {
      const float ey = kr * b.r + kg * b.g + kb * b.b;
      const float cb = (b.b - ey) / (2 * (1 - kb)), cr = (b.r - ey) / (2 * (1 - kr));
      const float yy = yp.full_range ? 255 * ey : 16 + 219 * ey;
      const float uu = 128 + (yp.full_range ? 255.0f : 224.0f) * cb, vv = 128 + (yp.full_range ? 255.0f : 224.0f) * cr;
      auto q = [](float v) { return static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, v + 0.5f))); };
      Y = q(yy); U = q(uu); V = q(vv);
    };
    for (uint32_t r = 0; r < h; ++r)
      for (uint32_t c = 0; c < w; ++c) { uint8_t Y, U, V; enc(bars[c * 8 / w], Y, U, V); y[r * w + c] = Y; }
    for (uint32_t r = 0; r < h / 2; ++r)
      for (uint32_t c = 0; c < w / 2; ++c) {
        uint8_t Y, U, V;
        enc(bars[(c * 2) * 8 / w], Y, U, V);
        uv[r * w + c * 2] = U;
        uv[r * w + c * 2 + 1] = V;
      }
    if (!g_renderer.uploadNv12(y.data(), uv.data(), w, h, yp)) { emit("COLORTEST FAIL: upload\n"); return 1; }
    std::vector<uint8_t> px;
    if (!g_renderer.readPreview(px)) { emit("COLORTEST FAIL: readback\n"); return 1; }
    int modeWorst = 0;
    for (int i = 0; i < 8; ++i) {
      const uint32_t cx = (static_cast<uint32_t>(i) * 2 + 1) * w / 16, cy = h / 2;
      const uint8_t* p = &px[(static_cast<size_t>(cy) * w + cx) * 4];
      const int er = static_cast<int>(bars[i].r * 255), eg = static_cast<int>(bars[i].g * 255), eb = static_cast<int>(bars[i].b * 255);
      const int d = std::max({std::abs(p[0] - er), std::abs(p[1] - eg), std::abs(p[2] - eb)});
      modeWorst = std::max(modeWorst, d);
      if (mode == 0)
        emitf("  %-9s esperado (%3d,%3d,%3d) obtido (%3d,%3d,%3d)\n", bars[i].name, er, eg, eb, p[0], p[1], p[2]);
    }
    emitf("COLORTEST %s %s: max erro %d\n", yp.bt709 ? "BT.709" : "BT.601", yp.full_range ? "full" : "limited", modeWorst);
    worst = std::max(worst, modeWorst);
    if (modeWorst > 3) allOk = false;
  }
  emitf("COLORTEST %s (pior erro %d)\n", allOk ? "PASS" : "FAIL", worst);
  return allOk ? 0 : 1;
}

// ------------------------------------------------------------------ self-test
struct SelfTest {
  bool sets_sent = false;
  ULONGLONG t_start = 0;
  ULONGLONG t_first = 0;
  uint64_t shown_first = 0;
  bool shot_requested = false;
  int rec_stage = 0;  // 0 idle, 1 waiting for Recording, 2 recording, 3 stopped
  ULONGLONG rec_t = 0;
  bool done = false;
  int exit_code = 1;
};
SelfTest g_st;

// --record: drive the same UI flow (App::recStart -> recTick -> recStop) and report.
void recordSelftest(ULONGLONG now) {
  App& app = *g_app;
  auto fail = [&](const char* why, int code) {
    emitf("SELFTEST RECORD FAIL: %s%s%s\n", why, app.rec_error.empty() ? "" : " - ", app.rec_error.c_str());
    g_st.exit_code = code;
    g_st.done = true;
  };
  if (g_st.rec_stage == 0) {
    app.settings.rec_mode = g_args.slowmo ? 1 : 0;
    if (g_args.rec_priority == "latency") app.settings.rec_priority = 1;
    else if (g_args.rec_priority == "quality") app.settings.rec_priority = 0;
    if (!g_args.rec_dir.empty()) app.settings.rec_dir = toUtf8(g_args.rec_dir);
    if (!app.recStart()) return fail("recStart", 5);
    emitf("SELFTEST RECORD: preparando (%s, %s)\n", g_args.slowmo ? "camera lenta" : "tempo real",
          app.settings.rec_priority ? "latencia" : "qualidade");
    g_st.rec_stage = 1;
    g_st.rec_t = now;
    return;
  }
  if (g_st.rec_stage == 1) {
    if (app.rec_phase == RecPhase::Recording) {
      emitf("SELFTEST RECORD: gravando. %s\n", app.rec_setup.summary.c_str());
      g_st.rec_stage = 2;
      g_st.rec_t = now;
    } else if (app.rec_phase == RecPhase::Idle) {
      return fail("gravacao nao iniciou", 5);
    } else if (now - g_st.rec_t > 30000) {
      return fail("timeout preparando", 5);
    }
    return;
  }
  if (g_st.rec_stage == 2) {
    const double el = static_cast<double>(now - g_st.rec_t) / 1000.0;
    if (app.rec_phase != RecPhase::Recording) return fail("gravacao parou sozinha", 6);
    if (!g_st.shot_requested && !g_args.screenshot.empty() && el >= g_args.record * 0.5) {
      g_renderer.requestScreenshot(g_args.screenshot);
      g_st.shot_requested = true;
    }
    if (el < g_args.record || (g_st.shot_requested && g_renderer.screenshotPending())) return;
    app.recStop();
    g_st.rec_stage = 3;
    const owc::RecordStatus& f = app.rec_final;
    bool ok = !app.rec_files.empty() && f.frames_written > 0 && f.frames_dropped == 0;
    for (const std::string& file : app.rec_files) {
      WIN32_FILE_ATTRIBUTE_DATA fa{};
      unsigned long long sz = 0;
      if (GetFileAttributesExW(fromUtf8(file).c_str(), GetFileExInfoStandard, &fa))
        sz = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
      else ok = false;
      emitf("RECFILE %s frames_written=%llu frames_dropped=%llu frames_missing=%llu media_duration=%.2fs bytes_on_disk=%llu\n",
            file.c_str(), static_cast<unsigned long long>(f.frames_written), static_cast<unsigned long long>(f.frames_dropped),
            static_cast<unsigned long long>(f.frames_missing), f.media_duration_s, sz);
    }
    emitf("  capture_fps=%.1f slowmo_factor=%.2f gaps=%u files=%zu error=%s\n", f.capture_fps, f.slowmo_factor, f.gaps,
          app.rec_files.size(), app.rec_error.empty() ? "-" : app.rec_error.c_str());
    emitf("SELFTEST RECORD %s\n", ok ? "PASS" : "FAIL");
    g_st.exit_code = ok ? 0 : 7;
    g_st.done = true;
  }
}

void selftestTick() {
  const ULONGLONG now = GetTickCount64();
  if (g_args.idle) {
    if (now - g_st.t_start > 1500 && !g_st.shot_requested && !g_args.screenshot.empty()) {
      g_renderer.requestScreenshot(g_args.screenshot);
      g_st.shot_requested = true;
    }
    if (now - g_st.t_start > 1800 && !g_renderer.screenshotPending()) {
      emitf("SELFTEST idle: screenshot %s\n", g_renderer.lastScreenshotOk() ? "ok" : "FALHOU");
      g_st.exit_code = g_renderer.lastScreenshotOk() ? 0 : 1;
      g_st.done = true;
    }
    return;
  }
  if (!g_st.t_first) {
    if (g_app->frames_shown > 0) {
      g_st.t_first = now;
      g_st.shown_first = g_app->frames_shown;
    } else if (now - g_st.t_start > 20000) {
      emitf("SELFTEST FAIL: nenhum quadro recebido em 20 s (estado: %s, %s)\n", stateText(g_app->client.state()),
            reasonText(g_app->client.lastDisconnectReason()));
      g_st.exit_code = 3;
      g_st.done = true;
    }
    return;
  }
  const double elapsed = static_cast<double>(now - g_st.t_first) / 1000.0;
  if (!g_st.sets_sent && elapsed >= 1.0) {
    g_st.sets_sent = true;
    for (const std::string& spec : g_args.sets) {
      const size_t k = spec.find(':');
      if (k == std::string::npos) continue;
      const owc::ParamId id = static_cast<owc::ParamId>(std::atoi(spec.substr(0, k).c_str()));
      const double v = std::atof(spec.c_str() + k + 1);
      owc::ParamDesc d;
      owc::ParamValue pv = owc::ParamValue::Int(static_cast<int32_t>(v));
      if (g_app->client.paramDesc(id, d)) {
        switch (d.type) {
          case owc::ValueType::Bool: pv = owc::ParamValue::Bool(v != 0); break;
          case owc::ValueType::Int32: pv = owc::ParamValue::Int(static_cast<int32_t>(v)); break;
          case owc::ValueType::Int64: pv = owc::ParamValue::Long(static_cast<int64_t>(v)); break;
          case owc::ValueType::Float: pv = owc::ParamValue::Real(static_cast<float>(v)); break;
          case owc::ValueType::Enum: pv = owc::ParamValue::EnumV(static_cast<int32_t>(v)); break;
        }
      }
      g_app->setParam(id, pv);
    }
  }
  if (g_args.record > 0) {
    if (elapsed >= 1.5) recordSelftest(now);
    return;
  }
  if (!g_st.shot_requested && !g_args.screenshot.empty() && elapsed >= g_args.seconds * 0.75) {
    g_renderer.requestScreenshot(g_args.screenshot);
    g_st.shot_requested = true;
  }
  if (elapsed >= g_args.seconds && (g_args.screenshot.empty() || (g_st.shot_requested && !g_renderer.screenshotPending()))) {
    const owc::ClientStats st = g_app->client.stats();
    const uint64_t rendered = g_app->frames_shown - g_st.shown_first;
    const double fps = st.has_format ? st.format.fps : st.rx_fps;
    const double expected = fps * elapsed;
    const double ratio = expected > 0 ? static_cast<double>(rendered) / expected : 0;
    const bool ok = ratio >= 0.8 && (g_args.screenshot.empty() || g_renderer.lastScreenshotOk());
    emitf("SELFTEST %s\n", ok ? "PASS" : "FAIL");
    emitf("  transport        : %s\n", st.transport == owc::VideoTransport::Udp ? "udp" : "tcp");
    emitf("  state            : %s\n", stateText(st.state));
    emitf("  stream           : %ux%u @ %.0f fps (%s)\n", st.output_width, st.output_height, fps,
          st.has_format && st.format.codec == owc::Codec::Hevc ? "HEVC" : "H.264");
    emitf("  window           : %.2f s measured\n", elapsed);
    emitf("  frames rendered  : %llu (expected %.0f, %.1f %%)\n", static_cast<unsigned long long>(rendered), expected,
          ratio * 100.0);
    emitf("  frames delivered : %llu (onFrame, total)\n", static_cast<unsigned long long>(g_app->frames_in.load()));
    emitf("  render fps       : %.1f (display), decoded %.1f, received %.1f\n", g_app->shown_fps, st.decoded_fps, st.rx_fps);
    emitf("  latency capture>publish : avg %.1f ms, p95 %.1f ms (clock_synced=%d, rtt %.2f ms)\n",
          st.capture_to_publish_ms_avg, st.capture_to_publish_ms_p95, st.clock_synced ? 1 : 0, st.rtt_ms);
    emitf("  latency capture>screen  : avg %.1f ms, p95 %.1f ms\n", g_app->shown_lat_avg, g_app->shown_lat_p95);
    emitf("  bitrate          : %.2f Mbps\n", st.rx_bitrate_bps / 1e6);
    emitf("  loss             : frames_lost=%llu fec_recovered=%llu keyframe_requests=%u\n",
          static_cast<unsigned long long>(st.frames_lost), static_cast<unsigned long long>(st.frames_recovered_fec),
          st.keyframe_requests);
    emitf("  decoder          : %s (%s)\n", st.decoder_name.c_str(), st.decoder_hardware ? "hardware" : "software");
    emitf("  decoder selection: %s\n", st.decoder_selection.empty() ? "-" : st.decoder_selection.c_str());
    emitf("  present          : tearing=%d adapter=%s\n", g_renderer.tearing() ? 1 : 0, toUtf8(g_renderer.adapterName()).c_str());
    if (!g_args.screenshot.empty())
      emitf("  screenshot       : %s (%s)\n", toUtf8(g_args.screenshot).c_str(), g_renderer.lastScreenshotOk() ? "ok" : "FALHOU");
    for (const std::string& spec : g_args.sets) {
      const size_t k = spec.find(':');
      if (k == std::string::npos) continue;
      const owc::ParamId id = static_cast<owc::ParamId>(std::atoi(spec.substr(0, k).c_str()));
      owc::ParamValue cur;
      owc::ParamValue pend;
      const bool have = g_app->client.paramValue(id, cur);
      emitf("  set %s -> confirmado=%s%lld pendente=%s\n", spec.c_str(), have ? "" : "(sem valor) ",
            static_cast<long long>(have ? (cur.type == owc::ValueType::Float ? static_cast<long long>(cur.f) : cur.i) : 0),
            g_app->pending(id, pend) ? "sim" : "nao");
    }
    for (const auto& t : g_app->toasts()) emitf("  toast%s: %s\n", t.error ? " (erro)" : "", t.text.c_str());
    g_st.exit_code = ok ? 0 : 1;
    g_st.done = true;
  }
}

}  // namespace

// ============================================================================
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
  g_inst = inst;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  parseArgs(g_args);
  g_logPath = g_args.logfile;
  if (g_args.help) {
    emit("OWC.exe [--selftest --host H --port P [--transport tcp|udp] [--seconds N] [--screenshot f.png] [--log f.txt] [--tab nome]\n         [--record N [--slowmo] [--rec-priority quality|latency] [--rec-dir pasta]]]\n");
    return 0;
  }

  HANDLE mutex = nullptr;
  if (!g_args.selftest) {
    mutex = CreateMutexW(nullptr, TRUE, L"Local\\OWC.WebcamApp.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
      HWND other = FindWindowW(kClassName, nullptr);
      if (other) {
        PostMessageW(other, kWmShowApp, 0, 0);
        AllowSetForegroundWindow(ASFW_ANY);
      }
      CloseHandle(mutex);
      return 0;
    }
  }

  App app(!g_args.selftest);
  g_app = &app;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kClassName;
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
  RegisterClassExW(&wc);
  g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

  int ww = app.settings.window_w, wh = app.settings.window_h;
  g_hwnd = CreateWindowExW(0, kClassName, L"OWC - Webcam do celular", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, ww, wh, nullptr, nullptr, inst, nullptr);
  if (!g_hwnd) return 2;
  g_dpiScale = static_cast<float>(GetDpiForWindow(g_hwnd)) / 96.0f;

  RECT rc;
  GetClientRect(g_hwnd, &rc);
  if (!g_renderer.init(g_hwnd, rc.right - rc.left, rc.bottom - rc.top)) {
    MessageBoxW(nullptr, L"Não foi possível inicializar o Direct3D 11.", L"OWC", MB_ICONERROR);
    return 2;
  }

  if (g_args.colortest) {
    int rc2 = runColorTest();
    g_renderer.shutdown();
    return rc2;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  ImGui_ImplWin32_Init(g_hwnd);
  ImGui_ImplDX11_Init(g_renderer.device(), g_renderer.context());
  uiSetup(g_dpiScale);
  if (!g_args.tab.empty() && !uiSelectTab(g_args.tab)) emitf("aviso: aba desconhecida '%s'\n", g_args.tab.c_str());

  ShowWindow(g_hwnd, SW_SHOWNORMAL);
  UpdateWindow(g_hwnd);
  app.startServices();

  if (g_args.selftest) {
    g_st.t_start = GetTickCount64();
    owc::VideoTransport t = g_args.transport == "udp" ? owc::VideoTransport::Udp : owc::VideoTransport::Tcp;
    if (!g_args.idle) app.connectNet(g_args.host, g_args.port, t, g_args.host);
  }

  ULONGLONG lastRender = 0;
  int extra = 3;
  HANDLE waitH[1] = {app.frameEvent()};
  while (g_running) {
    const bool idle = IsIconic(g_hwnd) || g_hidden;
    DWORD timeout = idle ? 250 : 100;
    if (extra > 0) timeout = 0;
    DWORD r = MsgWaitForMultipleObjects(1, waitH, FALSE, timeout, QS_ALLINPUT);
    bool activity = false;
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      if (m.message == WM_QUIT) g_running = false;
      TranslateMessage(&m);
      DispatchMessageW(&m);
      if (m.message != WM_TIMER && m.message != WM_PAINT) activity = true;
    }
    if (!g_running) break;
    if (activity) extra = 3;

    const ULONGLONG now = GetTickCount64();
    if (idle) {
      app.tick();
    } else {
      const bool newFrame = (r == WAIT_OBJECT_0);
      if (newFrame || extra > 0 || now - lastRender >= 100) {
        renderFrame();
        lastRender = now;
        if (extra > 0) --extra;
      }
    }
    if (g_args.selftest) {
      selftestTick();
      if (g_st.done) break;
    }
  }

  if (g_args.selftest && !g_st.done) g_st.exit_code = 4;
  const int code = g_args.selftest ? g_st.exit_code : 0;

  saveWindowSize();
  app.disconnect();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  g_renderer.shutdown();
  trayRemove();
  if (IsWindow(g_hwnd)) DestroyWindow(g_hwnd);
  g_app = nullptr;
  if (mutex) CloseHandle(mutex);
  return code;
}
