#pragma once

#include "shared/config.hpp"

namespace hyprcapture::ui {
// Only interactive preferences are persisted; launch/output/security settings stay configured.
bool restoreSettings(CaptureDefaults& defaults);
bool saveSettings(const CaptureDefaults& defaults);
void restoreAecPreferences(CaptureDefaults& defaults);
void saveAecPreferences(const CaptureDefaults& defaults);
}
