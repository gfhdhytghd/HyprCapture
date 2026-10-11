# XPS Omarchy installation, 2026-10-11

- Booted the XPS into its Omarchy Btrfs root through GRUB's one-shot `xps-omarchy` entry. The default boot order is unchanged.
- Native Hyprland 0.56.2 (`efb509937800`) build installed via the Omarchy shell plugin bridge, with the default screenshot/recording bindings. Open-editor and cancel passed without leaving the test helper running.
- Fusion is the plugin's factory default. Users can still disable `fusion_mode`; the legacy misspelled alias keeps its false default.
- Omarchy currently outputs sRGB / XRGB8888 on eDP-1. HDR capture availability now requires an output actively using HDR, rather than EDID capability alone. Dark SDR/HDR control text is white; the selection keeps its gray fill.

## AEC

The old Omarchy recipe built and staged only the compositor plugin and UI. It omitted the sibling `hyprcapture-aec` worker, `libspa-aec-dtln.so`, and runtime/model installation. The recipe now installs these components plus retry scripts and the DTLN license. Installation reuses the CPU runtime and models prepared locally; model hashes are verified by the worker.

The actual XPS worker reports `Auto · CPU 512`. Its golden-output/state and 48 kHz processing checks pass: correlation 0.9999999999998591, relative output error 3.79e-7, real-time factor about 0.164, and p99 processing time about 1.36 ms.

The optional legacy `audio_echo_live_test.py` independently confirms `aec: active` and suppresses simulated far-end echo: RMS 0.04511 → 0.00336 over its 4–6 second interval. **The complete live test fails** at the near-end retention assertion: mixed-input RMS 0.05833 → 0.00229. Its near-end fixture is a 900 Hz amplitude-modulated tone, not recorded speech. Human-speech double-talk preservation and reference-loss acceptance remain unverified; the later assertions were not reached. Model self-tests and an active AEC node do not establish complete acoustic acceptance.
