#pragma once
#include <imgui.h>

#include <string>

#include "app.h"
#include "params_meta.h"

namespace owcapp {
namespace ui {

extern float S;  // DPI scale
extern ImFont* fontBold;
extern ImFont* fontTitle;

inline float sc(float v) { return v * S; }

namespace col {
inline ImVec4 accent() { return ImVec4(0.24f, 0.56f, 0.96f, 1.0f); }
inline ImVec4 good() { return ImVec4(0.36f, 0.80f, 0.50f, 1.0f); }
inline ImVec4 warn() { return ImVec4(0.97f, 0.72f, 0.25f, 1.0f); }
inline ImVec4 bad() { return ImVec4(0.96f, 0.38f, 0.38f, 1.0f); }
inline ImVec4 dim() { return ImVec4(0.56f, 0.60f, 0.67f, 1.0f); }
inline ImVec4 card() { return ImVec4(0.125f, 0.140f, 0.170f, 1.0f); }
}  // namespace col

// Wrapped tooltip shown when the last item is hovered (also when disabled).
void tip(const char* text);
void sectionTitle(const char* text);
bool beginCard(const char* id, const ImVec4* bg = nullptr);
void endCard();
// Starts a new inline item on the same line if it fits in the remaining width, else wraps.
void flowItem(float width);
std::string escapePct(const std::string& s);

// tabs (ui_tabs.cpp / ui_params.cpp)
void tabPresets(App& app);
void tabCamera(App& app);
void tabEncoder(App& app);
void tabPc(App& app);
void tabDevice(App& app);
void tabOutputs(App& app);
void tabLogs(App& app);
void tabRecord(App& app);  // ui_record.cpp
// Header record button (red + time/size while recording).
void recordButton(App& app, float width);
std::string fmtBytes(uint64_t b);
std::string fmtClock(double seconds);  // mm:ss (h:mm:ss above 1 h)

const char* thermalName(int status);
ImVec4 thermalColor(int status);

}  // namespace ui
}  // namespace owcapp
