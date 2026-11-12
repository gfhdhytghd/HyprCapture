# Qt-free engine layer; also usable by the standalone offline test project.
add_library(hyprcapture-ocr STATIC
  ${CMAKE_CURRENT_LIST_DIR}/../src/shared/ocr_engine.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/shared/ocr_result.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/shared/config.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/shared/process_environment.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/shared/supervised_process.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/shared/trusted_path.cpp)
set_target_properties(hyprcapture-ocr PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF)
target_include_directories(hyprcapture-ocr PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../src)
target_link_libraries(hyprcapture-ocr PUBLIC nlohmann_json::nlohmann_json)
if(BUILD_TESTING)
  add_executable(hyprcapture-ocr-test ${CMAKE_CURRENT_LIST_DIR}/../tests/ocr_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/shared/protocol.cpp)
  set_target_properties(hyprcapture-ocr-test PROPERTIES AUTOMOC OFF)
  target_link_libraries(hyprcapture-ocr-test PRIVATE hyprcapture-ocr)
  target_compile_options(hyprcapture-ocr-test PRIVATE -UNDEBUG)
  find_program(HYPRCAPTURE_OCR_BWRAP bwrap)
  foreach(suite parser config adapters runner join)
    if(HYPRCAPTURE_OCR_BWRAP AND (suite STREQUAL "adapters" OR suite STREQUAL "runner"))
      # Keep fake executables in the build dir, but give their parent chain a
      # trusted namespace path even when the checkout has a writable ancestor.
      # Root-owned tools are unmapped in bwrap's user namespace. A read-only
      # synthetic Nix store exercises the existing unmapped-owner trust path.
      # Only the build directory is writable on the host; networking is off.
      add_test(NAME ocr-${suite} COMMAND ${HYPRCAPTURE_OCR_BWRAP}
        --die-with-parent --unshare-net --tmpfs /
        --ro-bind /usr /usr --ro-bind /lib /lib --ro-bind /lib64 /lib64
        --symlink usr/bin /bin --ro-bind /etc /etc --proc /proc --dev /dev
        --dir /nix/store --ro-bind /usr /nix/store/ocr-system
        --bind ${CMAKE_CURRENT_BINARY_DIR} /nix/store/ocr-build
        --setenv PATH /nix/store/ocr-system/bin
        --chdir /nix/store/ocr-build /nix/store/ocr-build/hyprcapture-ocr-test ${suite})
    else()
      add_test(NAME ocr-${suite} COMMAND hyprcapture-ocr-test ${suite})
    endif()
    set_tests_properties(ocr-${suite} PROPERTIES TIMEOUT 20)
  endforeach()
  foreach(engine tesseract paddleocr)
    add_test(NAME ocr-live-${engine} COMMAND hyprcapture-ocr-test live-${engine})
    set_tests_properties(ocr-live-${engine} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
  endforeach()
endif()
