#pragma once

#include "shared/config.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hyprcapture {

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct MonitorInfo {
    std::string name;
    Rect        logicalGeometry;
    double      scale = 1.0;
    int         transform = 0;
    bool        focused = false;
    std::string artifactPath;
    int         artifactWidth = 0;
    int         artifactHeight = 0;
    bool        artifactTopDown = true;
};

struct WindowInfo {
    std::string address;
    std::string title;
    std::string appClass;
    Rect        visibleGeometry;
    Rect        fullGeometry;
    double      rounding = 0.0;
    double      roundingPower = 2.0;
    double      borderSize = 0.0;
    std::string artifactPath;
    int         artifactWidth = 0;
    int         artifactHeight = 0;
    bool        artifactTopDown = true;
    std::string realBackgroundPath;
    int         realBackgroundWidth = 0;
    int         realBackgroundHeight = 0;
    bool        realBackgroundTopDown = true;
    int         zIndex = 0;
    bool        selectable = true;
};

struct CaptureSession {
    std::string              id;
    CaptureDefaults          defaults;
    std::optional<Point>     cursorPosition;
    std::vector<MonitorInfo> monitors;
    std::vector<WindowInfo>  windows;
};

struct RecordingRequest {
    std::string     id;
    CaptureDefaults defaults;
    CaptureMode     mode = CaptureMode::Region;
    Rect            targetGeometry;
    std::string     windowAddress;
};

std::string makeSessionId();
std::string encodeSessionJson(const CaptureSession& session);
std::optional<CaptureSession> decodeSessionJson(const std::string& json);
std::string encodeRecordingRequestJson(const RecordingRequest& request);
std::optional<RecordingRequest> decodeRecordingRequestJson(const std::string& json);

} // namespace hyprcapture
