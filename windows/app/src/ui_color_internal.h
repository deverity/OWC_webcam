// Colour page ("Cor / LUT", DaVinci Resolve style) internals shared by
// ui_color.cpp (page, header, viewer, undo, adjustments), ui_color_wheels.cpp
// (primaries), ui_color_curves.cpp (custom + hue/sat curves),
// ui_color_gallery.cpp (LUT gallery) and ui_scopes.cpp (video scopes).
#pragma once
#include <imgui.h>

#include <string>
#include <vector>

#include <owc/client.h>
#include <owc/scopes.h>

#include "color_worker.h"
#include "renderer.h"
#include "ui_internal.h"

namespace owcapp {
namespace ui {
namespace color {

// ---- neutral grey palette of the page (Resolve: panels ~#1e1e1e/#282828, #3a3a3a borders)
namespace pal {
inline ImVec4 page() { return ImVec4(0.098f, 0.098f, 0.102f, 1.0f); }    // #191919
inline ImVec4 panel() { return ImVec4(0.137f, 0.137f, 0.141f, 1.0f); }   // #232324
inline ImVec4 inset() { return ImVec4(0.090f, 0.090f, 0.094f, 1.0f); }   // viewer / graph backgrounds
inline ImVec4 border() { return ImVec4(0.227f, 0.227f, 0.235f, 1.0f); }  // #3a3a3c
inline ImVec4 text() { return ImVec4(0.84f, 0.84f, 0.85f, 1.0f); }
inline ImVec4 dim() { return ImVec4(0.55f, 0.55f, 0.57f, 1.0f); }
inline ImVec4 accent() { return col::accent(); }
inline ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
inline ImU32 grid() { return IM_COL32(255, 255, 255, 22); }
inline ImU32 gridStrong() { return IM_COL32(255, 255, 255, 44); }
inline ImU32 label() { return IM_COL32(150, 150, 155, 255); }
}  // namespace pal

// Panel = child window with the page's panel colour and a thin border.
bool beginPanel(const char* id, const ImVec2& size, bool scroll = false);
void endPanel();
// Small uppercase-ish caption in the dim colour.
void caption(const char* text);
// Drawn "reset" icon button (circular arrow). Returns true when clicked.
bool resetIcon(const char* id, float size, bool enabled, const char* tipText);
// Toggle-style button: highlighted when `active`.
bool toggleButton(const char* label, bool active, const ImVec2& size = ImVec2(0, 0));
// Shared worker (scopes, thumbnails, test chart).
ColorWorker& worker();
// True while some custom widget (wheel, jog, curve point) is being dragged:
// the undo snapshot waits for the drag to end.
extern bool g_customDrag;

// ---- primaries (ui_color_wheels.cpp)
bool drawWheels(owc::ColorGrade& g, ImVec2 size);

// ---- custom curves / hue-sat curves (ui_color_curves.cpp)
// `histogram`: optional texture drawn faintly behind the custom curves.
bool drawCurves(owc::ColorGrade& g, ImVec2 size, ImTextureID histogram);
bool drawHueSat(owc::ColorGrade& g, ImVec2 size);

// ---- LUT gallery (ui_color_gallery.cpp)
bool drawGallery(App& app, Renderer& r, owc::ColorGrade& g, ImVec2 size);
// Adds a .cube path to the recent list (front, no duplicates).
void rememberLut(App& app, const std::string& path);
// Loads a .cube with the file dialog into g (and the recent list). True if chosen.
bool loadCubeDialog(App& app, owc::ColorGrade& g);
void exportCubeDialog(App& app, const owc::ColorGrade& g);
// Display name of a LUT value ("" = none, builtin:key, path).
std::string lutDisplayName(const std::string& lut);
std::string baseName(const std::string& path);

// ---- scopes (ui_scopes.cpp)
// Draws the scope panel (selector + one or two scopes). Uploads new scope images
// from the worker. Records the wanted scope image size for the next jobs.
void drawScopes(App& app, Renderer& r, ImVec2 size);
// Scope options matching the current panel size (for submitFrame / submitChart).
owc::ScopeOptions scopeOptions();
// Histogram texture (for the curves background), 0 if none.
ImTextureID histogramTexture();

// Frees the GPU textures of the scopes / gallery (shutdown, before the device goes away).
void releaseScopeTextures();
void releaseGalleryTextures();

}  // namespace color
}  // namespace ui
}  // namespace owcapp
