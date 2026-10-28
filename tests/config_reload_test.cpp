#undef NDEBUG
#include "plugin/config_reload.hpp"
#include <cassert>
#include <functional>
#include <memory>
#include <vector>

struct Listener {
    std::shared_ptr<std::function<void()>> callback;
    void reset() { callback.reset(); }
};
struct Signal {
    std::vector<std::weak_ptr<std::function<void()>>> callbacks;
    Listener listen(std::function<void()> callback) {
        auto stored = std::make_shared<std::function<void()>>(std::move(callback));
        callbacks.push_back(stored);
        return {stored};
    }
    void emit() {
        for (const auto& ref : callbacks)
            if (auto callback = ref.lock()) (*callback)();
    }
};
struct Events { Signal preReload, reloaded; };
struct Manager { int functions = 0; int values = 0; };
struct WeakManager {
    std::weak_ptr<Manager> value;
    bool operator==(const std::shared_ptr<Manager>& current) const { return value.lock() == current; }
    WeakManager& operator=(const std::shared_ptr<Manager>& current) { value = current; return *this; }
};

int main() {
    Events events;
    auto manager = std::make_shared<Manager>();
    WeakManager registered;
    int registrations = 0, rules = 0, updates = 0;
    const auto restore = [&] {
        hyprcapture::registerForConfigManager(registered, manager, [&] {
            assert(manager->functions == 0 && manager->values == 0); // no collisions
            manager->functions = 16;
            manager->values = 1;
            ++registrations;
        });
    };
    const auto installRule = [&] { rules = 1; ++updates; };
    restore();
    installRule();
    hyprcapture::ConfigReloadListeners<Listener> listeners;
    listeners.listen(events, restore, installRule);
    const auto reload = [&] {
        events.preReload.emit();
        assert(manager->functions == 16 && manager->values == 1); // available to config evaluation
        rules = 0; // fork clearAllRules()
        events.reloaded.emit();
        assert(rules == 1);
    };
    reload();
    reload();
    assert(registrations == 1 && updates == 3);
    auto old = std::weak_ptr(manager);
    manager.reset(); // full-reset must be able to close the old Lua state
    assert(old.expired());
    manager = std::make_shared<Manager>();
    reload();
    assert(registrations == 2 && updates == 4);
    reload();
    assert(registrations == 2); // normal reload after full-reset
    listeners.reset();
    manager->functions = 0;
    rules = 0;
    events.preReload.emit();
    events.reloaded.emit();
    assert(manager->functions == 0 && rules == 0 && updates == 5);
}
