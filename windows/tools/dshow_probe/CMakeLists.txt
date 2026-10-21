# owc_dshow_probe: enumerates/tests the OWC Webcam DirectShow filter (and benchmarks its converter).
if(NOT TARGET owc_vcam_core)
  # virtualcam not part of this build tree: compile the shared sources directly
  set(_vc ${CMAKE_CURRENT_SOURCE_DIR}/../../virtualcam)
  add_library(owc_vcam_core STATIC ${_vc}/src/convert.cpp ${_vc}/src/mediatypes.cpp)
  target_include_directories(owc_vcam_core PUBLIC ${_vc}/src ${_vc}/include)
  target_link_libraries(owc_vcam_core PUBLIC strmiids ole32)
endif()

add_executable(owc_dshow_probe main.cpp sink.cpp sink.h)
target_link_libraries(owc_dshow_probe PRIVATE owc_vcam_core owc_common strmiids ole32 oleaut32 psapi advapi32)
