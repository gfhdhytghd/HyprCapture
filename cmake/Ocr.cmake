find_package(OpenSSL REQUIRED COMPONENTS Crypto)

# Qt-free engine layer; also usable by the standalone offline test project.
add_library(hyprcapture-ocr STATIC
  ${CMAKE_CURRENT_LIST_DIR}/../src/shared/ocr_api.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/shared/ocr_engine.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/shared/ocr_result.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/shared/config.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/shared/process_environment.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/shared/supervised_process.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/shared/trusted_path.cpp)
set_target_properties(hyprcapture-ocr PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF)
target_include_directories(hyprcapture-ocr PUBLIC ${CMAKE_CURRENT_LIST_DIR}/../src)
target_link_libraries(hyprcapture-ocr PUBLIC nlohmann_json::nlohmann_json OpenSSL::Crypto)
add_executable(hyprcapture-ocr-cli ${CMAKE_CURRENT_LIST_DIR}/../src/ocr/main.cpp)
set_target_properties(hyprcapture-ocr-cli PROPERTIES OUTPUT_NAME hyprcapture-ocr AUTOMOC OFF)
target_link_libraries(hyprcapture-ocr-cli PRIVATE hyprcapture-ocr)
include(GNUInstallDirs)
install(TARGETS hyprcapture-ocr-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(BUILD_TESTING)
  add_executable(hyprcapture-ocr-test ${CMAKE_CURRENT_LIST_DIR}/../tests/ocr_test.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/shared/protocol.cpp)
  set_target_properties(hyprcapture-ocr-test PROPERTIES AUTOMOC OFF)
  target_link_libraries(hyprcapture-ocr-test PRIVATE hyprcapture-ocr)
  target_compile_options(hyprcapture-ocr-test PRIVATE -UNDEBUG)
  find_program(HYPRCAPTURE_OCR_BWRAP bwrap)
  foreach(suite parser api-parser api-cache config adapters runner join)
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
  add_executable(hyprcapture-ocr-api-driver ${CMAKE_CURRENT_LIST_DIR}/../tests/ocr_api_driver.cpp)
  set_target_properties(hyprcapture-ocr-api-driver PROPERTIES AUTOMOC OFF)
  target_link_libraries(hyprcapture-ocr-api-driver PRIVATE hyprcapture-ocr)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  if(HYPRCAPTURE_OCR_BWRAP)
    find_program(HYPRCAPTURE_OCR_SYSTEM_PYTHON NAMES python3 PATHS /usr/bin NO_DEFAULT_PATH REQUIRED)
    add_test(NAME ocr-api-http COMMAND ${HYPRCAPTURE_OCR_BWRAP}
      --die-with-parent --unshare-net --tmpfs /
      --ro-bind /usr /usr --ro-bind /lib /lib --ro-bind /lib64 /lib64
      --symlink usr/bin /bin --ro-bind /etc /etc --proc /proc --dev /dev
      --dir /nix/store --ro-bind /usr /nix/store/ocr-system
      --bind ${CMAKE_CURRENT_BINARY_DIR} /nix/store/ocr-build
      --ro-bind ${CMAKE_CURRENT_LIST_DIR}/../tests/ocr_api_http_test.py /ocr_api_http_test.py
      --ro-bind ${CMAKE_CURRENT_LIST_DIR}/../resources/ocr-probe.png /ocr-probe.png
      --setenv PATH /nix/store/ocr-system/bin
      --chdir /nix/store/ocr-build ${HYPRCAPTURE_OCR_SYSTEM_PYTHON} -I /ocr_api_http_test.py /nix/store/ocr-build/hyprcapture-ocr-api-driver /nix/store/ocr-build/hyprcapture-ocr /ocr-probe.png)
  else()
    add_test(NAME ocr-api-http COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_LIST_DIR}/../tests/ocr_api_http_test.py $<TARGET_FILE:hyprcapture-ocr-api-driver> $<TARGET_FILE:hyprcapture-ocr-cli> ${CMAKE_CURRENT_LIST_DIR}/../resources/ocr-probe.png)
  endif()
  set_tests_properties(ocr-api-http PROPERTIES TIMEOUT 60)
  foreach(engine tesseract paddleocr)
    add_test(NAME ocr-live-${engine} COMMAND hyprcapture-ocr-test live-${engine})
    set_tests_properties(ocr-live-${engine} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
  endforeach()
endif()
