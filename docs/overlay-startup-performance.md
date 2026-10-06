# Overlay startup performance

Measured on 2026-10-05 (America/New_York), Hyprland 0.56.2, Release builds.

## Changes

- Spawn the Qt helper before compositor artifact capture. A private, bounded
  `SOCK_SEQPACKET` channel delivers the completed session filename on stdin;
  Qt initialization can overlap capture. The original frozen snapshot is used.
- Read raw RGBA directly into owned QImage storage, avoiding the intermediate
  byte array plus full image copy. Preserve size, ownership and budget checks.
- Compose the full desktop only when an export needs it. Overlay painting
  already uses each monitor artifact directly. No later live recapture is used.
- Create recording/audio/AEC panels on first use; reuse them across mode changes.
- Cache SVG renderers and raster icons by content, rotation, physical dimensions
  and state, with Qt's bounded pixmap cache and per-request DPR.
- Show frozen content at full opacity in the first frame. Keep close fade-out.
  Avoid a redundant initial cursor update when the frozen cursor is available.
- Add monotonic timing for spawn, capture, handoff, Qt startup, first paint and
  first update processing. These are diagnostic events, not presentation feedback.

The new plugin and helper must be upgraded together: the new plugin uses
`--session-json-stdin`. The helper retains legacy JSON/file launch arguments.
A failed/closed startup channel, invalid metadata or the 10-second receive timeout
exits the helper instead of falling back to a later live screenshot. Publishing
is nonblocking and cannot raise SIGPIPE in the compositor.

## Same-desktop A/B measurement

DP-4: 6144×3456, scale 2, 60 Hz; HEADLESS-1: 3840×2400, scale 2,
60 Hz. Overlay scope all, fusion enabled. Two layers expected per capture.
Before and after helpers were alternated for 24 trials (12 per version), using
identical installed plugin and settings. Each helper's `/proc/<pid>/exe` was
checked; helper configuration was restored after the test.

| Helper | Mean both layers mapped | Median | Min–max |
| --- | ---: | ---: | ---: |
| Before startup changes | 274.83 ms | 274.27 ms | 256.72–285.31 ms |
| Final optimized helper | 239.49 ms | 239.00 ms | 221.47–253.74 ms |

Reduction: **35.34 ms / 12.86%**. [All samples and binary hashes](performance/overlay-startup-samples.json).

This isolates UI changes. The production compositor plugin was not reloaded, so
these numbers do **not** include plugin/helper startup overlap. The baseline is
the pre-edit working-tree build, including existing unrelated audio changes;
this benchmark is not a comparison of two clean release tags.

The endpoint is Linux uinput injection of S while Super is held, through the
actual shortcut, until Hyprland's two `openlayer>>hyprcapture-ui` events. It
excludes physical keyboard and display scanout latency. Mapping is not proof of
presentation. The baseline first frame was transparent; the final helper paints
visible frozen content in its first frame.

An intermediate version with the old fade-in measured 262.43 → 215.12 ms in a
separate interleaved run. It is superseded by the final run above: transparent
frames can map sooner but still require later paints to show screenshot content.
Do not compare these runs as if they differed only in one controlled variable.

Three final instrumented captures confirmed both monitors' first nonzero-opacity
paint was their first paint. Both visible paint callbacks finished at 211.98,
240.11 and 232.68 ms after key injection; both layers mapped at 228.19, 256.19
and 248.68 ms. Paint completion and QWidget UpdateRequest processing are not
compositor presentation timestamps.

## Full startup protocol and regression validation

The final plugin and helper ran together in an isolated nested Hyprland output
(3048×1656). Logs confirmed spawn → capture → session publish → helper receive
→ visible first paint, with an actual mapped overlay and verified executable.
The nested compositor was then stopped. This is functional coverage, not a
representative dual-monitor performance result. Full production-plugin latency
remains to be measured after a manual upgrade.

Release UI and plugin builds passed. Ten focused tests passed in the working tree:

```sh
ctest --test-dir build-startup --output-on-failure -R '^(session-startup|session-startup-ui|material-icon|in-place-editor|annotation-editor|hyprcapture-sound-ui-test|pin-launch|aec-ui-feedback|hyprcapture-security-test|hyprcapture-overlay-paint-test)$'
```

Coverage includes startup descriptor inheritance and close policy, malformed and
oversized packets, timeout/EOF/dead helper, nonblocking publication, actual helper
startup failure handling, icon pixels at fractional DPR, raw image orientation
and crop pixels, editor behavior, lazy recording control reuse and AEC feedback.
`aec-ui-feedback` belongs to the pre-existing uncommitted audio work.

The staged source was also exported independently of that audio work. Its Release
plugin/UI build passed, along with six tests: session-startup, session-startup-ui,
material-icon, in-place-editor, hyprcapture-sound-ui-test and pin-launch.

Local reproducibility artifacts are in
`~/.cache/hyprcapture-startup-validation/`: `measure-live.py`,
`final-benchmark.log`, `final-tests.log`, `final-ui-profile.log`,
`nested-final-plugin.log`, `nested-final-ui.log`, and `nested-final-layers.json`.
The benchmark requires two outputs, access to `/dev/uinput`, no existing overlay,
and trusted absolute paths to both Release helpers. It temporarily selects each
helper via `hl.config`, cancels each capture with Escape, and restores the original
helper in `finally`. Do not run it during compilation or other heavy workloads.

For timing, set `HYPRCAPTURE_TIMING_FILE` for the helper to a private runtime path;
enable plugin `timing`/`timing_file` separately. Compare `monotonic_us` timestamps
with a CLOCK_MONOTONIC key timestamp. Keep instrumented runs separate from A/B data.
