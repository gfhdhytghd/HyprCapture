#include "ui/i18n.hpp"
#include "ui/translations_catalog.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "i18n test failed: " << message << '\n';
        std::exit(1);
    }
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace hyprcapture::ui;
    const QByteArray oldEnvironment = qgetenv("HYPRCAPTURE_LANGUAGE");
    const bool hadEnvironment = qEnvironmentVariableIsSet("HYPRCAPTURE_LANGUAGE");
    qputenv("HYPRCAPTURE_LANGUAGE", "de_DE.UTF-8");
    require(installUiTranslations("auto") == "de", "environment language normalization");
    require(uiText("Save") == QString::fromUtf8("Speichern"), "environment translation");
    require(installUiTranslations("zh-Hant-HK") == "zh_TW", "Traditional Chinese script alias");
    require(uiText("Save") == QString::fromUtf8("儲存"), "Traditional Chinese translation");
    require(QCoreApplication::translate("AnnotationEditor", "Save") == uiText("Save"), "QObject contexts share catalog");
    require(uiText(QStringLiteral("follow-system")) == QString::fromUtf8("跟隨系統"), "option labels localize without changing option values");
    require(installUiTranslations("en") == "en", "explicit language overrides environment");
    require(uiText("Save") == "Save", "switching to English removes translator");
    require(installUiTranslations("unrecognized_language") == "en", "unsupported language falls back to English");
    require(uiText("Uncatalogued source") == "Uncatalogued source", "unknown source text survives");
    require(installUiTranslations("zh_CN") == "zh_CN", "Simplified Chinese explicit locale");
    require(uiText("Save") == QString::fromUtf8("保存"), "Simplified Chinese translation");

    const QJsonObject catalog = QJsonDocument::fromJson(QByteArray(detail::kUiTranslationCatalog)).object();
    const QJsonObject reference = catalog.value("zh_CN").toObject();
    require(reference.size() >= 100, "shared capture and annotation vocabulary is present");
    for (const auto& language : availableUiLanguages()) {
        if (language == "en")
            continue;
        require(installUiTranslations(language) == language, "every advertised locale installs");
        const QJsonObject messages = catalog.value(language).toObject();
        require(messages.keys() == reference.keys(), "all translated locales have complete shared vocabulary");
        for (auto it = messages.begin(); it != messages.end(); ++it) {
            require(!it.value().toString().isEmpty(), "translations are nonempty");
            require(uiText(it.key()) == it.value().toString(), "every embedded catalog message is reachable");
        }
    }
    require(installUiTranslations("es_MX") == "es", "regional locale selects base language");
    require(installUiTranslations("zh_HK") == "zh_TW", "Hong Kong selects Traditional Chinese");
    require(installUiTranslations("C") == "en", "C locale uses English");
    if (hadEnvironment)
        qputenv("HYPRCAPTURE_LANGUAGE", oldEnvironment);
    else
        qunsetenv("HYPRCAPTURE_LANGUAGE");
    installUiTranslations("en");
    return 0;
}
