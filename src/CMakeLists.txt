cmake_minimum_required(VERSION 3.10)

project(selection_hook_flutter_library VERSION 0.0.1 LANGUAGES C CXX)

if (APPLE)
  set(SOURCES
    "bridge/c_api_mac.mm"
    "mac/selection_hook_core.mm"
    "mac/lib/utils.mm"
    "mac/lib/clipboard.mm"
    "mac/lib/keyboard.mm"
  )
elseif (LINUX)
  set(SOURCES
    "bridge/c_api_linux.cc"
    "linux/selection_hook_core.cc"
    "linux/selection_hook_core.h"
    "linux/common.h"
    "linux/lib/keyboard.cc"
    "linux/lib/keyboard.h"
    "linux/lib/utils.cc"
    "linux/lib/utils.h"
    "linux/protocols/x11.cc"
  )
elseif (WIN32)
  # TODO(Phase 6): Replace stub with real port.
  # Copy pattern from mac/selection_hook_core.{h,mm} — strip N-API,
  # replace Windows hooks with SHSelectionCallback.
  # Then add real sources:
  #   bridge/c_api_win.cc
  #   windows/selection_hook_core.cc
  #   windows/lib/clipboard.cc
  #   windows/lib/keyboard.cc
  #   windows/lib/string_pool.cc
  #   windows/lib/utils.cc
  set(SOURCES "bridge/c_api.cc")
else()
  set(SOURCES "bridge/c_api.cc")
endif()

add_library(selection_hook_flutter SHARED ${SOURCES})

target_include_directories(selection_hook_flutter PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/bridge"
)
target_include_directories(selection_hook_flutter PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

set_target_properties(selection_hook_flutter PROPERTIES
  PUBLIC_HEADER bridge/c_api.h
  OUTPUT_NAME "selection_hook_flutter"
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

if (APPLE)
  target_compile_options(selection_hook_flutter PRIVATE -fobjc-arc)
  target_link_libraries(selection_hook_flutter PRIVATE
    "-framework ApplicationServices"
    "-framework Cocoa"
  )
endif()

if (LINUX)
  target_link_libraries(selection_hook_flutter PRIVATE
    X11 Xfixes Xtst
  )
endif()

if (WIN32)
  # TODO(Phase 6): Add Windows dependencies when real port is ready.
  # target_link_libraries(selection_hook_flutter PRIVATE
  #   ole32 oleaut32 user32 gdi32
  # )
endif()

target_compile_definitions(selection_hook_flutter PUBLIC DART_SHARED_LIB)

if (ANDROID)
  target_link_options(selection_hook_flutter PRIVATE "-Wl,-z,max-page-size=16384")
endif()
