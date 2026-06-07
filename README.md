# HyprCapture

HyprCapture is a Hyprland-only screenshot tool split into a compositor plugin and a Qt layer-shell helper. The plugin captures frozen compositor artifacts and launches the helper; the helper provides the selection overlay, output rendering, clipboard integration, and result thumbnail.

> [!IMPORTANT]
> `hyprpm` builds the compositor plugin and installs the helper to `~/.local/bin/hyprcapture-ui`. Set `plugin:hyprcapture:helper` only when you want to override that default helper path.

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

```conf
permission = /usr/(bin|local/bin)/hyprpm, plugin, allow
```

Do not also manually `hyprctl plugin load` the same `.so` if you manage it through `hyprpm`.

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

Only configure `plugin:hyprcapture:helper` if you want to use a custom helper path:

```conf
plugin {
    hyprcapture {
        helper = /home/you/.local/bin/hyprcapture-ui
    }
}
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

For the Hyprland 0.55 preview checkout, point CMake at the matching source tree so the plugin is built against the new Lua-config-capable headers:

```sh
cmake -S . -B build-v055 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHYPRLAND_SOURCE_DIR="$HOME/data/Hyprland"
cmake --build build-v055
ctest --test-dir build-v055 --output-on-failure
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

### Dispatchers

```conf
bind = SUPER SHIFT, S, hyprcapture:open
bind = SUPER SHIFT, W, hyprcapture:open,window
bind = SUPER SHIFT, F, hyprcapture:open,fullscreen
```

| Dispatcher | Description |
| --- | --- |
| `hyprcapture:open` | Open the overlay using `default_mode`. |
| `hyprcapture:open,<mode>` | Open the overlay in `region`, `fullscreen`, or `window` mode. |
| `hyprcapture:quick` | Capture immediately using `default_mode`; disabled unless `allow_quick = 1`. |
| `hyprcapture:quick,<mode>` | Capture immediately in `region`, `fullscreen`, or `window` mode; disabled unless `allow_quick = 1`. |
| `hyprcapture:cancel` | Reserved dispatcher; currently returns success without changing an active helper. |

When Hyprland is running with Lua config, the plugin also exposes matching functions under `hl.plugin.hyprcapture`:

```lua
hl.bind("SUPER + s", function()
    hl.plugin.hyprcapture.open()
end)

hl.bind("SUPER + SHIFT + S", function()
    hl.plugin.hyprcapture.open("window")
end)
```

Available Lua functions are `open`, `quick`, `record`, `record_toggle`, `record_stop`, `record_start`, `cancel`, and `dispatch`.
`dispatch` accepts the dispatcher name plus an optional argument, for example `hl.plugin.hyprcapture.dispatch("open", "fullscreen")`.
The internal helper uses `record_start_dispatcher` and `record_stop_dispatcher` so `hyprctl dispatch` can call back into the plugin under Lua config.

Use lowercase `s` for `SUPER + s`. In Lua config key strings, uppercase `S` means Shift is part of the binding.

### Overlay

- Region mode: drag a rectangle, then release or press Enter.
- Fullscreen mode: captures according to `fullscreen_scope`.
- Window mode: hover a window and press Enter or click it.
- Fusion mode: the toolbar keeps the fullscreen action and configuration controls; drag anywhere to capture a region, or single-click a window to capture that window.
- Esc cancels the helper.
- The toolbar is anchored near the bottom of the screen and only shows controls relevant to the active mode.

### Recording

Recording is toggled from the normal screenshot overlay toolbar. Click the record icon, choose an output format, then choose fullscreen, drag a region, or choose a window exactly like a screenshot. Fullscreen and region video recordings are handed to `gpu-screen-recorder`; GIF, APNG, WebP, and the default window backend use HyprCapture's compositor renderer. Window recordings also have an optional visible-region `gpu-screen-recorder` backend for normal video formats.

To stop an active recording, open the same overlay and click the checked record icon.

Current recording output is a single file under `record_save_dir`. Fullscreen and region video recordings require `gpu-screen-recorder` and avoid Hyprland's screencopy, portal, and screenshare session paths. GIF and animated WebP use FFmpeg rawvideo input from compositor RGBA readback for all capture modes. APNG records a hidden 60 fps MKV intermediate first, then the helper transcodes it to APNG while showing the thumbnail with progress when thumbnails are enabled. Animation formats require a fixed duration of 3, 5, 10, 15, or 30 seconds and default to 5 seconds. `record_window_backend = gsr-visible` records the selected on-screen window rectangle through `gpu-screen-recorder` for lower overhead on normal video formats, without portal or managed screenshare sessions, but it captures what is visibly present in that screen region. Finished recordings use the same `clipboard` and `show_thumbnail` settings as screenshots; clipboard output is a local file URI.

### Thumbnail

The thumbnail appears after capture when `show_thumbnail = 1`.

- Left click opens the saved image.
- Drag starts a file drag for targets that accept image files.
- Right click opens the thumbnail menu.
- Swipe right with a touchpad or two-dimensional wheel to close.
- Swipe down to delete the file and restore the clipboard snapshot captured before the screenshot.

## Configuration

All user-facing settings live under `plugin:hyprcapture`.

Example:

```conf
plugin {
    hyprcapture {
        default_mode = region
        fullscreen_scope = all
        window_background = follow-system
        window_border = keep
        window_shadow = keep
        save = 1
        clipboard = 1
        show_thumbnail = 1
        allow_quick = 0
        confirm_before_capture = 0
        fusion_mode = 0
        save_dir = $XDG_PICTURES_DIR/Screenshots
        filename_template = Screenshot-%Y-%m-%d-%H%M%S.png
        record_save_dir = $XDG_VIDEOS_DIR/Screenrecords
        record_filename_template = Recording-%Y-%m-%d-%H%M%S.mp4
        record_format = mp4
        record_transparent_format = webm
        record_fps = 30
        record_fps_options = 15 24 30 60
        record_window_fps_limit = 12
        record_window_real_bg_fps_limit = 8
        record_codec = libx264
        record_transparent_codec = auto
        record_solid_alpha = 0
        record_preset = veryfast
        record_gsr_flags =
        record_window_backend = compositor
        record_max_seconds = 0
        record_countdown_seconds = 0
        include_cursor = 0
        thumbnail_timeout_ms = 5000
        watermark =
        watermark_position = central
        watermark_width = 20%
        watermark_offset = 0 0
    }
}
```

Lua config example:

```lua
hl.config({
    plugin = {
        hyprcapture = {
            default_mode = "region",
            fullscreen_scope = "all",
            window_background = "follow-system",
            window_border = "keep",
            window_shadow = "keep",
            save = true,
            clipboard = true,
            show_thumbnail = true,
            allow_quick = false,
            confirm_before_capture = false,
            fusion_mode = false,
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
            record_codec = "libx264",
            record_transparent_codec = "auto",
            record_solid_alpha = false,
            record_preset = "veryfast",
            record_gsr_flags = "",
            record_window_backend = "compositor",
            record_max_seconds = 0,
            record_countdown_seconds = 0,
            include_cursor = false,
            thumbnail_timeout_ms = 5000,
            watermark = "",
            watermark_position = "central",
            watermark_width = "20%",
            watermark_offset = "0 0",
        },
    },
})
```

### Lua Config Guide

Use `hl.config` for settings and `hl.plugin.hyprcapture` for actions:

```lua
hl.config({
    plugin = {
        hyprcapture = {
            default_mode = "region",
            fusion_mode = true,
            confirm_before_capture = false,
            fullscreen_scope = "all",
            window_background = "follow-system",
            save = true,
            clipboard = true,
            show_thumbnail = true,
            helper = "/home/you/.local/bin/hyprcapture-ui",
        },
    },
})

hl.bind("SUPER + s", function()
    hl.plugin.hyprcapture.open()
end)

hl.bind("SUPER + SHIFT + S", function()
    hl.plugin.hyprcapture.open("window")
end)
```

Do not use `hyprctl dispatch hyprcapture:open` as a Lua fallback. Hyprland's Lua config dispatcher parser treats `hyprcapture:open` as Lua syntax. Use `hl.plugin.hyprcapture.open()` once the plugin is loaded, or bind a direct helper command during development while testing an older loaded plugin.

The old misspelled `fushion_mode` key is still accepted as a compatibility alias, but new configs should use `fusion_mode`.

### Capture options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `default_mode` | string | `region` | Default mode for `hyprcapture:open` and `hyprcapture:quick`. Supports `region`, `fullscreen`, and `window`. |
| `fullscreen_scope` | string | `all` | Fullscreen capture scope. Supports `all`, `current`, and `per-monitor`. |
| `window_background` | string | `follow-system` | Background behind transparent window pixels. Supports `follow-system`, `white`, `black`, `real`, and `transparent`. |
| `window_border` | string | `keep` | Window border policy. Supports `keep` and `remove`. |
| `window_shadow` | string | `keep` | Window shadow policy. Supports `keep` and `remove`. Transparent window recordings keep shadows and normalize the alpha falloff so the shadow fades out instead of encoding as a hard border. |
| `include_cursor` | bool | `0` | Parsed and forwarded by the plugin/helper; cursor compositing is not currently rendered into the output. |
| `allow_quick` | bool | `0` | Enable no-confirmation `hyprcapture:quick` dispatchers. Leave disabled unless your Hyprland IPC policy already restricts untrusted same-user clients. |
| `confirm_before_capture` | bool | `0` | For `hyprcapture:open`, require an explicit confirmation after choosing a fullscreen, region, or window target. Region targets can be moved or resized; window targets can be switched before confirming. `hyprcapture:quick` and direct `hyprcapture:record` keep their existing no-extra-confirmation behavior. |
| `fusion_mode` | bool | `0` | Fuse region and window interactions in one overlay: drag to capture a region, or single-click a window to capture that window. The toolbar keeps the fullscreen action and configuration controls; fullscreen multi-monitor scope is shown only when multiple monitors are present. |
| `fushion_mode` | bool | `0` | Legacy compatibility alias for `fusion_mode`. New configs should use `fusion_mode`. |

### Output options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `save` | bool | `1` | Save the output image to `save_dir` as an owner-only file. |
| `clipboard` | bool | `1` | Copy the output image to the clipboard. Uses `wl-copy` when available so the clipboard survives helper exit. |
| `show_thumbnail` | bool | `1` | Show the result thumbnail after capture. |
| `save_dir` | string | `$XDG_PICTURES_DIR/Screenshots` | Output directory. `~` is expanded against `HOME`; `$XDG_PICTURES_DIR` is read from XDG user-dirs with `~/Pictures` as fallback. |
| `filename_template` | string | `Screenshot-%Y-%m-%d-%H%M%S.png` | `strftime` template for saved screenshot filenames. |
| `record_save_dir` | string | `$XDG_VIDEOS_DIR/Screenrecords` | Output directory for recordings. `$XDG_VIDEOS_DIR` is read from XDG user-dirs with `~/Videos` as fallback. Finished recordings can be copied to the clipboard as local file URIs and shown in the thumbnail when those global output settings are enabled. |
| `record_filename_template` | string | `Recording-%Y-%m-%d-%H%M%S.mp4` | `strftime` template for saved recording filenames. |
| `record_format` | string | `mp4` | Default recording format shown in the overlay for non-transparent window backgrounds, fullscreen recording, and region recording. Supports `mp4`, `mov`, `webm`, `mkv`, `gif`, `apng`, and `webp`; the selected value replaces the filename extension. |
| `record_transparent_format` | string | `webm` | Default recording container shown when `window_background = transparent`. |
| `record_fps` | int | `30` | Recording frame rate. Higher values increase compositor readback and encoder load. |
| `record_fps_options` | string | `15 24 30 60` | Whitespace, comma, or semicolon separated FPS choices shown in the overlay. The current `record_fps` value is added if it is not already listed. |
| `record_window_fps_limit` | int | `12` | Safety cap for window recording with the current compositor-readback backend. Use `0` to disable the cap. |
| `record_window_real_bg_fps_limit` | int | `8` | Additional safety cap for window recording with `window_background = real`. Use `0` to disable the cap. |
| `record_codec` | string | `libx264` | Default recording codec shown in the overlay for normal video formats. Supports `auto`, `libx264`/`h264`, `h264_vaapi`, `libx265`/`h265`, `hevc_vaapi`/`h265_vaapi`, `libsvtav1`/`av1`, `av1_vaapi`, `libvpx-vp9`/`vp9`, `vp9_vaapi`, and `ffv1`. GIF, APNG, and WebP use fixed FFmpeg image-animation encoders. |
| `record_transparent_codec` | string | `auto` | Default recording codec shown when `window_background = transparent`. `auto` probes a tiny FFmpeg encode/decode sample and uses a hardware alpha encoder only when it actually preserves alpha; otherwise it falls back to CPU VP9/FFV1 and shows a warning. |
| `record_solid_alpha` | bool | `0` | For window recordings with `window_background = follow-system`, `white`, or `black`, keep alpha outside the window content when the selected format/codec supports transparency. This uses the same edge behavior as screenshot output and falls back to opaque recording when unsupported. |
| `record_preset` | string | `veryfast` | FFmpeg preset used with `libx264`/`libx264rgb`. |
| `record_gsr_flags` | string | empty | Extra default flags passed to `gpu-screen-recorder` for fullscreen and region recordings. `-w` and `-o` are rejected because HyprCapture owns the capture target and output path. If defaults conflict with overlay-controlled format, codec, FPS, cursor, target, or output settings, the overlay settings are appended later and take precedence. |
| `record_window_backend` | string | `compositor` | Window recording backend. `compositor` preserves HyprCapture's offscreen window capture and background behavior. `gsr-visible` records the selected visible screen rectangle with `gpu-screen-recorder` for much lower overhead on normal video formats; occlusion/hidden-window capture and background replacement are not guaranteed. GIF, APNG, and WebP always use the compositor backend. |
| `record_max_seconds` | int | `0` | Optional automatic stop in seconds. `0` means no duration limit for normal video formats. GIF, APNG, and WebP require one of `3`, `5`, `10`, `15`, or `30` seconds in the overlay and fall back to `5` when configured otherwise. |
| `record_countdown_seconds` | int | `0` | Optional countdown before recording starts. `0` disables it; values are clamped to 60 seconds. When enabled, HyprCapture closes the capture overlay, shows an input-transparent countdown window centered on the active screen, then starts recording. |
| `thumbnail_timeout_ms` | int | `5000` | Thumbnail auto-close timeout in milliseconds. Use `0` to keep it open until user action. |
| `helper` | string | empty | Optional absolute helper override. By default the plugin tries `HYPRCAPTURE_HELPER`, then `$HOME/.local/bin/hyprcapture-ui`, then trusted system install paths. |

For 60 fps, prefer hardware encoding:

```conf
record_fps = 60
record_codec = auto
```

`auto` currently prefers VAAPI when a writable `/dev/dri/renderD*` device exists and falls back to `libx264` for the window-recording FFmpeg backend. For alpha-preserving window recordings, use `webm`/VP9, `mkv`/FFV1, APNG, or WebP; `mp4` is blocked by the overlay when `window_background = transparent`. MOV/HEVC alpha exists in Apple's ecosystem, but this Linux FFmpeg path does not currently encode that alpha profile, so transparent MOV is also blocked. WebP animation uses `libwebp_anim` in lossy mode at quality 75.

The compositor recording path uses synchronous compositor readback. To avoid making Hyprland sluggish, window recordings are capped by `record_window_fps_limit` until the GPU-only encoder path lands. GIF, APNG, and WebP use the same compositor path for fullscreen and region captures, so keep area and FPS modest. For visible on-screen windows where 60 fps matters more than offscreen/occlusion-safe capture, set `record_window_backend = gsr-visible` and use a normal video format.

### Watermark options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `watermark` | string | empty | Disabled when empty. Set to a PNG or JPG/JPEG path, or use built-in `activate-linux` / `hypercam2`. External SVG files are ignored; transparency is preserved for PNG. |
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
- `window_background = real` uses compositor-captured real background data when available and falls back to reconstructing from the frozen desktop snapshot.
- Recording applies window decoration cropping and solid/follow-system backgrounds in the compositor-side path. Since plugin-side recording cannot query the helper's Qt palette, `window_background = follow-system` uses the same light fallback color as the screenshot helper. `record_solid_alpha = 1` lets follow-system/white/black window recordings keep transparent pixels outside the window content when the selected format/codec supports alpha. `window_background = real` uses live compositor background data for window recordings and is treated as opaque for recording-format validation. Transparent output requires an alpha-capable format such as `webm`/VP9 or `mkv`/FFV1; MP4/MOV H.264/H.265 output is intentionally rejected for transparent recordings.
- Do not run `hyprpm update` or reload the plugin while an active screenshot overlay is being used.
