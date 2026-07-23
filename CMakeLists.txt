cmake_minimum_required(VERSION 3.19)
project(HyprCapture VERSION "0.2.4")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(CTest)
include(GNUInstallDirs)

add_compile_definitions(WLR_USE_UNSTABLE)
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
  src/plugin/recording.cpp
  src/plugin/session_launcher.cpp
  src/plugin/timing.cpp
  src/shared/config.cpp
  src/shared/protocol.cpp
)

target_include_directories(hyprcapture BEFORE PRIVATE src ${HYPRLAND_DERIVED_INCLUDE_DIRS} ${LUA_INCLUDE_DIRS})
target_link_directories(hyprcapture PRIVATE ${HYPR_DEPS_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS})
target_link_libraries(hyprcapture PRIVATE ${HYPR_DEPS_LIBRARIES} ${LUA_LIBRARIES} nlohmann_json::nlohmann_json)
target_link_options(hyprcapture PRIVATE -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)

add_executable(hyprcapture-ui
  src/ui/main.cpp
  src/ui/clipboard_utils.cpp
  src/ui/capture_overlay.cpp
  src/ui/result_thumbnail.cpp
  src/ui/watermark.cpp
  src/shared/config.cpp
  src/shared/protocol.cpp
)

target_include_directories(hyprcapture-ui PRIVATE src)
target_include_directories(hyprcapture-ui PRIVATE ${GIO_INCLUDE_DIRS})
target_link_directories(hyprcapture-ui PRIVATE ${GIO_LIBRARY_DIRS})
target_link_libraries(hyprcapture-ui PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::DBus LayerShellQt::Interface nlohmann_json::nlohmann_json)
target_link_libraries(hyprcapture-ui PRIVATE ${GIO_LIBRARIES})
target_link_options(hyprcapture-ui PRIVATE -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)

if(BUILD_TESTING)
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
  )
  target_include_directories(hyprcapture-security-test PRIVATE src)
  target_link_libraries(hyprcapture-security-test PRIVATE Qt6::Core Qt6::Gui)
  add_test(NAME hyprcapture-security-test COMMAND hyprcapture-security-test)
endif()

install(TARGETS hyprcapture LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS hyprcapture-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
