#pragma once

#include "shared/config.hpp"

#include <QString>

namespace hyprcapture::ui {

// A zero delivery timeout launches asynchronously (normal screenshot feedback).
// A positive timeout bounds each command's wait for delivery acknowledgement.
bool showNotification(NotificationBackend backend,
                      const QString& title,
                      const QString& body,
                      const QString& imagePath = {},
                      int deliveryTimeoutMs = 0);

bool showScreenshotNotification(const CaptureDefaults& defaults,
                                CaptureMode mode,
                                const FilenameMetadata& metadata,
                                const QString& savedPath);

} // namespace hyprcapture::ui
