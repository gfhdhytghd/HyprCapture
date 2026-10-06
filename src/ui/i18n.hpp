#pragma once

#include <QString>
#include <QStringList>

namespace hyprcapture::ui {

// Install before constructing widgets. Explicit language overrides the environment;
// empty, "auto", and "system" use HYPRCAPTURE_LANGUAGE, then the system locale.
// Returns the selected catalog locale. Unknown locales fall back to English.
QString installUiTranslations(const QString& language = {});
QString activeUiLanguage();
QStringList availableUiLanguages();

// Display-only translation: preserve protocol/config values and translate their
// labels instead. These also share the catalog used by QObject::tr().
QString uiText(const char* sourceText);
QString uiText(const QString& sourceText);

} // namespace hyprcapture::ui
