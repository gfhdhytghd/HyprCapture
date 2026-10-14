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

## Phase 2A helper-only A/B

The FP16 product path uses **linear BT.2020**, 80 nits/unit, parametric ext_linear,
with the same peak envelope/reference as the CPU PQ surface. The GPU applies
identical BT.2020 gamut clipping and opaque checkerboard before submission;
it neither clips negative input scRGB prematurely nor uses default Windows
reference metadata. `hdr-gpu` checks the actual exported pixels against
`hdrToPq10` at 80/203/252-nit reference, straight/premultiplied alpha and wide gamut.

`phase2a-live.py --prepare BUILD --baseline BEFORE_BINARY --out PRIVATE_CACHE`
prepares `activate-before.sh`, `activate-cpu.sh`, `activate-after.sh`, `restore.sh`.
They change only the helper option; **no plugin swapping and no plugin timing
option changes**. The phase-1 directory is reserved and must never be overwritten.
`before` is the original executable; `cpu` is the new executable with
`HYPRCAPTURE_HDR_DMABUF=0`, giving PQ fallback plus presentation instrumentation;
`after` enables FP16. Scripts and binary checksums are reviewable before execution.

No desktop startup acceptance was inferred from the offscreen tests. Measure
actual input→all expected parent layers mapped, first paint/update, and
`hdr.presented` separately; use its presentation seconds/nanoseconds only when
`clock=1` (CLOCK_MONOTONIC), not the later log-delivery timestamp. Keep cold/warm
and timing/wire-debug trials separate; check the active helper executable/hash,
close each overlay, restore the original helper, and retain all raw samples.
A separate `WAYLAND_DEBUG=client` run can verify the HDR child buffer commit
precedes the parent's first buffer commit. Input-to-map still excludes scanout.

## Phase 2B editor entry

`editor-live.py --prepare BUILD --baseline PHASE1_HELPER --out NEW_PRIVATE_CACHE`
prepares SHA-checked `activate-before.sh`, `activate-after.sh`, `activate-cpu.sh`
and `restore.sh`. These select helpers and isolated timing files only; they
**never load/unload a plugin**. Before is the Phase-1 synchronous helper; after
includes Phase 2A + 2B; cpu isolates 2B with FP16 monitor backdrop disabled.
Activation requires the same registered plugin handle as preparation. Reprepare
in a fresh directory after the user changes the live plugin. Restore the saved
helper/timing/timing_file only after closing overlays.

Run `editor-live.py --out NEW_PRIVATE_CACHE --record before-window-now-cold --seconds 30`
and manually perform `open_editor()` → click the same kitty window (2386×1656)
→ immediate/1s wait → one wheel notch → cancel. Alternate before/after; keep cold
and warm separate (5 each), keep eDP-1 HDR 2880×1800 scale2/output format/content
constant, and run without compilation. Repeat region as a control.
`editor-summary.json` includes request→editor ready, first canvas Paint entry,
HDR base readiness, hydration/composition sub-timings and wheel→thumbnail Paint;
all endpoints are distinguished from display presentation. Original helpers
lack the new editor Paint timestamp, so compare their begin_edit completion
separately rather than inventing an equivalent scanout measurement. Hashes,
registration, dimensions/output configuration and raw logs accompany the run.

HDR base snapshots run in workers only for HDR output; SDR editing does not
build an unused HDR base. Aligned editor regions retain ready native monitor
backdrops while HDR work finishes. Stage/centered or uncovered regions use the
safe raster preview. Compositor capture still occurs at selection time, keeping
the captured frame fresh. Its synchronous readback/real-background passes remain
a distinct source of latency, measurable via hydrate_dispatch and plugin logs.

Explicit offscreen diagnostic:
`QT_QPA_PLATFORM=offscreen HYPRCAPTURE_EDITOR_BENCHMARK=1 BUILD/test-ui/hyprcapture-in-place-editor-test hdrEditorCompositionBenchmark`
uses a prehydrated 2386×1656 HDR real-background fixture. It checks async output
against synchronous native HDR composition; it excludes live compositor/IPC.
`editor-background-benchmark` isolates the erased SDR→HDR background conversion.
Neither benchmark substitutes for manual desktop/color acceptance. The authorized
HEIC-OFF build cannot accept HDR encoding; the user's full build verifies that.
