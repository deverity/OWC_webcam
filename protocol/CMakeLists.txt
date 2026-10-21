cmake_minimum_required(VERSION 3.18)
project(owc_protocol CXX)

# Portable wire-protocol library shared by the Android NDK core and Windows.
add_library(owc_protocol STATIC
  src/protocol.cpp
)
target_include_directories(owc_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(owc_protocol PUBLIC cxx_std_17)

option(OWC_PROTOCOL_TESTS "Build protocol unit tests" OFF)
if(OWC_PROTOCOL_TESTS)
  enable_testing()
  add_executable(owc_protocol_tests tests/protocol_tests.cpp)
  target_link_libraries(owc_protocol_tests PRIVATE owc_protocol)
  add_test(NAME owc_protocol_tests COMMAND owc_protocol_tests)
endif()
