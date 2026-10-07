#pragma once
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

inline QPixmap scaledPixmap(const QPixmap& pixmap, const QScreen* targetScreen) {
    if (pixmap.isNull())
        return {};

    const QSizeF logicalPixmapSize = pixmap.deviceIndependentSize();
    QSize targetLogicalSize(std::max(1, static_cast<int>(std::ceil(logicalPixmapSize.width()))),
                            std::max(1, static_cast<int>(std::ceil(logicalPixmapSize.height()))));
    if (logicalPixmapSize.width() > kThumbnailMaxWidth || logicalPixmapSize.height() > kThumbnailMaxHeight)
        targetLogicalSize = targetLogicalSize.scaled(kThumbnailMaxWidth, kThumbnailMaxHeight, Qt::KeepAspectRatio);
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
