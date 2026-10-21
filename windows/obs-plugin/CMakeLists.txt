# OWC OBS Studio source plugin (owc-obs-source.dll). Built against the libobs
# 32.0.4 headers in third_party/obs and an import library generated at build
# time from third_party/obs/obs.def (exports of the installed obs.dll).
# NOTE: libobs is GPL-2.0-or-later; this plugin is therefore GPL-compatible only.
set(OWC_OBS_DIR ${OWC_THIRD_PARTY}/obs)
set(OWC_OBS_LIB ${CMAKE_CURRENT_BINARY_DIR}/obs.lib)

add_custom_command(
  OUTPUT ${OWC_OBS_LIB}
  COMMAND ${CMAKE_LINKER} /lib /nologo /machine:x64 /def:${OWC_OBS_DIR}/obs.def /out:${OWC_OBS_LIB}
  DEPENDS ${OWC_OBS_DIR}/obs.def
  COMMENT "Generating obs.lib from obs.def"
  VERBATIM)
add_custom_target(owc_obs_import_lib DEPENDS ${OWC_OBS_LIB})
add_library(owc_obs_import STATIC IMPORTED GLOBAL)
set_target_properties(owc_obs_import PROPERTIES IMPORTED_LOCATION ${OWC_OBS_LIB})
add_dependencies(owc_obs_import owc_obs_import_lib)

set(OWC_OBS_PLUGIN_DIR ${OWC_OUTPUT_DIR}/obs-plugins/owc-obs-source)

add_library(owc-obs-source MODULE src/plugin.cpp)
target_include_directories(owc-obs-source PRIVATE ${OWC_OBS_DIR}/include)
target_link_libraries(owc-obs-source PRIVATE owc_common owc_obs_import winmm)
set_target_properties(owc-obs-source PROPERTIES PREFIX "" OUTPUT_NAME "owc-obs-source")
set_target_properties(owc-obs-source PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OWC_OBS_PLUGIN_DIR}/bin/64bit LIBRARY_OUTPUT_DIRECTORY ${OWC_OBS_PLUGIN_DIR}/bin/64bit)
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(owc-obs-source PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${_cfg} ${OWC_OBS_PLUGIN_DIR}/bin/64bit LIBRARY_OUTPUT_DIRECTORY_${_cfg} ${OWC_OBS_PLUGIN_DIR}/bin/64bit)
endforeach()
add_custom_command(TARGET owc-obs-source POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data ${OWC_OBS_PLUGIN_DIR}/data
  COMMENT "Copying OBS plugin data")

# Loader test: proves every import resolves against the installed obs.dll
# without running obs_module_load. Usage: owc_obs_loadtest.exe [plugin.dll] [obs bin dir]
add_executable(owc_obs_loadtest tests/loadtest.cpp)
set_target_properties(owc_obs_loadtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OWC_OUTPUT_DIR})
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(owc_obs_loadtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${_cfg} ${OWC_OUTPUT_DIR})
endforeach()
