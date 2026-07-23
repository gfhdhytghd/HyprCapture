#pragma once

#include <filesystem>
#include <string>

namespace hyprcapture {

struct LaunchResult;

LaunchResult startRecordingFromRequestFile(const std::string& path);
LaunchResult stopRecording(const std::string& reason = "stopped");
bool isRecordingActive();
bool initializeRecordingStateServer(std::string* error = nullptr);
std::filesystem::path recordingStateSocketPath();
void shutdownRecording();

} // namespace hyprcapture
