cmake_minimum_required(VERSION 3.10)

project(selection_hook_flutter_library VERSION 0.0.1 LANGUAGES C CXX)

add_library(selection_hook_flutter SHARED
  "bridge/c_api.cc"
)

target_include_directories(selection_hook_flutter PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/bridge"
)

set_target_properties(selection_hook_flutter PROPERTIES
  PUBLIC_HEADER bridge/c_api.h
  OUTPUT_NAME "selection_hook_flutter"
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

target_compile_definitions(selection_hook_flutter PUBLIC DART_SHARED_LIB)

if (ANDROID)
  target_link_options(selection_hook_flutter PRIVATE "-Wl,-z,max-page-size=16384")
endif()
