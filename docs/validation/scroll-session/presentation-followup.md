# Scrolling annotations and background follow-up (2026-10-07)

Base commit: `597d5d3`. Candidate tested in a separate nested Hyprland instance at scale 1. Production plugin/helper were not replaced or reloaded. Temporary diagnostic helper overrides from investigation were restored; the configured helper is `/home/wilf/.local/bin/hyprcapture-ui`.

## Changes

- Annotation fade direction now belongs to the running animation. An unaligned frame stops a pending fade-in before hiding ink. Registration cannot reverse its animation midway.
- Fade-out retains the previous annotation layout while native frames continue registering. Ink moves once by 12 logical pixels and fades, then appears at its newly registered position after 180 ms without input and a fresh stable frame. The idle timer can restore presentation without requiring a further frame after its deadline.
- Starting a new capture clears stale hidden opacity and displacement.
- Editor toolbar help cannot open native Qt tooltip windows, extending the earlier canvas-only prevention of layer-shell fullscreen tooltip flashes.
- Window stream frames are interpreted as premultiplied RGBA and composed against the selected window background before stitching. The existing background painting and clipping policy is reused; explicit transparent mode remains transparent. Background preparation is cached and frame composition runs in the capture worker.

## Evidence

Full build passed. CTest: 37 passed, optional `scroll-live` skipped by default (`presentation-ctest.txt`). Three new annotation editor tests check frozen fade placement followed by relocation, new-capture presentation reset, and suppression of native toolbar tooltips.

Opt-in native capture tests were run separately with `HYPRCAPTURE_SCROLL_LIVE_TEST=1` and `HYPRCAPTURE_SCROLL_CONTINUOUS=1`. Forty 3-pixel FINGER-source commands at 32 ms intervals feed the real compositor capture pipeline; these commands enter through the controller, not physical libinput. Previews grew while the input timer was still running. Toolbar interactivity was hidden during input and restored after the final idle period. Both tests completed six forward/reverse phases, exact annotation placement, and undo/redo:

- Region: 800 x 1320, pixel-exact (`presentation-continuous-region.json`).
- Window: translucent fixture (alpha 128), white background, 804 x 1324 including native padding; every alpha value and RGB pixel matched the expected composite exactly (`presentation-continuous-alpha.json`). Enable this case with `HYPRCAPTURE_SCROLL_WINDOW=1 HYPRCAPTURE_SCROLL_FIXTURE_ALPHA=1` in addition to the above flags.

## Remaining acceptance

Physical Magic Trackpad scrolling in the user's Zen session is still unresolved. An earlier isolated Kitty test grew from 800 x 600 to 800 x 792, but neither that nor these controller-driven tests proves the reported desktop no-scroll/no-growth symptoms resolved. These changes fix demonstrated presentation and background defects; they do not establish a physical input root cause. New background runtime evidence covers scale 1 and white composition, not every background mode, display scale, or physical-device path. No new CPU-budget claim is made for translucent composition.
