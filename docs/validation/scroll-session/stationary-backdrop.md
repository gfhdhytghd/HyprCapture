# Scrolling text over a stationary backdrop (2026-10-08)

The user reported that region captures of kitty scrolled without growing, or
became misaligned. Read-only accessibility inspection of a production session
confirmed the UI's alignment failure. Earlier empty-frame samples were taken
after scrolling had stopped; they did not establish a rendering failure. Also,
absence of `start.json` is not evidence of a missing session: the plugin consumes
and deletes that request, while the frame/control sockets remain connected.

## Reproduction and repair

A deterministic terminal-like fixture reproduces both symptoms with the old
matcher: text moves over a fixed smooth background, while repeated line spacing
can favor an incorrect small displacement. The same text on a uniform background
works. An initial probe misregistered a 40-pixel movement as -1 pixel; other
movements were rejected. The new unit regression fails against the old source.

The matcher now compares horizontally filtered detail and verifies its
correlation at the original resolution, including color channels. Correlation
allows glyph contrast to vary over the backdrop; 95% of textured verification
tiles must agree. These filtered images are used only for registration. Output
strips retain captured pixels. Byte-identical overlaps take a fast path.

The sparse-patch fallback requires dense texture in both frames, so repeated
terminal phrases alone cannot authorize it. Historical relocation uses the same
detail registration. Boundary checks also recognize translated glyph detail:
otherwise tiny initial scrolls pin a few top rows as a false fixed header when
the smooth background prevents byte equality.

## Validation

All live checks used an isolated Hyprland compositor with the installed native
plugin and the candidate UI test binary. No production helper was installed and
no production plugin was reloaded.

| Check | Result |
| --- | --- |
| Unit regressions | Light/dark backdrops at three amplitudes; exact displacement/direction, growth, large historical return, retained pixels and unrelated-scene rejection pass |
| Existing matcher regressions | Periodic ambiguity, fixed headers/sidebars, patterned backdrop, sparse text, small scale-2 steps, original alpha and limits pass |
| Stationary backdrop, partial region, scale 2, Stage enabled | Six forward/reverse phases; grows during continuous input; 680×860; all 20,852 opaque document pixels have RGB error 0; annotations and undo/redo pass |
| Ordinary background, partial region, scale 2, Stage enabled | Six phases; 680×860; whole-image pixel equality; annotations and undo/redo pass |
| GTK partial region, scale 1, native FINGER first input | Six phases; 340×430; whole-image pixel equality; annotations and undo/redo pass |
| CTest | 37 passed; opt-in `scroll-live` skipped and exercised separately above |
| 2560×1440 viewport benchmark | 22 frames, complete output pixel-exact; median 19.22 ms / p95 20.02 ms, versus a baseline median 19.73 ms / p95 20.89 ms on this host |

The stationary-background live test deliberately reports `pixel_exact: false`:
its moving-document oracle checks opaque text/markers, not a fictitious scrolling
wallpaper. Unit tests separately check retained captured pixels. Benchmark runs
are individual host measurements, not a claim of a statistically significant
speedup. User-operated kitty/trackpad acceptance on the production desktop is
still pending after updating the helper.

Evidence: `stationary-backdrop-scale2.json`, `stationary-backdrop-opaque-scale2.json`,
`stationary-backdrop-gtk.json`, `stationary-backdrop-ctest.txt`, and
`stationary-backdrop-benchmark.txt`.

## Reproduction

Use the isolated compositor and fixture prerequisites in [README.md](README.md).
Build `hyprcapture-scroll-live-test`, then run with the isolated instance's
`WAYLAND_DISPLAY` and `HYPRLAND_INSTANCE_SIGNATURE`:

```sh
HYPRCAPTURE_SCROLL_LIVE_TEST=1 QT_QPA_PLATFORM=wayland \
HYPRCAPTURE_SCROLL_PARTIAL_REGION=1 HYPRCAPTURE_SCROLL_CONTINUOUS=1 \
HYPRCAPTURE_SCROLL_FIXTURE_BACKDROP=1 \
  build-scroll/test-ui/hyprcapture-scroll-live-test /tmp/scroll-backdrop-result
```

Omit `HYPRCAPTURE_SCROLL_FIXTURE_BACKDROP` for the opaque regression. For GTK,
use the corrected native driver and FINGER environment from
[gtk-touchpad.md](gtk-touchpad.md), without the continuous-input flag.
