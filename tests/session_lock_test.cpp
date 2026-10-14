#include "shared/session_lock.hpp"

#include <cstdlib>
#include <iostream>

namespace {
struct Manager {
    bool locked = false;
    int queries = 0;
    bool isSessionLocked() { ++queries; return locked; }
};
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
}

int main() {
    using hyprcapture::sessionLocked;
    require(!sessionLocked(static_cast<Manager*>(nullptr)), "absent manager is safe to query");
    Manager manager;
    require(!sessionLocked(&manager), "unlocked session allows input");
    manager.locked = true;
    require(sessionLocked(&manager), "lock is checked immediately rather than cached");
    manager.locked = false;
    require(!sessionLocked(&manager) && manager.queries == 3, "subsequent sessions query current lock state");
    using hyprcapture::unlockedSessionGeneration;
    require(unlockedSessionGeneration("unlocked:123") == "123", "new helper adopts unlocked generation");
    require(unlockedSessionGeneration("unlocked:123", "123") == "123", "child accepts its capture generation");
    require(!unlockedSessionGeneration("unlocked:124", "123"), "slow child is rejected after lock/unlock or reload");
    require(!unlockedSessionGeneration("locked") && !unlockedSessionGeneration("unlocked:"), "locked/missing generation fails closed");
    require(!unlockedSessionGeneration(std::string_view("unlocked:1\0", 11)), "embedded nul generation is rejected");
    require(!unlockedSessionGeneration(std::string(65, 'x')), "oversized handshake is rejected");
}
