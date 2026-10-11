# Session lock input audit

Changes are uncommitted. No plugin reload, desktop lock, device interaction, or `.git` writes were performed.

## Changes and lock behavior

- `src/shared/session_lock.hpp`, `src/plugin/session_lock.{hpp,cpp}`, `src/shared/process_environment.cpp`: one current-state predicate (`manager && manager->isSessionLocked()`), a session-lock listener, and a private per-compositor helper notification socket. Missing lock management/socket initialization prevents plugin initialization; helpers require an unlocked handshake. A generation inherited from plugin launches and helper descendants changes on lock/reload, rejecting even slow pins/thumbnails that connect after unlock. Lock notification or compositor disconnect permanently cancels each connected helper. Pending connections are rejected on lock, including across a fast unlock.
- `src/plugin/scroll_session.inc`: entry guards on every input callback, synthetic-control callback, delivery function and replay timer. Locked callbacks return before cancellation or seat delivery. Pointer focus is restored after each delivery, before returning to the event loop; `m_lastFocusOnLS` is also restored. Lock teardown discards the session, queued input, timers, transport and readbacks. No pointer constraints are installed by this plugin. Cleanup does not send seat events or restore desktop focus while locked.
- `src/plugin/artifact_capture.{cpp,hpp}`, `main.cpp`, `session_launcher.cpp`: guard all Lua entry points, capture/stream starts, frame reads and helper launches; stop streams, pending exports and recording readbacks on lock. Rendering detours forward the original locked renderer without capture/exclusion changes.
- `src/ui/session_lock_guard.{hpp,cpp}`, `main.cpp`, `capture_overlay.{hpp,cpp}`: gate Qt delivery before application/editor event filters, including queued input, focus, timers, socket callbacks and queued actions. Check the original startup channel too, before creating overlays, so a slow cancelled helper cannot resume via a new unlocked handshake. On cancellation, release grabs, remove keyboard-exclusive layer interactivity, hide/disable windows, abort selection/editor work, stop/join the scroll worker without finishing/exporting, and quit. Close handlers are not invoked; nothing resumes on unlock.
- `src/plugin/recording.{cpp,hpp}`: stop recording and pending audio preparation on lock. Compositor capture cannot read any new frames while locked. Signal gpu-screen-recorder with SIGINT; its remaining shutdown work uses normal compositor output. Finalize previously captured video/audio without clipboard, thumbnails, deferred UI, or backend fallback. APNG recording retains its finalized MKV intermediate instead of starting a later interactive transcode. Existing finalizers also lose their pending result actions.
- `CMakeLists.txt`, `tests/session_lock_test.cpp`, `session_lock_guard_test.cpp`, `session_lock_fixture.py`, `session_startup_ui_test.py`, `pin_launch_test.py`, `recording_permissions_test.py`, `process_environment_test.cpp`: predicate/generation/Qt guard tests and private lock-channel fixtures for real helper tests.

Pointer enter/leave now surrounds redirected scroll delivery. Touchpad/fling behavior and actual layer focus on a locked compositor require live validation; this report claims build/unit evidence only.

## Registered listener/hook inventory

All `registerCallback`, `.listen`, function-hook creation and installation sites in `src/plugin` were searched. There are no `registerCallback` calls or input/focus detours beyond those below.

| File | Registration / callback | Guard status |
| --- | --- | --- |
| `scroll_session.inc` | window `unmap` | Entry predicate; return while locked |
| `scroll_session.inc` | mouse `button` | Entry predicate before simulated motion; no cancellation |
| `scroll_session.inc` | mouse `move` | Entry predicate before cancellation, motion or frame delivery |
| `scroll_session.inc` | mouse `axis` | Entry predicate before cancellation, queueing or redirection; covers physical and virtual pointer axes |
| `artifact_capture.cpp` | target window `unmap` → `revokeTarget` | Entry predicate |
| `artifact_capture.cpp` | target surface `unmap` → `revokeTarget` | Entry predicate |
| `artifact_capture.cpp` | target surface `destroy` → `revokeTarget` | Entry predicate |
| `artifact_capture.cpp` | GPU target window `unmap` notification | Entry predicate |
| `session_lock.cpp` | session manager `lock` → `onSessionLock` | Intentionally unconditional teardown; no input/focus delivery |
| `scroll_session.inc` | `createFunctionHook` + `hook`: `renderLayer` → `hkScrollLayer` | Entry predicate; original rendering only while locked |
| `artifact_capture.cpp` | `createFunctionHook` + `hook`: `renderTextureInternal` | Entry predicate; original rendering only while locked |
| `artifact_capture.cpp` | `createFunctionHook` + `hook`: `renderTextureWithBlurInternal` | Entry predicate; original rendering only while locked |

No plugin keyboard, keyboard/pointer focus-change, touch, gesture, tablet, virtual-keyboard or other virtual-input listeners/detours are registered. All seat delivery and temporary input/focus redirection occur in the guarded scroll paths.

The 16 registered Lua functions also have entry guards: `open`, `open_editor`, `quick`, `record`, `record_toggle`, `record_stop`, `record_start`, `window_capture`, `region_capture`, `scroll_session_start`, `scroll_session_stop`, `export_pipe`, `window_stream_start`, `window_stream_stop`, `cancel`, `dispatch`.

Additional event-loop registrations were audited:

| File | Callback | Guard status |
| --- | --- | --- |
| `scroll_session.inc` | control fd → `scrollControlReadable` | Entry predicate before reading synthetic commands |
| `scroll_session.inc` | timer → `scrollTick` | Entry predicate; stop instead of replay/capture |
| `artifact_capture.cpp` | GPU notification fd → `onWindowGpuNotification` | Entry predicate |
| `artifact_capture.cpp` | capture timer → `captureWindowStreamTick` | Entry predicate; stop stream |
| `artifact_capture.cpp` | drain timer → `captureWindowStreamDrainTick` | Entry predicate; stop stream |
| `session_lock.cpp` | helper accept fd → `acceptHelpers` | Predicate selects locked rejection/unlocked handshake; control metadata only |
| `recording.cpp` | audio preparation timer | Entry predicate; cancel pending start |
| `recording.cpp` | recording capture timer → `captureRecordingTick` | Entry predicate; stop recording |
| `recording.cpp` | GSR monitor timer | Predicate stops recording; continues only shutdown/reaping |
| `recording.cpp` | retired audio timer | Cleanup only; no input/focus/capture |
| `recording.cpp` | audio finalization timer | Finalization only; lock teardown disables result actions |
| `recording.cpp` | raw-video finalization timer | Finalization only; lock teardown disables result/transcode actions |

`recording_state.cpp` also has a POSIX socket `listen` and worker poll loop, not a compositor listener/hook. It serves recording metadata and does not observe or inject input/focus.

## Verification

Configured and built successfully against installed Hyprland **0.56.2**, with HDR/HEIC enabled and cached pinned dependency sources:

```sh
cmake -S . -B /tmp/hyprcapture-lock-build -G Ninja -DBUILD_TESTING=ON \
  -DFETCHCONTENT_SOURCE_DIR_ULTRAHDR=/tmp/hyprcapture-review/libultrahdr-e5f5a022fe96fc4dc2ee35c19f733a50df807abe \
  -DHYPRCAPTURE_LIBHEIF_URL=/tmp/hyprcapture-heif-source.tar.gz
cmake --build /tmp/hyprcapture-lock-build -j 4
env -u QT_IM_MODULE -u QT_QPA_PLATFORMTHEME -u QT_LINUX_ACCESSIBILITY_ALWAYS_ON \
  LANG=C.UTF-8 LC_ALL=C.UTF-8 QT_QPA_PLATFORM=offscreen \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/hyprcapture-tests-no-bus \
  ctest --test-dir /tmp/hyprcapture-lock-build --output-on-failure -j 4
```

- Final suite: **35 passed, 10 failed, 2 skipped** (47 registered). `git diff --check` passed.
- Both focused tests passed. Predicate/protocol test covers missing manager, current lock/unlock state, generation adoption, rejection of stale descendants, and malformed/locked handshakes. Qt test covers terminal cancellation before queued input and later event filters, keyboard/mouse/wheel/touch/gesture/tablet/focus events, pending action callbacks, synchronous overlay abort, hiding/disabling and prevention of remapping.
- Sandbox denies Unix socket `bind` and `send` with EPERM. Qt guard test explicitly reports that lock-packet delivery is unavailable and exercises terminal EOF instead. Real overlay/pin lock-notification tests, recording-permission fixture, startup channel, state socket and CPU/GPU sender tests fail at those IPC boundaries; packet delivery is **not verified** here.
- Audio-finalize and recording-timestamps tests reject FFmpeg/ffprobe as untrusted: `/usr`, `/usr/bin` and their executables are owned by unmapped `nobody` in this sandbox. In-place-editor exits before its test runner; its fixture requires a temporary trusted HOME outside the writable roots. These remain failed, not reported as passed.
- `scroll-live` and `sdr-resolve-gpu` skipped. Live lock-screen delivery, layer focus and scroll gesture acceptance remain **PENDING**.

Logs: `/tmp/hyprcapture-lock-configure.log`, `/tmp/hyprcapture-lock-build.log`, `/tmp/hyprcapture-lock-focused-tests.log`, `/tmp/hyprcapture-lock-tests-final.log`.
