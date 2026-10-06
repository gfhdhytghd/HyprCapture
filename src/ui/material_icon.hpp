#pragma once

#include "ui/material_icons_catalog.hpp"
#include <QColor>
#include <QCryptographicHash>
#include <QIconEngine>
#include <QPainter>
#include <QPixmapCache>
#include <QSvgRenderer>
#include <QtMath>
#include <algorithm>
#include <utility>
#include <memory>

namespace hyprcapture::ui {
// Cache by physical size, color, rotation and state, never a fixed 24px raster.
// The shared, bounded Qt cache also reuses icons between monitor toolbars.
class MaterialIconEngine final : public QIconEngine {
  public:
    MaterialIconEngine(QByteArray svg, qreal rotation = 0) : m_svg(std::move(svg)), m_rotation(rotation) {
        m_cacheKey = QStringLiteral("hyprcapture-icon:%1:%2")
                         .arg(QString::fromLatin1(QCryptographicHash::hash(m_svg, QCryptographicHash::Sha256).toHex()))
                         .arg(m_rotation, 0, 'g', 17);
    }
    QIconEngine* clone() const override { return new MaterialIconEngine(m_svg, m_rotation); }
    bool isNull() override { return m_svg.isEmpty(); }
    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (mode == QIcon::Disabled) painter->setOpacity(painter->opacity() * 0.38);
        painter->translate(QRectF(rect).center());
        painter->rotate(m_rotation);
        const qreal side = std::min(rect.width(), rect.height());
        if (!m_renderer)
            m_renderer = std::make_unique<QSvgRenderer>(m_svg);
        m_renderer->render(painter, QRectF(-side / 2, -side / 2, side, side));
        painter->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        const QString key = m_cacheKey + QStringLiteral(":%1:%2:%3:%4").arg(size.width()).arg(size.height()).arg(mode).arg(state);
        QPixmap cached;
        if (QPixmapCache::find(key, &cached))
            return cached;
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        painter.end();
        QPixmapCache::insert(key, result);
        return result;
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        auto result = pixmap(QSize(qCeil(size.width() * scale), qCeil(size.height() * scale)), mode, state);
        result.setDevicePixelRatio(scale);
        return result;
    }
  private:
    QByteArray m_svg;
    qreal m_rotation;
    QString m_cacheKey;
    std::unique_ptr<QSvgRenderer> m_renderer;
};
inline QIcon materialIcon(std::string_view name, const QColor& color, qreal rotation = 0) {
    const auto source = materialSvg(name);
    QByteArray svg(source.data(), static_cast<qsizetype>(source.size()));
    svg.replace("<svg ", QByteArray("<svg fill=\"") + color.name().toUtf8() + "\" ");
    return QIcon(new MaterialIconEngine(std::move(svg), rotation));
}
} // namespace hyprcapture::ui
