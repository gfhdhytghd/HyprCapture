#include "ui/i18n.hpp"
#include "ui/translations_catalog.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QLocale>
#include <QPointer>
#include <QTranslator>

namespace hyprcapture::ui {
namespace {

QString selectedLanguage = QStringLiteral("en");
QPointer<QTranslator> installedCatalog;
QPointer<QTranslator> installedQtCatalog;

bool automaticLanguage(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    return normalized.isEmpty() || normalized == QLatin1String("auto") || normalized == QLatin1String("system");
}

QString catalogLanguage(QString value) {
    value = value.trimmed().replace(QLatin1Char('-'), QLatin1Char('_'));
    value = value.section(QLatin1Char('.'), 0, 0).section(QLatin1Char('@'), 0, 0).toLower();
    if (value == QLatin1String("c") || value == QLatin1String("posix"))
        return QStringLiteral("en");
    const QString base = value.section(QLatin1Char('_'), 0, 0);
    if (base == QLatin1String("zh")) {
        const auto subtags = value.split(QLatin1Char('_'));
        return subtags.contains(QStringLiteral("hant")) || subtags.contains(QStringLiteral("tw")) ||
                subtags.contains(QStringLiteral("hk")) || subtags.contains(QStringLiteral("mo"))
            ? QStringLiteral("zh_TW") : QStringLiteral("zh_CN");
    }
    for (const auto& supported : availableUiLanguages()) {
        if (base == supported.toLower())
            return supported;
    }
    return QStringLiteral("en");
}

class UiTranslator final : public QTranslator {
public:
    explicit UiTranslator(const QString& language, QObject* parent)
        : QTranslator(parent) {
        const QJsonObject catalogs = QJsonDocument::fromJson(QByteArray(detail::kUiTranslationCatalog)).object();
        m_messages = catalogs.value(language).toObject();
    }

    bool isEmpty() const override { return m_messages.isEmpty(); }

    QString translate(const char* context, const char* sourceText,
                      const char* disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(context);
        Q_UNUSED(disambiguation);
        // These catalogs contain only nonplural shared UI labels. Let another
        // translator handle plural messages or absent source strings.
        if (!sourceText || n >= 0)
            return {};
        const auto text = m_messages.value(QString::fromUtf8(sourceText));
        return text.isString() ? text.toString() : QString{};
    }

private:
    QJsonObject m_messages;
};

void removeCatalog(QPointer<QTranslator>& translator) {
    if (!translator)
        return;
    QCoreApplication::removeTranslator(translator);
    delete translator.data();
    translator.clear();
}

} // namespace

QStringList availableUiLanguages() {
    return {QStringLiteral("en"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW"),
            QStringLiteral("ja"), QStringLiteral("de"), QStringLiteral("fr"),
            QStringLiteral("es"), QStringLiteral("ko")};
}

QString installUiTranslations(const QString& language) {
    QString requested = language;
    if (automaticLanguage(requested)) {
        requested = qEnvironmentVariable("HYPRCAPTURE_LANGUAGE");
        if (automaticLanguage(requested))
            requested = QLocale::system().name();
    }
    selectedLanguage = catalogLanguage(requested);
    auto* application = QCoreApplication::instance();
    if (!application)
        return selectedLanguage;

    removeCatalog(installedCatalog);
    removeCatalog(installedQtCatalog);
    if (selectedLanguage == QLatin1String("en"))
        return selectedLanguage;

    // Qt's standard dialogs use their own contexts. Load the distro's optional
    // qtbase catalog first, so HyprCapture's shared labels take precedence.
    auto* qtCatalog = new QTranslator(application);
    if (qtCatalog->load(QLocale(selectedLanguage), QStringLiteral("qtbase"), QStringLiteral("_"),
                        QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QCoreApplication::installTranslator(qtCatalog);
        installedQtCatalog = qtCatalog;
    } else {
        delete qtCatalog;
    }
    auto* catalog = new UiTranslator(selectedLanguage, application);
    if (QCoreApplication::installTranslator(catalog))
        installedCatalog = catalog;
    else
        delete catalog;
    return selectedLanguage;
}

QString activeUiLanguage() { return selectedLanguage; }

QString uiText(const char* sourceText) {
    return QCoreApplication::translate("HyprCapture", sourceText);
}

QString uiText(const QString& sourceText) {
    const QByteArray source = sourceText.toUtf8();
    return uiText(source.constData());
}

} // namespace hyprcapture::ui
