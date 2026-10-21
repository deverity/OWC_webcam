cmake_minimum_required(VERSION 3.22.1)
project(owc_native CXX)

# OWC native core: Camera2 NDK -> AMediaCodec (surface) -> TCP/UDP server.
# Built by Gradle (externalNativeBuild) or standalone with the NDK toolchain file.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# Shared wire protocol (repo-root/protocol). Explicit binary dir because the
# source dir is outside this tree.
set(OWC_PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../protocol)
get_filename_component(OWC_PROTOCOL_DIR ${OWC_PROTOCOL_DIR} ABSOLUTE)
add_subdirectory(${OWC_PROTOCOL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/owc_protocol)

set(OWC_WARN -Wall -Wextra)

# Core sources (no JNI) — shared by the JNI library and the on-device self-test.
set(OWC_CORE_SOURCES
  app_core.cpp
  session.cpp
  params.cpp
  pipeline.cpp
  abr.cpp
  util/log.cpp
  util/thread.cpp
  net/socket_util.cpp
  net/sender.cpp
  net/net_server.cpp
  camera/camera_info.cpp
  camera/camera_controls.cpp
  camera/camera_stream.cpp
  encoder/encoder_options.cpp
  encoder/video_encoder.cpp
)

add_library(owc_core STATIC ${OWC_CORE_SOURCES})
target_include_directories(owc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(owc_core PRIVATE ${OWC_WARN})
target_link_libraries(owc_core PUBLIC owc_protocol camera2ndk mediandk android log)

add_library(owc_native SHARED jni_bridge.cpp)
target_compile_options(owc_native PRIVATE ${OWC_WARN})
target_link_libraries(owc_native PRIVATE owc_core)
# Only JNI_OnLoad + Java_* symbols are exported (visibility=hidden elsewhere);
# drop unused sections and keep the .so small.
target_link_options(owc_native PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_link_options(owc_native PRIVATE -s)
endif()

# Standalone on-device test binary (adb push to /data/local/tmp). Not built by
# Gradle unless requested: -DOWC_NATIVE_SELFTEST=ON
option(OWC_NATIVE_SELFTEST "Build the owc_selftest executable" OFF)
if(OWC_NATIVE_SELFTEST)
  add_executable(owc_selftest selftest/selftest.cpp)
  target_compile_options(owc_selftest PRIVATE ${OWC_WARN})
  target_link_libraries(owc_selftest PRIVATE owc_core)
endif()
