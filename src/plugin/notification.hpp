#pragma once

#include <string>

namespace hyprcapture {

enum class NotificationLevel { Info, Warning, Error };

void notifyUser(const std::string& message, NotificationLevel level = NotificationLevel::Info, int timeoutMs = 3000);

} // namespace hyprcapture
