#pragma once
#include "shared/config.hpp"
#include "shared/supervised_process.hpp"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace hyprcapture {
// Owns an isolated capture process and its recovery files. All waits/muxing run
// on the worker; the compositor only closes a pipe and polls small status events.
class AudioSession {
public:
    ~AudioSession();
    bool start(const CaptureDefaults&, const std::filesystem::path& video,
               std::string helper, std::string shell, std::vector<std::string> environment, std::string& error);
    void stopCapture();
    void abandon();
    void finalize(const std::filesystem::path& video, std::int64_t firstFrameUs, const std::string& format);
    std::vector<std::string> messages();
    bool finished() const { return m_finished.load(); }
    bool succeeded() const { return m_success.load(); }
    const std::filesystem::path& directory() const { return m_directory; }
private:
    SupervisedProcess spawn(const std::vector<std::string>& args, int input);
    std::filesystem::path m_directory, m_video;
    std::string m_helper, m_shell, m_format, m_buffer;
    std::vector<std::string> m_environment;
    int m_input = -1, m_reader = -1, m_writer = -1;
    std::int64_t m_firstFrameUs = 0;
    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_finalize = false, m_abandon = false, m_stopped = false, m_reportedExit = false;
    std::atomic_bool m_captureExited = false, m_finished = false, m_success = false;
};
}
