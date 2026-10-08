# Zen input and preview follow-up (2026-10-06)

Base commit: `61eb260`. Tests used a separate nested Hyprland instance and an isolated Zen profile, started through the same `wl-relabel` wrapper as the desktop browser. The production compositor, browser, plugin and helper were not replaced or reloaded.

**2026-10-07 correction:** The virtual-pointer driver set `axis_source` before `axis`, which reinitialized the pending event to WHEEL in Hyprland. The FINGER labels below describe the intended input, not the actual protocol source. These historical runs are wheel evidence only. See [gtk-touchpad.md](gtk-touchpad.md) for a source-checked GTK regression and the forwarding fix.

## Fixed and observed

- Moving the pointer over the canvas could create a native Qt tooltip containing the canvas help text. In the layer-shell session this became a full-screen white surface. Canvas help is now an accessible description rather than a popup tooltip.
- With the candidate helper, 20 screenshots sampled during continuous pointer movement contained zero white pixels in the monitored 3000-point sample. Raw samples: `zen-pointer-motion.txt`. This sampling is a regression check, not a guarantee against every rendering artifact.
- Scrolling preview uses the same transparent image styling, 180 × 120 logical aspect-fit bounds, DPR-aware scaling and 24-pixel edge margin as ResultThumbnail. Right-click reveals actions; left-click finishes. Both were exercised in the real overlay; see `zen-thumbnail-menu.png` and `zen-thumbnail-finished.png`.

## Input evidence and remaining gap

- Real Zen accepted virtual Wayland FINGER-source axis events, including axis-stop, in both region and window capture modes with the candidate helper.
- Window capture: 12 injected deltas produced 10 DOM wheel events (coalescing occurred), sum of DOM deltaY = 1368; scrollY advanced from 2736 to 4104. Raw log: `zen-window-finger.txt`.
- Before the tooltip fix, an already-running Zen scroll session also accepted 30 interleaved pointer-motion/FINGER events, total DOM deltaY approximately 456. Therefore the reported physical touchpad failure is not established as a compositor forwarding defect by this experiment.
- The input driver can now test FINGER source and pointer movement through `HYPRCAPTURE_SCROLL_NATIVE_FINGER=1` and `HYPRCAPTURE_SCROLL_POINTER_MOTION=1` in an explicitly enabled isolated live test.
- Physical touchpad/libinput behavior on the user's desktop remains unverified. The tooltip obstruction is fixed, but these protocol tests alone do not prove the user's no-scroll symptom resolved.

The candidate helper was staged in a private trusted directory for plugin-launched testing. A build path below a world-writable ancestor is rejected by the launcher's trusted-path check and would otherwise fall back to the installed helper.

## Build and regression

Full `build-scroll` build passed. CTest: 37 passed; opt-in `scroll-live` skipped by default. Output: `followup-ctest.txt`.

The explicit native live test with FINGER source and interleaved pointer motion passed all six phases: 800 × 1320 output, pixel-exact content, exact annotation placement and undo/redo. See `followup-native-finger.json`. An earlier attempt started before the driver linked successfully and timed out; it is not counted as input evidence.
