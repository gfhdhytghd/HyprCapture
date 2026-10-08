# Region stitching, independent background and Stage outline (2026-10-07)

HyprCapture base `432f846`; Hymission companion change `01b6a50`.
Validation used separately launched Hyprland 0.56.2 instances. No production
plugin update/reload or physical trackpad acceptance was performed.

## Causes and repairs

- An obscured region's client could stop repainting while waiting for a Wayland
  frame callback. Offscreen readback deliberately suppresses presentation
  feedback. Pace the actual scroll recipient's surface tree at capture cadence
  with frame callbacks, without fabricating presentation feedback.
- At scale 2, a tiny initial scroll left glyph stems unchanged in the top 20
  physical rows. Static-band detection incorrectly retained those rows as a
  header, corrupting later upward extension. Validate proposed top/bottom bands
  against the accepted translation before locking the layout. Only exact RGBA
  agreement expands the moving region. The new bidirectional small-step test
  fails with the old stitcher and passes with the repair; fixed-chrome tests pass.
- The monitor-image fallback contained opaque foreground pixels; inverse alpha
  compositing cannot recover their hidden background. Remove that fallback.
  Resolve the current renderer hook, reset per-frame plugin state, and generate
  native independent background artifacts for lazy window captures. Hydrating
  a missing background retains the frozen foreground and annotations. Existing
  long-image composition stretches the independent background content band.
- Stage has its own radius configuration. Hymission now exports optional
  `selectionRounding` from the final surface render data, after fitting,
  clamping and physical-pixel quantization. HyprCapture carries it through the
  validated session protocol and uses it for the outline. Older Hymission
  versions use a proportional native-radius fallback based on the full preview,
  never the clipped width. Native output-image rounding remains unchanged.

## Evidence

| Check | Result | Evidence |
| --- | --- | --- |
| Qt partial region, scale 2, continuous 3-logical-pixel steps | Six bidirectional phases, 680 x 860, RGB error 0; annotations and undo/redo pass | `region-scale2.json` |
| GTK partial region, scale 1, native FINGER first input | Six phases, 340 x 430, RGB error 0; receiver confirms touchpad source | `region-gtk-finger.json`, `region-gtk-finger.txt` |
| Transparent window, background switching and long output | 804 x 1324; original pixels, annotations and background round trip pass | `window-real-background.json` |
| Blurred native background independence | Replacing foreground changes foreground by up to 241, while background difference remains exactly 0 | `native-background-independence.json` |
| Long-image native-background composition against solid background | At most 1 channel value error at antialiased glyphs from alpha/raster rounding | `native-background-independence.json` |
| Stage native metadata, scale 2 | Configured 17 -> 17.5 logical rendered radius; 0 -> 0; default with decoration 40 -> 20.5; preview partially clipped | `stage-radius-scale2.json` |
| HyprCapture CTest | 37 passed, optional live test skipped in CTest and run separately above | `region-background-stage-ctest.txt` |
| Hymission CTest | 5 passed | `stage-radius-ctest.txt` |

The blurred-background test uses two stacked fixture windows. Replace only the
foreground with a fresh fixture at a different initial document offset, then
capture both foreground and native background again. The background was also
visually inspected: it is blurred underlying content, independent of the replaced
foreground. Blur changes solid-color values slightly in the native renderer, so
this independence check is separate from the unblurred solid-color composition
oracle; no pixel-exact blurred-color claim is made.

For the Stage readback, a native region render forces a fresh frame in the
isolated compositor before reading `hymission-stage-state`; an obscured nested
compositor may otherwise have no displayed preview frame. UI regressions check
both authoritative radius and legacy scaling with clipped selection geometry.

## Reproduction and remaining acceptance

Use the isolated compositor setup in `README.md` with the candidate plugin,
`QT_QPA_PLATFORM=wayland`, and `HYPRCAPTURE_SCROLL_LIVE_TEST=1`.
Set `HYPRCAPTURE_SCROLL_PARTIAL_REGION=1` and
`HYPRCAPTURE_SCROLL_CONTINUOUS=1` for the scale-2 test. For GTK use
`HYPRCAPTURE_SCROLL_GTK_FIXTURE`, `HYPRCAPTURE_SCROLL_NATIVE_INPUT` and
`HYPRCAPTURE_SCROLL_NATIVE_FINGER=1` as in `gtk-touchpad.md`.
For the background switch check use `HYPRCAPTURE_SCROLL_WINDOW=1`,
`HYPRCAPTURE_SCROLL_FIXTURE_ALPHA=1`, `HYPRCAPTURE_SCROLL_REAL_BG=1`, and
an unblurred compositor background of `0xff204060`.

The initial GTK retry accidentally omitted the FINGER flag and delivered a wheel
step of 15 rather than the intended touchpad movement of 60; its timeout is not
used as acceptance evidence. The corrected receiver log above verifies source.
Temporary production-code frame dumps and layer tracing are absent from the
committed changes.

Update both plugins from a safe context using the user's usual `hyprpm update`
workflow. Physical Zen/trackpad behavior and Stage outline appearance on the
production desktop still require the user's post-update check. Isolated scale-2
results do not establish fractional-scale support.
