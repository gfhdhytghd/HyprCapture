#include "plugin/audio_session.hpp"
#include <nlohmann/json.hpp>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace hyprcapture {
AudioSession::~AudioSession() {
    abandon();
    if (m_worker.joinable()) m_worker.join();
    if (m_reader >= 0) close(m_reader);
    if (m_writer >= 0) close(m_writer);
    // Failure/abandonment intentionally preserves recovery files.
}
SupervisedProcess AudioSession::spawn(const std::vector<std::string>& args, int input) {
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) return {.spawnError = error};
    if (input >= 0) posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO);
    else posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, m_writer, STDOUT_FILENO);
    std::vector<char*> env;
    for (auto& entry : m_environment) env.push_back(entry.data());
    env.push_back(nullptr);
    auto child = spawnSupervisedProcess(m_shell, args, env.data(), actions);
    posix_spawn_file_actions_destroy(&actions);
    return child;
}
bool AudioSession::start(const CaptureDefaults& defaults, const std::filesystem::path& video,
                         std::string helper, std::string shell, std::vector<std::string> environment, std::string& error) {
    auto pattern = (video.parent_path() / ".hyprcapture-sound-XXXXXX").string();
    if (!mkdtemp(pattern.data())) { error = "Cannot create audio recovery directory: " + std::string(strerror(errno)); return false; }
    m_directory = pattern; m_helper = std::move(helper); m_shell = std::move(shell); m_environment = std::move(environment);
    int control[2], events[2];
    if (pipe2(control, O_CLOEXEC) < 0) { error = strerror(errno); return false; }
    if (pipe2(events, O_CLOEXEC) < 0) { close(control[0]); close(control[1]); error = strerror(errno); return false; }
    m_input = control[1]; m_reader = events[0]; m_writer = events[1];
    fcntl(m_reader, F_SETFL, fcntl(m_reader, F_GETFL) | O_NONBLOCK);
    auto process = spawn({m_helper, "--sound-capture", toString(defaults.recordAudio), defaults.recordAudioOutput,
                          defaults.recordAudioInput, m_directory.string(), defaults.recordAudioMix,
                          std::to_string(defaults.recordAudioSystemGain), std::to_string(defaults.recordAudioMicGain), defaults.recordAudioEchoCancellation ? "1" : "0"}, control[0]);
    close(control[0]);
    if (process.spawnError) { error = strerror(process.spawnError); return false; }
    m_worker = std::thread([this, process]() mutable {
        (void)waitSupervisedProcess(process);
        m_captureExited.store(true);
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_finalize || m_abandon; });
            if (m_abandon) { m_finished.store(true); return; }
        }
        auto mux = spawn({m_helper, "--sound-finalize", m_directory.string(), m_video.string(),
                          std::to_string(m_firstFrameUs), m_format}, -1);
        m_success.store(mux.spawnError == 0 && waitSupervisedProcess(mux) == 0);
        m_finished.store(true);
    });
    return true;
}
void AudioSession::abandon() {
    stopCapture();
    { std::lock_guard lock(m_mutex); m_abandon = true; }
    m_cv.notify_one();
}
void AudioSession::stopCapture() {
    m_stopped = true;
    if (m_input >= 0) { close(m_input); m_input = -1; }
}
void AudioSession::finalize(const std::filesystem::path& video, std::int64_t firstFrameUs, const std::string& format) {
    stopCapture();
    { std::lock_guard lock(m_mutex); m_video = video; m_firstFrameUs = firstFrameUs; m_format = format; m_finalize = true; }
    m_cv.notify_one();
}
std::vector<std::string> AudioSession::messages() {
    std::vector<std::string> result;
    char bytes[4096];
    for (;;) {
        const auto n = read(m_reader, bytes, sizeof(bytes));
        if (n <= 0) break;
        m_buffer.append(bytes, n);
        if (m_buffer.size() > 32768) m_buffer.erase(0, m_buffer.size() - 32768);
    }
    for (auto end = m_buffer.find('\n'); end != std::string::npos; end = m_buffer.find('\n')) {
        auto event = nlohmann::json::parse(m_buffer.substr(0, end), nullptr, false);
        if (event.is_object() && event.contains("error") && event["error"].is_string()) result.push_back(event["error"].get<std::string>());
        m_buffer.erase(0, end + 1);
    }
    if (m_captureExited.load() && !m_stopped && !m_reportedExit) {
        result.emplace_back("Audio helper exited; video continues. Existing audio will be recovered on stop.");
        m_reportedExit = true;
    }
    return result;
}
}
