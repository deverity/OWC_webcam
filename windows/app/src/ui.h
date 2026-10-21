// Dear ImGui user interface (pt-BR).
#pragma once
#include <string>

namespace owcapp {

class App;
class Renderer;

// Fonts + style for the given DPI scale (call again after a DPI change; fonts
// are rebuilt, the D3D11 backend recreates its texture lazily).
void uiSetup(float dpiScale);
// Selects the right-panel tab by key ("presets", "camera", "encoder", "pc",
// "device", "outputs", "logs") or pt-BR name. Returns false if unknown.
bool uiSelectTab(const std::string& name);
// Builds one full UI frame (between ImGui::NewFrame and ImGui::Render).
void uiDraw(App& app, Renderer& renderer);
// Number of frames the preview was drawn with real video (for self-test).
bool uiPreviewHasVideo();

}  // namespace owcapp
