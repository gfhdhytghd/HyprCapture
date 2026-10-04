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

This is **not a NixOS acceptance result**. No full Nix derivation or reporter's
flake configuration was built here. The audio log did not include the helper's
error, so the namespace reproduction establishes a matching failure mechanism,
not a recovered diagnostic from that machine. The tests now print captured
stdout/stderr on command failure so any remaining Nix-specific cause is visible.
