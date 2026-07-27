{
  lib,
  cmake,
  glib,
  hyprland,
  hyprlandPlugins,
  kdePackages,
  lua,
  nlohmann_json,
  pkg-config,
  src,
}:
hyprlandPlugins.mkHyprlandPlugin {
  pluginName = "hyprcapture";
  version = "0.2.6";
  inherit src;

  nativeBuildInputs = [
    cmake
    pkg-config
    kdePackages.wrapQtAppsHook
  ];

  buildInputs = [
    glib
    kdePackages.layer-shell-qt
    kdePackages.qtbase
    kdePackages.qtsvg
    lua
    nlohmann_json
  ];

  cmakeFlags = [
    "-DHYPRCAPTURE_DEFAULT_HELPER_PATH=${builtins.placeholder "out"}/bin/hyprcapture-ui"
    "-DHYPRCAPTURE_TRUSTED_BIN_DIRS=${lib.makeBinPath [ hyprland ]}"
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
