cmake_minimum_required(VERSION 3.19)
project(HyprCapture VERSION "0.2.7")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(CTest)
include(GNUInstallDirs)

set(HYPRCAPTURE_DEFAULT_HELPER_PATH "" CACHE FILEPATH "Optional trusted default path to hyprcapture-ui")
set(HYPRCAPTURE_TRUSTED_BIN_DIRS "" CACHE STRING "Colon-separated extra trusted program directories")

add_compile_definitions(WLR_USE_UNSTABLE)
if(HYPRCAPTURE_DEFAULT_HELPER_PATH)
  add_compile_definitions(HYPRCAPTURE_DEFAULT_HELPER_PATH="${HYPRCAPTURE_DEFAULT_HELPER_PATH}")
endif()
if(HYPRCAPTURE_TRUSTED_BIN_DIRS)
  add_compile_definitions(HYPRCAPTURE_TRUSTED_BIN_DIRS="${HYPRCAPTURE_TRUSTED_BIN_DIRS}")
endif()
add_compile_options(
  -g
  -Wall
  -Wextra
  -Wno-unused-parameter
  -Wno-unused-value
  -Wno-missing-field-initializers
  -Wno-narrowing
  -Wno-pointer-arith
  --no-gnu-unique
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPR_DEPS REQUIRED hyprland)
pkg_check_modules(GIO REQUIRED gio-2.0 gio-unix-2.0)
pkg_check_modules(LUA REQUIRED lua)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg DBus)
find_package(LayerShellQt REQUIRED)
find_package(nlohmann_json REQUIRED)

set(HYPRLAND_SOURCE_DIR "" CACHE PATH "Optional Hyprland source checkout to use for plugin headers")
set(HYPRLAND_INCLUDE_DIRS ${HYPR_DEPS_INCLUDE_DIRS})

if(HYPRLAND_SOURCE_DIR)
  if(NOT EXISTS "${HYPRLAND_SOURCE_DIR}/src/plugins/PluginAPI.hpp")
    message(FATAL_ERROR "HYPRLAND_SOURCE_DIR must point at a Hyprland checkout")
  endif()

  set(HYPRLAND_LOCAL_HEADER_ROOT "${CMAKE_CURRENT_BINARY_DIR}/hyprland-local-headers")
  file(MAKE_DIRECTORY "${HYPRLAND_LOCAL_HEADER_ROOT}/hyprland")
  file(REMOVE "${HYPRLAND_LOCAL_HEADER_ROOT}/hyprland/src")
  file(REMOVE "${HYPRLAND_LOCAL_HEADER_ROOT}/hyprland/protocols")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink "${HYPRLAND_SOURCE_DIR}/src" "${HYPRLAND_LOCAL_HEADER_ROOT}/hyprland/src")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink "${HYPRLAND_SOURCE_DIR}/protocols" "${HYPRLAND_LOCAL_HEADER_ROOT}/hyprland/protocols")
  list(PREPEND HYPRLAND_INCLUDE_DIRS "${HYPRLAND_LOCAL_HEADER_ROOT}")
endif()

set(HYPRLAND_DERIVED_INCLUDE_DIRS)
foreach(HYPRLAND_INCLUDE_DIR IN LISTS HYPRLAND_INCLUDE_DIRS)
  list(APPEND HYPRLAND_DERIVED_INCLUDE_DIRS "${HYPRLAND_INCLUDE_DIR}")
  if(EXISTS "${HYPRLAND_INCLUDE_DIR}/hyprland")
    list(APPEND HYPRLAND_DERIVED_INCLUDE_DIRS "${HYPRLAND_INCLUDE_DIR}/hyprland")
  endif()
  if(EXISTS "${HYPRLAND_INCLUDE_DIR}/hyprland/src")
    list(APPEND HYPRLAND_DERIVED_INCLUDE_DIRS "${HYPRLAND_INCLUDE_DIR}/hyprland/src")
  endif()
  if(EXISTS "${HYPRLAND_INCLUDE_DIR}/hyprland/protocols")
    list(APPEND HYPRLAND_DERIVED_INCLUDE_DIRS "${HYPRLAND_INCLUDE_DIR}/hyprland/protocols")
  endif()
endforeach()
list(REMOVE_DUPLICATES HYPRLAND_DERIVED_INCLUDE_DIRS)

add_library(hyprcapture SHARED
  src/plugin/main.cpp
  src/plugin/notification.cpp
  src/plugin/artifact_capture.cpp
  src/plugin/window_stream.cpp
  src/plugin/window_stream_control.cpp
  src/plugin/window_stream_sender.cpp
  src/plugin/window_gpu_wire.cpp
  src/plugin/window_gpu_sender.cpp
  src/plugin/window_gpu_export.cpp
  src/plugin/recording.cpp
  src/plugin/recording_state.cpp
  src/plugin/session_launcher.cpp
  src/plugin/timing.cpp
  src/shared/config.cpp
  src/shared/process_environment.cpp
  src/shared/supervised_process.cpp
  src/shared/protocol.cpp
  src/shared/rgba_transform.cpp
  src/shared/trusted_path.cpp
)

target_include_directories(hyprcapture BEFORE PRIVATE src ${HYPRLAND_DERIVED_INCLUDE_DIRS} ${LUA_INCLUDE_DIRS})
target_link_directories(hyprcapture PRIVATE ${HYPR_DEPS_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS})
target_link_libraries(hyprcapture PRIVATE ${HYPR_DEPS_LIBRARIES} ${LUA_LIBRARIES} nlohmann_json::nlohmann_json)
target_link_options(hyprcapture PRIVATE -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)

add_executable(hyprcapture-ui
  src/ui/main.cpp
  src/ui/clipboard_utils.cpp
  src/ui/capture_overlay.cpp
  src/ui/overlay_paint.cpp
  src/ui/result_thumbnail.cpp
  src/ui/screenshot_notification.cpp
  src/ui/watermark.cpp
  src/shared/config.cpp
  src/shared/process_environment.cpp
  src/shared/protocol.cpp
  src/shared/trusted_path.cpp
)

target_include_directories(hyprcapture-ui PRIVATE src)
target_include_directories(hyprcapture-ui PRIVATE ${GIO_INCLUDE_DIRS})
target_link_directories(hyprcapture-ui PRIVATE ${GIO_LIBRARY_DIRS})
target_link_libraries(hyprcapture-ui PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::DBus LayerShellQt::Interface nlohmann_json::nlohmann_json)
target_link_libraries(hyprcapture-ui PRIVATE ${GIO_LIBRARIES})
target_link_options(hyprcapture-ui PRIVATE -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)

if(BUILD_TESTING)
  add_executable(hyprcapture-supervised-process-test
    tests/supervised_process_test.cpp
    src/shared/supervised_process.cpp
  )
  target_include_directories(hyprcapture-supervised-process-test PRIVATE src)
  add_test(NAME hyprcapture-supervised-process-test COMMAND hyprcapture-supervised-process-test)

  add_executable(hyprcapture-config-test
    tests/config_test.cpp
    src/shared/config.cpp
    src/shared/protocol.cpp
  )
  target_include_directories(hyprcapture-config-test PRIVATE src)
  target_link_libraries(hyprcapture-config-test PRIVATE nlohmann_json::nlohmann_json)
  add_test(NAME hyprcapture-config-test COMMAND hyprcapture-config-test)

  add_executable(hyprcapture-watermark-test
    tests/watermark_test.cpp
    src/ui/watermark.cpp
    src/shared/config.cpp
  )
  target_include_directories(hyprcapture-watermark-test PRIVATE src)
  target_link_libraries(hyprcapture-watermark-test PRIVATE Qt6::Gui Qt6::Svg)
  add_test(NAME hyprcapture-watermark-test COMMAND hyprcapture-watermark-test)

  add_executable(hyprcapture-security-test
    tests/security_test.cpp
    src/ui/clipboard_utils.cpp
    src/shared/process_environment.cpp
    src/shared/trusted_path.cpp
  )
  target_include_directories(hyprcapture-security-test PRIVATE src)
  target_link_libraries(hyprcapture-security-test PRIVATE Qt6::Core Qt6::Gui)
  add_test(NAME hyprcapture-security-test COMMAND hyprcapture-security-test)

  add_executable(hyprcapture-process-environment-test
    tests/process_environment_test.cpp
    src/shared/process_environment.cpp
  )
  target_include_directories(hyprcapture-process-environment-test PRIVATE src)
  add_test(NAME hyprcapture-process-environment-test COMMAND hyprcapture-process-environment-test)

  add_executable(hyprcapture-overlay-paint-test
    tests/overlay_paint_test.cpp
    src/ui/overlay_paint.cpp
  )
  target_include_directories(hyprcapture-overlay-paint-test PRIVATE src)
  target_link_libraries(hyprcapture-overlay-paint-test PRIVATE Qt6::Gui)
  add_test(NAME hyprcapture-overlay-paint-test COMMAND hyprcapture-overlay-paint-test)

  add_executable(hyprcapture-recording-state-test
    tests/recording_state_test.cpp
    src/plugin/recording_state.cpp
  )
  target_include_directories(hyprcapture-recording-state-test PRIVATE src)
  target_link_libraries(hyprcapture-recording-state-test PRIVATE nlohmann_json::nlohmann_json)
  add_test(NAME hyprcapture-recording-state-test COMMAND hyprcapture-recording-state-test)

  add_executable(hyprcapture-rgba-transform-test
    tests/rgba_transform_test.cpp
    src/shared/rgba_transform.cpp
  )
  target_include_directories(hyprcapture-rgba-transform-test PRIVATE src)
  add_test(NAME hyprcapture-rgba-transform-test COMMAND hyprcapture-rgba-transform-test)

  add_executable(hyprcapture-trusted-path-test
    tests/trusted_path_test.cpp
    src/shared/trusted_path.cpp
  )
  target_include_directories(hyprcapture-trusted-path-test PRIVATE src)
  add_test(NAME hyprcapture-trusted-path-test COMMAND hyprcapture-trusted-path-test)

  add_executable(hyprcapture-window-stream-test
    tests/window_stream_test.cpp
    src/plugin/window_stream.cpp
  )
  target_include_directories(hyprcapture-window-stream-test PRIVATE src)
  target_compile_options(hyprcapture-window-stream-test PRIVATE -UNDEBUG)
  add_test(NAME hyprcapture-window-stream-test COMMAND hyprcapture-window-stream-test)

  add_executable(hyprcapture-window-stream-sender-test
    tests/window_stream_sender_test.cpp
    src/plugin/window_stream.cpp
    src/plugin/window_stream_sender.cpp
  )
  target_include_directories(hyprcapture-window-stream-sender-test PRIVATE src)
  target_compile_options(hyprcapture-window-stream-sender-test PRIVATE -UNDEBUG)
  add_test(NAME hyprcapture-window-stream-sender-test COMMAND hyprcapture-window-stream-sender-test)

  add_executable(hyprcapture-window-gpu-wire-test
    tests/window_gpu_wire_test.cpp
    src/plugin/window_gpu_wire.cpp
  )
  target_include_directories(hyprcapture-window-gpu-wire-test PRIVATE src)
  target_compile_options(hyprcapture-window-gpu-wire-test PRIVATE -UNDEBUG)
  add_test(NAME hyprcapture-window-gpu-wire-test COMMAND hyprcapture-window-gpu-wire-test)

  add_executable(hyprcapture-window-gpu-sender-test
    tests/window_gpu_sender_test.cpp
    src/plugin/window_gpu_wire.cpp
    src/plugin/window_gpu_sender.cpp
  )
  target_include_directories(hyprcapture-window-gpu-sender-test PRIVATE src)
  target_compile_options(hyprcapture-window-gpu-sender-test PRIVATE -UNDEBUG)
  target_link_libraries(hyprcapture-window-gpu-sender-test PRIVATE Threads::Threads)
  add_test(NAME hyprcapture-window-gpu-sender-test COMMAND hyprcapture-window-gpu-sender-test)
  set_tests_properties(hyprcapture-window-gpu-sender-test PROPERTIES TIMEOUT 10)

  option(HYPRCAPTURE_GPU_EXPORT_TEST "Build hardware-dependent EGL DMA-BUF export test" OFF)
  if(HYPRCAPTURE_GPU_EXPORT_TEST)
    pkg_check_modules(GPU_EXPORT_EGL REQUIRED IMPORTED_TARGET egl glesv2)
    add_executable(hyprcapture-window-gpu-export-test
      tests/window_gpu_export_test.cpp
      src/plugin/window_gpu_export.cpp
      src/plugin/window_gpu_wire.cpp
      src/plugin/window_gpu_sender.cpp
    )
    target_include_directories(hyprcapture-window-gpu-export-test PRIVATE src)
    target_compile_options(hyprcapture-window-gpu-export-test PRIVATE -UNDEBUG)
    target_link_libraries(hyprcapture-window-gpu-export-test PRIVATE PkgConfig::GPU_EXPORT_EGL Threads::Threads)
    add_test(NAME hyprcapture-window-gpu-export-test COMMAND hyprcapture-window-gpu-export-test)
    set_tests_properties(hyprcapture-window-gpu-export-test PROPERTIES TIMEOUT 15)
  endif()

  add_executable(hyprcapture-window-stream-control-test
    tests/window_stream_control_test.cpp
    src/plugin/window_stream_control.cpp
  )
  target_include_directories(hyprcapture-window-stream-control-test PRIVATE src)
  target_compile_options(hyprcapture-window-stream-control-test PRIVATE -UNDEBUG)
  target_link_libraries(hyprcapture-window-stream-control-test PRIVATE nlohmann_json::nlohmann_json)
  add_test(NAME hyprcapture-window-stream-control-test COMMAND hyprcapture-window-stream-control-test)

  add_executable(hyprcapture-window-stream-extent-test tests/window_stream_extent_test.cpp)
  target_include_directories(hyprcapture-window-stream-extent-test PRIVATE src)
  target_compile_options(hyprcapture-window-stream-extent-test PRIVATE -UNDEBUG)
  add_test(NAME hyprcapture-window-stream-extent-test COMMAND hyprcapture-window-stream-extent-test)

  add_executable(hyprcapture-window-stream-cadence-test tests/window_stream_cadence_test.cpp)
  target_include_directories(hyprcapture-window-stream-cadence-test PRIVATE src)
  target_compile_options(hyprcapture-window-stream-cadence-test PRIVATE -UNDEBUG)
  add_test(NAME hyprcapture-window-stream-cadence-test COMMAND hyprcapture-window-stream-cadence-test)

endif()

install(TARGETS hyprcapture LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS hyprcapture-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
