#include "shared/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <utility>

namespace hyprcapture {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::size_t MAX_SESSION_JSON_BYTES = 8ULL * 1024ULL * 1024ULL;
constexpr std::size_t MAX_SESSION_MONITORS = 64;
constexpr std::size_t MAX_SESSION_WINDOWS = 512;
constexpr std::size_t MAX_METADATA_STRING_BYTES = 4096;
constexpr std::size_t MAX_PATH_BYTES = 4096;
constexpr int         MAX_ARTIFACT_DIMENSION = 32768;
constexpr double      MAX_LOGICAL_COORDINATE = 1'000'000.0;
constexpr std::int64_t MAX_RECORD_FPS = 240;
constexpr std::int64_t MAX_RECORD_SECONDS = 24 * 60 * 60;
constexpr std::int64_t MAX_RECORD_COUNTDOWN_SECONDS = 60;

std::string boundedString(const std::string& value, std::size_t maxBytes) {
    if (value.size() <= maxBytes)
        return value;
    return value.substr(0, maxBytes);
}

double boundedDouble(double value, double minimum, double maximum, double fallback = 0.0) {
    if (!std::isfinite(value))
        return fallback;
    return std::clamp(value, minimum, maximum);
}

int boundedDimension(int value) {
    return value > 0 && value <= MAX_ARTIFACT_DIMENSION ? value : 0;
}

Json rectJson(const Rect& rect) {
    return Json{
        {"x", boundedDouble(rect.x, -MAX_LOGICAL_COORDINATE, MAX_LOGICAL_COORDINATE)},
        {"y", boundedDouble(rect.y, -MAX_LOGICAL_COORDINATE, MAX_LOGICAL_COORDINATE)},
        {"width", boundedDouble(rect.width, 0.0, MAX_LOGICAL_COORDINATE)},
        {"height", boundedDouble(rect.height, 0.0, MAX_LOGICAL_COORDINATE)},
    };
}

Json pointJson(const Point& point) {
    return Json{
        {"x", boundedDouble(point.x, -MAX_LOGICAL_COORDINATE, MAX_LOGICAL_COORDINATE)},
        {"y", boundedDouble(point.y, -MAX_LOGICAL_COORDINATE, MAX_LOGICAL_COORDINATE)},
    };
}

Json defaultsJson(const CaptureDefaults& defaults) {
    return Json{
        {"mode", toString(defaults.mode)},
        {"fullscreenScope", toString(defaults.fullscreenScope)},
        {"overlayScope", toString(defaults.overlayScope)},
        {"windowBackground", toString(defaults.windowBackground)},
        {"windowBorder", toString(defaults.windowBorder)},
        {"windowShadow", toString(defaults.windowShadow)},
        {"save", defaults.save},
        {"clipboard", defaults.clipboard},
        {"showThumbnail", defaults.showThumbnail},
        {"includeCursor", defaults.includeCursor},
        {"allowQuick", defaults.allowQuick},
        {"confirmBeforeCapture", defaults.confirmBeforeCapture},
        {"fushionMode", defaults.fushionMode},
        {"captureFullscreenClientsAsMonitor", defaults.captureFullscreenClientsAsMonitor},
        {"fullscreenPreviewRounding", boundedString(defaults.fullscreenPreviewRounding, MAX_METADATA_STRING_BYTES)},
        {"saveDir", boundedString(defaults.saveDir, MAX_PATH_BYTES)},
        {"filenameTemplate", boundedString(defaults.filenameTemplate, MAX_METADATA_STRING_BYTES)},
        {"recordSaveDir", boundedString(defaults.recordSaveDir, MAX_PATH_BYTES)},
        {"recordFilenameTemplate", boundedString(defaults.recordFilenameTemplate, MAX_METADATA_STRING_BYTES)},
        {"recordFormat", boundedString(defaults.recordFormat, MAX_METADATA_STRING_BYTES)},
        {"recordTransparentFormat", boundedString(defaults.recordTransparentFormat, MAX_METADATA_STRING_BYTES)},
        {"recordCodec", boundedString(defaults.recordCodec, MAX_METADATA_STRING_BYTES)},
        {"recordTransparentCodec", boundedString(defaults.recordTransparentCodec, MAX_METADATA_STRING_BYTES)},
        {"recordSolidAlpha", defaults.recordSolidAlpha},
        {"recordPreset", boundedString(defaults.recordPreset, MAX_METADATA_STRING_BYTES)},
        {"recordGsrFlags", boundedString(defaults.recordGsrFlags, MAX_METADATA_STRING_BYTES)},
        {"recordWindowBackend", toString(defaults.recordWindowBackend)},
        {"recordFps", std::clamp<std::int64_t>(defaults.recordFps, 1, MAX_RECORD_FPS)},
        {"recordFpsOptions", boundedString(defaults.recordFpsOptions, MAX_METADATA_STRING_BYTES)},
        {"recordWindowFpsLimit", std::clamp<std::int64_t>(defaults.recordWindowFpsLimit, 0, MAX_RECORD_FPS)},
        {"recordWindowRealBgFpsLimit", std::clamp<std::int64_t>(defaults.recordWindowRealBgFpsLimit, 0, MAX_RECORD_FPS)},
        {"recordMaxSeconds", std::clamp<std::int64_t>(defaults.recordMaxSeconds, 0, MAX_RECORD_SECONDS)},
        {"recordCountdownSeconds", std::clamp<std::int64_t>(defaults.recordCountdownSeconds, 0, MAX_RECORD_COUNTDOWN_SECONDS)},
        {"thumbnailTimeoutMs", std::clamp<std::int64_t>(defaults.thumbnailTimeoutMs, 0, 60 * 60 * 1000)},
        {"thumbnailMonitor", boundedString(defaults.thumbnailMonitor, MAX_METADATA_STRING_BYTES)},
        {"watermark", boundedString(defaults.watermark, MAX_PATH_BYTES)},
        {"watermarkPosition", toString(defaults.watermarkPosition)},
        {"watermarkWidth", boundedString(defaults.watermarkWidth, MAX_METADATA_STRING_BYTES)},
        {"watermarkOffset", boundedString(defaults.watermarkOffset, MAX_METADATA_STRING_BYTES)},
    };
}

bool stringValue(const Json& obj, const char* key, std::string& out, std::size_t maxBytes, bool required = true) {
    const auto it = obj.find(key);
    if (it == obj.end())
        return !required;
    if (!it->is_string())
        return false;
    out = it->get<std::string>();
    if (out.size() > maxBytes)
        return false;
    return true;
}

bool boolValue(const Json& obj, const char* key, bool& out, bool required = true) {
    const auto it = obj.find(key);
    if (it == obj.end())
        return !required;
    if (!it->is_boolean())
        return false;
    out = it->get<bool>();
    return true;
}

bool intValue(const Json& obj, const char* key, int& out, int minimum, int maximum, bool required = true) {
    const auto it = obj.find(key);
    if (it == obj.end())
        return !required;
    if (!it->is_number_integer())
        return false;
    const auto value = it->get<long long>();
    if (value < minimum || value > maximum)
        return false;
    out = static_cast<int>(value);
    return true;
}

bool int64Value(const Json& obj, const char* key, std::int64_t& out, std::int64_t minimum, std::int64_t maximum, bool required = true) {
    const auto it = obj.find(key);
    if (it == obj.end())
        return !required;
    if (!it->is_number_integer())
        return false;
    const auto value = it->get<std::int64_t>();
    if (value < minimum || value > maximum)
        return false;
    out = value;
    return true;
}

bool doubleValue(const Json& obj, const char* key, double& out, double minimum, double maximum, bool required = true) {
    const auto it = obj.find(key);
    if (it == obj.end())
        return !required;
    if (!it->is_number())
        return false;
    const double value = it->get<double>();
    if (!std::isfinite(value) || value < minimum || value > maximum)
        return false;
    out = value;
    return true;
}

bool rectValue(const Json& obj, const char* key, Rect& out) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_object())
        return false;
    Rect rect;
    if (!doubleValue(*it, "x", rect.x, -MAX_LOGICAL_COORDINATE, MAX_LOGICAL_COORDINATE) ||
        !doubleValue(*it, "y", rect.y, -MAX_LOGICAL_COORDINATE, MAX_LOGICAL_COORDINATE) ||
        !doubleValue(*it, "width", rect.width, 1.0, MAX_LOGICAL_COORDINATE) ||
        !doubleValue(*it, "height", rect.height, 1.0, MAX_LOGICAL_COORDINATE))
        return false;
    out = rect;
    return true;
}

bool pointValue(const Json& obj, const char* key, Point& out) {
    const auto it = obj.find(key);
    if (it == obj.end())
        return true;
    if (!it->is_object())
        return false;
    Point point;
    if (!doubleValue(*it, "x", point.x, -MAX_LOGICAL_COORDINATE, MAX_LOGICAL_COORDINATE) ||
        !doubleValue(*it, "y", point.y, -MAX_LOGICAL_COORDINATE, MAX_LOGICAL_COORDINATE))
        return false;
    out = point;
    return true;
}

bool parseDefaults(const Json& obj, CaptureDefaults& defaults) {
    if (!obj.is_object())
        return false;

    std::string value;
    if (stringValue(obj, "mode", value, MAX_METADATA_STRING_BYTES, false))
        defaults.mode = parseCaptureMode(value, defaults.mode);
    else
        return false;
    if (stringValue(obj, "fullscreenScope", value, MAX_METADATA_STRING_BYTES, false))
        defaults.fullscreenScope = parseFullscreenScope(value, defaults.fullscreenScope);
    else
        return false;
    if (obj.contains("overlayScope")) {
        if (!stringValue(obj, "overlayScope", value, MAX_METADATA_STRING_BYTES))
            return false;
        defaults.overlayScope = parseOverlayScope(value, defaults.overlayScope);
    }
    if (stringValue(obj, "windowBackground", value, MAX_METADATA_STRING_BYTES, false))
        defaults.windowBackground = parseWindowBackground(value, defaults.windowBackground);
    else
        return false;
    if (stringValue(obj, "windowBorder", value, MAX_METADATA_STRING_BYTES, false))
        defaults.windowBorder = parseDecorationPolicy(value, defaults.windowBorder);
    else
        return false;
    if (stringValue(obj, "windowShadow", value, MAX_METADATA_STRING_BYTES, false))
        defaults.windowShadow = parseDecorationPolicy(value, defaults.windowShadow);
    else
        return false;
    if (stringValue(obj, "recordWindowBackend", value, MAX_METADATA_STRING_BYTES, false))
        defaults.recordWindowBackend = parseRecordWindowBackend(value, defaults.recordWindowBackend);
    else
        return false;

    return boolValue(obj, "save", defaults.save, false) && boolValue(obj, "clipboard", defaults.clipboard, false) &&
        boolValue(obj, "showThumbnail", defaults.showThumbnail, false) && boolValue(obj, "includeCursor", defaults.includeCursor, false) &&
        boolValue(obj, "allowQuick", defaults.allowQuick, false) && boolValue(obj, "confirmBeforeCapture", defaults.confirmBeforeCapture, false) &&
        boolValue(obj, "fushionMode", defaults.fushionMode, false) &&
        boolValue(obj, "captureFullscreenClientsAsMonitor", defaults.captureFullscreenClientsAsMonitor, false) &&
        stringValue(obj, "fullscreenPreviewRounding", defaults.fullscreenPreviewRounding, MAX_METADATA_STRING_BYTES, false) &&
        stringValue(obj, "saveDir", defaults.saveDir, MAX_PATH_BYTES, false) &&
        stringValue(obj, "filenameTemplate", defaults.filenameTemplate, MAX_METADATA_STRING_BYTES, false) &&
        stringValue(obj, "recordSaveDir", defaults.recordSaveDir, MAX_PATH_BYTES, false) &&
        stringValue(obj, "recordFilenameTemplate", defaults.recordFilenameTemplate, MAX_METADATA_STRING_BYTES, false) &&
        stringValue(obj, "recordFormat", defaults.recordFormat, MAX_METADATA_STRING_BYTES, false) &&
        stringValue(obj, "recordTransparentFormat", defaults.recordTransparentFormat, MAX_METADATA_STRING_BYTES, false) &&
        stringValue(obj, "recordCodec", defaults.recordCodec, MAX_METADATA_STRING_BYTES, false) &&
        stringValue(obj, "recordTransparentCodec", defaults.recordTransparentCodec, MAX_METADATA_STRING_BYTES, false) &&
        boolValue(obj, "recordSolidAlpha", defaults.recordSolidAlpha, false) &&
        stringValue(obj, "recordPreset", defaults.recordPreset, MAX_METADATA_STRING_BYTES, false) &&
        stringValue(obj, "recordGsrFlags", defaults.recordGsrFlags, MAX_METADATA_STRING_BYTES, false) &&
        int64Value(obj, "recordFps", defaults.recordFps, 1, MAX_RECORD_FPS, false) &&
        stringValue(obj, "recordFpsOptions", defaults.recordFpsOptions, MAX_METADATA_STRING_BYTES, false) &&
        int64Value(obj, "recordWindowFpsLimit", defaults.recordWindowFpsLimit, 0, MAX_RECORD_FPS, false) &&
        int64Value(obj, "recordWindowRealBgFpsLimit", defaults.recordWindowRealBgFpsLimit, 0, MAX_RECORD_FPS, false) &&
        int64Value(obj, "recordMaxSeconds", defaults.recordMaxSeconds, 0, MAX_RECORD_SECONDS, false) &&
        int64Value(obj, "recordCountdownSeconds", defaults.recordCountdownSeconds, 0, MAX_RECORD_COUNTDOWN_SECONDS, false) &&
        int64Value(obj, "thumbnailTimeoutMs", defaults.thumbnailTimeoutMs, 0, 60 * 60 * 1000, false) &&
        stringValue(obj, "thumbnailMonitor", defaults.thumbnailMonitor, MAX_METADATA_STRING_BYTES, false) &&
        stringValue(obj, "watermark", defaults.watermark, MAX_PATH_BYTES, false) &&
        stringValue(obj, "watermarkWidth", defaults.watermarkWidth, MAX_METADATA_STRING_BYTES, false) &&
        stringValue(obj, "watermarkOffset", defaults.watermarkOffset, MAX_METADATA_STRING_BYTES, false) &&
        (!obj.contains("watermarkPosition") ||
         (stringValue(obj, "watermarkPosition", value, MAX_METADATA_STRING_BYTES, false) &&
          (defaults.watermarkPosition = parseWatermarkPosition(value, defaults.watermarkPosition), true)));
}

bool monitorValue(const Json& obj, MonitorInfo& out) {
    if (!obj.is_object())
        return false;

    MonitorInfo monitor;
    if (!stringValue(obj, "name", monitor.name, MAX_METADATA_STRING_BYTES) || !rectValue(obj, "geometry", monitor.logicalGeometry) ||
        !doubleValue(obj, "scale", monitor.scale, 0.01, 100.0) || !intValue(obj, "transform", monitor.transform, 0, 7) ||
        !boolValue(obj, "focused", monitor.focused, false) ||
        !stringValue(obj, "artifactPath", monitor.artifactPath, MAX_PATH_BYTES, false) ||
        !intValue(obj, "artifactWidth", monitor.artifactWidth, 0, MAX_ARTIFACT_DIMENSION, false) ||
        !intValue(obj, "artifactHeight", monitor.artifactHeight, 0, MAX_ARTIFACT_DIMENSION, false) ||
        !boolValue(obj, "artifactTopDown", monitor.artifactTopDown, false) ||
        !stringValue(obj, "cursorArtifactPath", monitor.cursorArtifactPath, MAX_PATH_BYTES, false) ||
        !intValue(obj, "cursorArtifactWidth", monitor.cursorArtifactWidth, 0, MAX_ARTIFACT_DIMENSION, false) ||
        !intValue(obj, "cursorArtifactHeight", monitor.cursorArtifactHeight, 0, MAX_ARTIFACT_DIMENSION, false) ||
        !boolValue(obj, "cursorArtifactTopDown", monitor.cursorArtifactTopDown, false))
        return false;

    if (!monitor.artifactPath.empty() && (monitor.artifactWidth <= 0 || monitor.artifactHeight <= 0))
        return false;
    if (!monitor.cursorArtifactPath.empty() && (monitor.cursorArtifactWidth <= 0 || monitor.cursorArtifactHeight <= 0))
        return false;
    out = std::move(monitor);
    return true;
}

bool windowValue(const Json& obj, WindowInfo& out) {
    if (!obj.is_object())
        return false;

    WindowInfo window;
    if (!stringValue(obj, "address", window.address, MAX_METADATA_STRING_BYTES) || !stringValue(obj, "title", window.title, MAX_METADATA_STRING_BYTES) ||
        !stringValue(obj, "class", window.appClass, MAX_METADATA_STRING_BYTES) ||
        !boolValue(obj, "focused", window.focused, false) || !boolValue(obj, "fullscreen", window.fullscreen, false) ||
        !rectValue(obj, "visibleGeometry", window.visibleGeometry) ||
        !rectValue(obj, "fullGeometry", window.fullGeometry) || !doubleValue(obj, "rounding", window.rounding, 0.0, MAX_LOGICAL_COORDINATE, false) ||
        !doubleValue(obj, "roundingPower", window.roundingPower, 1.0, 10.0, false) ||
        !doubleValue(obj, "borderSize", window.borderSize, 0.0, MAX_LOGICAL_COORDINATE, false) ||
        !stringValue(obj, "artifactPath", window.artifactPath, MAX_PATH_BYTES, false) ||
        !intValue(obj, "artifactWidth", window.artifactWidth, 0, MAX_ARTIFACT_DIMENSION, false) ||
        !intValue(obj, "artifactHeight", window.artifactHeight, 0, MAX_ARTIFACT_DIMENSION, false) ||
        !boolValue(obj, "artifactTopDown", window.artifactTopDown, false) ||
        !stringValue(obj, "realBackgroundPath", window.realBackgroundPath, MAX_PATH_BYTES, false) ||
        !intValue(obj, "realBackgroundWidth", window.realBackgroundWidth, 0, MAX_ARTIFACT_DIMENSION, false) ||
        !intValue(obj, "realBackgroundHeight", window.realBackgroundHeight, 0, MAX_ARTIFACT_DIMENSION, false) ||
        !boolValue(obj, "realBackgroundTopDown", window.realBackgroundTopDown, false) || !intValue(obj, "zIndex", window.zIndex, 0, MAX_SESSION_WINDOWS) ||
        !boolValue(obj, "selectable", window.selectable, false))
        return false;

    if (!window.artifactPath.empty() && (window.artifactWidth <= 0 || window.artifactHeight <= 0))
        return false;
    if (!window.realBackgroundPath.empty() && (window.realBackgroundWidth <= 0 || window.realBackgroundHeight <= 0))
        return false;
    if (obj.contains("selectionGeometry")) {
        Rect selection;
        if (!rectValue(obj, "selectionGeometry", selection))
            return false;
        window.selectionGeometry = selection;
    }
    if (obj.contains("selectionClipGeometry")) {
        Rect clip;
        if (!rectValue(obj, "selectionClipGeometry", clip))
            return false;
        window.selectionClipGeometry = clip;
    }
    out = std::move(window);
    return true;
}

} // namespace

std::string makeSessionId() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::random_device rd;
    std::mt19937_64 rng((static_cast<std::uint64_t>(now) << 1U) ^ rd());
    std::ostringstream out;
    out << std::hex << now << "-" << rng();
    return out.str();
}

std::string encodeSessionJson(const CaptureSession& session) {
    Json root;
    root["id"] = boundedString(session.id, MAX_METADATA_STRING_BYTES);
    root["defaults"] = defaultsJson(session.defaults);
    if (session.cursorPosition)
        root["cursorPosition"] = pointJson(*session.cursorPosition);

    root["monitors"] = Json::array();
    for (const auto& mon : session.monitors) {
        if (root["monitors"].size() >= MAX_SESSION_MONITORS)
            break;
        root["monitors"].push_back(Json{
            {"name", boundedString(mon.name, MAX_METADATA_STRING_BYTES)},
            {"geometry", rectJson(mon.logicalGeometry)},
            {"scale", boundedDouble(mon.scale, 0.01, 100.0, 1.0)},
            {"transform", std::clamp(mon.transform, 0, 7)},
            {"focused", mon.focused},
            {"artifactPath", boundedString(mon.artifactPath, MAX_PATH_BYTES)},
            {"artifactWidth", boundedDimension(mon.artifactWidth)},
            {"artifactHeight", boundedDimension(mon.artifactHeight)},
            {"artifactTopDown", mon.artifactTopDown},
            {"cursorArtifactPath", boundedString(mon.cursorArtifactPath, MAX_PATH_BYTES)},
            {"cursorArtifactWidth", boundedDimension(mon.cursorArtifactWidth)},
            {"cursorArtifactHeight", boundedDimension(mon.cursorArtifactHeight)},
            {"cursorArtifactTopDown", mon.cursorArtifactTopDown},
        });
    }

    root["windows"] = Json::array();
    for (const auto& win : session.windows) {
        if (root["windows"].size() >= MAX_SESSION_WINDOWS)
            break;
        Json windowJson{
            {"address", boundedString(win.address, MAX_METADATA_STRING_BYTES)},
            {"title", boundedString(win.title, MAX_METADATA_STRING_BYTES)},
            {"class", boundedString(win.appClass, MAX_METADATA_STRING_BYTES)},
            {"focused", win.focused},
            {"fullscreen", win.fullscreen},
            {"visibleGeometry", rectJson(win.visibleGeometry)},
            {"fullGeometry", rectJson(win.fullGeometry)},
            {"rounding", boundedDouble(win.rounding, 0.0, MAX_LOGICAL_COORDINATE)},
            {"roundingPower", boundedDouble(win.roundingPower, 1.0, 10.0, 2.0)},
            {"borderSize", boundedDouble(win.borderSize, 0.0, MAX_LOGICAL_COORDINATE)},
            {"artifactPath", boundedString(win.artifactPath, MAX_PATH_BYTES)},
            {"artifactWidth", boundedDimension(win.artifactWidth)},
            {"artifactHeight", boundedDimension(win.artifactHeight)},
            {"artifactTopDown", win.artifactTopDown},
            {"realBackgroundPath", boundedString(win.realBackgroundPath, MAX_PATH_BYTES)},
            {"realBackgroundWidth", boundedDimension(win.realBackgroundWidth)},
            {"realBackgroundHeight", boundedDimension(win.realBackgroundHeight)},
            {"realBackgroundTopDown", win.realBackgroundTopDown},
            {"zIndex", std::clamp(win.zIndex, 0, static_cast<int>(MAX_SESSION_WINDOWS))},
            {"selectable", win.selectable},
        };
        if (win.selectionGeometry)
            windowJson["selectionGeometry"] = rectJson(*win.selectionGeometry);
        if (win.selectionClipGeometry)
            windowJson["selectionClipGeometry"] = rectJson(*win.selectionClipGeometry);
        root["windows"].push_back(std::move(windowJson));
    }

    return root.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::optional<CaptureSession> decodeSessionJson(const std::string& json) {
    if (json.empty() || json.size() > MAX_SESSION_JSON_BYTES || json.front() != '{')
        return std::nullopt;

    const auto root = Json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return std::nullopt;

    CaptureSession session;
    if (!stringValue(root, "id", session.id, MAX_METADATA_STRING_BYTES) || session.id.empty())
        return std::nullopt;
    if (!root.contains("defaults") || !parseDefaults(root.value("defaults", Json{}), session.defaults))
        return std::nullopt;

    Point cursorPosition;
    if (!pointValue(root, "cursorPosition", cursorPosition))
        return std::nullopt;
    if (root.contains("cursorPosition"))
        session.cursorPosition = cursorPosition;

    const auto monitors = root.find("monitors");
    const auto windows = root.find("windows");
    if (monitors == root.end() || windows == root.end() || !monitors->is_array() || !windows->is_array() ||
        monitors->size() > MAX_SESSION_MONITORS || windows->size() > MAX_SESSION_WINDOWS)
        return std::nullopt;

    for (const auto& value : *monitors) {
        MonitorInfo monitor;
        if (!monitorValue(value, monitor))
            return std::nullopt;
        session.monitors.push_back(std::move(monitor));
    }

    for (const auto& value : *windows) {
        WindowInfo window;
        if (!windowValue(value, window))
            return std::nullopt;
        session.windows.push_back(std::move(window));
    }

    return session;
}

std::string encodeRecordingRequestJson(const RecordingRequest& request) {
    Json root;
    root["id"] = boundedString(request.id, MAX_METADATA_STRING_BYTES);
    root["defaults"] = defaultsJson(request.defaults);
    root["mode"] = toString(request.mode);
    root["targetGeometry"] = rectJson(request.targetGeometry);
    root["windowAddress"] = boundedString(request.windowAddress, MAX_METADATA_STRING_BYTES);
    return root.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::optional<RecordingRequest> decodeRecordingRequestJson(const std::string& json) {
    if (json.empty() || json.size() > MAX_SESSION_JSON_BYTES || json.front() != '{')
        return std::nullopt;

    const auto root = Json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return std::nullopt;

    RecordingRequest request;
    std::string      mode;
    if (!stringValue(root, "id", request.id, MAX_METADATA_STRING_BYTES) || request.id.empty() ||
        !root.contains("defaults") || !parseDefaults(root.value("defaults", Json{}), request.defaults) ||
        !stringValue(root, "mode", mode, MAX_METADATA_STRING_BYTES) || !rectValue(root, "targetGeometry", request.targetGeometry) ||
        !stringValue(root, "windowAddress", request.windowAddress, MAX_METADATA_STRING_BYTES, false))
        return std::nullopt;

    request.mode = parseCaptureMode(mode, request.defaults.mode);
    request.defaults.mode = request.mode;
    if (request.mode == CaptureMode::Window && request.windowAddress.empty())
        return std::nullopt;
    return request;
}

} // namespace hyprcapture
