# owc_core tests. The fakephone engine sources are compiled directly into the
# tests (tools/ is configured after core/, so no target dependency).
set(OWC_FAKEPHONE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/fakephone/src)

add_executable(owc_core_unit_tests unit_tests.cpp ${OWC_FAKEPHONE_DIR}/pattern.cpp)
target_include_directories(owc_core_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${OWC_FAKEPHONE_DIR})
target_link_libraries(owc_core_unit_tests PRIVATE owc_core)
add_test(NAME owc_core_unit_tests COMMAND owc_core_unit_tests)

if(EXISTS ${OWC_FAKEPHONE_DIR}/fake_phone.cpp)
  add_executable(owc_core_integration_test integration_test.cpp
    ${OWC_FAKEPHONE_DIR}/fake_phone.cpp
    ${OWC_FAKEPHONE_DIR}/h264_encoder.cpp
    ${OWC_FAKEPHONE_DIR}/pattern.cpp)
  target_include_directories(owc_core_integration_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${OWC_FAKEPHONE_DIR})
  target_link_libraries(owc_core_integration_test PRIVATE owc_core ws2_32 mfplat mfuuid mfreadwrite wmcodecdspuuid ole32 oleaut32)
  # Real-phone H.264 samples (Note 9 / Qualcomm) for the replay regression.
  target_compile_definitions(owc_core_integration_test PRIVATE
    OWC_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  add_test(NAME owc_core_integration_test COMMAND owc_core_integration_test)
  set_tests_properties(owc_core_integration_test PROPERTIES TIMEOUT 600)
endif()

# Manual probe (not a ctest): read-only adb listing + LAN discovery + decoder check.
add_executable(owc_core_probe probe.cpp)
target_include_directories(owc_core_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(owc_core_probe PRIVATE owc_core)
