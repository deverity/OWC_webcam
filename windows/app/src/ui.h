// Dear ImGui user interface (pt-BR).
#pragma once
#include <string>

namespace owcapp {

class App;
class Renderer;
struct PreviewFrame;

// Fonts + style for the given DPI scale (call again after a DPI change; fonts
// are rebuilt, the D3D11 backend recreates its texture lazily).
void uiSetup(float dpiScale);
// Selects the right-panel tab by key ("presets", "camera", "color", "encoder", "pc",
// "device", "outputs", "logs") or pt-BR name. Returns false if unknown.
bool uiSelectTab(const std::string& name);
// Builds one full UI frame (between ImGui::NewFrame and ImGui::Render).
void uiDraw(App& app, Renderer& renderer);
// Number of frames the preview was drawn with real video (for self-test).
bool uiPreviewHasVideo();
// Call after a new preview frame was uploaded: the colour page samples it for its
// scopes / LUT thumbnails (at most 15 Hz, only while visible). May swap the
// frame's buffers with spare ones (no copy), so don't use the pixels afterwards.
void uiOnPreviewFrame(PreviewFrame& frame);
// Stops the colour page's worker thread and frees its textures (before the D3D device goes away).
void uiShutdown();

}  // namespace owcapp
