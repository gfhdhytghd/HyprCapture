# DTLN-AEC RC1 acceptance — 2026-09-06

This is a local candidate, not approval to promote the feature to a formal
release. No personal recording is included in the repository or candidate.
The running desktop plugin was not replaced or reloaded during these checks.

## Verified

- Isolated AEC-only Release build against the current Hyprland development ABI;
  all 24 CTest tests passed. Unrelated dirty GPU recording files were excluded.
- Intel Core Ultra 7 265K, 20 logical CPUs: packaged CPU 512 under concurrent
  3840×2160/30 H.264 encoding (libx264, four threads) processed each ten-second
  complete-chain trial in 2.413 / 2.471 s; P99 2.512 / 2.417 ms. Auto selected 512.
- Packaged experimental NPU 512: 1.426 / 1.486 s per ten seconds; P99
  1.531 / 1.535 ms. Output correlation 0.99913, relative output error 0.04167,
  recurrent-state relative error 0.07130 over all 1,250 frames. Only NPU was
  selected; there is no AUTO, HETERO, or NVIDIA/GPU delegate fallback.
- Native CPU 512 and 256 versus the prior Python experiment on the same local
  74.36-second recording: correlations 0.99999985 / 0.99999979. The small remaining
  differences include the reference WAV's PCM quantization. These files remain
  private and outside the candidate.
- Single-core scheduling throttle (12 ms running / 68 ms paused): both models
  remained correct but missed automatic thresholds. Auto switched off;
  explicit On selected correct CPU 256. This is an emulated slow environment,
  not a measurement from separate low-end physical hardware.
- Two minutes of live PipeWire processing through private virtual sources under
  4K H.264 encoding load: AEC stayed active, output remained finite, and nodes
  were removed on stop. No physical microphone or speaker was used.
- Worker SIGKILL: raw-microphone fallback and continued audio. Capture-helper
  SIGKILL: worker and virtual-node cleanup. Original source and playback-output removal: reported
  failure and cleanup. No automatic model switch during a recording.
- SPA queue tests: sustained slow inference and a completely hung inference
  call both trigger protection independently from the inference thread.
- Impulse-verified Stream48 delay: 40 ms. The SPA scheduling queue adds 40 ms;
  recording compensates 80 ms and drains the tail before finalizing. An identity
  model through the complete private PipeWire/Pulse transport had residual
  timestamp errors of 15.3 ms and 8.4 ms in two windows, within the test's 20 ms
  tolerance. Model-dependent envelope changes are not used as a timing oracle.
- Existing mux tests cover four containers, video bitstream preservation,
  timestamp trimming/padding, source loss and recoverable mux failures.
- UI: all mixing presets, Auto/On/Off propagation, CPU default, narrow layout,
  monitor handoff, preview stop and independent persisted choices.
- Cache: copied-machine fingerprint rejection, changed runtime / CPU affinity,
  pending/busy state, no raw machine ID, legacy boolean migration and explicit
  configuration precedence. Damaged models and absent NPU components stay pending.
- Offline model installation (test-only network-connect denial) returned pending
  without replacing models or failing ordinary installation. Restoring network
  access downloaded all four verified models and completed the CPU check.
- Pinned native runtime installation script built TensorFlow Lite 2.14.0 with
  XNNPACK and retained licenses. The independent Nix runtime derivation also
  built successfully using fixed-hash sources; it copies Eigen into writable
  build storage before TensorFlow modifies it.

## Still required before formal promotion

- Real room / speaker / microphone listening acceptance: continuous near-end
  speech, double talk, echo residuals and longer recording sessions.
- Reload the candidate plugin and verify real compositor and GSR recordings,
  including first-frame warmup handoff, cancellation during preparation and
  visible lip synchronization. These paths compile, but the live desktop plugin
  was intentionally left for the user's manual update/testing workflow.
- A second physical low-performance CPU and another installation/driver profile.
- Full Nix application build/runtime and aarch64 runtime execution. Only the Nix
  native runtime build and package-expression evaluation were exercised here.

## Reproduction

Run `ctest --test-dir BUILD --output-on-failure` after the normal build. The
optional `tests/aec_pipewire_test.py` uses only private virtual audio sources;
set `HYPRCAPTURE_TEST_HELPER` to the built UI executable and provide validated
models/runtime (or use the candidate's packaged paths).

`HYPRCAPTURE_AEC_TEST_SECONDS=120 HYPRCAPTURE_AEC_TEST_ENCODER=1` enables the
longer encoding-load test. `HYPRCAPTURE_AEC_TEST_FAULT=worker|parent|device|output`
selects a fault scenario. `HYPRCAPTURE_AEC_TEST_TRANSPORT=1` uses the test-only
identity runtime to check timestamps and requires NumPy. None of these fault
injection controls exist in the installed inference implementation.

`hyprcapture-aec --check --force` performs silent correctness/performance tests;
add `--npu` only for the experimental backend. `--install` retries fixed-hash
model downloads and then checks. Cache files and user preferences are not shipped.
