#pragma once

#include "shared/config.hpp"
#include "shared/protocol.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <string>
#include <string_view>
#include <optional>
#include <vector>

extern HANDLE g_pluginHandle;

namespace hyprcapture {

struct LaunchResult;

struct RecordingFrameRequest {
    CaptureDefaults defaults;
    CaptureMode     mode = CaptureMode::Region;
    Rect            targetGeometry;
    std::string     windowAddress;
};

struct RecordingFrame {
    std::vector<unsigned char> rgba;
    int                        width = 0;
    int                        height = 0;
};

CaptureSession captureCompositorArtifacts(const CaptureDefaults& defaults, bool quick);
LaunchResult captureWindowArtifactFromRequestFile(const std::string& path);
LaunchResult captureExportPipeFromRequestFile(const std::string& path);
std::optional<RecordingFrame> captureRecordingFrame(const RecordingFrameRequest& request);
void resetRecordingCaptureState();
std::string writeCompositorSessionJsonFile(const CaptureSession& session, std::string_view json);
void cleanupCompositorArtifacts(const CaptureSession& session);
void shutdownArtifactCapture();

} // namespace hyprcapture
