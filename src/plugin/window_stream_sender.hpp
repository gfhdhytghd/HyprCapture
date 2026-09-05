#pragma once

#include "plugin/window_stream.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hyprcapture {

enum class WindowStreamSenderState : std::uint8_t {
    Idle,
    Connecting,
    Connected,
    Disconnected,
    Stopped,
};

enum class WindowStreamSenderFailure : std::uint8_t {
    None,
    InvalidFrame,
    ConnectDeadline,
    Socket,
    SendWouldBlock,
    Send,
};

struct WindowStreamSenderConfig {
    std::string socketPath;
    std::chrono::milliseconds connectDeadline{250};
    std::chrono::milliseconds retryInterval{10};
    // Test/deployment tuning only; zero preserves the kernel default. Linux
    // may clamp and/or double this value, so it is not an exact frame limit.
    int socketSendBufferBytes = 0;
};

// A bounded hand-off from the compositor thread to an I/O-only worker. It
// intentionally knows only window_stream.hpp metadata, never Hyprland types.
class WindowStreamSender {
  public:
    explicit WindowStreamSender(WindowStreamSenderConfig config);
    ~WindowStreamSender();
    WindowStreamSender(const WindowStreamSender&) = delete;
    WindowStreamSender& operator=(const WindowStreamSender&) = delete;

    // Does no socket or filesystem I/O. A contended compositor call simply
    // drops this frame rather than waiting behind the worker.
    [[nodiscard]] bool submit(WindowStreamFrameMetadata metadata, std::vector<unsigned char> rgba) noexcept;
    void stop();

    [[nodiscard]] WindowStreamSenderState state() const noexcept;
    [[nodiscard]] WindowStreamSenderFailure failure() const noexcept;
    [[nodiscard]] std::uint64_t droppedFrames() const noexcept;

  private:
    struct Frame {
        WindowStreamFrameMetadata metadata;
        std::vector<unsigned char> rgba;
    };

    void run(std::stop_token stopToken);
    std::optional<Frame> takeLatest();
    bool connectUntil(std::stop_token stopToken, std::chrono::steady_clock::time_point deadline, int& socket);
    enum class SendResult { Sent, WouldBlock, Disconnected };
    SendResult sendFrame(const Frame& frame, int socket);

    WindowStreamSenderConfig m_config;
    std::mutex m_mutex;
    std::condition_variable_any m_ready;
    std::optional<Frame> m_latest;
    std::jthread m_worker;
    std::atomic<WindowStreamSenderState> m_state{WindowStreamSenderState::Idle};
    std::atomic<WindowStreamSenderFailure> m_failure{WindowStreamSenderFailure::None};
    std::atomic<std::uint64_t> m_droppedFrames{0};
};

} // namespace hyprcapture
