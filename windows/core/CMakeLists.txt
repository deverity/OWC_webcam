# owc_core: PC client library (session, decoder, shm producer, adb, discovery).
add_library(owc_core STATIC
  src/adb.cpp
  src/client.cpp
  src/decoder.cpp
  src/discovery.cpp
  src/metrics.cpp
  src/presets.cpp
  src/sps_patch.cpp
  src/transform.cpp
  src/util.cpp
)
target_include_directories(owc_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(owc_core
  PUBLIC owc_protocol owc_common
  PRIVATE ws2_32 iphlpapi mfplat mfuuid wmcodecdspuuid d3d11 dxgi ole32 oleaut32)
target_compile_features(owc_core PUBLIC cxx_std_17)

if(OWC_BUILD_TESTS)
  add_subdirectory(tests)
endif()
