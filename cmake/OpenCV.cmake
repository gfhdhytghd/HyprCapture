# The standalone helper must survive system OpenCV SONAME changes.
if(HYPRCAPTURE_USE_SYSTEM_OPENCV)
  return()
endif()

include(FetchContent)
FetchContent_Declare(hyprcapture_opencv
  URL https://codeload.github.com/opencv/opencv/tar.gz/refs/tags/5.1.0
  URL_HASH SHA256=638c1dae7e37e45afea21aa217dc667ad7cf57acc66cdf652c074f1e0e5ad6d2)

function(hyprcapture_build_opencv)
  set(BUILD_SHARED_LIBS OFF)
  set(CMAKE_AUTOMOC OFF)
  set(CMAKE_CXX_STANDARD 17)
  set(BUILD_LIST core,imgproc CACHE STRING "OpenCV modules used by HyprCapture" FORCE)
  set(BUILD_TESTS OFF)
  set(BUILD_PERF_TESTS OFF)
  set(BUILD_opencv_apps OFF)
  set(BUILD_opencv_python2 OFF)
  set(BUILD_opencv_python3 OFF)
  set(BUILD_JAVA OFF)
  set(WITH_IPP OFF)
  set(WITH_ITT OFF)
  set(WITH_OPENCL OFF)
  set(WITH_LAPACK OFF)
  set(WITH_EIGEN OFF)
  set(WITH_ADE OFF)
  set(WITH_PROTOBUF OFF)
  set(WITH_VTK OFF)
  set(WITH_GTK OFF)
  set(WITH_FFMPEG OFF)
  set(WITH_GSTREAMER OFF)
  set(WITH_OPENJPEG OFF)
  set(WITH_OPENEXR OFF)
  set(WITH_JPEG OFF)
  set(WITH_PNG OFF)
  set(WITH_TIFF OFF)
  set(WITH_WEBP OFF)
  FetchContent_GetProperties(hyprcapture_opencv)
  if(NOT hyprcapture_opencv_POPULATED)
    FetchContent_Populate(hyprcapture_opencv)
  endif()
  # OpenCV initializes this legacy cache entry. Keep HyprCapture's existing
  # executable layout, which is also used by hyprpm and Omarchy.
  if(DEFINED CACHE{EXECUTABLE_OUTPUT_PATH})
    set(keep_executable_output_path TRUE)
  endif()
  add_subdirectory("${hyprcapture_opencv_SOURCE_DIR}" "${hyprcapture_opencv_BINARY_DIR}" EXCLUDE_FROM_ALL)
  if(NOT keep_executable_output_path)
    unset(EXECUTABLE_OUTPUT_PATH CACHE)
  endif()
  set(OpenCV_INCLUDE_DIRS
    "${CMAKE_BINARY_DIR}"
    "${hyprcapture_opencv_SOURCE_DIR}/modules/core/include"
    "${hyprcapture_opencv_SOURCE_DIR}/modules/imgproc/include"
    PARENT_SCOPE)
  set(HYPRCAPTURE_OPENCV_LICENSE "${hyprcapture_opencv_SOURCE_DIR}/LICENSE" PARENT_SCOPE)
endfunction()

hyprcapture_build_opencv()
set(OpenCV_LIBS opencv_core opencv_imgproc)
configure_file("${HYPRCAPTURE_OPENCV_LICENSE}" "${CMAKE_CURRENT_BINARY_DIR}/opencv-LICENSE" COPYONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/opencv-LICENSE"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/hyprcapture")
