"""Configure a tiny project to exercise missing-dependency diagnostics offline."""
import pathlib
import subprocess
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="hyprcapture-dependencies-") as folder:
    root = pathlib.Path(folder)
    modules = root / "modules"
    modules.mkdir()
    (modules / "FindPkgConfig.cmake").write_text("""
set(PkgConfig_FOUND TRUE)
macro(pkg_check_modules prefix)
  set(${prefix}_FOUND TRUE)
  set(${prefix}_INCLUDE_DIRS fixture-include)
  if("${prefix}" STREQUAL "${FIXTURE_MISSING_PKG}")
    set(${prefix}_FOUND FALSE)
  else()
    add_library(PkgConfig::${prefix} INTERFACE IMPORTED)
  endif()
endmacro()
""")
    for package in ("LayerShellQt", "nlohmann_json", "OpenCV"):
        (modules / f"Find{package}.cmake").write_text(f"set({package}_FOUND TRUE)\n")
    (modules / "FindQt6.cmake").write_text("""
set(Qt6_FOUND TRUE)
foreach(component IN LISTS Qt6_FIND_COMPONENTS)
  set(Qt6${component}_FOUND TRUE)
  if("${component}" STREQUAL "${FIXTURE_MISSING_QT}")
    set(Qt6${component}_FOUND FALSE)
    set(Qt6_FOUND FALSE)
  endif()
endforeach()
""")
    (root / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.19)
project(dependency-diagnostics NONE)
list(PREPEND CMAKE_MODULE_PATH "{modules.as_posix()}")
include("{(repo / 'cmake/Dependencies.cmake').as_posix()}")
if(NOT TARGET PkgConfig::PULSE OR NOT TARGET PkgConfig::FFTW OR
   NOT HYPR_DEPS_INCLUDE_DIRS STREQUAL "fixture-include")
  message(FATAL_ERROR "Dependency results were lost outside the checker")
endif()
""")
    cases = [
        ([], []),
        (["-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=TRUE"], ["OpenCV core/imgproc", "--needed opencv"]),
        (["-DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=TRUE",
          "-DCMAKE_DISABLE_FIND_PACKAGE_LayerShellQt=TRUE"], ["LayerShellQt", "OpenCV core/imgproc", "--needed layer-shell-qt opencv"]),
        (["-DFIXTURE_MISSING_PKG=PIPEWIRE"], ["libpipewire-0.3 (pkg-config)", "--needed libpipewire"]),
        (["-DFIXTURE_MISSING_QT=Svg"], ["Qt6Svg", "--needed qt6-svg"]),
        (["-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=TRUE"], ["pkg-config (required", "--needed pkgconf"]),
    ]
    for index, (options, expected) in enumerate(cases):
        result = subprocess.run(
            ["cmake", "-S", str(root), "-B", str(root / f"build-{index}"), *options],
            capture_output=True, text=True, timeout=30,
        )
        output = result.stdout + result.stderr
        if expected:
            assert result.returncode != 0, output
            assert "HyprCapture: missing required build dependencies:" in output, output
            assert "hyprpm update -v" in output, output
            for value in expected:
                assert value in output, output
        else:
            assert result.returncode == 0, output
print("PASS: dependency success, OpenCV, multiple missing packages, PipeWire, Qt Svg, and pkg-config")
