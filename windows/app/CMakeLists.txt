# OWC.exe: Dear ImGui + D3D11 + Win32 desktop app.
enable_language(RC)

# Third-party code (Dear ImGui) is built at the default warning level; our own
# target re-enables /W4.
get_directory_property(_owc_opts COMPILE_OPTIONS)
list(REMOVE_ITEM _owc_opts /W4)
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_owc_opts}")

set(IMGUI_DIR ${OWC_THIRD_PARTY}/imgui)
add_library(owc_imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(owc_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(owc_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

add_executable(OWC WIN32
  src/main.cpp
  src/app.cpp
  src/app_record.cpp
  src/ui_record.cpp
  src/ui_color.cpp
  src/ui_color_wheels.cpp
  src/ui_color_curves.cpp
  src/ui_color_gallery.cpp
  src/ui_scopes.cpp
  src/color_worker.cpp
  src/platform.cpp
  src/settings.cpp
  src/params_meta.cpp
  src/renderer.cpp
  src/ui.cpp
  src/ui_params.cpp
  src/ui_tabs.cpp
  OWC.rc)
target_compile_options(OWC PRIVATE /W4)
target_include_directories(OWC PRIVATE src)
target_link_libraries(OWC PRIVATE owc_core owc_imgui
  d3d11 dxgi d3dcompiler windowscodecs shell32 ole32 user32 gdi32 advapi32 dwmapi crypt32)
# The manifest (per-monitor-v2 DPI) is a resource of OWC.rc.
target_link_options(OWC PRIVATE /MANIFEST:NO)
set_target_properties(OWC PROPERTIES OUTPUT_NAME OWC)
