#pragma once
#include "shared/config.hpp"
namespace hyprcapture::shot {
inline void applyOverrides(CaptureDefaults& defaults) {
    defaults.save = false;
    defaults.clipboard = false;
    defaults.showThumbnail = false;
    defaults.screenshotNotification = false;
    defaults.confirmBeforeCapture = false;
    defaults.rememberSettings = false;
    defaults.screenshotFormat = "png";
    defaults.showHdrControl = false;
}
}
