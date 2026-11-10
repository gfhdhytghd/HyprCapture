#pragma once
#include "shared/thumbnail_config.hpp"
#include <QColor>
#include <QCommandLineParser>
#include <QRect>
#include <QCursor>
#include <QGuiApplication>
#include <QPalette>
#include <QPixmap>
#include <QScreen>
#include <QString>
#include <algorithm>
#include <cmath>

namespace hyprcapture::ui::thumbnail {
inline constexpr int kThumbnailMaxWidth = 180;
inline constexpr int kThumbnailMaxHeight = 120;
inline constexpr qreal kThumbnailMaxDevicePixelRatio = 4.0;
inline constexpr int kThumbnailScreenMargin = 24;
inline QString imageStyleSheet() {
    return QStringLiteral("#thumbnailImage { background: transparent; border: none; }");
}
inline QString menuStyleSheet(const QPalette& palette) {
    const auto bg = palette.color(QPalette::Window);
    const auto fg = palette.color(QPalette::WindowText);
    const auto highlight = palette.color(QPalette::Highlight);
    return QStringLiteral(
        "#thumbnailMenu { background: rgba(%1,%2,%3,242); border: 1px solid rgba(%4,%5,%6,90); border-radius: 7px; }"
        "#thumbnailMenu QLabel { color: rgba(%4,%5,%6,255); background: transparent; border: none; }"
        "#thumbnailMenu QPushButton { color: rgba(%4,%5,%6,255); background: transparent; padding: 7px 10px; border: none; border-radius: 5px; text-align: left; }"
        "#thumbnailMenu QPushButton:hover { background: rgba(%7,%8,%9,75); }")
        .arg(bg.red()).arg(bg.green()).arg(bg.blue()).arg(fg.red()).arg(fg.green()).arg(fg.blue()).arg(highlight.red()).arg(highlight.green()).arg(highlight.blue());
}
inline void readStyleOptions(const QCommandLineParser &parser, CaptureDefaults &defaults) {
    const auto integer = [&](const char *key, std::int64_t fallback) {
        bool ok = false;
        const auto value = parser.value(key).toLongLong(&ok);
        return ok ? static_cast<std::int64_t>(value) : fallback;
    };
    defaults.thumbnailMaxWidth = integer("thumbnail-max-width", 180);
    defaults.thumbnailMaxHeight = integer("thumbnail-max-height", 120);
    defaults.thumbnailMargin = integer("thumbnail-margin", 24);
    defaults.thumbnailRounding = integer("thumbnail-rounding", 7);
    defaults.thumbnailOpacity = integer("thumbnail-opacity", 100);
    defaults.thumbnailBorderSize = integer("thumbnail-border-size", 1);
    defaults.thumbnailPosition = parser.value("thumbnail-position").size() <= 4096 ? parser.value("thumbnail-position").toStdString() : "";
    defaults.thumbnailBorderColor = parser.value("thumbnail-border-color").size() <= 4096 ? parser.value("thumbnail-border-color").toStdString() : "";
    defaults.thumbnailBackground = parser.value("thumbnail-background").size() <= 4096 ? parser.value("thumbnail-background").toStdString() : "";
    defaults.thumbnailForeground = parser.value("thumbnail-foreground").size() <= 4096 ? parser.value("thumbnail-foreground").toStdString() : "";
    defaults.thumbnailAccent = parser.value("thumbnail-accent").size() <= 4096 ? parser.value("thumbnail-accent").toStdString() : "";
    defaults.thumbnailAnimation = parser.value("thumbnail-animation") != "0";
    defaults = normalizedThumbnailDefaults(std::move(defaults));
}

// QColor's eight-digit # syntax is ARGB, whereas these options use RGBA.
// Accept only these four formats; never pass config strings to a stylesheet.
inline QColor parseColor(const QString &input) {
    const auto value = input.trimmed();
    QString digits;
    bool argb = false;
    if (value.startsWith('#') && (value.size() == 7 || value.size() == 9))
        digits = value.mid(1);
    else if (value.startsWith("rgba(", Qt::CaseInsensitive) && value.endsWith(')') && value.size() == 14)
        digits = value.mid(5, 8);
    else if (value.startsWith("0x", Qt::CaseInsensitive) && value.size() == 10) {
        digits = value.mid(2);
        argb = true;
    } else
        return {};
    for (const auto c : digits) {
        const auto ch = c.unicode();
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
            return {};
    }
    bool ok = false;
    const auto number = digits.toUInt(&ok, 16);
    if (!ok)
        return {};
    if (digits.size() == 6)
        return QColor::fromRgb(number);
    if (argb)
        return QColor::fromRgba(number);
    return QColor((number >> 24) & 255, (number >> 16) & 255, (number >> 8) & 255, number & 255);
}

inline QColor colorOr(const std::string &input, const QColor &fallback) {
    // Bound before allocating a QString, including callers outside the CLI path.
    if (input.size() > 4096)
        return fallback;
    const auto color = parseColor(QString::fromStdString(input));
    return color.isValid() ? color : fallback;
}

inline Qt::Alignment positionAlignment(const CaptureDefaults &defaults) {
    const auto position = normalizedThumbnailDefaults(defaults).thumbnailPosition;
    return (position.ends_with("left") ? Qt::AlignLeft : Qt::AlignRight) | (position.starts_with("top") ? Qt::AlignTop : Qt::AlignBottom);
}

// The margin belongs inside the card so default layer geometry stays identical.
inline QRect cardImageRect(const QSize &imageSize, const CaptureDefaults &raw) {
    const auto defaults = normalizedThumbnailDefaults(raw);
    const auto alignment = positionAlignment(defaults);
    return QRect(QPoint(alignment.testFlag(Qt::AlignLeft) ? defaults.thumbnailMargin : 0, alignment.testFlag(Qt::AlignTop) ? defaults.thumbnailMargin : 0),
                 imageSize);
}

inline QSize maximumSize(const CaptureDefaults &raw) {
    const auto defaults = normalizedThumbnailDefaults(raw);
    return QSize(defaults.thumbnailMaxWidth, defaults.thumbnailMaxHeight);
}

inline QString resultStyleSheet(const QPalette &palette, const CaptureDefaults &raw = {}) {
    const auto defaults = normalizedThumbnailDefaults(raw);
    const auto alpha = [](QColor color, int value) {
        color.setAlpha(value);
        return color;
    };
    const QColor bg = colorOr(defaults.thumbnailBackground, alpha(palette.color(QPalette::Window), 242));
    const QColor fg = colorOr(defaults.thumbnailForeground, alpha(palette.color(QPalette::WindowText), 255));
    const QColor highlight = colorOr(defaults.thumbnailAccent, alpha(palette.color(QPalette::Highlight), 75));
    const QColor border = colorOr(defaults.thumbnailBorderColor, alpha(fg, 90));
    return imageStyleSheet() +
           QStringLiteral("#thumbnail { background: transparent; border: none; }"
                          "#thumbnailMenu { background: rgba(%1,%2,%3,%10); border: %18px solid rgba(%15,%16,%17,%11); border-radius: %19px; }"
                          "#thumbnailOpenWithMenu { background: rgba(%1,%2,%3,%10); border: %18px solid rgba(%15,%16,%17,%11); border-radius: %19px; }"
                          "#thumbnailMenu QPushButton { color: rgba(%4,%5,%6,%12); background: transparent; padding: 7px 10px; border: none; border-radius: "
                          "%20px; text-align: left; }"
                          "#thumbnailOpenWithMenu QPushButton { color: rgba(%4,%5,%6,%12); background: transparent; padding: 7px 10px; border: none; "
                          "border-radius: %20px; text-align: left; }"
                          "#thumbnailMenu QPushButton:disabled { color: rgba(%4,%5,%6,%13); }"
                          "#thumbnailMenu QPushButton:hover { background: rgba(%7,%8,%9,%14); }"
                          "#thumbnailOpenWithMenu QPushButton:hover { background: rgba(%7,%8,%9,%14); }")
               .arg(bg.red())
               .arg(bg.green())
               .arg(bg.blue())
               .arg(fg.red())
               .arg(fg.green())
               .arg(fg.blue())
               .arg(highlight.red())
               .arg(highlight.green())
               .arg(highlight.blue())
               .arg(bg.alpha())
               .arg(border.alpha())
               .arg(fg.alpha())
               .arg(qRound(fg.alpha() * 100.0 / 255))
               .arg(highlight.alpha())
               .arg(border.red())
               .arg(border.green())
               .arg(border.blue())
               .arg(defaults.thumbnailBorderSize)
               .arg(defaults.thumbnailRounding)
               .arg(std::max<std::int64_t>(0, defaults.thumbnailRounding - 2));
}

inline qreal targetDevicePixelRatio(const QPixmap& pixmap, const QScreen* targetScreen) {
    qreal dpr = std::max<qreal>(1.0, pixmap.devicePixelRatio());
    const QScreen* screen = targetScreen;
    if (!screen)
        screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen)
        dpr = std::max(dpr, screen->devicePixelRatio());
    return std::clamp(dpr, qreal{1.0}, kThumbnailMaxDevicePixelRatio);
}

inline QPixmap scaledPixmap(const QPixmap &pixmap, const QScreen *targetScreen, QSize maximum = QSize(kThumbnailMaxWidth, kThumbnailMaxHeight)) {
    maximum = maximum.boundedTo(QSize(1024, 1024)).expandedTo(QSize(48, 48));
    if (pixmap.isNull())
        return {};

    const QSizeF logicalPixmapSize = pixmap.deviceIndependentSize();
    QSize targetLogicalSize(std::max(1, static_cast<int>(std::ceil(logicalPixmapSize.width()))),
                            std::max(1, static_cast<int>(std::ceil(logicalPixmapSize.height()))));
    if (logicalPixmapSize.width() > maximum.width() || logicalPixmapSize.height() > maximum.height())
        targetLogicalSize = targetLogicalSize.scaled(maximum, Qt::KeepAspectRatio);
    targetLogicalSize = targetLogicalSize.expandedTo(QSize(1, 1));

    qreal dpr = targetDevicePixelRatio(pixmap, targetScreen);
    dpr = std::min(dpr, static_cast<qreal>(pixmap.width()) / targetLogicalSize.width());
    dpr = std::min(dpr, static_cast<qreal>(pixmap.height()) / targetLogicalSize.height());
    dpr = std::clamp(dpr, qreal{1.0}, kThumbnailMaxDevicePixelRatio);

    const QSize targetPhysicalSize(std::max(1, static_cast<int>(std::ceil(targetLogicalSize.width() * dpr))),
                                   std::max(1, static_cast<int>(std::ceil(targetLogicalSize.height() * dpr))));

    QPixmap scaledPixmap = pixmap;
    if (scaledPixmap.size() != targetPhysicalSize || !qFuzzyCompare(scaledPixmap.devicePixelRatio(), dpr)) {
        scaledPixmap = scaledPixmap.scaled(targetPhysicalSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaledPixmap.setDevicePixelRatio(dpr);
    }
    return scaledPixmap;
}

} // namespace hyprcapture::ui::thumbnail
