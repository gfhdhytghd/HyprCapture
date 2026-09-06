#include "shared/config.hpp"
#include "shared/audio_source.hpp"
#include "shared/protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (condition)
        return;

    std::cerr << "config test failed: " << message << '\n';
    std::exit(1);
}

void eraseJsonField(std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    auto begin = json.find(needle);
    require(begin != std::string::npos, "legacy json field exists");
    auto end = json.find(',', begin);
    const auto objectEnd = json.find('}', begin);
    if (end != std::string::npos && end < objectEnd) {
        json.erase(begin, end - begin + 1);
        return;
    }
    require(begin > 0 && json[begin - 1] == ',', "legacy final field has separator");
    json.erase(begin - 1, objectEnd - begin + 1);
}

void eraseDefaultsJsonField(std::string& json, const std::string& key) {
    auto parsed = nlohmann::ordered_json::parse(json);
    require(parsed.contains("defaults") && parsed["defaults"].is_object(), "legacy defaults object exists");
    require(parsed["defaults"].erase(key) == 1, "legacy defaults field exists");
    json = parsed.dump();
}

} // namespace

int main() {
    using namespace hyprcapture;

    require(parseCaptureMode("full") == CaptureMode::Fullscreen, "full mode parse");
    require(parseCaptureMode("selection") == CaptureMode::Region, "selection mode parse");
    require(parseCaptureMode("window") == CaptureMode::Window, "window mode parse");
    require(parseCaptureMode("bad", CaptureMode::Window) == CaptureMode::Window, "mode fallback");
    require(!CaptureDefaults{}.confirmBeforeCapture, "confirm before capture default");
    require(CaptureDefaults{}.dynamicWindowMetadata, "dynamic window metadata default");
    require(CaptureDefaults{}.windowWheelScroll, "window wheel scroll default");
    require(CaptureDefaults{}.windowWheelScope == WindowWheelScope::Workspace, "window wheel scope default");
    require(CaptureDefaults{}.screenshotNotification, "screenshot notification default");
    require(CaptureDefaults{}.notificationBackend == NotificationBackend::Hyprland, "notification backend default");
    require(CaptureDefaults{}.recordWindowBackend == RecordWindowBackend::Auto, "record backend default");
    require(CaptureDefaults{}.fullscreenPreviewRounding == "auto", "fullscreen preview rounding default");

    require(parseFullscreenScope("all-monitors") == FullscreenScope::All, "all monitor scope parse");
    require(parseFullscreenScope("current-monitor") == FullscreenScope::Current, "current monitor scope parse");
    require(parseFullscreenScope("per_monitor") == FullscreenScope::PerMonitor, "per monitor scope parse");
    require(parseOverlayScope("fix") == OverlayScope::Fix, "fixed overlay scope parse");
    require(parseOverlayScope("forcus") == OverlayScope::Focus, "legacy misspelled focus overlay scope parse");
    require(parseOverlayScope("all-monitors") == OverlayScope::All, "all monitor overlay scope parse");

    require(parseWindowBackground("follow_system") == WindowBackground::FollowSystem, "follow system background parse");
    require(parseWindowBackground("transparent") == WindowBackground::Transparent, "transparent background parse");
    require(parseDecorationPolicy("strip") == DecorationPolicy::Remove, "decoration policy parse");
    require(parseWindowWheelScope("workspace") == WindowWheelScope::Workspace, "workspace window wheel scope parse");
    require(parseWindowWheelScope("under_cursor") == WindowWheelScope::UnderCursor, "under cursor window wheel scope parse");
    require(parseWindowWheelScope("cursor") == WindowWheelScope::UnderCursor, "cursor window wheel scope alias");
    require(parseWindowWheelScope("bad", WindowWheelScope::UnderCursor) == WindowWheelScope::UnderCursor, "window wheel scope fallback");
    require(parseRecordWindowBackend("automatic") == RecordWindowBackend::Auto, "automatic record backend alias");
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
    require(toString(OverlayScope::Focus) == "focus", "focus overlay scope stringify");
    require(toString(WindowBackground::FollowSystem) == "follow-system", "follow system stringify");
    require(toString(RecordWindowBackend::Auto) == "auto", "auto record backend stringify");
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
    require(formatScreenshotNotificationTemplate("Saved {filename} from {window_title} [{window_class}] in {mode}: {path}",
                                                 CaptureMode::Window,
                                                 "org.mozilla.firefox",
                                                 "Private Window",
                                                 "shot.png",
                                                 "/tmp/shot.png") ==
                "Saved shot.png from Private Window [org.mozilla.firefox] in window: /tmp/shot.png",
            "screenshot notification template variables");
    require(formatScreenshotNotificationTemplate("{window_title}/{filename}", CaptureMode::Region, {}, {}, {}, {}) == "unknown/unknown",
            "screenshot notification missing values");

    const FilenameMetadata selectedMetadata{.windowClass = "selected.class", .windowTitle = "Selected Title"};
    const FilenameMetadata focusedMetadata{.windowClass = "focused.class", .windowTitle = "Focused Title"};
    const FilenameMetadata fullscreenMetadata{.windowClass = "single.class", .windowTitle = "Single Title"};
    const auto regionMetadata =
        resolveFilenameMetadata(true, CaptureMode::Region, selectedMetadata, focusedMetadata, 1, fullscreenMetadata);
    require(regionMetadata.windowClass == "region" && regionMetadata.windowTitle == "region", "region filename metadata");
    const auto windowMetadata =
        resolveFilenameMetadata(true, CaptureMode::Window, selectedMetadata, focusedMetadata, 0, {});
    require(windowMetadata.windowClass == "selected.class" && windowMetadata.windowTitle == "Selected Title", "window filename metadata");
    const auto oneWindowFullscreen =
        resolveFilenameMetadata(true, CaptureMode::Fullscreen, selectedMetadata, focusedMetadata, 1, fullscreenMetadata);
    require(oneWindowFullscreen.windowClass == "single.class" && oneWindowFullscreen.windowTitle == "Single Title",
            "single-window fullscreen filename metadata");
    const auto emptyFullscreen =
        resolveFilenameMetadata(true, CaptureMode::Fullscreen, selectedMetadata, focusedMetadata, 0, {});
    require(emptyFullscreen.windowClass == "fullscreen" && emptyFullscreen.windowTitle == "fullscreen",
            "empty fullscreen filename metadata");
    const std::size_t firstMonitorWindowCount = 1;
    const std::size_t secondMonitorWindowCount = 1;
    const auto multiMonitorFullscreen =
        resolveFilenameMetadata(true,
                                CaptureMode::Fullscreen,
                                selectedMetadata,
                                focusedMetadata,
                                firstMonitorWindowCount + secondMonitorWindowCount,
                                fullscreenMetadata);
    require(multiMonitorFullscreen.windowClass == "fullscreen" && multiMonitorFullscreen.windowTitle == "fullscreen",
            "multi-monitor fullscreen aggregates workspace window counts");
    const auto legacyMetadata =
        resolveFilenameMetadata(false, CaptureMode::Region, selectedMetadata, focusedMetadata, 0, {});
    require(legacyMetadata.windowClass == "focused.class" && legacyMetadata.windowTitle == "Focused Title",
            "legacy focused filename metadata");
    const auto missingWindowMetadata =
        resolveFilenameMetadata(true, CaptureMode::Window, {}, focusedMetadata, 0, {});
    require(makeTimestampedFilename("{window_class}-{window_title}.png",
                                    missingWindowMetadata.windowClass,
                                    missingWindowMetadata.windowTitle) == "unknown-unknown.png",
            "missing selected window metadata falls back to unknown");
    require(makeTimestampedFilename("{window_class}-{window_title}.png", "selected.class", {}) == "selected.class-unknown.png",
            "missing window title falls back independently");
    require(makeTimestampedFilename("{window_class}-{window_title}.png", {}, "Selected Title") == "unknown-Selected-Title.png",
            "missing window class falls back independently");

    CaptureSession session;
    session.id = "test-session";
    session.defaults.mode = CaptureMode::Window;
    session.defaults.overlayScope = OverlayScope::All;
    session.defaults.allowQuick = true;
    session.defaults.confirmBeforeCapture = true;
    session.defaults.fushionMode = true;
    session.defaults.captureFullscreenClientsAsMonitor = true;
    session.defaults.dynamicWindowMetadata = false;
    session.defaults.windowWheelScroll = false;
    session.defaults.windowWheelScope = WindowWheelScope::UnderCursor;
    session.defaults.notificationBackend = NotificationBackend::System;
    session.defaults.screenshotNotification = false;
    session.defaults.notificationTitleTemplate = "Captured {mode}";
    session.defaults.notificationBodyTemplate = "Saved {filename}";
    session.defaults.fullscreenPreviewRounding = "27";
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
    session.monitors.back().cursorArtifactPath = "/tmp/cursor.rgba";
    session.monitors.back().cursorArtifactWidth = 3840;
    session.monitors.back().cursorArtifactHeight = 2160;
    session.monitors.back().workspaceWindowCount = 1;
    session.monitors.back().singleWorkspaceWindowClass = "Class";
    session.monitors.back().singleWorkspaceWindowTitle = "Title";
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
    session.windows.back().selectionClipGeometry = Rect{.x = 0, .y = 0, .width = 100, .height = 100};
    session.windows.back().realBackgroundPath = "/tmp/window-real.rgba";
    session.windows.back().realBackgroundWidth = 200;
    session.windows.back().realBackgroundHeight = 100;
    session.windows.back().title = std::string("Title") + '\x01';
    const auto json = encodeSessionJson(session);
    require(json.find("\"fushionMode\":true") != std::string::npos, "fushion mode json");
    require(json.find("\"overlayScope\":\"all\"") != std::string::npos, "overlay scope json");
    require(json.find("\"allowQuick\":true") != std::string::npos, "allow quick json");
    require(json.find("\"confirmBeforeCapture\":true") != std::string::npos, "confirm before capture json");
    require(json.find("\"captureFullscreenClientsAsMonitor\":true") != std::string::npos, "fullscreen client behavior json");
    require(json.find("\"dynamicWindowMetadata\":false") != std::string::npos, "dynamic window metadata json");
    require(json.find("\"windowWheelScroll\":false") != std::string::npos, "window wheel scroll json");
    require(json.find("\"windowWheelScope\":\"under-cursor\"") != std::string::npos, "window wheel scope json");
    require(json.find("\"notificationBackend\":\"system\"") != std::string::npos, "notification backend json");
    require(json.find("\"screenshotNotification\":false") != std::string::npos, "screenshot notification json");
    require(json.find("\"notificationTitleTemplate\":\"Captured {mode}\"") != std::string::npos, "notification title template json");
    require(json.find("\"notificationBodyTemplate\":\"Saved {filename}\"") != std::string::npos, "notification body template json");
    require(json.find("\"fullscreenPreviewRounding\":\"27\"") != std::string::npos, "fullscreen preview rounding json");
    require(json.find("\"thumbnailMonitor\":\"DP-2\"") != std::string::npos, "thumbnail monitor json");
    require(json.find("\"windowBackground\":\"follow-system\"") != std::string::npos, "window background json");
    require(json.find("\"recordTransparentFormat\":\"webm\"") != std::string::npos, "transparent record format json");
    require(json.find("\"recordCodec\":\"auto\"") != std::string::npos, "record codec default json");
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
    require(json.find("\"selectionClipGeometry\"") != std::string::npos, "selection clip geometry json");
    require(json.find("\"rounding\":12") != std::string::npos, "rounding json");
    require(json.find("\"roundingPower\":2.5") != std::string::npos, "rounding power json");
    require(json.find("\"borderSize\":2") != std::string::npos, "border size json");
    require(json.find("\"artifactPath\":\"/tmp/window.rgba\"") != std::string::npos, "artifact path json");
    require(json.find("\"focused\":true") != std::string::npos, "focused monitor json");
    require(json.find("\"artifactTopDown\":true") != std::string::npos, "artifact orientation json");
    require(json.find("\"cursorArtifactPath\":\"/tmp/cursor.rgba\"") != std::string::npos, "cursor artifact path json");
    require(json.find("\"cursorArtifactWidth\":3840") != std::string::npos, "cursor artifact width json");
    require(json.find("\"workspaceWindowCount\":1") != std::string::npos, "workspace window count json");
    require(json.find("\"singleWorkspaceWindowClass\":\"Class\"") != std::string::npos, "single workspace window class json");
    require(json.find("\"singleWorkspaceWindowTitle\":\"Title\"") != std::string::npos, "single workspace window title json");
    require(json.find("\"realBackgroundPath\":\"/tmp/window-real.rgba\"") != std::string::npos, "real background path json");
    require(json.find("\"realBackgroundWidth\":200") != std::string::npos, "real background width json");
    require(json.find("Title\\u0001") != std::string::npos, "control byte json escaping");
    const auto decoded = decodeSessionJson(json);
    require(decoded.has_value(), "encoded session decodes");
    require(decoded->id == "test-session", "decoded id");
    require(decoded->defaults.mode == CaptureMode::Window, "decoded mode");
    require(decoded->defaults.overlayScope == OverlayScope::All, "decoded overlay scope");
    require(decoded->defaults.allowQuick, "decoded allow quick");
    require(decoded->defaults.confirmBeforeCapture, "decoded confirm before capture");
    require(decoded->defaults.captureFullscreenClientsAsMonitor, "decoded fullscreen client behavior");
    require(!decoded->defaults.dynamicWindowMetadata, "decoded dynamic window metadata");
    require(!decoded->defaults.windowWheelScroll, "decoded window wheel scroll");
    require(decoded->defaults.windowWheelScope == WindowWheelScope::UnderCursor, "decoded window wheel scope");
    require(decoded->defaults.notificationBackend == NotificationBackend::System, "decoded notification backend");
    require(!decoded->defaults.screenshotNotification, "decoded screenshot notification");
    require(decoded->defaults.notificationTitleTemplate == "Captured {mode}", "decoded notification title template");
    require(decoded->defaults.notificationBodyTemplate == "Saved {filename}", "decoded notification body template");
    require(decoded->defaults.fullscreenPreviewRounding == "27", "decoded fullscreen preview rounding");
    require(decoded->defaults.thumbnailMonitor == "DP-2", "decoded thumbnail monitor");
    require(decoded->defaults.fushionMode, "decoded fushion mode");
    require(decoded->defaults.recordTransparentFormat == "webm", "decoded transparent record format");
    require(decoded->defaults.recordCodec == "auto", "decoded record codec default");
    require(decoded->defaults.recordTransparentCodec == "auto", "decoded transparent record codec");
    require(decoded->defaults.recordSolidAlpha, "decoded solid alpha record");
    require(decoded->defaults.recordSaveDir == "$XDG_VIDEOS_DIR/Screenrecords", "decoded record save dir");
    require(decoded->cursorPosition.has_value() && decoded->cursorPosition->x == 120 && decoded->cursorPosition->y == 240, "decoded cursor position");
    require(decoded->monitors.size() == 1 && decoded->windows.size() == 1, "decoded object counts");
    require(decoded->monitors.front().focused, "decoded focused monitor");
    require(decoded->monitors.front().cursorArtifactPath == "/tmp/cursor.rgba", "decoded cursor artifact path");
    require(decoded->monitors.front().cursorArtifactWidth == 3840 && decoded->monitors.front().cursorArtifactHeight == 2160,
            "decoded cursor artifact dimensions");
    require(decoded->monitors.front().workspaceWindowCount == 1, "decoded workspace window count");
    require(decoded->monitors.front().singleWorkspaceWindowClass == "Class", "decoded single workspace window class");
    require(decoded->monitors.front().singleWorkspaceWindowTitle == "Title", "decoded single workspace window title");
    require(decoded->windows.front().artifactPath == "/tmp/window.rgba", "decoded artifact path");
    require(decoded->windows.front().selectionGeometry.has_value(), "decoded selection geometry exists");
    require(decoded->windows.front().selectionClipGeometry.has_value(), "decoded selection clip geometry exists");
    require(decoded->windows.front().focused && decoded->windows.front().fullscreen, "decoded window state");
    require(decoded->windows.front().selectionGeometry->x == 30 && decoded->windows.front().selectionGeometry->width == 120, "decoded selection geometry values");
    require(decoded->windows.front().selectionClipGeometry->x == 0 && decoded->windows.front().selectionClipGeometry->width == 100,
            "decoded selection clip geometry values");

    auto legacyOverlayJson = json;
    const auto overlayScopeField = legacyOverlayJson.find("\"overlayScope\":\"all\",");
    require(overlayScopeField != std::string::npos, "overlay scope field found for legacy test");
    legacyOverlayJson.erase(overlayScopeField, std::string("\"overlayScope\":\"all\",").size());
    const auto legacyOverlayDecoded = decodeSessionJson(legacyOverlayJson);
    require(legacyOverlayDecoded.has_value(), "session without overlay scope decodes");
    require(legacyOverlayDecoded->defaults.overlayScope == OverlayScope::Fix, "missing overlay scope keeps fixed default");

    auto legacyPreviewRoundingJson = json;
    eraseJsonField(legacyPreviewRoundingJson, "fullscreenPreviewRounding");
    const auto legacyPreviewRoundingDecoded = decodeSessionJson(legacyPreviewRoundingJson);
    require(legacyPreviewRoundingDecoded.has_value(), "session without fullscreen preview rounding decodes");
    require(legacyPreviewRoundingDecoded->defaults.fullscreenPreviewRounding == "auto",
            "missing fullscreen preview rounding keeps automatic detection");

    auto legacyDynamicMetadataJson = json;
    eraseJsonField(legacyDynamicMetadataJson, "dynamicWindowMetadata");
    const auto legacyDynamicMetadataDecoded = decodeSessionJson(legacyDynamicMetadataJson);
    require(legacyDynamicMetadataDecoded.has_value(), "session without dynamic window metadata decodes");
    require(legacyDynamicMetadataDecoded->defaults.dynamicWindowMetadata,
            "missing dynamic window metadata keeps enabled default");

    auto legacyWindowWheelScrollJson = json;
    eraseJsonField(legacyWindowWheelScrollJson, "windowWheelScroll");
    const auto legacyWindowWheelScrollDecoded = decodeSessionJson(legacyWindowWheelScrollJson);
    require(legacyWindowWheelScrollDecoded.has_value(), "session without window wheel scroll decodes");
    require(legacyWindowWheelScrollDecoded->defaults.windowWheelScroll,
            "missing window wheel scroll keeps enabled default");

    auto legacyWindowWheelScopeJson = json;
    eraseJsonField(legacyWindowWheelScopeJson, "windowWheelScope");
    const auto legacyWindowWheelScopeDecoded = decodeSessionJson(legacyWindowWheelScopeJson);
    require(legacyWindowWheelScopeDecoded.has_value(), "session without window wheel scope decodes");
    require(legacyWindowWheelScopeDecoded->defaults.windowWheelScope == WindowWheelScope::Workspace,
            "missing window wheel scope keeps workspace default");

    auto legacyNotificationBackendJson = json;
    eraseDefaultsJsonField(legacyNotificationBackendJson, "notificationBackend");
    const auto legacyNotificationBackendDecoded = decodeSessionJson(legacyNotificationBackendJson);
    require(legacyNotificationBackendDecoded.has_value(), "session without notification backend decodes");
    require(legacyNotificationBackendDecoded->defaults.notificationBackend == NotificationBackend::Hyprland,
            "missing notification backend keeps hyprland default");

    auto legacyScreenshotNotificationJson = json;
    eraseDefaultsJsonField(legacyScreenshotNotificationJson, "screenshotNotification");
    const auto legacyScreenshotNotificationDecoded = decodeSessionJson(legacyScreenshotNotificationJson);
    require(legacyScreenshotNotificationDecoded.has_value(), "session without screenshot notification toggle decodes");
    require(legacyScreenshotNotificationDecoded->defaults.screenshotNotification,
            "missing screenshot notification keeps enabled default");

    auto legacyNotificationTitleJson = json;
    eraseDefaultsJsonField(legacyNotificationTitleJson, "notificationTitleTemplate");
    require(decodeSessionJson(legacyNotificationTitleJson).has_value(), "session without notification title template decodes");

    auto legacyNotificationBodyJson = json;
    eraseDefaultsJsonField(legacyNotificationBodyJson, "notificationBodyTemplate");
    require(decodeSessionJson(legacyNotificationBodyJson).has_value(), "session without notification body template decodes");

    auto legacyWorkspaceMetadataJson = json;
    eraseJsonField(legacyWorkspaceMetadataJson, "workspaceWindowCount");
    eraseJsonField(legacyWorkspaceMetadataJson, "singleWorkspaceWindowClass");
    eraseJsonField(legacyWorkspaceMetadataJson, "singleWorkspaceWindowTitle");
    const auto legacyWorkspaceMetadataDecoded = decodeSessionJson(legacyWorkspaceMetadataJson);
    require(legacyWorkspaceMetadataDecoded.has_value(), "session without workspace window metadata decodes");
    require(!legacyWorkspaceMetadataDecoded->monitors.front().workspaceWindowCount,
            "missing workspace window metadata stays unavailable");

    CaptureSession boundedWorkspaceMetadataSession = session;
    boundedWorkspaceMetadataSession.monitors.front().singleWorkspaceWindowClass = std::string(5000, 'c');
    boundedWorkspaceMetadataSession.monitors.front().singleWorkspaceWindowTitle = std::string(5000, 't');
    const auto boundedWorkspaceMetadataDecoded = decodeSessionJson(encodeSessionJson(boundedWorkspaceMetadataSession));
    require(boundedWorkspaceMetadataDecoded.has_value(), "bounded workspace metadata session decodes");
    require(boundedWorkspaceMetadataDecoded->monitors.front().singleWorkspaceWindowClass.size() == 4096,
            "workspace window class is bounded");
    require(boundedWorkspaceMetadataDecoded->monitors.front().singleWorkspaceWindowTitle.size() == 4096,
            "workspace window title is bounded");

    auto legacyCursorJson = json;
    eraseJsonField(legacyCursorJson, "cursorArtifactPath");
    eraseJsonField(legacyCursorJson, "cursorArtifactWidth");
    eraseJsonField(legacyCursorJson, "cursorArtifactHeight");
    eraseJsonField(legacyCursorJson, "cursorArtifactTopDown");
    const auto legacyCursorDecoded = decodeSessionJson(legacyCursorJson);
    require(legacyCursorDecoded.has_value(), "session without cursor artifact fields decodes");
    require(legacyCursorDecoded->monitors.front().cursorArtifactPath.empty(), "missing cursor artifact stays empty");

    CaptureSession invalidCursorSession = session;
    invalidCursorSession.monitors.front().cursorArtifactWidth = 0;
    require(!decodeSessionJson(encodeSessionJson(invalidCursorSession)).has_value(), "cursor artifact path requires valid dimensions");

    CaptureSession legacySession = session;
    legacySession.windows.front().selectionGeometry.reset();
    legacySession.windows.front().selectionClipGeometry.reset();
    const auto legacyJson = encodeSessionJson(legacySession);
    require(legacyJson.find("\"selectionGeometry\"") == std::string::npos, "missing optional selection geometry omitted");
    const auto legacyDecoded = decodeSessionJson(legacyJson);
    require(legacyDecoded.has_value(), "session without selection geometry decodes");
    require(!legacyDecoded->windows.front().selectionGeometry.has_value(), "missing optional selection geometry stays empty");
    require(!legacyDecoded->windows.front().selectionClipGeometry.has_value(), "missing optional selection clip geometry stays empty");

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
    require(CaptureDefaults{}.recordAudio == RecordAudio::Off, "sound defaults off");
    require(CaptureDefaults{}.recordAudioOutput == "auto", "sound source defaults auto");
    require(audio::resolveOutput("auto", CaptureMode::Fullscreen, "0x123") == "default", "fullscreen auto source");
    require(audio::resolveOutput("auto", CaptureMode::Region, "0x123") == "default", "region auto source");
    require(audio::resolveOutput("auto", CaptureMode::Window, "0x123") == "window:0x123", "window auto source");
    require(audio::resolveOutput("auto", CaptureMode::Window, "") == "window:", "unselected window never captures desktop");
    require(audio::resolveOutput("window:0x456", CaptureMode::Region, "0x123") == "window:0x456", "explicit window overrides auto target");
    require(audio::resolveOutput("default", CaptureMode::Window, "0x123") == "default", "explicit default overrides window auto");
    for (auto mode : {RecordAudio::Off, RecordAudio::System, RecordAudio::Microphone, RecordAudio::Mix}) {
        recording.defaults.recordAudio = mode;
        recording.defaults.recordAudioMix = "manual";
        recording.defaults.recordAudioSystemGain = -6;
        recording.defaults.recordAudioMicGain = 12;
        recording.defaults.recordAudioOutput = "alsa_output.test.stereo";
        recording.defaults.recordAudioInput = "mic name with spaces";
        const auto decoded = decodeRecordingRequestJson(encodeRecordingRequestJson(recording));
        require(decoded && decoded->defaults.recordAudio == mode, "sound mode roundtrip");
        require(decoded->defaults.recordAudioMix == "manual" && decoded->defaults.recordAudioSystemGain == -6 && decoded->defaults.recordAudioMicGain == 12, "mix settings roundtrip");
        require(decoded->defaults.recordAudioOutput == recording.defaults.recordAudioOutput, "output device roundtrip");
        require(decoded->defaults.recordAudioInput == recording.defaults.recordAudioInput, "input device roundtrip");
    }
    auto legacySound = nlohmann::json::parse(encodeRecordingRequestJson(recording));
    for (const auto* key : {"recordAudio", "recordAudioOutput", "recordAudioInput"}) legacySound["defaults"].erase(key);
    auto legacySoundDecoded = decodeRecordingRequestJson(legacySound.dump());
    require(legacySoundDecoded && legacySoundDecoded->defaults.recordAudio == RecordAudio::Off && legacySoundDecoded->defaults.recordAudioInput == "default", "legacy requests remain silent");
    legacySound["defaults"]["recordAudio"] = "invalid";
    require(!decodeRecordingRequestJson(legacySound.dump()), "invalid sound mode rejected");
    legacySound["defaults"]["recordAudio"] = 12;
    require(!decodeRecordingRequestJson(legacySound.dump()), "non-string sound mode rejected");
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
