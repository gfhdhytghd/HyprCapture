#pragma once

#include <utility>

namespace hyprcapture {

// A weak owner distinguishes a replacement manager even if its address is reused.
// Do not keep the old config manager (and its Lua state) alive through full-reset.
template <typename WeakManager, typename Manager, typename Register>
void registerForConfigManager(WeakManager& previous, const Manager& current, Register&& registerFunctions) {
    if (!current || previous == current)
        return;
    registerFunctions();
    previous = current;
}

template <typename Listener>
struct ConfigReloadListeners {
    Listener preReload;
    Listener reloaded;

    template <typename Events, typename Before, typename After>
    void listen(Events& events, Before before, After after) {
        preReload = events.preReload.listen(std::move(before));
        reloaded = events.reloaded.listen(std::move(after));
    }

    void reset() {
        preReload.reset();
        reloaded.reset();
    }
};

} // namespace hyprcapture
