#pragma once

#include "shared/config.hpp"

#include <QString>

namespace hyprcapture::ui {

bool showScreenshotNotification(const CaptureDefaults& defaults,
                                CaptureMode mode,
                                const FilenameMetadata& metadata,
                                const QString& savedPath);

} // namespace hyprcapture::ui
