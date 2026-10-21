# owc_fakephone: Windows emulator of the phone side (tests / UI development).
# The engine sources are also compiled into owc_core's integration test.
set(OWC_FAKEPHONE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/fake_phone.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/h264_encoder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/pattern.cpp)

add_executable(owc_fakephone src/main.cpp ${OWC_FAKEPHONE_SOURCES})
target_include_directories(owc_fakephone PRIVATE src)
target_link_libraries(owc_fakephone PRIVATE owc_protocol ws2_32 mfplat mfuuid wmcodecdspuuid ole32 oleaut32)
