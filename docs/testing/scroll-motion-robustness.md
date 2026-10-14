# Scrolling capture with animated and fixed content

## Design references

- [ScrollShot's engine description](https://scrollshot.work/blog/ios-stitching-engine-v2/): independent template offsets, consensus voting, rejection of uninformative templates, and a changing video thumbnail as an outlier. This is the author's implementation description, not an independently reproduced benchmark.
- [Snapzy's scrolling capture design](https://github.com/duongductrong/Snapzy/blob/master/docs/SCROLLING_CAPTURE.md): static chrome exclusion, bounded recovery, and output built from accepted slices. HyprCapture retains its own bidirectional coordinate system.
- [Eden, Uyttendaele and Szeliski, CVPR 2006](https://www.microsoft.com/en-us/research/wp-content/uploads/2006/06/Eden-CVPR06.pdf): separate registration from source/seam selection to avoid mixing moving objects. Here a horizontal agreeing band suffices; no graph-cut dependency or panorama warp is introduced.

## Implementation

The exact-overlap and strict foreground matchers remain the fast paths. If they cannot establish motion, report zero motion, or infer a small moving region before the first scroll, a bounded 6-column by 12-row template grid supplies independent translation votes. Each cell contributes at most one vote. Blank patches, changing patches and repeated patches without a unique native-pixel correspondence cannot vote.

NCC proposes offsets at reduced horizontal resolution. Native RGBA comparisons require near-identical pixels, including a bound on individual changed pixels; average error alone can hide differing line numbers beside repeated text. At least six agreeing patches must cover several vertical bands and a broad horizontal span. A narrow surviving text column needs stronger vertical coverage. Zero-motion support is considered separately from scrolling support. Conflicting supported nonzero motions are ambiguous.

Idle animation does not lock the initial content rectangle, advance the registration anchor, add rows, or modify retained pixels. The controller treats consecutive accepted `Unchanged` results as a stationary document even if animation continues. Its existing input-idle delay and capture timestamp checks still apply before restoring annotations.

The same consensus recovery can relocate a whole viewport inside retained history. It cannot fill an unobserved gap. Registration failures preserve the accepted image.

For consensus-based extensions, the stitcher checks the overlapping rows at the insertion edge. If that edge cuts changing content, it searches inward for eight consecutive agreeing rows and moves the cut there. Replaced overlap rows and new rows come from the same incoming frame. SDR, original RGBA and HDR strips are trimmed and copied at identical document coordinates; no averaging, warping, or fabricated pixels are used. Limit checks precede all strip mutations.

## Regression coverage

`tests/scroll_stitcher_test.cpp` covers:

- Moving document content with a changing image or an interior fixed panel, in both directions.
- Animation before the first scroll and after scrolling stops, with no canvas growth or layout locking.
- Motion inside a video on a stationary page.
- Sparse text partially obscured by animated/fixed content.
- Distant historical relocation while an image has changed.
- Two conflicting scrolling panes and a wholly unrelated/full-animation frame: reject without modifying the canvas.
- An animated object crossing either insertion edge: retain the complete incoming object and byte-consistent SDR/original/HDR planes.
- Existing exact pixels, one-pixel movement, repeated-pattern rejection, fixed chrome, transparent backdrops, history, corners and resource limits.

The new animated-document regression failed on the original stitcher before the implementation changed.

## Local validation, 2026-10-11

- `cmake --build build-scroll --target hyprcapture-scroll-stitcher-test hyprcapture-scroll-capture-test hyprcapture-scroll-benchmark -j 4`: passed.
- `ctest --test-dir build-scroll -R '^scroll-stitcher$' --output-on-failure`: passed, 9.25 s.
- `ctest --test-dir build-scroll -R '^scroll-capture$' --output-on-failure`: passed, 0.43 s.
- Standalone stitcher regression binary built with AddressSanitizer and UndefinedBehaviorSanitizer: passed (`ASAN_OPTIONS=detect_leaks=0`, `UBSAN_OPTIONS=halt_on_error=1`). This checks address/undefined behavior, not leak freedom.

One sequential benchmark pass per case, pinned to CPU 0, with one OpenCV thread and OpenCL disabled; Qt 6.11.2, OpenCV 5.0.0. Each run captures 22 viewports of 2560×1440 into a 2560×16384 image. Times include append and preview. This is a local comparison, not a statistical speedup claim.

| Case | Median / p95 | Pixel check |
| --- | --- | --- |
| Original stitcher, ordinary page | 28.15 / 32.46 ms | Entire output exact |
| Updated stitcher, ordinary page | 26.76 / 28.76 ms | Entire output exact |
| Updated stitcher, changing 640×400 panel | 68.05 / 79.11 ms | Dimensions and unobscured 800-pixel-wide document strip exact |

Run the last case with `QT_QPA_PLATFORM=offscreen build-scroll/hyprcapture-scroll-benchmark --dynamic`. The original and updated ordinary cases use the benchmark without that argument. Local benchmark logs and the standalone sanitizer binary are in `/tmp/hyprcapture-scroll-motion-audit/` (temporary artifacts).

## Boundaries

This uses observed pixels and geometric evidence. It cannot infer scroll position when no persistent document detail survives, distinguish every large coherent animation from scrolling without application metadata, or reconstruct content never visible beneath an overlay. A fixed element is excluded from motion estimation where evidence allows; this is not general-purpose object removal. A complete animated object is retained when the overlap contains a suitable cut and one incoming frame contains that object. Otherwise existing overlap pixels are retained.

Automated fixtures and offscreen controller tests are local algorithm/build evidence. They do not establish acceptance in the reporter's application or the running compositor.
