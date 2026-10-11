# Keep gain-map HEIF support private: released system libheif lacks tmap.
# Offline packagers can supply FETCHCONTENT_SOURCE_DIR_ULTRAHDR, or a
# gain-map-enabled libuhdr >= 2 through pkg-config.
option(HYPRCAPTURE_USE_SYSTEM_ULTRAHDR "Use a packager-provided libuhdr with the HEIC matrix/alpha fixes" OFF)
if(HYPRCAPTURE_USE_SYSTEM_ULTRAHDR)
  pkg_check_modules(ULTRAHDR REQUIRED IMPORTED_TARGET libuhdr>=2.0.2)
  set(HYPRCAPTURE_ULTRAHDR_TARGET PkgConfig::ULTRAHDR)
else()
  include(FetchContent)
  FetchContent_Declare(ultrahdr
    URL https://codeload.github.com/google/libultrahdr/tar.gz/e5f5a022fe96fc4dc2ee35c19f733a50df807abe
    URL_HASH SHA256=5a7b6347a4a32c6936b81392cd6394250649380202f86c8214042aa645cc385c
    PATCH_COMMAND patch -p1 -i ${CMAKE_CURRENT_LIST_DIR}/ultrahdr-heic.patch)
  function(hyprcapture_build_ultrahdr)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_AUTOMOC OFF)
    set(UHDR_BUILD_DEPS ON)
    set(UHDR_BUILD_EXAMPLES OFF)
    set(UHDR_BUILD_TESTS OFF)
    set(UHDR_ENABLE_INSTALL OFF)
    set(UHDR_ENABLE_HEIF ON)
    set(UHDR_WRITE_ISO ON)
    set(UHDR_WRITE_XMP OFF)
    FetchContent_MakeAvailable(ultrahdr)
    # Upstream's imported static libheif does not export codec dependencies.
    # Mirror those actually enabled by its private CMake configuration.
    foreach(codec x265 libde265 aom openh264 zlib libbrotlidec)
      string(MAKE_C_IDENTIFIER "UHDR_${codec}" module)
      pkg_check_modules(${module} QUIET IMPORTED_TARGET ${codec})
      if(TARGET PkgConfig::${module})
        target_link_libraries(uhdr PRIVATE PkgConfig::${module})
      endif()
    endforeach()
  endfunction()
  hyprcapture_build_ultrahdr()
  set(HYPRCAPTURE_ULTRAHDR_TARGET uhdr)
endif()
