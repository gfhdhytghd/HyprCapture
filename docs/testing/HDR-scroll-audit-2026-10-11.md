# HDR and plugin regression audit — 2026-10-11

## Scope and environment

Implemented paired SDR/HDR scrolling capture and native HDR thumbnails, then audited the current plugin. Live checks ran on the XPS OLED display: Hyprland 0.56.2, Qt 6.12, 2880×1800 at scale 2, PQ HDR, 153-nit SDR reference white. The local Qt 6.11.2 build also passed the automated suite. Genuine HDR fixtures declare PQ/BT.2020 through Wayland color-management; the tests do not treat ordinary SDR content on an HDR monitor as an HDR source.

## Repairs

- Match SDR frames and retain paired canonical scRGB half-float strips. The HEIC encoder receives the assembled HDR image, including HDR annotations; SDR remains available for independent export, clipboard and dragging.
- Display both completed screenshot thumbnails and live long-shot previews through native HDR child surfaces. Completed previews use a bounded private half-float file, consumed by the helper. Unsupported native color management retains the SDR fallback. Preview generation paints strips at thumbnail resolution; sidebar gradients also use bounded device-resolution samples.
- Preserve native touchpad frame boundaries, including both axis-stop events. Warm fixed-window baselines before scrolling and replay queued gestures at their original cadence. Show the source application immediately when scrolling begins.
- Synchronize warmed native window padding before anchoring annotations. Take the editor snapshot first so cancel restores the original geometry and history.
- Recover sparse moving gutters and rounded viewport corners from verified overlaps. Repair SDR, original-alpha and HDR planes together; an almost opaque corner must not leave a colored seam inside the long image.
- Make the cancel dispatcher deliver a private control packet to owned capture helpers instead of succeeding without doing anything.

## Verified results

| Check | Result |
| --- | --- |
| Full local and XPS CTest suites | 41 executed tests passed on each machine. The registered `scroll-live` test is skipped by default and was exercised separately below. The updated thumbnail integration test also passed independently locally. |
| HDR window long shot, real uinput touchpad | 2440×2060, six bidirectional phases, verified document interior, maximum RGB error 4/255 against the independent CPU SDR reference. Native HDR scrolling preview, annotation coordinates, undo/redo and normal export passed. |
| HDR region long shot, real uinput touchpad | 1476×1316, six bidirectional phases, maximum RGB error 1/255; native HDR preview, annotation and undo/redo passed. |
| Independent HEIC gain-map decoding | Full-resolution gain maps: 2440×2060 and 1476×1316. Reconstructed BT.2020 peak channel values were 320.95 and 319.76 nits respectively. The captured linear source retained the 480-nit red-channel fixture. Channel maxima in different primaries are not interchangeable luminance measurements. |
| Cancel, region and warmed window | Original image, geometry and annotation history restored. |
| Continuous SDR scrolling | Image grew during input; document pixels were exact; annotation and undo/redo passed. |
| Stationary textured backdrop | All opaque glyph/marker pixels matched exactly. This does not claim a pixel-exact stitched stationary wallpaper. |
| Completed HDR thumbnail helper | Real layer surface and native child surface, PQ transfer enum 11, BT.2020 primaries enum 6, 10-bit buffer; private float preview consumed, timeout and process cleanup passed. |
| HDR thumbnail transport and export integration | Float highlights, negative wide-gamut values and alpha retained; malformed input rejected. The real export-to-helper argument path retained the 12.5 scRGB highlight instead of clipping it to SDR. |
| Chrome native two-finger touchpad | Kernel uinput → libinput → Hyprland → Chrome. Source scroll position was monotonic, 400→1726 CSS pixels; maximum step across 20ms samples was 68 pixels. Cancel left no capture helper. This is evidence for this gesture and configuration, not every touchpad/browser combination. |
| Genuine HDR source → SDR video | Region, window and fullscreen compositor recordings decoded successfully. Outputs were yuv420p, BT.709 primaries, sRGB transfer, approximately 3 seconds, with strictly increasing file presentation timestamps. Region video vs matching GPU-resolved SDR screenshot: mean RGB error 2.0226/255. |
| Auto recording backend | Region and fullscreen selected compositor on the managed HDR display and produced valid SDR video. This is the intended managed-color backend selection. |
| Private PipeWire application-audio test | Application isolation, child processes, multiple/late streams, microphone-only capture, meter and mux passed. Expected 440/660Hz amplitudes ≈0.08837; unrelated 1000Hz amplitude ≈0.000015. No host default audio devices were changed. |

The HDR window comparison excludes the outer native decoration/shadow margin. Synthetic unit tests separately verify exact strip placement, fixed headers/footers, sidebar interpolation, alpha recovery and HDR channel preservation. Small SDR color differences from the PQ 10-bit round trip are not reported as pixel-exact captures.

Two test-harness issues were corrected: waiting for a mock drag-path file to exist could read it before its contents were written, and comparing straight-alpha source bytes to the editor's premultiplied representation incorrectly rejected unchanged transparent pixels. The live fixture now announces HDR first-frame readiness and uses a bounded startup wait; a prior consecutive-run attempt failed because its fixture was not ready at the old fixed delay. Successful reruns retain that anomaly in the audit evidence.

## Reproduction and evidence

Build and run the normal suite with `cmake --build build-hdr -j4` and `ctest --test-dir build-hdr --output-on-failure -j4`. The XPS build directory was `build-hdr-xps`. Run `tests/audio_application_live_test.py` against the built helper for the isolated audio check.

`tests/scroll_uinput_touchpad.c` is the actual Linux virtual touchpad driver used in this audit. Compile it with `cc -O2 tests/scroll_uinput_touchpad.c -o /tmp/hyprcapture-touchpad`, run it with permission to access `/dev/uinput`, and feed a signed movement such as `-800` through stdin. It emits two-finger motion over 60 frames at 10ms intervals on a 120×70mm device; `0` destroys the device and exits. The live-test positioning helper only placed the pointer; the first scrolling gesture came from this kernel input device.

`tests/scroll_capture_live_test.cpp` supports `HYPRCAPTURE_SCROLL_LIVE_TEST=1`, `HYPRCAPTURE_SCROLL_HDR=1`, `HYPRCAPTURE_SCROLL_HDR_FIXTURE=1`, `HYPRCAPTURE_SCROLL_WINDOW=1`, `HYPRCAPTURE_SCROLL_PARTIAL_REGION=1`, `HYPRCAPTURE_SCROLL_CANCEL=1`, `HYPRCAPTURE_SCROLL_CONTINUOUS=1` and `HYPRCAPTURE_SCROLL_FIXTURE_BACKDROP=1`. Native input uses `HYPRCAPTURE_SCROLL_NATIVE_INPUT` and `HYPRCAPTURE_SCROLL_NATIVE_VARIABLE`. Use a private test configuration and the intended Hyprland instance; these are interactive desktop tests.

Collected local evidence is under `/tmp/hyprcapture-audit/`; XPS scrolling evidence is `/tmp/hyprcapture-scroll-audit/`, thumbnail evidence is `/dev/shm/hyprcapture-1000/audit-thumbnail/`, and video evidence is `/dev/shm/hyprcapture-1000/audit-recording/` and `audit-recording-auto/`. These temporary artifacts are not committed capture data.

## Acceptance boundaries

- Native HDR protocol and data preservation were verified; this is not a claim of subjective visual approval or accuracy of every external HEIC viewer.
- Multi-monitor hot-plug, other GPUs/scales, hardware touchpads beyond the virtual device, and the external GSR path on an SDR/unmanaged monitor were not live-accepted in this run.
- The optional legacy `audio_echo_live_test.py` failed its assumption that AEC must activate. In this configuration the helper correctly reported `pending / test required` and continued raw microphone capture because no validated AEC model was available. Measured raw/requested-AEC RMS was approximately 0.04512/0.04511 for the echo segment. Active echo suppression and physical acoustic calibration remain unverified; application-audio capture passing does not establish AEC suppression.
- HDR video recording was not requested or added. The validated recording output is SDR.
