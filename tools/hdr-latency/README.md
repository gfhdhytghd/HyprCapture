# HDR latency phase-1 tools

Standalone experiments; these targets are not part of the product build.

```sh
cmake -S tools/hdr-latency -B /tmp/hyprcapture-hdr-tools -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/hyprcapture-hdr-tools -j2
/tmp/hyprcapture-hdr-tools/fp16-probe                     # capabilities only
/tmp/hyprcapture-hdr-tools/fp16-probe --show              # temporary 3-second xdg window
/tmp/hyprcapture-hdr-tools/fp16-probe --show --parametric # sRGB + ext_linear + 80-nit normalization
# Default render node: /dev/dri/renderD128; override with --render-node PATH.
tools/hdr-latency/run-benchmarks.sh /tmp/hyprcapture-hdr-tools /tmp/hdr-latency-results
```

Dependencies: CMake, C++23, wayland-scanner/protocols, wayland-client, GBM,
libdrm, EGL/GLES3, Qt6 Widgets/Svg, OpenCV core/imgproc, powerprofilesctl (runner).
The probe binds CM v1 and dmabuf v3 deliberately; format/modifier events list
import capabilities. It never submits an unadvertised format. Intel GBM CPU map
failed for FP16 in an early trial, so it imports a linear FP16 BO into EGL and
uploads/copies the fixture without PQ encoding. `PRESENTED` comes from
`wp_presentation`, not a frame callback. This verifies transport/presentation,
not calibrated panel luminance or gamut. Parametric and Windows-scRGB metadata
are separate experiments and are not asserted colorimetrically equivalent.

CPU tools call actual product conversion, stitcher and editor code, with opaque,
textured FP16/SDR inputs, empty annotations, native resolution, 720×480 preview
bounds and a quarter-viewport translation. HDR stitching receives the exact
premultiplied format used by transport. 6K HDR stitching is expected to hit the
existing 16M-pixel limit; 6K paired transport exceeds 128MiB. CPU conversion,
editor composition and hypothetical GPU readback still benchmark at 6K.
The runner records power/load and fails if compilers are present. Trial 0 is
reported separately; medians use trials 1–4. GPU timings include `glFinish` and
are wall-clock operation timings, not GPU timer-query or display latency.
The PQ shader is a feasibility prototype: sampled six patches must differ by
at most two codes from the CPU algorithm, and direct FP16 copy must be bit-exact.
NaN/Inf, all alpha/gamut combinations and production synchronization need phase-2
coverage. `PQ_readback` already produces CPU RGB30 channel order.

## Live scrolling measurement (user-operated)

Build the product plugin and helper with diagnostics, then prepare scripts:

```sh
cmake -S . -B /tmp/hyprcapture-hdr-phase1 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build /tmp/hyprcapture-hdr-phase1 --target hyprcapture hyprcapture-ui -j2
tools/hdr-latency/measure-live.py --prepare /tmp/hyprcapture-hdr-phase1 --out "$HOME/.cache/hyprcapture-hdr-phase1-new"
```

`--prepare` only reads compositor state and writes reviewable scripts/binary
copies. The build can reside under /tmp; the helper is copied to an owner-private
cache directory because the product rejects executable paths below writable
/tmp parents. Loaded plugin identity is found in `/proc` mappings using its
static API/description signature, including arbitrary temporary filenames;
a hyprpm cache path is not assumed to be the loaded module. Mappings may persist
after unload; discovery refuses ambiguous mappings. With a known original module,
use --original-plugin PATH. Activation also verifies the registered plugin handle.

Review `state.json`, `activate.sh`, `restore.sh`. Close existing overlays and run
`activate.sh` yourself: it temporarily swaps the live plugin and sets helper,
`timing`, `timing_file` via `hl.config`. It never edits config files. If loading or
configuration fails, inspect the output and use `restore.sh`; no auto-reload
or rollback runs from the Python tool. Restoration reloads the saved original
path when its hash matches, otherwise the saved copy, and restores the three
option values. Before activation verify the saved state still matches the
current session; do not reuse it after another plugin/config upgrade.

```sh
tools/hdr-latency/measure-live.py --out "$HOME/.cache/hyprcapture-hdr-phase1-new" --record hdr-window --seconds 30
# Now manually open a screenshot, select the same target, scroll once, then cancel.
# Repeat hdr-region, hdr-window-png, sdr-window, sdr-region; 5 trials each.
# Change output HDR/SDR yourself. Keep target, native dimensions, content,
# export format and input gesture consistent; alternate HDR/SDR trials.
# Close overlay; run restore.sh yourself.
```

Window baseline is prewarmed, region baseline is lazy: test window scrolling both
immediately after selection and after waiting 1s. Monitor HDR with PNG export
still transports HDR; it isolates HDR thumbnail presentation from capture cost.
The recorder observes actual helper executables/hashes and mapped plugin hashes,
saves output mode and logs, and summarizes `scroll.input_begin` → first accepted
thumbnail Paint event entry. This is **not display presentation**. Missing events
or wrong binaries mean no accepted live result. Sequence-tagged logs separate
capture/fence polling/IPC, receive, append, previews, queued delivery and present;
`elapsed_us` scopes may nest and must not be summed blindly.
