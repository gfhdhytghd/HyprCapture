#include "ui/overlay_paint.hpp"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <cmath>
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

QImage roundedDesktop(const QSize& size, int radius) {
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(Qt::black);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    QPainterPath path;
    path.addRoundedRect(QRectF(image.rect()), radius, radius, Qt::AbsoluteSize);
    painter.fillPath(path, QColor(45, 110, 190));
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

    require(hyprcapture::ui::mapLogicalRectToPixels(QRect(110, 55, 20, 10), QRect(100, 50, 200, 100), QRect(0, 0, 400, 200)) ==
                QRect(20, 10, 40, 20),
            "HiDPI cursor layer mapping");
    require(hyprcapture::ui::mapLogicalRectToPixels(QRect(90, 40, 30, 30), QRect(100, 50, 50, 40), QRect(5, 7, 100, 80)) ==
                QRect(5, 7, 40, 40),
            "window cursor layer is clipped and mapped into the output");
    require(hyprcapture::ui::mapLogicalRectToPixels(QRect(10, 20, 20, 40), QRect(0, 0, 100, 200), QRect(0, 0, 200, 400)) ==
                QRect(20, 40, 40, 80),
            "normalized portrait cursor layer mapping");
    require(!hyprcapture::ui::mapLogicalRectToPixels(QRect(500, 500, 10, 10), QRect(0, 0, 100, 100), QRect(0, 0, 200, 200)).isValid(),
            "cursor outside the capture is omitted");

    const auto roundedCorners = hyprcapture::ui::detectScreenCornerRadii(roundedDesktop(QSize(240, 160), 28), QSize(240, 160));
    require(std::abs(roundedCorners.topLeft - 28.0) <= 2.0 && std::abs(roundedCorners.topRight - 28.0) <= 2.0 &&
                std::abs(roundedCorners.bottomRight - 28.0) <= 2.0 && std::abs(roundedCorners.bottomLeft - 28.0) <= 2.0,
            "rounded desktop corners are detected");
    const auto hidpiCorners = hyprcapture::ui::detectScreenCornerRadii(roundedDesktop(QSize(480, 320), 56), QSize(240, 160));
    require(std::abs(hidpiCorners.topLeft - 28.0) <= 2.0, "HiDPI desktop corner radius is converted to logical pixels");
    QImage squareDesktop(QSize(240, 160), QImage::Format_RGBA8888);
    squareDesktop.fill(QColor(45, 110, 190));
    const auto squareCorners = hyprcapture::ui::detectScreenCornerRadii(squareDesktop, squareDesktop.size());
    require(squareCorners.topLeft == 0.0 && squareCorners.topRight == 0.0 && squareCorners.bottomRight == 0.0 && squareCorners.bottomLeft == 0.0,
            "uniform square desktop does not produce a false rounded corner");

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

    QImage cursorLayer(QSize(6, 4), QImage::Format_RGBA8888);
    cursorLayer.fill(Qt::transparent);
    cursorLayer.setPixelColor(2, 1, QColor(255, 40, 20, 255));
    cursorLayer.setPixelColor(3, 1, QColor(255, 40, 20, 128));

    QImage cursorComposite(QSize(5, 3), QImage::Format_RGBA8888);
    cursorComposite.fill(QColor(10, 20, 30, 255));
    {
        QPainter painter(&cursorComposite);
        hyprcapture::ui::paintImageLayer(painter, QRect(1, 0, 3, 2), cursorLayer, QRect(1, 1, 3, 2));
    }
    require(cursorComposite.pixelColor(0, 0) == QColor(10, 20, 30, 255), "cursor layer escaped its target");
    require(cursorComposite.pixelColor(2, 0) == QColor(255, 40, 20, 255), "opaque cursor pixel was not composited");
    require(cursorComposite.pixelColor(1, 1) == QColor(10, 20, 30, 255), "transparent cursor pixel replaced the capture");
    require(cursorComposite.pixelColor(3, 0).red() > 10 && cursorComposite.pixelColor(3, 0).red() < 255,
            "partial-alpha cursor pixel was not blended");

    const QImage unchanged = cursorComposite;
    {
        QPainter painter(&cursorComposite);
        hyprcapture::ui::paintImageLayer(painter, QRect{}, cursorLayer, cursorLayer.rect());
    }
    require(cursorComposite == unchanged, "invalid cursor target changed the capture");

    std::cout << "hyprcapture overlay paint tests passed\n";
    return 0;
}
