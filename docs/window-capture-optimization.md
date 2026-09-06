# Window capture optimization

## Implementation

Window screenshots, CPU/GPU streams and window recordings now share outward-rounded capture geometry. The target covers the window's full bounding box (including compositor-reported decoration and popup extents), rather than the entire output. Logical origins include the subpixel padding so clients retain correct input/image alignment.

The local render pass sets `fbSize`, `RPT_EXPORT`, viewport and damage together. Hyprland 0.56's shaders still consult the monitor transform for rounded corners and borders, so a synchronous scope temporarily normalizes that transform and restores it, the viewport and projection state afterwards. Pass simplification is disabled within that scope because it otherwise clips against monitor bounds. No output configuration or mode is committed. Snapshot mode, background composition, transparent-edge/shadow repair and FFmpeg input remain unchanged. The real-background matte now renders directly into its final compact target, avoiding a second mask allocation and blit.

GPU streams keep one exported allocation in flight. EGL capabilities are cached per context; EGLImage and DMA-BUF layout/FD are cached per allocation. Each packet receives its own duplicated image FD and fresh native fence. Resize/storage replacement resets the cache before reallocation; a failed or uncertain release retires the allocation. The renderer never waits for a fence or socket. A worker-owned nonblocking eventfd wakes the compositor after connection, release or retirement, and the capture timer respects the previous sample's FPS deadline. Unmap also wakes target validation while the timer is idle.

External command/configuration and wire formats are unchanged. This does not add GPU-to-encoder input, change codecs, or introduce a multi-frame-in-flight protocol.

## Source and hardware checks (2026-09-06)

- Compatibility headers: Hyprland v0.56.2, `efb50993780079460b0cbed1363e2166a2de1d9f`.
- RelWithDebInfo build of the plugin and helper passed; all 20 CTest cases passed in the working tree.
- Geometry checks cover all eight transforms, fractional scales, negative output/window coordinates, asymmetric outer bounds, oversized windows, subpixel extents and invalid/overflow inputs.
- EGL test checks exact RGBA import, updated pixels/alpha through a reused import, preserved GL bindings, fresh fence/FD ownership, stable FD counts across repeated exports, resize, same-size storage replacement, unsupported format rejection and context changes.
- Socket tests check exact release matching, malformed releases, ancillary FDs, disconnect, timeout, eventfd wakeups and bounded stop while a peer withholds release.
- No plugin reload/update was performed. Compositor image equivalence and end-to-end performance remain pending the runtime acceptance below.

An isolated EGL export microbenchmark (128×64 RGBA8, 20 warmup + 100 measured exports per variant, fence wait outside the timed export call) reported:

| Export resource lifetime | p50 (µs) | p95 (µs) |
| --- | ---: | ---: |
| Recreate for each frame | 52.426 | 109.220 |
| Reuse allocation | 8.288 | 9.927 |

These are CPU wall times for exporting an already-rendered texture in the test process, **not** compositor render times, achieved FPS or recording speedups. Repeated runs vary with GPU/CPU load. This benchmark runs sequentially: the driver can reject two EGLImages created concurrently from the same source texture.

For illustration, a 1280×720 outer capture rectangle on a 3840×2160 output now needs 921,600 rather than 8,294,400 color-buffer pixels (88.9% fewer). This is geometry arithmetic, not a measured total VRAM reduction; additional compositor and consumer buffers still exist. Windows whose compositor bounding box deliberately covers the output, such as some `dimaround` cases, can still require an output-sized target.

## Reproduce automated validation

```sh
cmake -S . -B /tmp/hyprcapture-check -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHYPRCAPTURE_GPU_EXPORT_TEST=ON
cmake --build /tmp/hyprcapture-check -j 4
ctest --test-dir /tmp/hyprcapture-check --output-on-failure
/tmp/hyprcapture-check/hyprcapture-window-gpu-export-test
```

Use the matching Hyprland SDK/pkg-config path when multiple header installations exist. The optional EGL test requires DMA-BUF export/import and native-fence support; failing to initialize that test is not proof of a compositor regression.

## Runtime acceptance after the user's reload

Compare baseline and optimized builds on the same host, window content, physical capture size, FPS and encoder settings. Keep timing instrumentation equally enabled for both when comparing timing logs, and disable it for the CPU/FPS comparison to avoid log I/O affecting results.

1. Capture an asymmetric visual fixture with corner labels, a partially transparent region, rounded corners, a title bar and a popup. Exercise transparent, white, black, follow-system and real-background modes, plus border/shadow combinations. Verify orientation, complete edges/shadow, background alignment and alpha with a checkerboard viewer and raw alpha inspection. Verify frozen overview/Hymission selection still captures the intended window.
2. Repeat on normal and rotated outputs, fractional scaling, negative positions, a window crossing output edges, an oversized window, and while resizing/moving between outputs. Test CPU and GPU streams independently, including their reported logical/input geometry. For recording, check final dimensions and decode the entire result; confirm transparency only for formats/settings that previously supported it.
3. Record a static fixture and a continuously animated fixture for 30 seconds at the same requested FPS. Compare render/readback p50/p95, process CPU usage, delivered frame count and capture timestamps/age; report dropped frames from the consumer. Measure screenshot completion latency separately. Do not substitute the EGL microbenchmark for these measurements.
4. With the existing `timing` option, inspect `window.target.<width>x<height>`, `window.render`, `window.readback`, `window.gpu.export`, `window.gpu.image_build` and `window.gpu.release_to_submit`. Stable GPU geometry must produce one image-build event per allocation, not per frame. The release-to-submit metric includes the intentional FPS wait and the next render/export, and should not accrue an additional missed polling interval under a slow consumer.
5. Delay or withhold releases, close the consumer, close the target window and stop the session. Confirm a busy buffer is never overwritten, the desktop remains responsive, capture stops cleanly and subsequent sessions work. Check descriptor counts return to a stable baseline.

Passing the build and automated checks does not close this runtime matrix; record its results after loading the new plugin.
