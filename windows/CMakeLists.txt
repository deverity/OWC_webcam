cmake_minimum_required(VERSION 3.20)
project(owc_windows CXX C)

# Toolchain: VS2019 Build Tools (MSVC 14.29), Windows SDK 10.0.19041, x64.
#   cmake -S windows -B build/<name> -G "Visual Studio 16 2019" -A x64 [-DOWC_BUILD_X=OFF ...]
#   cmake --build build/<name> --config Release

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: DLLs load in foreign apps
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
add_compile_options(/W4 /utf-8 /permissive- /MP)

option(OWC_BUILD_TESTS "Protocol + core unit tests" ON)
option(OWC_BUILD_CORE "owc_core client library" ON)
option(OWC_BUILD_APP "OWC desktop app (ImGui)" ON)
option(OWC_BUILD_VIRTUALCAM "DirectShow virtual camera filter" ON)
option(OWC_BUILD_OBS_PLUGIN "OBS Studio source plugin" ON)
option(OWC_BUILD_TOOLS "Dev tools (test pattern producer, shm viewer)" ON)

set(OWC_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set(OWC_OUTPUT_DIR ${CMAKE_BINARY_DIR}/out)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OWC_OUTPUT_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${OWC_OUTPUT_DIR})

set(OWC_PROTOCOL_TESTS ${OWC_BUILD_TESTS} CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../protocol ${CMAKE_BINARY_DIR}/protocol)
if(OWC_BUILD_TESTS)
  enable_testing()
endif()

add_library(owc_common INTERFACE)
target_include_directories(owc_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/common/include)

foreach(comp core app virtualcam obs-plugin tools)
  string(TOUPPER ${comp} _u)
  string(REPLACE "-" "_" _u ${_u})
  if(OWC_BUILD_${_u} AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${comp}/CMakeLists.txt)
    add_subdirectory(${comp})
  endif()
endforeach()
