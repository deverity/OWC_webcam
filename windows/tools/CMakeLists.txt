# Each tool lives in its own subdirectory (owned by different work streams).
# -DOWC_TOOLS_ONLY="testsrc;shmview" restricts the build to those subdirs.
set(OWC_TOOLS_ONLY "" CACHE STRING "Semicolon list of tool subdirs to build (empty = all)")
file(GLOB _owc_tool_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(_d ${_owc_tool_dirs})
  get_filename_component(_name ${_d} NAME)
  if(IS_DIRECTORY ${_d} AND EXISTS ${_d}/CMakeLists.txt)
    if(OWC_TOOLS_ONLY STREQUAL "" OR _name IN_LIST OWC_TOOLS_ONLY)
      add_subdirectory(${_d})
    endif()
  endif()
endforeach()
