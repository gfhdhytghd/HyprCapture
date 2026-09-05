#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace hyprcapture {
struct WindowGpuPacket {
    std::array<unsigned char, 232> header{};
    int imageFd = -1;
    int fenceFd = -1;
    WindowGpuPacket() = default;
    ~WindowGpuPacket();
    WindowGpuPacket(WindowGpuPacket&&) noexcept;
    WindowGpuPacket& operator=(WindowGpuPacket&&) noexcept;
    WindowGpuPacket(const WindowGpuPacket&) = delete;
    WindowGpuPacket& operator=(const WindowGpuPacket&) = delete;
};

enum class WindowGpuSenderState { Connecting, Ready, Busy, Retired };

// The caller keeps the source framebuffer immutable while Busy. Retired means
// it must discard that allocation, not render into it or reconnect/reuse it.
// No GL resources or filesystem operations occur in submit(). The supplied
// path must already pass the plugin's private socket path validation.
class WindowGpuSender {
  public:
    explicit WindowGpuSender(std::string privateSocketPath);
    ~WindowGpuSender();
    WindowGpuSender(const WindowGpuSender&) = delete;
    WindowGpuSender& operator=(const WindowGpuSender&) = delete;
    bool submit(WindowGpuPacket&&) noexcept;
    WindowGpuSenderState state() const noexcept { return m_state.load(std::memory_order_acquire); }
  private:
    void run(std::stop_token);
    std::string m_path;
    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::optional<WindowGpuPacket> m_pending;
    std::uint64_t m_lastSequence = 0;
    std::uint64_t m_lastEpoch = 0;
    std::atomic<WindowGpuSenderState> m_state{WindowGpuSenderState::Connecting};
    std::jthread m_worker;
};
} // namespace hyprcapture
