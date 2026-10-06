#include "ui/material_icon.hpp"
#include <QGuiApplication>
#include <iostream>
#include <cstdlib>

void require(bool ok, const char* reason) {
    if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const QByteArray red = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"><path fill="#ff0000" d="M2 2h8v20H2z"/></svg>)";
    QByteArray blue = red; blue.replace("#ff0000", "#0000ff");
    hyprcapture::ui::MaterialIconEngine first(red), same(red), other(blue), rotated(red, 180);
    const auto original = first.pixmap({24, 24}, QIcon::Normal, QIcon::Off);
    require(original.toImage().pixelColor(5, 12) == QColor(Qt::red), "native SVG color");
    require(original.toImage().pixelColor(18, 12).alpha() == 0, "native shape remains asymmetric");
    require(same.pixmap({24, 24}, QIcon::Normal, QIcon::Off).toImage() == original.toImage(), "shared cache retains pixels");
    require(other.pixmap({24, 24}, QIcon::Normal, QIcon::Off).toImage().pixelColor(5, 12) == QColor(Qt::blue), "color has independent cache entry");
    const auto reversed = rotated.pixmap({24, 24}, QIcon::Normal, QIcon::Off).toImage();
    require(reversed.pixelColor(18, 12) == QColor(Qt::red) && reversed.pixelColor(5, 12).alpha() == 0, "rotation has independent cache entry");
    const auto disabled = first.pixmap({24, 24}, QIcon::Disabled, QIcon::Off).toImage();
    require(disabled.pixelColor(5, 12).alpha() > 80 && disabled.pixelColor(5, 12).alpha() < 110, "disabled opacity retained");
    for (qreal scale : {1.0, 1.25, 1.5, 2.0}) {
        const auto raster = first.scaledPixmap({24, 24}, QIcon::Normal, QIcon::Off, scale);
        require(raster.size() == QSize(qCeil(24 * scale), qCeil(24 * scale)) && raster.devicePixelRatio() == scale, "fractional/high DPI dimensions");
        require(raster.toImage().pixelColor(qRound(5 * scale), qRound(12 * scale)) == QColor(Qt::red), "high DPI pixels");
    }
    require(first.pixmap({24, 24}, QIcon::Normal, QIcon::Off).devicePixelRatio() == 1.0, "scaled request does not contaminate native cache DPR");
    QPixmapCache::clear();
    require(first.pixmap({24, 24}, QIcon::Normal, QIcon::Off).toImage() == original.toImage(), "cache eviction preserves rendering");
}
