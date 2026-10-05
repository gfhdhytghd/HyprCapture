#pragma once

#include "ui/material_icons_catalog.hpp"
#include <QColor>
#include <QIconEngine>
#include <QPainter>
#include <QSvgRenderer>
#include <QtMath>
#include <algorithm>
#include <utility>

namespace hyprcapture::ui {
// Keep the original SVG until paint time. No 24px raster cache: Qt can ask
// for a new physical size when moving between fractional/high-DPI outputs.
class MaterialIconEngine final : public QIconEngine {
  public:
    MaterialIconEngine(QByteArray svg, qreal rotation = 0) : m_svg(std::move(svg)), m_rotation(rotation) {}
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
        QSvgRenderer renderer(m_svg);
        renderer.render(painter, QRectF(-side / 2, -side / 2, side, side));
        painter->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
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
};
inline QIcon materialIcon(std::string_view name, const QColor& color, qreal rotation = 0) {
    const auto source = materialSvg(name);
    QByteArray svg(source.data(), static_cast<qsizetype>(source.size()));
    svg.replace("<svg ", QByteArray("<svg fill=\"") + color.name().toUtf8() + "\" ");
    return QIcon(new MaterialIconEngine(std::move(svg), rotation));
}
} // namespace hyprcapture::ui
