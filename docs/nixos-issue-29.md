# Issue 29: follow-up test failures

Evidence was checked against all discussion in [issue 29](https://github.com/gfhdhytghd/HyprCapture/issues/29)
through 2026-10-04, starting from `bb9ab93`.

## Findings

- The September 29 log completes compilation and fails `audio-finalize`,
  `recording-timestamps`, and `supervised-process`. Both Python failures occur
  inside `--sound-finalize`; the old test runners hide its captured output.
- Nix's [Linux sandbox builder](https://github.com/NixOS/nix/blob/master/src/libstore/linux/build/linux-derivation-builder.cc)
  maps the build user's UID into its user namespace. Store owners can therefore
  appear as the kernel's unmapped UID. The helper rejects those executables as
  untrusted. A local user-namespace reproduction reaches the same finalization
  failure and reports `FFmpeg/ffprobe unavailable`.
- Trust now permits an **unmapped** owner only below the existing immutable
  boundary, with read-only permissions or a read-only mount. It reads the kernel
  overflow UID and checks the UID map, rather than trusting account 65534.
  Group/other-writable files and directories remain rejected. Nix inputs with
  mode 0555 do not need a read-only filesystem mount.
- Nix can use an embedded BusyBox sandbox shell at `/bin/sh`. With BusyBox ash,
  the original process-group test fails at `group stop preserves exit report`:
  the supervisor receives SIGINT as well as the recorder and exits before
  writing its report. Catching INT/TERM in the supervisor preserves the report
  without making the child inherit ignored signals.
- The September 30 claim of the original compile error includes no new log or
  resolved flake source. The extra parentheses are present in `bb9ab93`.
  Compiling the preceding revision with a single-argument assert macro reproduces
  the original diagnostic; compiling and running `bb9ab93` with that macro
  succeeds. The remaining report needs the resolved input revision and fresh
  compiler diagnostic, including the source line, before attributing a cause.

## Validation and limits

Local validation on Arch Linux includes a full Release build and all 25 CTests,
the native and single-argument-assert window-stream tests, and the expanded
supervisor regression with system Bash, locally built Bash 5.3.15, and BusyBox
ash. The original implementation fails the new signal-death regression and the
BusyBox version of the original group-stop scenario.

`trusted-path-namespace-test` checks remapped owners, including read-only mounts,
0555 executables on writable mounts, and rejected writable/outside paths. It
skips when Bubblewrap or user namespaces are unavailable and reports unavailable
subcases. The original trust implementation fails its acceptance regression.
Both real FFmpeg integration tests also pass with a read-only `/usr` bind at a
temporary `/nix/store` path inside a user namespace; no host store is changed.

The reporter's own flake configuration and multi-user setup remain unverified.
The audio log did not include the helper's error, so the namespace reproduction
establishes a matching failure mechanism, not a recovered diagnostic from that
machine. The tests now print captured stdout/stderr on command failure so any
remaining Nix-specific cause is visible.

## Full Nix derivation build (2026-10-04)

Commit `07da87f` successfully completed a sandboxed Nix build on the Arch host
using Nix 2.35.2 and the repository's unchanged `flake.lock`. The system store
was uninitialized, so a separate single-user chroot store was used inside the
worktree; no system installation or plugin loading was performed.

```sh
nix --extra-experimental-features 'nix-command flakes' build .#hyprcapture \
  --store "local?root=$PWD/build-issue29/nix-root" \
  --option build-users-group '' --option sandbox true \
  --no-link --no-write-lock-file --print-out-paths --cores 4 --max-jobs 2 -L
```

The locked toolchain used GCC 16.1.0, Hyprland
`0.56.0+date=2026-07-25_453d96e`, and Python 3.14.6. All dependencies, including
TensorFlow Lite and Hyprland, built successfully. HyprCapture completed build,
check, install, and fixup phases with exit status 0.

CTest: **24 passed, 1 skipped, 0 failed**. `audio-finalize`,
`supervised-process`, `recording-timestamps`, and `window-stream` all passed.
The optional Bubblewrap namespace test was skipped in this derivation; it
passed separately on the host as described above. This verifies the locked Nix
package, not the reporter's different lock/toolchain or a live NixOS desktop.

Output: `/nix/store/yxsa18qcq7kpnmrg422dx7i4dwpzswpm-hyprcapture-0.2.8`
(physically under `build-issue29/nix-root`). Full local log:
`build-issue29/nix-build.log`. The store is retained for repeat builds.
