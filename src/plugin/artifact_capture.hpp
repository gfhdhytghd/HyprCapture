#pragma once

#include "shared/config.hpp"
#include "shared/protocol.hpp"
#include "plugin/window_stream.hpp"

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

struct WindowStreamCaptureRequest {
    CaptureDefaults defaults;
    std::string     windowAddress;
    std::uint64_t   sequence = 0;
    std::uint64_t   geometryEpoch = 0;
};

struct WindowStreamCapturedFrame {
    WindowStreamFrameMetadata metadata;
    std::vector<unsigned char> rgba;
};

CaptureSession captureCompositorArtifacts(const CaptureDefaults& defaults, bool quick);
LaunchResult captureWindowArtifactFromRequestFile(const std::string& path);
LaunchResult captureExportPipeFromRequestFile(const std::string& path);
LaunchResult startWindowStreamFromRequestFile(const std::string& path);
LaunchResult stopWindowStreamFromRequestFile(const std::string& path);
bool isValidWindowStreamStartRequest(const std::string& json);
std::optional<RecordingFrame> captureRecordingFrame(const RecordingFrameRequest& request);
std::optional<WindowStreamCapturedFrame> captureWindowStreamFrame(const WindowStreamCaptureRequest& request);
void resetWindowStreamCapture();
void resetRecordingCaptureState();
std::string writeCompositorSessionJsonFile(const CaptureSession& session, std::string_view json);
void cleanupCompositorArtifacts(const CaptureSession& session);
void shutdownArtifactCapture();

} // namespace hyprcapture
