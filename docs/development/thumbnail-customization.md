# Result thumbnail constants (before customization)

Scope: `src/ui/result_thumbnail.{hpp,cpp}`, `thumbnail_style.hpp` and the
`ResultThumbnailCollection` callers in `main.cpp`. Capture/editor toolbars and
the in-progress scroll-capture controls are separate UI and are not customized.

| Area | Existing constants / behaviour | Decision |
| --- | --- | --- |
| Preview | 180 x 120 logical-pixel maximum; preserve aspect ratio; no enlargement; ceil logical dimensions; smooth resampling | Configurable maximum box |
| Preview transport | Screenshot preview cap and image decode cap 720 x 480; first recording frame decoded at 720 x 480 and centered on an 180 x 120 canvas | Caps expand to at least custom box x 4; recording canvas follows custom box |
| Recording placeholder | Max screen DPR 1..4; antialiasing; half-pixel inset, border 1 at RGBA 235,238,242,95; fill 28,31,36,238; radius 8; play glyph 235,238,242,235 with offsets -13,-18 / -13,18 / 20,0 | Retain placeholder artwork; canvas follows custom maximum |
| Pixel density | Source/target screen DPR, bounded to 1..4 and source resolution | Retain quality policy |
| Placement | Bottom-right overlay, zero layer margins; card adds 24 logical pixels on right and bottom | Configurable corner and card edge margin; retain exact default layout |
| Surface | Frameless, no focus/activation, overlay layer, no exclusive zone, transparent root/image | Retain shell/input behaviour |
| Opacity | Fully opaque image; palette-derived menu alpha below | Configurable overall opacity, percent |
| Image corners/border/shadow | Square image; no border; no shadow; native HDR child surface | `thumbnail_border_size` (default 0) and `thumbnail_border_color` add a frame outside the full-size image; `thumbnail_rounding` (default 0) is the image radius R, and a border of width b has outer radius R+b; HDR surface is inset by the border width, and rounding > 0 retains native HDR with an ARGB2101010 corner mask when supported, otherwise uses the SDR preview; no shadow key |
| Menu panels | Qt Window RGB at alpha 242; 1 px WindowText border at alpha 90; radius 7 | Changed: no border, square, palette colors; not configurable |
| Menu buttons | WindowText RGB at alpha 255; disabled alpha 100; hover Highlight RGB at alpha 75; radius 5; padding 7 vertical / 10 horizontal; left text | Fixed palette colors and square buttons; retain padding/text alignment |
| Layout | Main/menu-shell spacing 6; menu/action-row margins 6 each; menu/action-row spacing 2; right alignment; open-with panels bottom-aligned | Retain spacing; align to configured corner |
| Drag | SDR preview icon capped at 180 x 120 physical pixels; centered hotspot; platform drag threshold | Preview cap follows maximum box; retain gesture policy |
| Swipe | Positive x closes, positive y deletes; thresholds min(120 / 90, image dimension x 0.55); overshoot 80; wheel angle divisor 8 | Retain gesture policy regardless of corner |
| Swipe animation | Reset idle delay 180 ms; reset 160 ms OutCubic; exit 190 ms InCubic; exit overshoot 24 | Configurable animation enable; retain gesture delay/curves/distances |
| Delete icon | Idle RGBA 92,94,98,130 to danger 220,38,38,245; pen 3; size scale min(width,height)/120; body 34 x 42; radius 4; offsets 5,7,9,14,6,10,8 | Retain semantic artwork |
| Transcode indicator | Ring max 64; available min 32 and image dimension minus 20; pen clamp(side/13,4,9); bold font clamp(side/5,12,28); rotation 8 degrees/33 ms; spinner arc 62 degrees | Retain progress artwork/animation |
| Transcode colors | Scrim 0,0,0,105; track 255,255,255,72; progress 59,130,246,235; spinner 147,197,253,245; failure 248,113,113,235; text 255,255,255,245 | Retain semantic status colors |
| Other | At most 16 open-with apps; HDR reference 203 nits; 5000 ms error timeout fallback; existing configurable timeout 5000 ms and monitor active | Retain application/format policy |

Defaults preserve the original image stylesheet, pixel scaling and layout. Result
thumbnail menus deliberately changed to square and borderless; the in-progress
scroll-capture menu keeps the original 1 px border and 7 px radius.
The original scaler first fits logical dimensions to the maximum box, then fits
the pixmap to the rounded physical dimensions. For a 1280 x 800 image at DPR 1,
these two `KeepAspectRatio` resamples produce 179 x 112 pixels, not 180 x 112.
The default card is therefore 203 x 136, including its 24 px padding. The tests
compare against the frozen original algorithm and preserve this rounding.
Non-default overall opacity uses Qt raster composition. Native HDR presentation
does not apply overall Qt opacity, so translucent thumbnails use their existing SDR preview.
Rounded HDR thumbnails bake an antialiased premultiplied ARGB2101010 mask into
the PQ image. Its 2-bit alpha has four levels, so corners have limited smoothness.
The image retains its full scaled size. A border of width b grows outward at a
constant distance: the label adds 2b on each axis, its contents margins are b,
and the Qt ring runs from outer radius R+b to inner image radius R. The HDR
surface covers the label rectangle inset by b and uses mask radius R (not R+b).
The mask maps logical pixels through the inner destination size to source
pixels, including HiDPI scaling. Transparent corners
are not declared opaque. Missing ARGB2101010 support or upload failure retains
the rounded SDR preview; radius zero retains opaque XRGB2101010.
Saved files, clipboard content and drag file representations retain their formats.

## Transport and reload coverage

`registerConfigValues()` and `readDefaults()` in `plugin/main.cpp` own all keys.
`restoreConfigRegistrations()` uses `config_reload.hpp` to register the whole
option set again when the config manager is replaced. No cached pointers or
thumbnail-specific reload hook is introduced. Existing thumbnail windows keep
their launch-time settings.

Flat `CaptureDefaults` fields travel in session JSON and explicit helper arguments:
normal capture, recording result, recording transcode, and `CaptureOverlay`'s
thumbnail subprocess. Every `ResultThumbnailCollection` caller passes them,
including pending recording previews. Final scrolling captures use the normal
output path. Pin helpers display a separate desktop image, have no result
thumbnail options, and are intentionally unaffected.

Protocol encoders clamp numeric values; decoders reject out-of-bounds values and
wrong types. Color inputs are limited to 64 bytes at the plugin, protocol and UI
boundaries; supported spellings are validated and trimmed to at most 14 bytes
before argv/JSON construction. Old sessions with absent fields retain defaults. UI argument parsing
clamps numeric values again, defaults malformed values/corners, bounds strings,
and converts supported colors to numeric CSS values without CSS interpolation.

Quick actions always appear below the image. Top anchors keep the image at the
configured top margin while actions grow inward. Bottom anchors preserve the
original image/card/actions order, including the action row and spacing in the
image's distance from the bottom edge (71 px at defaults with `save=false`).
Native HDR pixels are clipped to the card, matching SDR clipping during swipes. For left anchors the
open-with submenu grows to the right of the main menu; right anchors retain the
original submenu placement on the left.
