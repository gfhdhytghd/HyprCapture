# HyprCapture

HyprCapture is a Hyprland-only screenshot tool split into a compositor plugin and a Qt layer-shell helper. The plugin captures frozen compositor artifacts and launches the helper; the helper provides the selection overlay, output rendering, clipboard integration, and result thumbnail.

All Hyprland configuration examples in this document use the Lua config API available in Hyprland 0.56 and later.

> [!IMPORTANT]
> `hyprpm` builds the compositor plugin and installs the helper to `~/.local/bin/hyprcapture-ui`. Set `plugin.hyprcapture.helper` with `hl.config` only when you want to override that default helper path.

> [!WARNING]
> Hyprland plugins run inside the compositor process and with high permission. Install plugins only from sources you trust.

> [!WARNING]
> This software is 99% vibe coded with OpenAI CodeX, but have been manual audited, warn in case you mind it.

## Features

- Fullscreen, region, and window capture modes
- Fullscreen, region, and window recording as video or fixed-duration GIF/APNG/WebP animations
- Immediate frozen desktop image behind the overlay
- Display-resolution output for fullscreen and region captures
- Compositor-side window artifacts, including windows that are occluded or partly off-screen
- Window output backgrounds: follow system, white, black, real background, or transparent
- Optional window border and shadow removal
- Optional image watermarks from PNG, JPG/JPEG, or built-in presets
- Save-to-file and clipboard output
- Stable Wayland clipboard writes through `wl-copy` when available, with Qt clipboard fallback
- macOS-style result thumbnail with open, copy, show in folder, delete, and close actions
- Thumbnail swipe gestures: swipe right to close, swipe down to delete and restore the previous clipboard snapshot
- Recording does not use `wf-recorder`, `grim`, screencopy, PipeWire portals, or Hyprland managed screenshare sessions


https://github.com/user-attachments/assets/2c986639-7a3d-44ee-9f33-1b9b79ad9f1d


## Installation
### Dependencies

- Hyprland development headers for the exact Hyprland build you are running
- `cmake`
- `pkg-config`
- a C++23-capable compiler
- nlohmann-json
- Qt 6 Core, Gui, and Widgets
- LayerShellQt `layer-shell-qt`
- FFmpeg for recording output
- `wl-clipboard` for persistent Wayland clipboard ownership

### Install with `hyprpm`

Use `hyprpm` for the compositor plugin:

```sh
hyprpm update
hyprpm add https://github.com/gfhdhytghd/HyprCapture
hyprpm enable hyprcapture
hyprpm reload
```

If you use Hyprland's permission system, allow `hyprpm` in your config:

```lua
hl.permission("/usr/(bin|local/bin)/hyprpm", "plugin", "allow")
```

Do not also manually `hyprctl plugin load` the same `.so` if you manage it through `hyprpm`.

### Install on NixOS

Add HyprCapture to the same flake as Hyprland and make its Hyprland input follow yours. Keeping both inputs on the same revision is required because Hyprland plugins are ABI-sensitive:

```nix
{
  inputs = {
    hyprland.url = "github:hyprwm/Hyprland";
    hyprcapture = {
      url = "github:gfhdhytghd/HyprCapture";
      inputs.hyprland.follows = "hyprland";
    };
  };
}
```

With the Home Manager Hyprland module:

```nix
{ inputs, pkgs, ... }:
{
  wayland.windowManager.hyprland.plugins = [
    inputs.hyprcapture.packages.${pkgs.stdenv.hostPlatform.system}.hyprcapture
  ];
}
```

The Nix package contains both `lib/libhyprcapture.so` and `bin/hyprcapture-ui`. It embeds the matching helper path and trusted Nix store paths for its runtime tools, so no manual helper setting is needed.

Install the optional runtime tools you use (`ffmpeg`, `gpu-screen-recorder`, `wl-clipboard`, `grim`, `libnotify`, and `xdg-utils`) in your NixOS or Home Manager profile. HyprCapture accepts executables from the standard system and per-user Nix profiles after applying the same ownership and permission checks as other trusted paths.

You can also build it directly:

```sh
nix build github:gfhdhytghd/HyprCapture
```

### Helper install path

The `hyprpm` manifest installs the helper automatically:

```toml
build = [
    "cmake -S . -B build-hyprpm -DCMAKE_BUILD_TYPE=Release",
    "cmake --build build-hyprpm",
    "ctest --test-dir build-hyprpm --output-on-failure",
    "install -Dm755 build-hyprpm/hyprcapture-ui \"$HOME/.local/bin/hyprcapture-ui\""
]
```

The plugin default helper lookup is:

1. `HYPRCAPTURE_HELPER`
2. `$HOME/.local/bin/hyprcapture-ui`
3. `/usr/local/bin/hyprcapture-ui`
4. `/usr/bin/hyprcapture-ui`

Helper paths must resolve to trusted regular executables owned by the current user or root, without group/other write permission, and with trusted parent directory permissions.

Only configure `plugin.hyprcapture.helper` if you want to use a custom helper path:

```lua
hl.config({
    plugin = {
        hyprcapture = {
            helper = "/home/you/.local/bin/hyprcapture-ui",
        },
    },
})
```

### Manual helper install

Install the Dependencies

Build and install the helper:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
install -Dm755 build-release/hyprcapture-ui "$HOME/.local/bin/hyprcapture-ui"
```

To build against a Hyprland source checkout, point CMake at the matching tree so the plugin uses the same Lua-config-capable headers:

```sh
cmake -S . -B build-local -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHYPRLAND_SOURCE_DIR="$HOME/data/Hyprland"
cmake --build build-local
ctest --test-dir build-local --output-on-failure
```

For development without installing, point `helper` at the build-tree executable or launch Hyprland with:

```sh
HYPRCAPTURE_HELPER=/path/to/hyprcapture-ui Hyprland
```

### Manual build and reload

For local development, the plugin output is `build-cmake/libhyprcapture.so`.

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

Safe reload shape:

```sh
hyprctl plugin unload "$(pwd)/build-cmake/libhyprcapture.so"
hyprctl plugin unload "$(pwd)/build-hyprpm/libhyprcapture.so"
hyprctl plugin load "$(pwd)/build-cmake/libhyprcapture.so"
hyprctl plugin list
```

`plugin not loaded` is expected when the unloaded path is not the active copy.

Build outputs:

- Plugin: `build-cmake/libhyprcapture.so`
- Helper: `build-cmake/hyprcapture-ui`
- Config test: `build-cmake/hyprcapture-config-test`

## Usage

### Lua actions and key bindings

```lua
hl.bind("SUPER + SHIFT + s", hl.plugin.hyprcapture.open)

hl.bind("SUPER + SHIFT + w", function()
    hl.plugin.hyprcapture.open("window")
end)

hl.bind("SUPER + SHIFT + f", function()
    hl.plugin.hyprcapture.open("fullscreen")
end)
```

| Lua action | Description |
| --- | --- |
| `hl.plugin.hyprcapture.open()` | Open the overlay using `default_mode`. |
| `hl.plugin.hyprcapture.open(mode)` | Open the overlay in `region`, `fullscreen`, or `window` mode. |
| `hl.plugin.hyprcapture.quick()` | Capture immediately using `default_mode`; disabled unless `allow_quick = true`. |
| `hl.plugin.hyprcapture.quick(mode)` | Capture immediately in `region`, `fullscreen`, or `window` mode; disabled unless `allow_quick = true`. |
| `hl.plugin.hyprcapture.export_pipe(fifo)` | Export fullscreen compositor RGBA frames to a trusted private FIFO client. Intended for tools such as screenland. |
| `hl.plugin.hyprcapture.cancel()` | Reserved action; currently returns successfully without changing an active helper. |

Available Lua functions are `open`, `quick`, `record`, `record_toggle`, `record_stop`, `record_start`, `window_capture`, `export_pipe`, `cancel`, and `dispatch`.
`dispatch` accepts the dispatcher name plus an optional argument, for example `hl.plugin.hyprcapture.dispatch("open", "fullscreen")`.

Use lowercase `s` for `SUPER + s`. In Lua config key strings, uppercase `S` means Shift is part of the binding.

### Overlay

- Region mode: drag a rectangle, then release or press Enter.
- Fullscreen mode: captures according to `fullscreen_scope`.
- Window mode: hover a window and press Enter or click it. With `window_wheel_scroll` enabled, scroll down to select a lower window or scroll up to move back toward the top; selection stops at each end of the stack. `window_wheel_scope` chooses between every window on the pointer's current workspace/output and only windows directly under the pointer.
- Fusion mode: the toolbar keeps the fullscreen action and configuration controls; drag anywhere to capture a region, single-click a window to capture that window, or use the same wheel selection before clicking. A region drag still takes precedence over a wheel-selected window.
- Esc cancels the helper.
- The toolbar is anchored near the bottom of the screen and only shows controls relevant to the active mode.

### External FIFO capture backend

HyprCapture can act as a fullscreen compositor capture backend for another local tool. The client creates a private request FIFO and response FIFO under the per-user HyprCapture runtime root, writes a newline-terminated JSON request to the request FIFO, then calls:

```sh
hyprctl eval "hl.plugin.hyprcapture.export_pipe('/dev/shm/hyprcapture-$UID/<client>/request.fifo')"
```

Request JSON v1:

```json
{"version":1,"responseFifo":"/dev/shm/hyprcapture-1000/<client>/response.fifo","mode":"fullscreen","fullscreenScope":"all"}
```

The response FIFO starts with `HYPRCAP_PIPE_V1`, a decimal JSON header length, the JSON header, and then the raw `rgba8888` monitor payloads in header order. FIFO paths must be absolute, owned by the current user, not group/other writable, and inside the private `hyprcapture-$UID` runtime root. Large payload writes happen from a managed writer thread so the compositor dispatcher does not block on FIFO backpressure.

### Recording

Recording is toggled from the normal screenshot overlay toolbar. Click the record icon, choose an output format and backend, then choose fullscreen, drag a region, or choose a window exactly like a screenshot. The default `auto` backend uses HyprCapture's compositor renderer for windows and `gpu-screen-recorder` for fullscreen or region video. Virtual-output targets always use the compositor because GPU Screen Recorder cannot address them.

To stop an active recording, open the same overlay and click the checked record icon.

Current recording output is a single file under `record_save_dir`. With `record_window_backend = "auto"`, normal fullscreen and region video use `gpu-screen-recorder`, while windows, virtual outputs, GIF, APNG, and WebP use compositor RGBA readback. When the codec is `auto`, fullscreen targets above the 4096 H.264 dimension limit and within 8192 on both axes stay on GSR and switch to HEVC automatically. If the preferred backend fails during startup, HyprCapture tries the compatible alternate backend; a GSR process that exits without producing data also falls back to compositor recording. Transparent recordings and virtual outputs stay on compositor because GSR is not an equivalent fallback. APNG records a hidden 60 fps MKV intermediate first, then the helper transcodes it to APNG while showing the thumbnail with progress when thumbnails are enabled. Animation formats require a fixed duration of 3, 5, 10, 15, or 30 seconds and default to 5 seconds. `gsr-visible` records a selected on-screen window rectangle with lower overhead, without portal or managed screenshare sessions, but captures what is visibly present in that region. Finished recordings use the same `clipboard` and `show_thumbnail` settings as screenshots; clipboard output is a local file URI.

#### Recording state socket

Local status bars and other integrations can read recording state from a Unix stream socket. The primary path is:

```text
$XDG_RUNTIME_DIR/hyprcapture/recording.sock
```

If `XDG_RUNTIME_DIR` is unavailable or unsafe, HyprCapture falls back to `/dev/shm/hyprcapture-$UID/recording.sock`, then `/tmp/hyprcapture-$UID/recording.sock`. The runtime directory is owner-only and the socket mode is `0600`. The socket is removed on a clean plugin unload; a user-owned stale socket is replaced on the next load.

Connect once to receive one newline-terminated JSON object, after which HyprCapture closes the connection. No request body is needed and the socket does not accept commands:

```sh
socat - UNIX-CONNECT:"${XDG_RUNTIME_DIR}/hyprcapture/recording.sock"
```

Protocol v1 example:

```json
{"version":1,"active":true,"phase":"recording","backend":"gpu-screen-recorder","output":"/home/user/Videos/Screenrecords/Recording.mp4","elapsed":12.345,"elapsedMs":12345,"startedAt":"2026-07-23T12:34:56Z","mode":"fullscreen","format":"mp4"}
```

`phase` is `inactive`, `recording`, or `finalizing`. `active` is true only while frames are being captured. During `finalizing`, the encoder is draining and `elapsed`/`elapsedMs` remain frozen at the captured duration; `output` remains available until finalization completes. `backend` is `gpu-screen-recorder` or `compositor`. Inactive string fields are empty and elapsed values are zero. Clients should ignore unknown fields so later protocol versions can add metadata without breaking existing integrations.

### Thumbnail

The thumbnail appears after capture when `show_thumbnail = true`.

- Left click opens the saved image.
- Drag starts a file drag for targets that accept image files.
- Right click opens the thumbnail menu.
- Swipe right with a touchpad or two-dimensional wheel to close.
- Swipe down to delete the file and restore the clipboard snapshot captured before the screenshot.

## Configuration

All user-facing settings live under `plugin.hyprcapture` in `hl.config`.

Example:

```lua
hl.config({
    plugin = {
        hyprcapture = {
            default_mode = "region",
            fullscreen_scope = "all",
            overlay_scope = "fix",
            window_background = "follow-system",
            window_border = "keep",
            window_shadow = "keep",
            notification_backend = "hyprland",
            screenshot_notification = true,
            notification_title_template = "Screenshot captured",
            notification_body_template = "Saved {filename} ({window_title})",
            save = true,
            clipboard = true,
            show_thumbnail = true,
            allow_quick = false,
            confirm_before_capture = false,
            fusion_mode = false,
            capture_fullscreen_clients_as_monitor = false,
            dynamic_window_metadata = true,
            window_wheel_scroll = true,
            window_wheel_scope = "workspace",
            fullscreen_preview_rounding = "auto",
            save_dir = "$XDG_PICTURES_DIR/Screenshots",
            filename_template = "Screenshot-%Y-%m-%d-%H%M%S.png",
            record_save_dir = "$XDG_VIDEOS_DIR/Screenrecords",
            record_filename_template = "Recording-%Y-%m-%d-%H%M%S.mp4",
            record_format = "mp4",
            record_transparent_format = "webm",
            record_fps = 30,
            record_fps_options = "15 24 30 60",
            record_window_fps_limit = 12,
            record_window_real_bg_fps_limit = 8,
            record_codec = "auto",
            record_transparent_codec = "auto",
            record_solid_alpha = false,
            record_preset = "veryfast",
            record_gsr_flags = "",
            record_window_backend = "auto",
            record_max_seconds = 0,
            record_countdown_seconds = 0,
            include_cursor = false,
            thumbnail_timeout_ms = 5000,
            thumbnail_monitor = "active",
            watermark = "",
            watermark_position = "central",
            watermark_width = "20%",
            watermark_offset = "0 0",
        },
    },
})
```

### Lua API notes

Use `hl.config` for settings and `hl.plugin.hyprcapture` for actions:

```lua
hl.bind("SUPER + s", hl.plugin.hyprcapture.open)

hl.bind("SUPER + SHIFT + s", function()
    hl.plugin.hyprcapture.open("window")
end)

hl.bind("SUPER + CTRL + s", hl.plugin.hyprcapture.record_toggle)
```

Do not use `hyprctl dispatch hyprcapture:open` as a Lua fallback. Hyprland's Lua config dispatcher parser treats `hyprcapture:open` as Lua syntax. Use `hl.plugin.hyprcapture.open()` once the plugin is loaded, or bind a direct helper command during development while testing an older loaded plugin.

The old misspelled `fushion_mode` key is still accepted as a compatibility alias, but new configs should use `fusion_mode`.

### Capture options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `default_mode` | string | `region` | Default mode for `hl.plugin.hyprcapture.open()` and `hl.plugin.hyprcapture.quick()`. Supports `region`, `fullscreen`, and `window`. |
| `fullscreen_scope` | string | `all` | Fullscreen capture scope. Supports `all`, `current`, and `per-monitor`. |
| `overlay_scope` | string | `fix` | Overlay monitor behavior. `fix` keeps it on the monitor where capture starts, `focus` moves it to the monitor under the pointer while it is open, and `all` shows it on every monitor. The legacy typo `forcus` is accepted as `focus`. |
| `window_background` | string | `follow-system` | Background behind transparent window pixels. Supports `follow-system`, `white`, `black`, `real`, and `transparent`. |
| `window_border` | string | `keep` | Window border policy. Supports `keep` and `remove`. |
| `window_shadow` | string | `keep` | Window shadow policy. Supports `keep` and `remove`. Transparent window recordings keep shadows and normalize the alpha falloff so the shadow fades out instead of encoding as a hard border. |
| `notification_backend` | string | `hyprland` | Backend for screenshot notifications plus non-error recording status and warnings. `hyprland` uses Hyprland's overlay; `system` uses the desktop notification service through `notify-send` (libnotify), includes the saved screenshot as its image/icon hint, and falls back to the Hyprland overlay when the command cannot be launched. Errors always use the Hyprland overlay so missing external notification infrastructure cannot hide failures. |
| `include_cursor` | bool | `false` | Include the cursor visible when the capture session starts in fullscreen, region, and window screenshots. The interactive overlay cursor is not baked into the output. |
| `allow_quick` | bool | `false` | Enable no-confirmation `hl.plugin.hyprcapture.quick()` calls. Leave disabled unless your Hyprland IPC policy already restricts untrusted same-user clients. |
| `confirm_before_capture` | bool | `false` | For `hl.plugin.hyprcapture.open()`, require an explicit confirmation after choosing a fullscreen, region, or window target. Region targets can be moved or resized; window targets can be switched before confirming. `quick()` and direct `record()` calls keep their existing no-extra-confirmation behavior. |
| `fusion_mode` | bool | `false` | Fuse region and window interactions in one overlay: drag anywhere, including from the desktop background, to capture a region; single-click a window to capture that window; or single-click the background to capture the clicked monitor. The toolbar keeps the fullscreen action and configuration controls; fullscreen multi-monitor scope is shown only when multiple monitors are present. |
| `fushion_mode` | bool | `false` | Legacy compatibility alias for `fusion_mode`. New configs should use `fusion_mode`. |
| `capture_fullscreen_clients_as_monitor` | bool | `false` | In window and fusion modes, capture the fullscreen client's entire monitor instead of the isolated client. Disabled by default for compatibility. |
| `dynamic_window_metadata` | bool | `true` | Resolve `{window_class}` and `{window_title}` from the final screenshot mode. Region captures use `region`; window captures use the selected window; fullscreen captures use the only window in the captured workspace scope, or `fullscreen` when there are zero or multiple windows. Set to `false` to restore the focused-window fallback. |
| `window_wheel_scroll` | bool | `true` | Enable mouse-wheel and touchpad-scroll window cycling in window and fusion modes. Set to `false` to leave wheel events unused by window selection. |
| `window_wheel_scope` | string | `workspace` | Controls wheel-based window cycling in window and fusion modes. `workspace` cycles every selectable window on the pointer's current workspace/output; `under-cursor` cycles only windows whose selectable geometry contains the pointer. Both modes follow window z-order and stop at the top and bottom instead of wrapping. |
| `fullscreen_preview_rounding` | string | `auto` | Fullscreen preview border rounding. `auto` detects opaque or near-uniform desktop corner masks from each frozen monitor image, a non-negative number forces that logical-pixel radius, and `0` keeps square corners. Low-confidence auto detection falls back to square corners and never crops the captured image. |

### Output options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `save` | bool | `true` | Save the output image to `save_dir` as an owner-only file. |
| `clipboard` | bool | `true` | Copy the output image to the clipboard. Uses `wl-copy` when available so the clipboard survives helper exit. |
| `show_thumbnail` | bool | `true` | Show the result thumbnail after capture. |
| `screenshot_notification` | bool | `true` | Show a notification after a screenshot is successfully saved. Disable this independently of recording status notifications. |
| `save_dir` | string | `$XDG_PICTURES_DIR/Screenshots` | Output directory. `~` is expanded against `HOME`; `$XDG_PICTURES_DIR` is read from XDG user-dirs with `~/Pictures` as fallback. |
| `filename_template` | string | `Screenshot-%Y-%m-%d-%H%M%S.png` | `strftime` template for saved screenshot filenames. `{window_class}` and `{window_title}` follow `dynamic_window_metadata`; values are filename-sanitized and missing metadata expands to `unknown`. Existing `strftime` directives, including `%w`, keep their original meaning. |
| `notification_title_template` | string | `Screenshot captured` | Screenshot notification title. Supports `strftime` directives and `{mode}`, `{window_class}`, `{window_title}`, `{filename}`, and `{path}`. Window metadata is not filename-sanitized. |
| `notification_body_template` | string | `Saved {filename} ({window_title})` | Screenshot notification body. Supports the same placeholders and `strftime` directives as `notification_title_template`. |
| `record_save_dir` | string | `$XDG_VIDEOS_DIR/Screenrecords` | Output directory for recordings. `$XDG_VIDEOS_DIR` is read from XDG user-dirs with `~/Videos` as fallback. Finished recordings can be copied to the clipboard as local file URIs and shown in the thumbnail when those global output settings are enabled. |
| `record_filename_template` | string | `Recording-%Y-%m-%d-%H%M%S.mp4` | `strftime` template for saved recording filenames. |
| `record_format` | string | `mp4` | Default recording format shown in the overlay for non-transparent window backgrounds, fullscreen recording, and region recording. Supports `mp4`, `mov`, `webm`, `mkv`, `gif`, `apng`, and `webp`; the selected value replaces the filename extension. |
| `record_transparent_format` | string | `webm` | Default recording container shown when `window_background = "transparent"`. |
| `record_fps` | int | `30` | Recording frame rate. Higher values increase compositor readback and encoder load. |
| `record_fps_options` | string | `15 24 30 60` | Whitespace, comma, or semicolon separated FPS choices shown in the overlay. The current `record_fps` value is added if it is not already listed. |
| `record_window_fps_limit` | int | `12` | Safety cap for window recording with the current compositor-readback backend. Use `0` to disable the cap. |
| `record_window_real_bg_fps_limit` | int | `8` | Additional safety cap for window recording with `window_background = "real"`. Use `0` to disable the cap. |
| `record_codec` | string | `auto` | Default recording codec shown in the overlay for normal video formats. The UI exposes codec families only: `auto`, `h264`, `h265`, `av1`, `vp9`, and `ffv1`. HyprCapture automatically probes NVENC and VAAPI at the selected resolution, then falls back to the corresponding software encoder. Legacy implementation-specific values remain accepted and are folded into their codec family. GIF, APNG, and WebP use fixed FFmpeg image-animation encoders. |
| `record_transparent_codec` | string | `auto` | Default recording codec shown when `window_background = "transparent"`. `auto` probes a tiny FFmpeg encode/decode sample and uses a hardware alpha encoder only when it actually preserves alpha; otherwise it falls back to CPU VP9/FFV1 and shows a warning. |
| `record_solid_alpha` | bool | `false` | For window recordings with `window_background` set to `"follow-system"`, `"white"`, or `"black"`, keep alpha outside the window content when the selected format/codec supports transparency. This uses the same edge behavior as screenshot output and falls back to opaque recording when unsupported. |
| `record_preset` | string | `veryfast` | FFmpeg preset used with `libx264`/`libx264rgb`. |
| `record_gsr_flags` | string | empty | Extra default flags passed to `gpu-screen-recorder` for fullscreen and region recordings. `-w` and `-o` are rejected because HyprCapture owns the capture target and output path. If defaults conflict with overlay-controlled format, codec, FPS, cursor, target, or output settings, the overlay settings are appended later and take precedence. |
| `record_window_backend` | string | `auto` | Recording backend. `auto` uses compositor for windows and GSR for ordinary fullscreen/region video. `compositor` prefers compositor for every mode; `gsr-visible` prefers GSR, including a visible window region. Compatible startup failures try the other backend. Virtual outputs, transparent recordings, GIF, APNG, and WebP always use compositor. |
| `record_max_seconds` | int | `0` | Optional automatic stop in seconds. `0` means no duration limit for normal video formats. GIF, APNG, and WebP require one of `3`, `5`, `10`, `15`, or `30` seconds in the overlay and fall back to `5` when configured otherwise. |
| `record_countdown_seconds` | int | `0` | Optional countdown before recording starts. `0` disables it; values are clamped to 60 seconds. When enabled, HyprCapture closes the capture overlay, shows an input-transparent countdown window centered on the active screen, then starts recording. |
| `thumbnail_timeout_ms` | int | `5000` | Thumbnail auto-close timeout in milliseconds. Use `0` to keep it open until user action. |
| `thumbnail_monitor` | string | `active` | Monitor used for result thumbnails. Supports `active`, `primary`, `all`, or a case-insensitive output name such as `DP-2`. `active` is resolved from Hyprland's pointer position before the helper starts. `all` shows one synchronized thumbnail per monitor; swipe progress and swipe-to-close/delete animations are shared, while right-click menus remain independent. Unknown names fall back to the active monitor. |
| `helper` | string | empty | Optional absolute helper override. By default the plugin tries `HYPRCAPTURE_HELPER`, then `$HOME/.local/bin/hyprcapture-ui`, then trusted system install paths. |

For 60 fps, prefer hardware encoding:

```lua
hl.config({
    plugin = {
        hyprcapture = {
            record_fps = 60,
            record_codec = "auto",
        },
    },
})
```

`auto` selects H.264. For compositor recording, HyprCapture probes NVENC and VAAPI with the actual output dimensions and automatically falls back to the software encoder when hardware encoding is unavailable or rejects that size. For alpha-preserving window recordings, use `webm`/VP9, `mkv`/FFV1, APNG, or WebP; `mp4` is blocked by the overlay when `window_background = "transparent"`. MOV/HEVC alpha exists in Apple's ecosystem, but this Linux FFmpeg path does not currently encode that alpha profile, so transparent MOV is also blocked. WebP animation uses `libwebp_anim` in lossy mode at quality 75.

The compositor recording path uses synchronous compositor readback. To avoid making Hyprland sluggish, window recordings are capped by `record_window_fps_limit` until the GPU-only encoder path lands. GIF, APNG, and WebP use the same compositor path for fullscreen and region captures, so keep area and FPS modest. For visible on-screen windows where 60 fps matters more than offscreen/occlusion-safe capture, set `record_window_backend = "gsr-visible"` and use a normal video format.

### Watermark options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `watermark` | string | empty | Disabled when empty. Set to a PNG or JPG/JPEG path, or use built-in `activate-linux`, `hypercam2`, or `hyprcam2`. External SVG files are ignored; transparency is preserved for PNG. |
| `watermark_position` | string | `central` | Supports `up-left`, `up-middle`, `up-right`, `left-middle`, `central`, `right-middle`, `down-left`, `down-middle`, and `down-right`. Common aliases like `center`, `top-center`, and `right-meddle` are accepted. |
| `watermark_width` | string | `20%` | Watermark width. Use pixels like `320` / `320px`, or screenshot-width percent like `18%`. |
| `watermark_offset` | string | `0 0` | X/Y offset from the selected position. Vec2-like values such as `12 -8`, `2% -4%`, or `12px, -8px` are accepted. Percent X is relative to screenshot width; percent Y is relative to screenshot height. |

## Development

Useful commands:

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
./build-cmake/hyprcapture-ui --help
```

Temporary compositor artifacts and thumbnail/clipboard scratch files are written under a per-user private runtime directory such as `/dev/shm/hyprcapture-$UID` when available, with `/tmp/hyprcapture-$UID` as fallback. The directory is forced to `0700`, scratch files are written as owner-only files, and compositor artifact files are removed by the helper after loading.

## Notes

- The repository includes a root [`hyprpm.toml`](hyprpm.toml) manifest, which is expected by `hyprpm`.
- `window_background = "real"` uses compositor-captured real background data when available and falls back to reconstructing from the frozen desktop snapshot.
- Recording applies window decoration cropping and solid/follow-system backgrounds in the compositor-side path. Since plugin-side recording cannot query the helper's Qt palette, `window_background = "follow-system"` uses the same light fallback color as the screenshot helper. `record_solid_alpha = true` lets follow-system/white/black window recordings keep transparent pixels outside the window content when the selected format/codec supports alpha. `window_background = "real"` uses live compositor background data for window recordings and is treated as opaque for recording-format validation. Transparent output requires an alpha-capable format such as `webm`/VP9 or `mkv`/FFV1; MP4/MOV H.264/H.265 output is intentionally rejected for transparent recordings.
- Do not run `hyprpm update` or reload the plugin while an active screenshot overlay is being used.
