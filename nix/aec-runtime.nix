{ lib, stdenv, fetchurl, runCommand, cmake, unzip }:
let
  # Exact dependency revisions from TensorFlow 2.14.0's CMake files.
  manifest = builtins.fromJSON (builtins.readFile ./aec-sources.json);
  sources = builtins.listToAttrs (map (entry: {
    name = entry.name;
    value = runCommand "aec-${entry.name}-source" { nativeBuildInputs = [ unzip ]; } ''
      mkdir unpack "$out"
      cd unpack
      ${if lib.hasSuffix ".zip" entry.url then "unzip -q" else "tar -xf"} ${fetchurl { inherit (entry) url sha256; }}
      cp -r */. "$out/"
    '';
  }) manifest);
in stdenv.mkDerivation {
  pname = "hyprcapture-tflite";
  version = "2.14.0";
  src = fetchurl {
    url = "https://codeload.github.com/tensorflow/tensorflow/tar.gz/refs/tags/v2.14.0";
    sha256 = "ce357fd0728f0d1b0831d1653f475591662ec5bca736a94ff789e6b1944df19f";
    name = "tensorflow-2.14.0.tar.gz";
  };
  nativeBuildInputs = [ cmake ];
  cmakeDir = "../tensorflow/lite/c";
  cmakeFlags = [
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
    "-DTFLITE_ENABLE_XNNPACK=ON"
    "-DTFLITE_ENABLE_GPU=OFF"
    "-DTFLITE_ENABLE_NNAPI=OFF"
    "-DTFLITE_ENABLE_INSTALL=OFF"
    "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
  ];
  # TensorFlow patches Eigen during configure; the Nix store is immutable.
  preConfigure = ''
    mkdir -p aec-deps
    ${lib.concatMapStringsSep "\n" (entry: ''
      cp -r ${sources.${entry.name}} aec-deps/${entry.name}
      chmod -R u+w aec-deps/${entry.name}
      cmakeFlagsArray+=("-DFETCHCONTENT_SOURCE_DIR_${lib.toUpper entry.name}=$PWD/aec-deps/${entry.name}")
    '') manifest}
    cmakeFlagsArray+=("-DFP16_SOURCE_DIR=$PWD/aec-deps/fp16" "-DFXDIV_SOURCE_DIR=$PWD/aec-deps/fxdiv"
      "-DPTHREADPOOL_SOURCE_DIR=$PWD/aec-deps/pthreadpool" "-DPSIMD_SOURCE_DIR=$PWD/aec-deps/psimd")
  '';
  buildFlags = [ "tensorflowlite_c" ];
  installPhase = ''
    install -Dm755 libtensorflowlite_c.so "$out/lib/libtensorflowlite_c.so"
    install -Dm644 ../LICENSE "$out/share/licenses/TensorFlow-LICENSE.txt"
    ${lib.concatMapStringsSep "\n" (entry: ''
      mkdir -p "$out/share/licenses/${entry.name}"
      find ${sources.${entry.name}} -maxdepth 2 -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' \) -exec cp {} "$out/share/licenses/${entry.name}/" \;
    '') manifest}
  '';
  meta.license = lib.licenses.asl20;
  meta.platforms = [ "x86_64-linux" "aarch64-linux" ];
}
