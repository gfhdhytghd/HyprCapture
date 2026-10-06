# Hyprland mainline adaptation

## Target and scope

The `hyprland-master` branch targets Hyprland development/main. The 2026-10-06
adaptation uses `5a78b5e927345860a27e2893bf894f97ee620c48`, recorded in
`flake.lock`, with nixpkgs `a7868a727837f3c09cee2ce0ca671c76b1589fed`.
Later upstream commits are not covered by this result. Use HyprCapture `master`
with the latest official Hyprland release for the stable setup.

This update integrates committed HyprCapture `master` through `684bde1` into
the existing mainline branch, retaining its workspace/window API adaptations
and the newer editor, startup, native region-capture, and scrolling features.
It does not incorporate uncommitted work from the stable checkout.

## API changes

- Pass `Render::CRenderContext` through renderer, OpenGL, cursor, overlay, and
  custom render-pass calls. Feedback suppression is set after `beginRender`,
  because starting a render session resets that flag.
- Pass the native `SWindowRenderPresentation` snapshot to window rendering and
  use its workspace/floating offsets for capture geometry.
- Update the real-background render hooks and symbol lookup for
  `Render::IHyprRenderer`. Derive trampoline types from upstream declarations
  and statically check all three hook callbacks against those types.
- Restore the audio-finalize, recording-timestamps, and supervised-process
  tests previously disabled in this branch's Nix package. The fixes imported
  from `master` let them run in the sandbox.

The merged editor test needed a writable private fixture directory instead of
Nix's placeholder HOME, and a resolved `cat` executable instead of `/bin/cat`.
Only the test fixture and Nix check environment changed; executable trust
checks remain enabled. A sender test also incorrectly required every
nonblocking `submit()` to succeed despite the documented drop-on-contention
contract. Delivery assertions now retry with a deadline; the backpressure
test still requires an actual `SendWouldBlock` and verifies recovery on the
same connection. Production sender behavior is unchanged.

## Validation

On 2026-10-06, a complete Release derivation built successfully in a Nix 2.35.2
sandbox on Arch Linux with GCC 16.2.0. Hyprland itself, the compositor plugin,
Qt helper, audio helper, checks, installation, and fixup completed.

- CTest: **35 passed, 2 skipped, 0 failed** (37 registered).
- Skips: `scroll-live` needs a live graphical session;
  `hyprcapture-trusted-path-namespace-test` needs Bubblewrap/user namespaces
  unavailable inside this build sandbox.
- The original sender test reproduced its contention assertion on run 3 of a
  repeated host check. The updated test passed **100 consecutive runs**.
- Symbol inspection of the installed plugin found all 79 selected
  compositor-specific imports in the matching Hyprland executable. All three
  render-hook targets were exported. This is a symbol check, not a load test.
- `git diff --check` passed.

Local reproduction from this worktree:

```sh
nix --extra-experimental-features 'nix-command flakes' build .#hyprcapture \
  --store 'local?root=/home/wilf/data/issue-worktrees/hyprcapture-29/build-issue29/nix-root' \
  --option build-users-group '' --option sandbox true \
  --no-link --no-write-lock-file --print-out-paths --cores 6 --max-jobs 2 -L
```

The validated output was
`/nix/store/p6a24qcp1lfzqf6gd33hcfb0i83qysw0-hyprcapture-0.2.8`, physically
under that separate store root. Local evidence is retained under the worktree's
ignored `build-mainline/`: `nix-final-build.log`, `final-exit-code`,
`sender-stress.log`, and `symbol-check.json`.

## Runtime acceptance remains pending

A separate compositor startup was attempted with a private runtime directory,
no connection to the desktop Wayland/DBus sockets, and only a GPU render node
exposed. The unmodified mainline compositor exited during backend creation
with `CBackend::create() failed!`, before plugin loading. The test configuration
passed Lua syntax validation. Logs are in `build-mainline/runtime-probe/`.

No current desktop plugin was installed or reloaded. Real window/region/fullscreen
captures, real-background blur hooks, transformed outputs, cursor capture,
scrolling, and recording still require a session running this exact Hyprland
revision. The successful sandbox build does not establish NixOS reporter or
live compositor acceptance.
