#!/bin/sh
# Keep all test mutations inside this worktree or disposable namespace tmpfs.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${HYPRCAPTURE_TEST_BUILD_DIR:-"$root/build"}
case "$build" in /*) ;; *) build="$root/$build" ;; esac
mkdir -p "$build/full-suite-home/.nix-profile/bin" "$build/test-runtime"
chmod 700 "$build/test-runtime"
ln -sfn /nix/store/ocr-system/bin/ffmpeg "$build/full-suite-home/.nix-profile/bin/ffmpeg"
exec env -u QT_IM_MODULE LANG=C.UTF-8 LC_ALL=C.UTF-8 capmem 8G \
 bwrap --die-with-parent --unshare-net --ro-bind / / --dev /dev --proc /proc \
 --tmpfs /tmp --tmpfs /dev/shm \
 --bind "$build" "$build" \
 --tmpfs /nix --dir /nix/store --ro-bind /usr /nix/store/ocr-system \
 --bind "$build" /nix/store/ocr-build \
 --setenv PATH /nix/store/ocr-system/bin:/usr/bin:/bin \
 --setenv HYPRCAPTURE_TEST_HOME /nix/store/ocr-build \
 --setenv HOME /nix/store/ocr-build/full-suite-home \
 --setenv HYPRCAPTURE_OCR_PREVIEW /nix/store/ocr-build/ocr-layer-preview.png \
 --setenv XDG_RUNTIME_DIR /nix/store/ocr-build/test-runtime --setenv QT_QPA_PLATFORM offscreen \
 --setenv DBUS_SESSION_BUS_ADDRESS unix:path=/tmp/no-session-bus \
 --setenv QT_QPA_PLATFORMTHEME '' --setenv QT_LINUX_ACCESSIBILITY_ALWAYS_ON 0 \
 --unsetenv HYPRLAND_INSTANCE_SIGNATURE --unsetenv WAYLAND_DISPLAY \
 --chdir "$root" "$@"
