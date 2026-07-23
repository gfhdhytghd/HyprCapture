#include "shared/config.hpp"
#include "shared/protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (condition)
        return;

    std::cerr << "config test failed: " << message << '\n';
    std::exit(1);
}

} // namespace

int main() {
    using namespace hyprcapture;

    require(parseCaptureMode("full") == CaptureMode::Fullscreen, "full mode parse");
    require(parseCaptureMode("selection") == CaptureMode::Region, "selection mode parse");
    require(parseCaptureMode("window") == CaptureMode::Window, "window mode parse");
    require(parseCaptureMode("bad", CaptureMode::Window) == CaptureMode::Window, "mode fallback");
    require(!CaptureDefaults{}.confirmBeforeCapture, "confirm before capture default");

    require(parseFullscreenScope("all-monitors") == FullscreenScope::All, "all monitor scope parse");
    require(parseFullscreenScope("current-monitor") == FullscreenScope::Current, "current monitor scope parse");
    require(parseFullscreenScope("per_monitor") == FullscreenScope::PerMonitor, "per monitor scope parse");

    require(parseWindowBackground("follow_system") == WindowBackground::FollowSystem, "follow system background parse");
    require(parseWindowBackground("transparent") == WindowBackground::Transparent, "transparent background parse");
    require(parseDecorationPolicy("strip") == DecorationPolicy::Remove, "decoration policy parse");
    require(parseRecordWindowBackend("visible_gsr") == RecordWindowBackend::GsrVisible, "visible gsr backend parse");
    require(parseNotificationBackend("libnotify") == NotificationBackend::System, "libnotify notification backend alias");
    require(parseNotificationBackend("dbus") == NotificationBackend::System, "dbus notification backend alias");
    require(parseNotificationBackend("bad", NotificationBackend::System) == NotificationBackend::System, "notification backend fallback");
    require(normalizeRecordFormat("animated_webp") == "mp4", "unknown record format fallback");
    require(normalizeRecordFormat("matroska") == "mkv", "matroska record format parse");
    require(normalizeRecordFormat("gif") == "gif", "gif record format parse");
    require(normalizeRecordFormat("apng") == "apng", "apng record format parse");
    require(normalizeRecordFormat("webp") == "webp", "webp record format parse");
    require(recordFormatIsImageAnimation("gif"), "gif animation format detection");
    require(recordFormatIsImageAnimation("apng"), "apng animation format detection");
    require(recordFormatIsImageAnimation("webp"), "webp animation format detection");
    require(!recordFormatIsImageAnimation("webm"), "webm is not image animation format");
    require(parseWatermarkPosition("central") == WatermarkPosition::Central, "central watermark position parse");
    require(parseWatermarkPosition("right-meddle") == WatermarkPosition::RightMiddle, "legacy watermark position alias");
    require(parseWatermarkPosition("top_center") == WatermarkPosition::UpMiddle, "top center watermark position parse");
    require(parseWatermarkPosition("bad", WatermarkPosition::DownRight) == WatermarkPosition::DownRight, "watermark position fallback");

    require(toString(CaptureMode::Fullscreen) == "fullscreen", "fullscreen stringify");
    require(toString(FullscreenScope::PerMonitor) == "per-monitor", "per monitor stringify");
    require(toString(WindowBackground::FollowSystem) == "follow-system", "follow system stringify");
    require(toString(RecordWindowBackend::GsrVisible) == "gsr-visible", "visible gsr backend stringify");
    require(toString(NotificationBackend::System) == "system", "system notification backend stringify");
    require(toString(WatermarkPosition::DownMiddle) == "down-middle", "down middle stringify");

    const auto expanded = expandUserPath("~/Pictures/Screenshots").string();
    require(expanded.find("Pictures/Screenshots") != std::string::npos, "home path expansion");
    const auto expandedPictures = expandUserPath("$XDG_PICTURES_DIR/Screenshots").string();
    require(expandedPictures.find("Screenshots") != std::string::npos, "xdg pictures path expansion");
    const auto expandedVideos = expandUserPath("$XDG_VIDEOS_DIR/Screenrecords").string();
    require(expandedVideos.find("Screenrecords") != std::string::npos, "xdg videos path expansion");
    require(makeTimestampedFilename("Screenshot-%Y.png").ends_with(".png"), "timestamp filename suffix");
    require(makeTimestampedFilename("../escape.png") == "escape.png", "filename basename clamp");
    require(makeTimestampedFilename("..") == "Screenshot.png", "invalid filename fallback");
    require(sanitizeFilenameVariable("  Firefox / Private: window?  ") == "Firefox-Private-window", "filename variable sanitization");
    require(sanitizeFilenameVariable("../../") == "unknown", "empty filename variable fallback");
    require(makeTimestampedFilename("Shot-%w-{window_class}-{window_title}.png", "org.mozilla/firefox", "Private: Window")
                .find("org.mozilla-firefox-Private-Window.png") != std::string::npos,
            "window filename template variables");

    CaptureSession session;
    session.id = "test-session";
    session.defaults.mode = CaptureMode::Window;
    session.defaults.allowQuick = true;
    session.defaults.confirmBeforeCapture = true;
    session.defaults.fushionMode = true;
    session.defaults.captureFullscreenClientsAsMonitor = true;
    session.defaults.thumbnailMonitor = "DP-2";
    session.defaults.windowBackground = WindowBackground::FollowSystem;
    session.defaults.recordTransparentFormat = "webm";
    session.defaults.recordTransparentCodec = "auto";
    session.defaults.recordSolidAlpha = true;
    session.defaults.recordSaveDir = "$XDG_VIDEOS_DIR/Screenrecords";
    session.defaults.watermark = "activate-linux";
    session.defaults.watermarkPosition = WatermarkPosition::RightMiddle;
    session.defaults.watermarkWidth = "18%";
    session.defaults.watermarkOffset = "-2% 24px";
    session.cursorPosition = Point{.x = 120, .y = 240};
    session.monitors.push_back({.name = "eDP-1", .logicalGeometry = {.x = 0, .y = 0, .width = 1920, .height = 1080}, .scale = 2.0, .transform = 0, .focused = true});
    session.monitors.back().artifactPath = "/tmp/monitor.rgba";
    session.monitors.back().artifactWidth = 3840;
    session.monitors.back().artifactHeight = 2160;
    session.windows.push_back({.address = "0x1",
                               .title = "Title",
                               .appClass = "Class",
                               .focused = true,
                               .fullscreen = true,
                               .visibleGeometry = {.x = 10, .y = 10, .width = 100, .height = 100},
                               .fullGeometry = {.x = -40, .y = 10, .width = 200, .height = 100},
                               .rounding = 12,
                               .roundingPower = 2.5,
                               .borderSize = 2,
                               .zIndex = 1,
                               .selectable = true});
    session.windows.back().artifactPath = "/tmp/window.rgba";
    session.windows.back().artifactWidth = 200;
    session.windows.back().artifactHeight = 100;
    session.windows.back().selectionGeometry = Rect{.x = 30, .y = 40, .width = 120, .height = 80};
    session.windows.back().realBackgroundPath = "/tmp/window-real.rgba";
    session.windows.back().realBackgroundWidth = 200;
    session.windows.back().realBackgroundHeight = 100;
    session.windows.back().title = std::string("Title") + '\x01';
    const auto json = encodeSessionJson(session);
    require(json.find("\"fushionMode\":true") != std::string::npos, "fushion mode json");
    require(json.find("\"allowQuick\":true") != std::string::npos, "allow quick json");
    require(json.find("\"confirmBeforeCapture\":true") != std::string::npos, "confirm before capture json");
    require(json.find("\"captureFullscreenClientsAsMonitor\":true") != std::string::npos, "fullscreen client behavior json");
    require(json.find("\"thumbnailMonitor\":\"DP-2\"") != std::string::npos, "thumbnail monitor json");
    require(json.find("\"windowBackground\":\"follow-system\"") != std::string::npos, "window background json");
    require(json.find("\"recordTransparentFormat\":\"webm\"") != std::string::npos, "transparent record format json");
    require(json.find("\"recordTransparentCodec\":\"auto\"") != std::string::npos, "transparent record codec json");
    require(json.find("\"recordSolidAlpha\":true") != std::string::npos, "solid alpha record json");
    require(json.find("\"recordSaveDir\":\"$XDG_VIDEOS_DIR/Screenrecords\"") != std::string::npos, "record save dir json");
    require(json.find("\"watermark\":\"activate-linux\"") != std::string::npos, "watermark json");
    require(json.find("\"watermarkPosition\":\"right-middle\"") != std::string::npos, "watermark position json");
    require(json.find("\"watermarkWidth\":\"18%\"") != std::string::npos, "watermark width json");
    require(json.find("\"watermarkOffset\":\"-2% 24px\"") != std::string::npos, "watermark offset json");
    require(json.find("\"cursorPosition\"") != std::string::npos, "cursor position json");
    require(json.find("\"fullGeometry\"") != std::string::npos, "full geometry json");
    require(json.find("\"fullscreen\":true") != std::string::npos, "fullscreen window json");
    require(json.find("\"selectionGeometry\"") != std::string::npos, "selection geometry json");
    require(json.find("\"rounding\":12") != std::string::npos, "rounding json");
    require(json.find("\"roundingPower\":2.5") != std::string::npos, "rounding power json");
    require(json.find("\"borderSize\":2") != std::string::npos, "border size json");
    require(json.find("\"artifactPath\":\"/tmp/window.rgba\"") != std::string::npos, "artifact path json");
    require(json.find("\"focused\":true") != std::string::npos, "focused monitor json");
    require(json.find("\"artifactTopDown\":true") != std::string::npos, "artifact orientation json");
    require(json.find("\"realBackgroundPath\":\"/tmp/window-real.rgba\"") != std::string::npos, "real background path json");
    require(json.find("\"realBackgroundWidth\":200") != std::string::npos, "real background width json");
    require(json.find("Title\\u0001") != std::string::npos, "control byte json escaping");
    const auto decoded = decodeSessionJson(json);
    require(decoded.has_value(), "encoded session decodes");
    require(decoded->id == "test-session", "decoded id");
    require(decoded->defaults.mode == CaptureMode::Window, "decoded mode");
    require(decoded->defaults.allowQuick, "decoded allow quick");
    require(decoded->defaults.confirmBeforeCapture, "decoded confirm before capture");
    require(decoded->defaults.captureFullscreenClientsAsMonitor, "decoded fullscreen client behavior");
    require(decoded->defaults.thumbnailMonitor == "DP-2", "decoded thumbnail monitor");
    require(decoded->defaults.fushionMode, "decoded fushion mode");
    require(decoded->defaults.recordTransparentFormat == "webm", "decoded transparent record format");
    require(decoded->defaults.recordTransparentCodec == "auto", "decoded transparent record codec");
    require(decoded->defaults.recordSolidAlpha, "decoded solid alpha record");
    require(decoded->defaults.recordSaveDir == "$XDG_VIDEOS_DIR/Screenrecords", "decoded record save dir");
    require(decoded->cursorPosition.has_value() && decoded->cursorPosition->x == 120 && decoded->cursorPosition->y == 240, "decoded cursor position");
    require(decoded->monitors.size() == 1 && decoded->windows.size() == 1, "decoded object counts");
    require(decoded->monitors.front().focused, "decoded focused monitor");
    require(decoded->windows.front().artifactPath == "/tmp/window.rgba", "decoded artifact path");
    require(decoded->windows.front().selectionGeometry.has_value(), "decoded selection geometry exists");
    require(decoded->windows.front().focused && decoded->windows.front().fullscreen, "decoded window state");
    require(decoded->windows.front().selectionGeometry->x == 30 && decoded->windows.front().selectionGeometry->width == 120, "decoded selection geometry values");

    CaptureSession legacySession = session;
    legacySession.windows.front().selectionGeometry.reset();
    const auto legacyJson = encodeSessionJson(legacySession);
    require(legacyJson.find("\"selectionGeometry\"") == std::string::npos, "missing optional selection geometry omitted");
    const auto legacyDecoded = decodeSessionJson(legacyJson);
    require(legacyDecoded.has_value(), "session without selection geometry decodes");
    require(!legacyDecoded->windows.front().selectionGeometry.has_value(), "missing optional selection geometry stays empty");

    RecordingRequest recording;
    recording.id = "recording-request";
    recording.defaults = session.defaults;
    recording.defaults.recordFps = 60;
    recording.defaults.recordWindowFpsLimit = 12;
    recording.defaults.recordWindowRealBgFpsLimit = 8;
    recording.defaults.recordGsrFlags = "-k h264 -q very_high";
    recording.defaults.recordWindowBackend = RecordWindowBackend::GsrVisible;
    recording.defaults.recordFilenameTemplate = "Recording-%Y.mp4";
    recording.defaults.recordTransparentFormat = "mkv";
    recording.defaults.recordTransparentCodec = "ffv1";
    recording.defaults.recordMaxSeconds = 10;
    recording.defaults.recordCountdownSeconds = 3;
    recording.mode = CaptureMode::Window;
    recording.targetGeometry = {.x = 10, .y = 20, .width = 640, .height = 480};
    recording.windowAddress = "0x1";
    const auto recordingJson = encodeRecordingRequestJson(recording);
    require(recordingJson.find("\"recordFps\":60") != std::string::npos, "record fps json");
    require(recordingJson.find("\"recordWindowFpsLimit\":12") != std::string::npos, "record window fps limit json");
    require(recordingJson.find("\"recordWindowRealBgFpsLimit\":8") != std::string::npos, "record window real bg fps limit json");
    require(recordingJson.find("\"recordGsrFlags\":\"-k h264 -q very_high\"") != std::string::npos, "record gsr flags json");
    require(recordingJson.find("\"recordTransparentFormat\":\"mkv\"") != std::string::npos, "record transparent format json");
    require(recordingJson.find("\"recordTransparentCodec\":\"ffv1\"") != std::string::npos, "record transparent codec json");
    require(recordingJson.find("\"recordMaxSeconds\":10") != std::string::npos, "record max seconds json");
    require(recordingJson.find("\"recordCountdownSeconds\":3") != std::string::npos, "record countdown seconds json");
    require(recordingJson.find("\"recordWindowBackend\":\"gsr-visible\"") != std::string::npos, "record window backend json");
    require(recordingJson.find("\"recordFilenameTemplate\":\"Recording-%Y.mp4\"") != std::string::npos, "record filename json");
    const auto decodedRecording = decodeRecordingRequestJson(recordingJson);
    require(decodedRecording.has_value(), "encoded recording request decodes");
    require(decodedRecording->mode == CaptureMode::Window, "decoded recording mode");
    require(decodedRecording->windowAddress == "0x1", "decoded recording window address");
    require(decodedRecording->defaults.recordFps == 60, "decoded recording fps");
    require(decodedRecording->defaults.recordWindowFpsLimit == 12, "decoded recording window fps limit");
    require(decodedRecording->defaults.recordWindowRealBgFpsLimit == 8, "decoded recording window real bg fps limit");
    require(decodedRecording->defaults.recordGsrFlags == "-k h264 -q very_high", "decoded recording gsr flags");
    require(decodedRecording->defaults.recordTransparentFormat == "mkv", "decoded recording transparent format");
    require(decodedRecording->defaults.recordTransparentCodec == "ffv1", "decoded recording transparent codec");
    require(decodedRecording->defaults.recordMaxSeconds == 10, "decoded recording max seconds");
    require(decodedRecording->defaults.recordCountdownSeconds == 3, "decoded recording countdown seconds");
    require(decodedRecording->defaults.recordWindowBackend == RecordWindowBackend::GsrVisible, "decoded recording window backend");
    require(!decodeRecordingRequestJson("{}").has_value(), "missing recording request fields rejected");

    require(!decodeSessionJson("{not json").has_value(), "malformed json is rejected");
    require(!decodeSessionJson("{}").has_value(), "missing required protocol fields rejected");

    session.windows.front().fullGeometry.width = std::numeric_limits<double>::infinity();
    const auto finiteJson = encodeSessionJson(session);
    require(finiteJson.find("inf") == std::string::npos && finiteJson.find("nan") == std::string::npos, "non-finite values not serialized");

    session.windows.front().fullGeometry.width = 200;
    session.windows.front().title = std::string("bad utf8 ") + static_cast<char>(0xff);
    require(decodeSessionJson(encodeSessionJson(session)).has_value(), "invalid utf8 metadata is replaced during encoding");

    std::cout << "hyprcapture config tests passed\n";
    return 0;
}
