#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace hyprcapture {

struct RecordingState {
    enum class Phase {
        Inactive,
        Recording,
        Finalizing,
    };

    Phase                                  phase = Phase::Inactive;
    std::string                            backend;
    std::string                            output;
    std::string                            mode;
    std::string                            format;
    std::chrono::steady_clock::time_point  startedAt;
    std::chrono::system_clock::time_point  startedAtWallClock;
    std::chrono::milliseconds              elapsedAtStop{0};
};

std::string recordingStateJson(const RecordingState& state);
std::filesystem::path defaultRecordingStateSocketPath();

class RecordingStateServer {
  public:
    RecordingStateServer() = default;
    ~RecordingStateServer();

    RecordingStateServer(const RecordingStateServer&) = delete;
    RecordingStateServer& operator=(const RecordingStateServer&) = delete;

    bool start(const std::filesystem::path& socketPath = {}, std::string* error = nullptr);
    void stop();

    void begin(std::string backend, std::filesystem::path output, std::string mode, std::string format);
    void beginFinalizing();
    void clear();

    [[nodiscard]] bool running() const;
    [[nodiscard]] std::filesystem::path socketPath() const;
    [[nodiscard]] std::string snapshotJson() const;

  private:
    void run();

    mutable std::mutex       m_mutex;
    RecordingState           m_state;
    std::filesystem::path    m_socketPath;
    int                      m_listenFd = -1;
    int                      m_wakeReadFd = -1;
    int                      m_wakeWriteFd = -1;
    unsigned long long       m_socketDevice = 0;
    unsigned long long       m_socketInode = 0;
    std::thread              m_thread;
};

} // namespace hyprcapture
