cmake_minimum_required(VERSION 3.18)
# C is needed for the vendored Monocypher (third_party/monocypher/monocypher.c).
project(owc_protocol C CXX)

# Portable wire-protocol + session-crypto library shared by the Android NDK core
# and Windows (both add_subdirectory() this file).
add_library(owc_protocol STATIC
  src/protocol.cpp
  src/crypto.cpp
  src/cpu_features.cpp
  src/aes_gcm_x86.cpp
  src/aes_gcm_arm.cpp
  third_party/monocypher/monocypher.c
)
target_include_directories(owc_protocol
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher)
target_compile_features(owc_protocol PUBLIC cxx_std_17)
set_target_properties(owc_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Warnings for our sources; the vendored C file is kept untouched and quiet.
if(MSVC)
  target_compile_options(owc_protocol PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4>)
  set(_owc_mc_opts /wd4146 /wd4244 /wd4245 /wd4267 /wd4334)
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    # MSVC 14.29 x86 /O2 (the SSA optimizer under /Og /Ot) miscompiles Monocypher's
    # X25519: RFC 7748 vectors fail, /Od and /O1 pass. owc_crypto_tests catches it.
    list(APPEND _owc_mc_opts /d2SSAOptimizer-)
  endif()
  set_source_files_properties(third_party/monocypher/monocypher.c PROPERTIES
    COMPILE_OPTIONS "${_owc_mc_opts}")
else()
  target_compile_options(owc_protocol PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
  set_source_files_properties(third_party/monocypher/monocypher.c PROPERTIES
    COMPILE_OPTIONS "-w")
endif()

# Hardware AES-GCM backends: each one is compiled with the ISA flags it needs,
# for that file only, and is only called after the runtime CPU check
# (src/cpu_features.cpp). Without the flags the backend compiles to a stub and
# Aes128Gcm is simply not offered (localCipherMask()).
if(ANDROID_ABI)
  set(_owc_arch ${ANDROID_ABI})
elseif(CMAKE_ANDROID_ARCH_ABI)
  set(_owc_arch ${CMAKE_ANDROID_ARCH_ABI})
else()
  set(_owc_arch ${CMAKE_SYSTEM_PROCESSOR})
endif()
string(TOLOWER "${_owc_arch}" _owc_arch)
if(NOT MSVC)
  # The per-packet hot paths are always optimised, also in Debug builds (an -O0
  # intrinsic is a function call: AES-GCM would run ~20x slower).
  set_property(SOURCE src/aes_gcm_arm.cpp src/aes_gcm_x86.cpp third_party/monocypher/monocypher.c
    APPEND PROPERTY COMPILE_OPTIONS -O3)
  if(_owc_arch MATCHES "^(arm64-v8a|aarch64|arm64)$")
    set_property(SOURCE src/aes_gcm_arm.cpp APPEND PROPERTY COMPILE_OPTIONS -march=armv8-a+crypto)
  elseif(_owc_arch MATCHES "^(armeabi-v7a|armv7.*|arm)$")
    # AArch32 crypto extension; used only when AT_HWCAP2 reports AES + PMULL.
    set_property(SOURCE src/aes_gcm_arm.cpp APPEND PROPERTY COMPILE_OPTIONS
      -march=armv8-a -mfpu=crypto-neon-fp-armv8)
  elseif(_owc_arch MATCHES "^(x86_64|amd64|x86|i[3-6]86)$")
    set_property(SOURCE src/aes_gcm_x86.cpp APPEND PROPERTY COMPILE_OPTIONS -maes -mpclmul -mssse3)
  endif()
endif()

option(OWC_PROTOCOL_TESTS "Build protocol unit tests" OFF)
if(OWC_PROTOCOL_TESTS)
  enable_testing()
  add_executable(owc_protocol_tests tests/protocol_tests.cpp)
  target_link_libraries(owc_protocol_tests PRIVATE owc_protocol)
  add_test(NAME owc_protocol_tests COMMAND owc_protocol_tests)

  add_executable(owc_crypto_tests tests/crypto_tests.cpp)
  target_link_libraries(owc_crypto_tests PRIVATE owc_protocol)
  add_test(NAME owc_crypto_tests COMMAND owc_crypto_tests)

  # Not a test: prints MB/s and microseconds per message for every local cipher.
  add_executable(owc_crypto_bench tests/crypto_bench.cpp)
  target_link_libraries(owc_crypto_bench PRIVATE owc_protocol)

  foreach(_t owc_protocol_tests owc_crypto_tests owc_crypto_bench)
    if(MSVC)
      target_compile_options(${_t} PRIVATE /W4)
    else()
      target_compile_options(${_t} PRIVATE -Wall -Wextra)
    endif()
  endforeach()
endif()
