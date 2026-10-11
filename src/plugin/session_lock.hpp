#pragma once

#include <hyprland/src/managers/SessionLockManager.hpp>
#include "shared/session_lock.hpp"

namespace hyprcapture {

inline bool sessionLocked() {
    return sessionLocked(g_pSessionLockManager.get());
}

void initializeSessionLock();
void shutdownSessionLock();
std::string sessionLockGeneration();

} // namespace hyprcapture
