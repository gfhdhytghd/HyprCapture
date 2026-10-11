# HDRView for Arch Linux / Hyprland

Arch package recipe for `hdrview` 3.0.2-1, built from source.

Launch with `hdrview /path/to/image.heic`, or select HDRView in the application menu.
Use a Wayland session for HDR output. `--sdr` explicitly disables HDR display.

Rebuild and install:

```sh
cd packaging/hdrview
makepkg -si
```

The first build downloads upstream's pinned CMake/CPM dependencies. The main source
archives and local patches have SHA-256 checksums in PKGBUILD; `.SRCINFO` is included.

Local patches:

- Enable ISO 21496-1 HEIC gain maps through private libultrahdr 2.0.2 and its patched
  libheif. System libheif remains in place for normal HEIF decoding. Only public
  UltraHDR C API symbols are exported from the private shared library.
- Use Wayland target display luminance, rather than the preferred SDR color volume,
  for the HDR display ceiling. Advertise that ceiling as the rendering peak to avoid
  tone mapping an already display-limited image from a fictitious 10000-nit peak.
- Avoid requiring the optional `en_US.UTF-8` locale. Preserve the release version
  when building the source tarball without Git metadata.

Validation performed on the development desktop:

- `makepkg` check decodes the synthetic HEIC fixture and verifies BT.2020,
  100/203/630/4000-nit sample values and alpha. The fixture contains only test colors.
- Native Wayland startup on DP-4 reports 173-nit SDR white and a 630-nit HDR ceiling.
- The installed viewer successfully decoded the repaired HyprCapture HEIC via its
  gain map. This verifies the decode and HDR rendering path, not a physical display
  luminance measurement.
- namcap reports no errors; pacman file verification reports no altered files.

Display luminance and runtime behavior must be checked again on each target machine.
