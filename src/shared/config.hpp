#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace hyprcapture {

enum class CaptureMode { Fullscreen, Region, Window };
enum class FullscreenScope { All, Current, PerMonitor };
enum class OverlayScope { Fix, Focus, All };
enum class WindowBackground { White, Black, FollowSystem, Real, Transparent };
enum class DecorationPolicy { Keep, Remove };
enum class RecordWindowBackend { Compositor, GsrVisible };
enum class NotificationBackend { Hyprland, System };
enum class WatermarkPosition { UpLeft, UpMiddle, UpRight, LeftMiddle, Central, RightMiddle, DownLeft, DownMiddle, DownRight };

struct CaptureDefaults {
    CaptureMode      mode = CaptureMode::Region;
    FullscreenScope  fullscreenScope = FullscreenScope::All;
    OverlayScope     overlayScope = OverlayScope::Fix;
    WindowBackground windowBackground = WindowBackground::FollowSystem;
    DecorationPolicy windowBorder = DecorationPolicy::Keep;
    DecorationPolicy windowShadow = DecorationPolicy::Keep;
    bool             save = true;
    bool             clipboard = true;
    bool             showThumbnail = true;
    bool             includeCursor = false;
    bool             allowQuick = false;
    bool             confirmBeforeCapture = false;
    bool             fushionMode = false;
    bool             captureFullscreenClientsAsMonitor = false;
    bool             dynamicWindowMetadata = true;
    std::string      fullscreenPreviewRounding = "auto";
    std::string      saveDir = "$XDG_PICTURES_DIR/Screenshots";
    std::string      filenameTemplate = "Screenshot-%Y-%m-%d-%H%M%S.png";
    std::string      helper;
    std::string      recordSaveDir = "$XDG_VIDEOS_DIR/Screenrecords";
    std::string      recordFilenameTemplate = "Recording-%Y-%m-%d-%H%M%S.mp4";
    std::string      recordFormat = "mp4";
    std::string      recordTransparentFormat = "webm";
    std::string      recordCodec = "libx264";
    std::string      recordTransparentCodec = "auto";
    bool             recordSolidAlpha = false;
    std::string      recordPreset = "veryfast";
    std::string      recordGsrFlags;
    RecordWindowBackend recordWindowBackend = RecordWindowBackend::Compositor;
    std::int64_t     recordFps = 30;
    std::string      recordFpsOptions = "15 24 30 60";
    std::int64_t     recordWindowFpsLimit = 12;
    std::int64_t     recordWindowRealBgFpsLimit = 8;
    std::int64_t     recordMaxSeconds = 0;
    std::int64_t     recordCountdownSeconds = 0;
    std::int64_t     thumbnailTimeoutMs = 5000;
    std::string      thumbnailMonitor = "active";
    std::string      watermark;
    WatermarkPosition watermarkPosition = WatermarkPosition::Central;
    std::string      watermarkWidth = "20%";
    std::string      watermarkOffset = "0 0";
};

struct FilenameMetadata {
    std::string windowClass;
    std::string windowTitle;
};

CaptureMode parseCaptureMode(std::string_view value, CaptureMode fallback = CaptureMode::Region);
FullscreenScope parseFullscreenScope(std::string_view value, FullscreenScope fallback = FullscreenScope::All);
OverlayScope parseOverlayScope(std::string_view value, OverlayScope fallback = OverlayScope::Fix);
WindowBackground parseWindowBackground(std::string_view value, WindowBackground fallback = WindowBackground::FollowSystem);
DecorationPolicy parseDecorationPolicy(std::string_view value, DecorationPolicy fallback = DecorationPolicy::Keep);
RecordWindowBackend parseRecordWindowBackend(std::string_view value, RecordWindowBackend fallback = RecordWindowBackend::Compositor);
NotificationBackend parseNotificationBackend(std::string_view value, NotificationBackend fallback = NotificationBackend::Hyprland);
WatermarkPosition parseWatermarkPosition(std::string_view value, WatermarkPosition fallback = WatermarkPosition::Central);
std::string normalizeRecordFormat(std::string_view value);
bool recordFormatIsImageAnimation(std::string_view value);

std::string toString(CaptureMode value);
std::string toString(FullscreenScope value);
std::string toString(OverlayScope value);
std::string toString(WindowBackground value);
std::string toString(DecorationPolicy value);
std::string toString(RecordWindowBackend value);
std::string toString(NotificationBackend value);
std::string toString(WatermarkPosition value);

std::filesystem::path expandUserPath(std::string_view path);
std::string sanitizeFilenameVariable(std::string_view value);
FilenameMetadata resolveFilenameMetadata(bool dynamicWindowMetadata,
                                         CaptureMode mode,
                                         const FilenameMetadata& selectedWindow,
                                         const FilenameMetadata& legacyWindow,
                                         std::size_t fullscreenWindowCount,
                                         const FilenameMetadata& singleFullscreenWindow);
std::string makeTimestampedFilename(std::string_view filenameTemplate,
                                    std::string_view windowClass = {},
                                    std::string_view windowTitle = {});

} // namespace hyprcapture
