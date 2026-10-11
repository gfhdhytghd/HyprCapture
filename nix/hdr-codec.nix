{ lib, stdenv, fetchurl, cmake, pkg-config, libheif, libjpeg }:
let
  source = fetchurl {
    name = "libultrahdr-2.0.2.tar.gz";
    url = "https://codeload.github.com/google/libultrahdr/tar.gz/e5f5a022fe96fc4dc2ee35c19f733a50df807abe";
    sha256 = "5a7b6347a4a32c6936b81392cd6394250649380202f86c8214042aa645cc385c";
  };
  unpacked = stdenv.mkDerivation {
    pname = "hyprcapture-ultrahdr-source";
    version = "2.0.2";
    src = source;
    dontBuild = true;
    installPhase = ''cp -r . "$out"'';
  };
  gainMapHeif = libheif.overrideAttrs (old: {
    version = "1.19.7-hyprcapture-gainmap";
    src = fetchurl {
      name = "libheif-1.19.7-gainmap.tar.gz";
      url = "https://codeload.github.com/strukturag/libheif/tar.gz/4a3f74bc593ebfc29becc1ed5dd0a61cc66d40e1";
      sha256 = "b497316d557b8a5683066b446dc0df500a6d19da36ecf03753f40f22ac4c05d3";
    };
    patches = [ "${unpacked}/cmake/patches/libheif_pr1503.patch" ];
    cmakeFlags = (old.cmakeFlags or []) ++ [ "-DWITH_EXPERIMENTAL_GAIN_MAP=ON" ];
    doCheck = false;
  });
in stdenv.mkDerivation {
  pname = "hyprcapture-ultrahdr";
  version = "2.0.2";
  src = source;
  patches = [ ../cmake/ultrahdr-heic.patch ];
  nativeBuildInputs = [ cmake pkg-config ];
  buildInputs = [ gainMapHeif libjpeg ];
  cmakeFlags = [
    "-DBUILD_SHARED_LIBS=ON"
    "-DUHDR_BUILD_DEPS=OFF"
    "-DUHDR_BUILD_EXAMPLES=ON"
    "-DUHDR_BUILD_TESTS=OFF"
    "-DUHDR_ENABLE_HEIF=ON"
    "-DUHDR_ENABLE_INSTALL=ON"
  ];
  meta.license = lib.licenses.asl20;
}
