#pragma once

#include <filesystem>
#include <string>

namespace hyprcapture {
void stopRecordingForSessionLock();

struct LaunchResult;

LaunchResult startRecordingFromRequestFile(const std::string& path, const std::string& configuredHelper = {});
LaunchResult stopRecording(const std::string& reason = "stopped");
bool isRecordingActive();
bool initializeRecordingStateServer(std::string* error = nullptr);
std::filesystem::path recordingStateSocketPath();
void shutdownRecording();

} // namespace hyprcapture
