#include "ui/overlay_paint.hpp"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (condition)
        return;

    std::cerr << "overlay paint test failed: " << message << '\n';
    std::exit(1);
}

QImage testPattern() {
    QImage image(QSize(13, 7), QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor((x * 37 + y * 11) % 256, (x * 13 + y * 43) % 256, (x * 71 + y * 5) % 256));
    }
    return image;
}

} // namespace

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    require(hyprcapture::ui::clippedSelectionGeometry(QRect(900, 100, 300, 500), QRect(0, 0, 1000, 800)) == QRect(900, 100, 100, 500),
            "scrolling selection is clipped to its output");
    require(hyprcapture::ui::clippedSelectionGeometry(QRect(900, 100, 300, 500), QRect{}) == QRect(900, 100, 300, 500),
            "ordinary cross-output selection remains unchanged");

    const QImage source = testPattern();
    const QRect  destination(3, 2, 20, 11);

    QImage reference(QSize(28, 17), QImage::Format_RGBA8888);
    reference.fill(Qt::transparent);
    {
        QPainter painter(&reference);
        painter.drawImage(destination, source);
    }

    // These clips model a region selection growing one logical pixel per
    // mouse event. Every revealed pixel must stay on the same sampling grid as
    // the complete desktop, even though 13 source pixels map to 20 output
    // pixels.
    for (int width = 4; width <= 15; ++width) {
        const QRect clip(7, 4, width, 7);
        QImage      clipped(reference.size(), QImage::Format_RGBA8888);
        clipped.fill(Qt::transparent);
        {
            QPainter painter(&clipped);
            hyprcapture::ui::paintClippedImage(painter, clip, destination, source);
        }

        for (int y = 0; y < clipped.height(); ++y) {
            for (int x = 0; x < clipped.width(); ++x) {
                const QColor actual = clipped.pixelColor(x, y);
                if (clip.contains(x, y))
                    require(actual == reference.pixelColor(x, y), "changing clip moved the image sampling grid");
                else
                    require(actual.alpha() == 0, "painting escaped the selection clip");
            }
        }
    }

    std::cout << "hyprcapture overlay paint tests passed\n";
    return 0;
}
