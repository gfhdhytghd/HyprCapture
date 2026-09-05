#include "plugin/window_stream_sender.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <string_view>

using namespace hyprcapture;
using namespace std::chrono_literals;

namespace {

struct Socket {
    int fd = -1;
    ~Socket() { if (fd >= 0) close(fd); }
    Socket() = default;
    explicit Socket(int next) : fd(next) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) close(fd);
            fd = std::exchange(other.fd, -1);
        }
        return *this;
    }
};

WindowStreamFrameMetadata metadata(std::uint64_t sequence) {
    return {.sequence = sequence, .captureMonotonicNs = 99 + sequence, .geometryEpoch = 1, .logicalX = 0, .logicalY = 0, .logicalWidth = 2, .logicalHeight = 1, .pixelWidth = 2, .pixelHeight = 1, .stride = 8, .payloadBytes = 8};
}

std::uint64_t monotonicNowNs() {
    timespec now{};
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(now.tv_nsec);
}

WindowStreamFrameMetadata interopMetadata(std::uint64_t sequence) {
    return {
        .sequence = sequence,
        .captureMonotonicNs = monotonicNowNs(),
        .geometryEpoch = 1,
        .logicalX = 0,
        .logicalY = 0,
        .logicalWidth = 1,
        .logicalHeight = 0.5,
        .pixelWidth = 2,
        .pixelHeight = 1,
        .stride = 8,
        .payloadBytes = 8,
    };
}

sockaddr_un addressFor(const std::string& path, socklen_t& length) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    return address;
}

Socket listenerAt(const std::string& path) {
    Socket listener{socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)};
    assert(listener.fd >= 0);
    socklen_t length = 0;
    const auto address = addressFor(path, length);
    assert(bind(listener.fd, reinterpret_cast<const sockaddr*>(&address), length) == 0);
    assert(listen(listener.fd, 2) == 0);
    return listener;
}

std::string tempSocketPath() {
    std::array<char, 64> directory{};
    std::strcpy(directory.data(), "/tmp/hyprcapture-sender.XXXXXX");
    assert(mkdtemp(directory.data()));
    return std::string(directory.data()) + "/stream.sock";
}

Socket acceptClient(Socket& listener) {
    Socket client{accept4(listener.fd, nullptr, nullptr, SOCK_CLOEXEC)};
    assert(client.fd >= 0);
    return client;
}

void receiveAndVerify(Socket& client, std::uint64_t expectedSequence) {
    std::array<unsigned char, WINDOW_STREAM_FRAME_HEADER_BYTES> header{};
    alignas(cmsghdr) std::array<unsigned char, CMSG_SPACE(sizeof(int))> control{};
    iovec iov{.iov_base = header.data(), .iov_len = header.size()};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    assert(recvmsg(client.fd, &message, 0) == static_cast<ssize_t>(header.size()));
    assert(!(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)));
    const auto decoded = decodeWindowStreamFrameHeader(header.data(), header.size());
    assert(decoded && decoded->sequence == expectedSequence);
    const auto* cmsg = CMSG_FIRSTHDR(&message);
    assert(cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS && cmsg->cmsg_len == CMSG_LEN(sizeof(int)));
    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    assert(fd >= 0);
    assert((fcntl(fd, F_GET_SEALS) & (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) == (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL));
    std::array<unsigned char, 8> pixels{};
    assert(pread(fd, pixels.data(), pixels.size(), 0) == static_cast<ssize_t>(pixels.size()));
    assert(pixels[0] == static_cast<unsigned char>(expectedSequence));
    close(fd);
}

void sendsSealedFrame() {
    const auto path = tempSocketPath();
    auto listener = listenerAt(path);
    WindowStreamSender sender({.socketPath = path, .connectDeadline = 300ms, .retryInterval = 5ms});
    assert(sender.submit(metadata(7), std::vector<unsigned char>(8, 7)));
    auto client = acceptClient(listener);
    receiveAndVerify(client, 7);
    sender.stop();
    unlink(path.c_str());
    rmdir(std::filesystem::path(path).parent_path().c_str());
}

void connectsBeforeFirstFrame() {
    const auto path = tempSocketPath();
    auto listener = listenerAt(path);
    WindowStreamSender sender({.socketPath = path, .connectDeadline = 300ms, .retryInterval = 5ms});
    const auto deadline = std::chrono::steady_clock::now() + 300ms;
    while (sender.state() == WindowStreamSenderState::Idle || sender.state() == WindowStreamSenderState::Connecting) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
    assert(sender.state() == WindowStreamSenderState::Connected);
    auto client = acceptClient(listener);
    sender.stop();
    unlink(path.c_str());
    rmdir(std::filesystem::path(path).parent_path().c_str());
}

void replacementWinsWhileEndpointIsAbsent() {
    const auto path = tempSocketPath();
    WindowStreamSender sender({.socketPath = path, .connectDeadline = 500ms, .retryInterval = 5ms});
    assert(sender.submit(metadata(1), std::vector<unsigned char>(8, 1)));
    std::this_thread::sleep_for(25ms);
    assert(sender.submit(metadata(2), std::vector<unsigned char>(8, 2)));
    auto listener = listenerAt(path);
    auto client = acceptClient(listener);
    receiveAndVerify(client, 2);
    sender.stop();
    unlink(path.c_str());
    rmdir(std::filesystem::path(path).parent_path().c_str());
}

void oneConnectionCarriesThreeFrames() {
    const auto path = tempSocketPath();
    auto listener = listenerAt(path);
    WindowStreamSender sender({.socketPath = path, .connectDeadline = 300ms, .retryInterval = 5ms});
    assert(sender.submit(metadata(11), std::vector<unsigned char>(8, 11)));
    auto client = acceptClient(listener);
    receiveAndVerify(client, 11);
    assert(sender.submit(metadata(12), std::vector<unsigned char>(8, 12)));
    receiveAndVerify(client, 12);
    assert(sender.submit(metadata(13), std::vector<unsigned char>(8, 13)));
    receiveAndVerify(client, 13);
    sender.stop();
    unlink(path.c_str());
    rmdir(std::filesystem::path(path).parent_path().c_str());
}

void eagainDropsThenDrainingAllowsTheSameConnectionToRecover() {
    const auto path = tempSocketPath();
    auto listener = listenerAt(path);
    WindowStreamSender sender({.socketPath = path, .connectDeadline = 300ms, .retryInterval = 1ms, .socketSendBufferBytes = 1024});
    assert(sender.submit(metadata(20), std::vector<unsigned char>(8, 20)));
    auto client = acceptClient(listener);
    for (std::uint64_t sequence = 21; sequence < 180; ++sequence) {
        assert(sender.submit(metadata(sequence), std::vector<unsigned char>(8, static_cast<unsigned char>(sequence))));
        std::this_thread::sleep_for(1ms);
        if (sender.failure() == WindowStreamSenderFailure::SendWouldBlock)
            break;
    }
    assert(sender.failure() == WindowStreamSenderFailure::SendWouldBlock);
    assert(sender.state() == WindowStreamSenderState::Connected);
    const int flags = fcntl(client.fd, F_GETFL);
    assert(fcntl(client.fd, F_SETFL, flags | O_NONBLOCK) == 0);
    std::array<unsigned char, WINDOW_STREAM_FRAME_HEADER_BYTES> discarded{};
    while (recv(client.fd, discarded.data(), discarded.size(), 0) > 0) {}
    assert(fcntl(client.fd, F_SETFL, flags) == 0);
    assert(sender.submit(metadata(250), std::vector<unsigned char>(8, 250)));
    receiveAndVerify(client, 250);
    assert(sender.state() == WindowStreamSenderState::Connected);
    sender.stop();
    unlink(path.c_str());
    rmdir(std::filesystem::path(path).parent_path().c_str());
}

void slowConsumerStopIsBounded() {
    const auto path = tempSocketPath();
    auto listener = listenerAt(path);
    WindowStreamSender sender({.socketPath = path, .connectDeadline = 2s, .retryInterval = 50ms});
    assert(sender.submit(metadata(3), std::vector<unsigned char>(8, 3)));
    std::this_thread::sleep_for(10ms);
    const auto started = std::chrono::steady_clock::now();
    sender.stop();
    assert(std::chrono::steady_clock::now() - started < 250ms);
    unlink(path.c_str());
    rmdir(std::filesystem::path(path).parent_path().c_str());
}

int runInterop(const std::string& socketPath) {
    std::cout << "interop sender pid=" << getpid()
              << " fixture=HCSF/v1 seq=1,2,3 epoch=1 logical=0,0,1,0.5 pixels=2x1/stride8/RGBA"
              << " seq1=[1,17,33,255,49,65,81,255] seq2=[2,18,34,255,50,66,82,255]"
              << " seq3=[3,19,35,255,51,67,83,255]"
              << std::endl;
    WindowStreamSender sender({.socketPath = socketPath, .connectDeadline = 10s, .retryInterval = 10ms});
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        const std::vector<unsigned char> pixels{
            static_cast<unsigned char>(sequence),
            static_cast<unsigned char>(sequence + 16),
            static_cast<unsigned char>(sequence + 32),
            255,
            static_cast<unsigned char>(sequence + 48),
            static_cast<unsigned char>(sequence + 64),
            static_cast<unsigned char>(sequence + 80),
            255,
        };
        if (!sender.submit(interopMetadata(sequence), pixels)) {
            std::cerr << "interop submit failed sequence=" << sequence << " failure=" << static_cast<int>(sender.failure()) << '\n';
            sender.stop();
            return 1;
        }
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (sender.state() == WindowStreamSenderState::Connecting || sender.state() == WindowStreamSenderState::Idle) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::cerr << "interop connect timeout\n";
                sender.stop();
                return 1;
            }
            std::this_thread::sleep_for(5ms);
        }
        if (sender.state() != WindowStreamSenderState::Connected) {
            std::cerr << "interop disconnected failure=" << static_cast<int>(sender.failure()) << '\n';
            sender.stop();
            return 1;
        }
        std::this_thread::sleep_for(100ms);
    }
    const auto waitUntil = std::chrono::steady_clock::now() + 1s;
    while (sender.state() == WindowStreamSenderState::Connected && std::chrono::steady_clock::now() < waitUntil)
        std::this_thread::sleep_for(10ms);
    sender.stop();
    std::cout << "interop sender complete\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--interop")
        return runInterop(argv[2]);
    if (argc != 1) {
        std::cerr << "usage: " << argv[0] << " [--interop <socketpath>]\n";
        return 2;
    }
    sendsSealedFrame();
    connectsBeforeFirstFrame();
    replacementWinsWhileEndpointIsAbsent();
    oneConnectionCarriesThreeFrames();
    eagainDropsThenDrainingAllowsTheSameConnectionToRecover();
    slowConsumerStopIsBounded();
    std::cout << "window stream sender tests passed\n";
}
