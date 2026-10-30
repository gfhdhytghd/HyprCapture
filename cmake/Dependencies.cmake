# Check required system dependencies before downloading private HEIC sources.
# Keep optional HDR/inference probes outside this list.
set(_hyprcapture_missing "")
set(_hyprcapture_arch_packages "")

macro(hyprcapture_missing_dependency name arch_packages)
  string(APPEND _hyprcapture_missing "\n  - ${name}")
  list(APPEND _hyprcapture_arch_packages ${arch_packages})
endmacro()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  macro(hyprcapture_check_pkg prefix arch_packages)
    pkg_check_modules(${prefix} QUIET IMPORTED_TARGET ${ARGN})
    if(NOT ${prefix}_FOUND)
      hyprcapture_missing_dependency("${ARGN} (pkg-config)" "${arch_packages}")
    endif()
  endmacro()
  hyprcapture_check_pkg(HYPR_DEPS "hyprland" hyprland)
  hyprcapture_check_pkg(GIO "glib2" gio-2.0 gio-unix-2.0)
  hyprcapture_check_pkg(LUA "lua" lua)
  hyprcapture_check_pkg(PULSE "libpulse" libpulse)
  hyprcapture_check_pkg(PIPEWIRE "libpipewire" libpipewire-0.3)
  hyprcapture_check_pkg(FFMPEG_MUX "ffmpeg" libavformat libavcodec libavutil)
  hyprcapture_check_pkg(FFTW "fftw" fftw3)
  hyprcapture_check_pkg(SWRESAMPLE "ffmpeg" libswresample libavutil)
else()
  hyprcapture_missing_dependency("pkg-config (required to check Hyprland and audio libraries)" "pkgconf")
endif()

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Svg DBus Network)
if(NOT Qt6_FOUND)
  foreach(_component Core Gui Widgets Svg DBus Network)
    if(NOT Qt6${_component}_FOUND)
      if(_component STREQUAL "Svg")
        hyprcapture_missing_dependency("Qt6${_component}" "qt6-svg")
      else()
        hyprcapture_missing_dependency("Qt6${_component}" "qt6-base")
      endif()
    endif()
  endforeach()
  if(Qt6_NOT_FOUND_MESSAGE)
    string(APPEND _hyprcapture_missing "\n    Qt6: ${Qt6_NOT_FOUND_MESSAGE}")
  endif()
endif()

find_package(LayerShellQt QUIET)
if(NOT LayerShellQt_FOUND)
  hyprcapture_missing_dependency("LayerShellQt" "layer-shell-qt")
endif()
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  hyprcapture_missing_dependency("nlohmann_json" "nlohmann-json")
endif()
find_package(OpenCV QUIET COMPONENTS core imgproc)
if(NOT OpenCV_FOUND)
  hyprcapture_missing_dependency("OpenCV core/imgproc (OpenCVConfig.cmake or opencv-config.cmake)" "opencv")
endif()

if(_hyprcapture_missing)
  list(REMOVE_DUPLICATES _hyprcapture_arch_packages)
  list(JOIN _hyprcapture_arch_packages " " _hyprcapture_arch_command)
  message(FATAL_ERROR
    "HyprCapture: missing required build dependencies:${_hyprcapture_missing}\n\n"
    "Arch Linux:\n  sudo pacman -Syu --needed ${_hyprcapture_arch_command}\n\n"
    "Other distributions: install the matching development packages.\n"
    "For a custom prefix, set CMAKE_PREFIX_PATH or PKG_CONFIG_PATH;\n"
    "OpenCV_DIR must point to the directory containing OpenCVConfig.cmake.\n"
    "Hyprland headers must match the running compositor (hyprctl version).\n"
    "See README.md: Dependencies and Troubleshoot hyprpm build failures.\n"
    "When using hyprpm, run hyprpm update -v to display this report.")
endif()
