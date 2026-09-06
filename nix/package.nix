{
  lib,
  callPackage,
  fetchurl,
  runCommand,
  pipewire,
  fftw,
  cmake,
  glib,
  hyprland,
  hyprlandPlugins,
  kdePackages,
  lua,
  libpulseaudio,
  ffmpeg,
  nlohmann_json,
  pkg-config,
  src,
}:
let
  aecRuntime = callPackage ./aec-runtime.nix { };
  aecModels = runCommand "hyprcapture-dtln-models" { } ''
    mkdir -p "$out"
    cp ${fetchurl { url = "https://raw.githubusercontent.com/breizhn/DTLN-aec/9d24e128b4f409db18227b8babb343016625921f/pretrained_models/dtln_aec_256_1.tflite"; sha256 = "4a3a588b69fd79d837bc068b579a26faa92cac39dddbb00001d2dc1c3d869d60"; }} "$out/dtln_aec_256_1.tflite"
    cp ${fetchurl { url = "https://raw.githubusercontent.com/breizhn/DTLN-aec/9d24e128b4f409db18227b8babb343016625921f/pretrained_models/dtln_aec_256_2.tflite"; sha256 = "fa2590243aad1bf893c5be45b20709e8c50feec65e3604d1d52bae6eeddc23d3"; }} "$out/dtln_aec_256_2.tflite"
    cp ${fetchurl { url = "https://raw.githubusercontent.com/breizhn/DTLN-aec/9d24e128b4f409db18227b8babb343016625921f/pretrained_models/dtln_aec_512_1.tflite"; sha256 = "569f7c3cfac96b1e093229c3ca10b5d892f5b1906105b644e457f8245b4f7383"; }} "$out/dtln_aec_512_1.tflite"
    cp ${fetchurl { url = "https://raw.githubusercontent.com/breizhn/DTLN-aec/9d24e128b4f409db18227b8babb343016625921f/pretrained_models/dtln_aec_512_2.tflite"; sha256 = "fb423d867ab25d5f4716bd369c7126b6c84926175c019b832e71bf21de0e9907"; }} "$out/dtln_aec_512_2.tflite"
  '';
in hyprlandPlugins.mkHyprlandPlugin {
  pluginName = "hyprcapture";
  version = "0.2.7";
  inherit src;

  nativeBuildInputs = [
    cmake
    pkg-config
    kdePackages.wrapQtAppsHook
  ];

  buildInputs = [
    pipewire
    fftw
    ffmpeg
    glib
    kdePackages.layer-shell-qt
    kdePackages.qtbase
    kdePackages.qtsvg
    lua
    libpulseaudio
    nlohmann_json
  ];

  cmakeFlags = [
    "-DHYPRCAPTURE_AEC_MODEL_DIR=${aecModels}"
    "-DHYPRCAPTURE_TFLITE_LIBRARY=${aecRuntime}/lib/libtensorflowlite_c.so"
    "-DHYPRCAPTURE_DEFAULT_HELPER_PATH=${builtins.placeholder "out"}/bin/hyprcapture-ui"
    "-DHYPRCAPTURE_TRUSTED_BIN_DIRS=${lib.makeBinPath [ hyprland ffmpeg ]}"
  ];

  doCheck = true;
  preCheck = ''
    export QT_QPA_PLATFORM=offscreen
  '';

  meta = {
    homepage = "https://github.com/gfhdhytghd/HyprCapture";
    description = "Hyprland-only screenshot and recording tool";
    license = lib.licenses.gpl3Only;
    inherit (hyprland.meta) platforms;
    mainProgram = "hyprcapture-ui";
  };
}
