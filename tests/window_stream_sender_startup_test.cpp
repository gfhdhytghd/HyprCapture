#include "plugin/window_stream_sender.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <pthread.h>
#include <thread>

namespace {
struct Invocation {
    void* (*entry)(void*);
    void* argument;
    std::atomic<bool> finished = false;
};
std::atomic<unsigned> completedWorkers = 0;
} // namespace

// Interpose on the dynamically linked std::thread implementation too. Force a
// legal schedule: the empty-path worker finishes before pthread_create returns,
// while the sender constructor is still initializing its jthread member.
extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                              void* (*entry)(void*), void* argument) noexcept {
    using CreateThread = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
    static const auto createThread = reinterpret_cast<CreateThread>(dlsym(RTLD_NEXT, "pthread_create"));
    assert(createThread);
    Invocation invocation{entry, argument};
    const int result = createThread(thread, attributes, [](void* raw) -> void* {
        auto* call = static_cast<Invocation*>(raw);
        void* returned = call->entry(call->argument);
        call->finished.store(true, std::memory_order_release);
        return returned;
    }, &invocation);
    if (result == 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!invocation.finished.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) std::abort();
            std::this_thread::yield();
        }
        completedWorkers.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

int main() {
    using namespace hyprcapture;
    WindowStreamSender sender({.socketPath = ""});
    assert(completedWorkers.load() == 1); // Ensure the schedule was exercised.
    assert(sender.state() == WindowStreamSenderState::Disconnected);
    assert(sender.failure() == WindowStreamSenderFailure::Socket);
    assert(sender.droppedFrames() == 0);
    sender.stop();
}
