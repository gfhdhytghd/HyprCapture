# Background controls after scrolling (2026-10-07)

Base: `35ffca1`. The completed-scroll path hid every capture-toolbar control, including the background selector, then showed the empty styled toolbar. This produced the small white rectangle below the annotation bar.

Window captures now retain the uncomposited native pixels for each accepted strip alongside the preview/composite pixels. Both channels use exactly the same registration; there is no second matcher. After finishing, the existing background selector remains available and recomposes the saved native pixels while preserving the annotation layout/history. Original native frame geometry is retained so shadow padding and transparent margins are not mistaken for window content. Background extension preserves top/bottom clipping; the content band expands to the long-image height. The extra pixel channel increases window-capture memory use; region captures do not allocate it.

For region captures, there is no separate window alpha/background to adjust, so the empty toolbar is hidden and no space is reserved for it. Returning to selection restores the toolbar.

Validation in an isolated nested compositor, scale 1:

- Semitransparent window: 804 x 1324, six forward/reverse phases, pixel-exact initial output; transparent -> black -> white round trip reproduces the original marked image, with native alpha and exact annotation positions retained. Undo/redo still succeeds. `background-controls-window.json`.
- Region: 800 x 1320, six phases, continuous-input growth, exact pixels and annotation/undo-redo checks; empty capture toolbar is hidden. `background-controls-region.json`.
- Stitcher test verifies append and prepend of paired native alpha pixels and rejects a missing native channel on a subsequent frame. Focused annotation/editor/stitcher CTest results: `background-controls-ctest.txt`.

## Separate input investigation

User reports Moonlight input scrolls Zen, while their precision touchpad does not. The user confirmed the touchpad is directly connected to Linux. An isolated Zen fixture accepted 40 FINGER-source events carrying vertical delta 3 and horizontal delta 0.8 in the same native protocol frame, spaced 16 ms apart. With the installed `35ffca1` helper and the native capture plugin, DOM scrollY advanced from 912 to 1824 and the scrolling preview grew. This does not reproduce the physical/remote touchpad failure. No compositor input-forwarding change is included, and no claim is made that the device failure is fixed. Production plugin/helper were not reloaded or replaced during these tests.
