# GTK / Zen touchpad forwarding (2026-10-07)

Base source: `892c25c`. Runtime validation used a separate Hyprland 0.56.2 instance, GTK 3.24.52, Qt 6.11.2 and Zen 1.23b with an isolated profile and synthetic local page. The production plugin was not reloaded or replaced.

## Cause and change

`scrollDeliver()` positioned the client pointer, then appended an axis to the same `wl_pointer.frame`. GTK 3's `gdk_wayland_seat_flush_frame_event()` delivers its pending motion event instead of flushing scroll data when both are pending. The [GTK implementation](https://github.com/GNOME/gtk/blob/gtk-3-24/gdk/wayland/gdkdevice-wayland.c) explains why actual FINGER input was lost while Zen's separate wheel handler continued working.

The plugin now sends a frame after synthetic pointer positioning and before forwarding the axis. Source, fractional delta, device/application factors, stop events and keyboard focus are unchanged. This applies to native axes and the controller's forwarded axes.

The virtual-pointer test driver also needed correction: Hyprland's `axis` request initializes the pending event, overwriting a previously supplied source. The driver now sets source after the axis/stop request. Earlier reports that labeled this driver as FINGER actually exercised WHEEL. A GTK receiver now checks that its first nonzero event really comes from a touchpad and rejects mislabeled input.

## Before / after evidence

- Before: the installed `892c25c` baseline plugin plus the corrected driver and GTK fixture stayed at offset 400 until the 25-second timeout. No scroll event reached the GTK widget. `gtk-touchpad-before.txt`.
- After, GTK region: all six offsets 520, 200, 450, 700, 920, 200 accepted; 800 x 1320 output, zero RGB error, exact annotation and undo/redo. `gtk-touchpad-region.json` and `.txt`.
- After, GTK window with interleaved pointer movement: same six offsets, 804 x 1324 including native padding, zero RGB error, exact annotation and undo/redo. `gtk-touchpad-window.json` and `.txt`.
- Qt native-wheel regression: 800 x 1320, six phases, exact pixels and annotation/undo-redo. `gtk-touchpad-wheel-regression.json`.
- Zen: actual FINGER input moved the page during capture from scrollY 1460 to 2606 and back to 1460. `zen-touchpad-after.json`. The protocol excerpt in `zen-touchpad-protocol.txt` confirms source 1, separate motion/axis frames, and axis-stop; the input source is no longer inferred from the driver flag. Browser momentum can continue after injected input ends, so the normal-page and capture starting offsets differ.
- Four focused CTests passed: annotation-editor, in-place-editor, scroll-capture, scroll-stitcher. `gtk-touchpad-ctest.txt`.

## Reproduction

Use the isolated compositor prerequisites in [README.md](README.md), including the 800 x 600 fixture rule. Build `hyprcapture-scroll-live-test` and, when GTK 3 development files are available, `hyprcapture-scroll-gtk-fixture`. Compile the corrected `tests/scroll_native_input.c` as documented there. With the candidate plugin loaded only into that isolated compositor:

```sh
HYPRCAPTURE_SCROLL_LIVE_TEST=1 QT_QPA_PLATFORM=wayland \
HYPRCAPTURE_SCROLL_NATIVE_INPUT=/absolute/path/to/scroll-native-input \
HYPRCAPTURE_SCROLL_NATIVE_FINGER=1 \
HYPRCAPTURE_SCROLL_GTK_FIXTURE="$PWD/build-scroll/test-ui/hyprcapture-scroll-gtk-fixture" \
  build-scroll/test-ui/hyprcapture-scroll-live-test /tmp/gtk-scroll-result
# Also set HYPRCAPTURE_SCROLL_WINDOW=1 and HYPRCAPTURE_SCROLL_POINTER_MOTION=1
# for the window/pointer-motion case. GTK is an optional test dependency only.
```

These corrected FINGER tests were run at scale 1. The user's physical Magic Trackpad on the production desktop still needs retesting after their plugin update; the isolated tests do not establish that final hardware acceptance.
