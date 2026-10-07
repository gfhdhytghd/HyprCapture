# Scroll-session validation — 2026-10-06

Candidate source tested against Hyprland 0.56.2 (`efb50993780079460b0cbed1363e2166a2de1d9f`), Qt 6.11.2 and OpenCV 5.0. No production compositor/plugin was reloaded. Runtime cases used an isolated nested Wayland compositor and the candidate plugin/helper.

For the subsequent GTK/Zen touchpad forwarding fix and corrected FINGER-source regression, see [gtk-touchpad.md](gtk-touchpad.md). Earlier virtual-pointer FINGER labels in follow-up reports were invalidated by a driver ordering error; those runs exercised wheel input.

## Runtime evidence

The fixture renders deterministic native pixels, starts at document offset 400, then visits 520, 200, 450, 700, 920 and 200. Tests compare the assembled output to the source document, verify an annotation at its document position, and undo/redo it. The fixture uses its window's actual device pixel ratio (QScreen's rounded DPR is unsuitable for fractional-scale pixel goldens).

| Evidence | Capture | Scale | Output | Result |
|---|---|---:|---:|---|
| `native-input.json` | Region; first wheel from Wayland virtual pointer | 1 | 800×1320 | Pixels, annotation, undo/redo pass |
| `fractional-fixed.json` | Region | 1.25 | 1000×1650 | Pixels, annotation, undo/redo pass |
| `window-verified.json` | Native window render, including transparent edge | 1.25 | 1006×1656 | Pixels, annotation, undo/redo pass |
| `scale-2-negative.json` | Region; monitor origin −1280,−100 | 2 | 1600×2640 | Pixels, annotation, undo/redo pass |
| `rotated-2-negative.json` | Region; same origin, transform 1 (90°) | 2 | 1600×2640 | Pixels, annotation, undo/redo pass |
| `final-native-rotated.json` | Region; native first wheel, negative origin, 90° | 2 | 1600×2640 | Pixels, annotation, undo/redo pass |
| `cancel-2-negative.json` | Cancel after three accepted positions | 2 | Original | Image, selection geometry and undo/redo restored |

Except for the first input in `native-input` and `final-native-rotated`, these fixtures inject scroll-session axis commands (the path used by touchscreen gestures). They still capture the actual source application through the compositor. This distinction matters: they are not physical touchscreen or touchpad tests. The normal first-wheel path preserves the original pointer device and replays the event once after baseline acknowledgement.

Runtime runner prerequisites: a **separate** compositor, fixture window rule `float=true, move="100 120", size="800 600"`, disabled animations/borders/shadows, candidate plugin loaded there, and `HYPRLAND_INSTANCE_SIGNATURE` plus `WAYLAND_DISPLAY` pointing to that instance. Never point this test at a working desktop. Run:

```sh
HYPRCAPTURE_SCROLL_LIVE_TEST=1 QT_QPA_PLATFORM=wayland \
  build-scroll/test-ui/hyprcapture-scroll-live-test /tmp/scroll-result
# Optional: HYPRCAPTURE_SCROLL_WINDOW=1 or HYPRCAPTURE_SCROLL_CANCEL=1
```

For the native first-wheel driver, generate `scroll-virtual-pointer.h/.c` with `wayland-scanner` from Hyprland's `wlr-virtual-pointer-unstable-v1.xml`, compile them with `tests/scroll_native_input.c` and `-lwayland-client -lm`, then set `HYPRCAPTURE_SCROLL_NATIVE_INPUT` to that executable. The driver requires the live-test environment opt-in.

## Single-core algorithm benchmark

Intel Core Ultra 7 265K, pinned to E-core CPU 8 (4.6 GHz maximum; P-cores reach 5.4–5.5 GHz). Release build, `cv::setNumThreads(1)`, OpenCL disabled. Input frames are 2560×1440; output is 2560×16384; stride is 720 rows with a final 544-row extension. All three runs accepted 22 frames and matched the original document exactly. CSVs contain every frame's measurement.

```sh
QT_QPA_PLATFORM=offscreen taskset -c 8 build-scroll/hyprcapture-scroll-benchmark
```

| Run | Median append + preview | p95 | Maximum | Final assembly | Total | Process VmHWM |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 21.09 ms | 22.11 ms | 40.01 ms | 81.26 ms | 586.00 ms | 660036 KiB |
| 2 | 21.45 ms | 22.33 ms | 36.34 ms | 85.77 ms | 591.81 ms | 661764 KiB |
| 3 | 21.47 ms | 22.96 ms | 34.88 ms | 84.49 ms | 590.59 ms | 661144 KiB |

Per-frame timing covers matching, strip retention and preview generation, but excludes copying the source fixture into a frame. Total includes those copies and final assembly, but excludes fixture construction and the final equality comparison. Memory is the whole benchmark process: it includes the full 160 MiB source fixture, retained strips, final output and Qt/OpenCV allocations. It is **not** isolated worker memory. Use `/proc/self/status` VmHWM; inherited pre-exec `getrusage` high-water marks can be misleading.

These are algorithm timings, not end-to-end compositor latency or a sustained 30 fps guarantee. The live pipeline additionally includes source rendering, PBO readiness, sealed-memfd transport, event delivery and presentation. The capture timer is bounded to approximately 30 Hz and the worker retains only its newest pending frame. Maximum per-frame timings here exceed 33 ms.

## Regression coverage and limits

Full CTest run: 37 passed, zero failed, one opt-in live test skipped (46.09 s); see `regression.txt`. Live evidence is supplied separately above. After the final two-axis touch-pan change, the helper rebuilt and all four focused editor/scroll suites passed again (3.85 s; `final-focused.txt`).

Focused tests cover bidirectional extension, revisiting history, repeated-pattern rejection, sparse text, fixed headers/footers, opaque sidebar fill, preserving text above/below the insertion band, stationary patterned backgrounds, limits, touchscreen second-finger rollback, object coordinates and undo/redo. Existing editor tests cover copy/save/pin paths and failures. Zoom regression cases now explicitly use Ctrl+wheel.

A quiet sidebar insertion band must have stable rows; its endpoints supply per-column linear gradients. Dense patterned backgrounds may repeat captured blocks. Uncertain alignment or an unsafe insertion band retains confirmed content. Layout inference is based on observed frame differences, not application DOM information; unusual dynamic/parallax pages remain an acceptance limitation.

Physical touchscreen/touchpad behavior, multi-monitor simultaneous interaction and broad real-world webpage coverage remain unverified. The nested tests prove the listed scales/transforms and deterministic fixture, not every display/application combination. Output/export regression tests are separate from physical clipboard or desktop pin acceptance. Nix packaging was updated but not built in a Nix sandbox.
