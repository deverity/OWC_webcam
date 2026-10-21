# owc_virtualcam.dll — DirectShow source filter "OWC Webcam" (no BaseClasses).
# Consumer of the shm frame bus (common/include/owc/shm.h).

# Pixel conversion/scaling + media type helpers, shared with tools/dshow_probe
# (which benchmarks exactly this code and parses negotiated types with it).
add_library(owc_vcam_core STATIC
  src/convert.cpp
  src/convert.h
  src/mediatypes.cpp
  src/mediatypes.h
)
target_include_directories(owc_vcam_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(owc_vcam_core PUBLIC strmiids ole32)

add_library(owc_virtualcam SHARED
  src/dll.cpp
  src/filter.cpp
  src/filter.h
  include/owc/vcam_ids.h
  owc_virtualcam.def
  owc_virtualcam.rc
)
target_link_libraries(owc_virtualcam PRIVATE owc_vcam_core owc_common avrt winmm advapi32)
set_target_properties(owc_virtualcam PROPERTIES OUTPUT_NAME owc_virtualcam PREFIX "")
