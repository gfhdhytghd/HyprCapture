# Fullscreen window capture decorations

Validated on 2026-10-21 from base `c46aa27`, against installed Hyprland 0.56.2
headers and binary `efb50993780079460b0cbed1363e2166a2de1d9f`. The supplied
`pc056/hyprland.pc` selects `/usr/include`, not the system Git HEAD header root.
All live operations addressed a private nested 0.56.2 instance. No host reload,
plugin load/unload, monitor command or configuration change was issued.

## State selection

`Fullscreen::controller()->getFullscreenModes(window)` exposes separate
`internal` and `client` fields. The installed 0.56 header defines NONE=0,
MAXIMIZED=1, FULLSCREEN=2. Use internal FULLSCREEN, matching the compositor's
own decoration/rounding suppression. Do not infer fullscreen from a rectangle
merely matching the monitor.

| Internal | Client | Capture policy |
| --- | --- | --- |
| 0 | 0 | Normal decoration settings |
| 1 | 1 | Maximized, retain normal decoration settings |
| 2 | 2 | Content-only compositor/output fullscreen |
| 0 | 2 | Client-only fake fullscreen, retain normal decoration settings |
| 2 | 0 | Compositor fullscreen without telling the client, content-only |

The latter still occupies the output. Client-reported fullscreen alone does
not establish that the window fills an output. Content pixels with intrinsic
alpha retain their normal background handling; this fix removes external
padding, not transparency belonging to the client.

## Reproduction and repair

The nested fixture paints every client pixel `[34,149,98,255]`. Its output is
800x600 at scale 1, border size 10, rounding 32, shadow range 30, and animations
disabled. Capture requests explicitly keep borders and shadows. Before fixing:

- Screenshot: visible content 800x600, full geometry `[-30,-30,860,660]`,
  87,600 pixels with alpha below 255, and a transparent top-left corner.
- CPU window stream, GPU DMA-BUF frame and FFV1 recording: 884x684.
- The GPU frame explicitly advertised `shadowEnabled=true`.

Hyprland already skipped fullscreen decorations while rendering, but
`getFullWindowBoundingBox()` still supplied decoration extents. Screenshot
alpha trimming removed some initial empty area; the capture-side shadow
reconstruction then expanded it again. Streams and recording retained the
original padded allocation, producing the different oversized dimensions.

The shared capture-bounds function now selects the goal main-surface box for
internal fullscreen, with the existing physical-pixel alignment. The renderer
receives `decorate=false`, retains the entire content rectangle instead of
alpha-trimming it, and neither reconstructs nor advertises a fullscreen shadow.
The same bounds feed real-background recording, CPU/GPU sampling and stream
epoch tracking. Background masks and UI selection/export ignore fullscreen
border and rounding metadata, including stale decorated artifact metadata.

| Path | Before, full 800x600 client | After |
| --- | --- | --- |
| Screenshot | 860x660, alpha minimum 0 | 800x600, alpha minimum 255 |
| CPU stream | 884x684, alpha minimum 0 | 800x600, alpha minimum 255 |
| GPU DMA-BUF | 884x684, shadow enabled | 800x600, shadow disabled |
| FFV1 window recording | 884x684 | 800x600; decoded first frame entirely matches fixture RGBA |

Both internal=2/client=2 and internal=2/client=0 passed all four paths. A second
nested run at 2586x1656 also passed the size/content assertions. For matched
800x600 output geometry, normal, maximized and client-only fake-fullscreen
metadata were identical before/after. Their screenshot PNGs were byte-identical:
SHA-256 `ab6acbda5ba11d4ea65b2dd46d430feb573d70111cab5efcde56bfebbedaa705`.
Their screenshot size remains 780x580 and stream/recording size 784x584.

The local hyprbars v0.56.0 tag (`67f532115601fe5bf0f9e6efaa3a39866bb1e025`)
was built against the same headers in the validation build directory and loaded
only into the nested instance. All four fullscreen paths passed again with
hyprbars present. Normal/client-only captures still included the title bar;
for example, client content was 700x485 and its decorated screenshot 780x576.
The cached Git HEAD hyprbars binary was ABI-incompatible and was not forced.

## Re-running

Build the plugin, UI and fixture with the target pkg-config path and
`-DBUILD_TESTING=ON`. Launch a new 0.56.2 compositor with a short private 0700
`XDG_RUNTIME_DIR` (Unix socket paths must fit 108 bytes), an explicit minimal
Lua config and `HYPRLAND_NO_SD_VARS=1 HYPRLAND_NO_SD_TARGET=1
HYPRLAND_NO_SD_NOTIFY=1`. Set `WAYLAND_DISPLAY` to the absolute parent socket;
set `AQ_DRM_DEVICES=/dev/null` to use the Wayland backend. The parent may be a
Wayland desktop, while every control command must use the child's private
runtime and exact instance signature. An attempted Weston 15 parent was not
usable here because Aquamarine bound wl_compositor v6 to its v5 global.

Use this private config (no host config is sourced):

```lua
hl.monitor({output="", mode="800x600@60", position="0x0", scale="1"})
hl.config({
  general={border_size=10, gaps_in=10, gaps_out=40},
  decoration={rounding=32,
    shadow={enabled=true, range=30, render_power=3, color=0xeeff0000},
    blur={enabled=false}},
  animations={enabled=false},
  misc={disable_hyprland_logo=true, disable_splash_rendering=true},
  xwayland={enabled=false},
})
```

Load the newly built plugin into the child, launch
`hyprcapture-fullscreen-window-fixture` with the child's runtime/display, and
set the child's output mode again with `hl.monitor` once the parent has
configured the nested window. Verify the output dimensions before comparing
runs; a parent tiling compositor can initially offer another window size.
Save the explicitly selected child identity in a JSON file:

```json
{"runtime":"/tmp/private-runtime","instance":"exact-child-signature","socket":"wayland-1"}
```

The opt-in harness requires Python numpy/Pillow, hyprctl, FFmpeg and ffprobe.
It refuses the usual host runtime and never defaults to a host instance:

```sh
python3 tests/fullscreen_window_live_test.py \
  --session build-fsdeco/validation/nested.json \
  --output build-fsdeco/validation/fixed-800 \
  --label after --expect-content
```

Use a fresh output directory per run; recordings are not overwritten. The
harness checks five fullscreen states over screenshot, CPU stream, GPU stream
and recording, writes PNG/MKV/JSON evidence, closes received descriptors, and
stops its streams/recordings. The caller owns stopping the fixture and nested
compositor. Evidence for this run is under `build-fsdeco/validation/`:
`baseline-suite`, `fixed-800`, and `fixed-hyprbars`.

## Automated coverage and limits

- The in-place-editor test exercises 30 cases: normal/maximized/fullscreen,
  native SDR/FP16 HDR artifacts, and white/black/transparent/real/follow-system
  backgrounds. It verifies output extent, opaque content corners, and selection
  geometry even with stale border/rounding metadata. Real backgrounds include
  their native HDR plane.
- Window-stream extent tests cover the three bounds at negative output origins,
  fractional/integer scales and all eight output transforms.
- Full Debug build and CTest run use the supplied 0.56 pkg-config path: 60 tests
  passed, zero failed, and `scroll-live` skipped. The offscreen HDR GPU test passed.
- Nested live evidence is SDR. FP16 HDR composition/export tests are offline;
  this does not establish physical HDR display appearance. GPU live checks
  validate exported dimensions and shadow metadata, while CPU/recording checks
  inspect pixels.
