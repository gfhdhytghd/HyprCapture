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
    require(hyprcapture::ui::selectionOutlineGeometry(QRect(-200, 100, 500, 500), QRect(0, 0, 1000, 800), QRect(0, 0, 1000, 800)) ==
                QRect(-200, 100, 500, 500),
            "scrolling outline keeps its off-screen window geometry on the owning output");
    require(!hyprcapture::ui::selectionOutlineGeometry(QRect(900, 100, 300, 500), QRect(0, 0, 1000, 800), QRect(1000, 0, 1000, 800)).isValid(),
            "scrolling outline is not painted on an adjacent output");
    require(hyprcapture::ui::selectionOutlineGeometry(QRect(900, 100, 300, 500), QRect{}, QRect(1000, 0, 1000, 800)) ==
                QRect(900, 100, 300, 500),
            "ordinary cross-output outline remains available on adjacent outputs");

    const std::vector<hyprcapture::ui::WindowSelectionCandidate> windowCandidates{
        {.index = 0, .geometry = QRect(100, 100, 300, 300), .zIndex = 1},
        {.index = 1, .geometry = QRect(120, 120, 300, 300), .zIndex = 9},
        {.index = 2, .geometry = QRect(1100, 100, 300, 300), .zIndex = 12},
        {.index = 3, .geometry = QRect(900, 100, 300, 300), .zIndex = 5},
        {.index = 4, .geometry = QRect{}, .zIndex = 20},
    };
    const auto orderedCandidates =
        hyprcapture::ui::orderedWindowSelectionCandidates(windowCandidates, QRect(0, 0, 1000, 800));
    require(orderedCandidates == std::vector<int>({1, 3, 0}), "current-output candidates follow descending z order");
    require(hyprcapture::ui::windowSelectionStartPosition(orderedCandidates, 3) == 1, "focused candidate is the cycle start");
    require(hyprcapture::ui::windowSelectionStartPosition(orderedCandidates, 2) == 0, "off-output focus falls back to topmost candidate");
    require(hyprcapture::ui::windowSelectionStartPosition({}, 3) == -1, "empty candidate list has no cycle start");
    require(hyprcapture::ui::stepWindowSelectionPosition(1, 1, 3) == 2, "wheel down moves deeper in the stack");
    require(hyprcapture::ui::stepWindowSelectionPosition(1, -1, 3) == 0, "wheel up moves toward the top of the stack");
    require(hyprcapture::ui::stepWindowSelectionPosition(0, -4, 3) == 0, "top of stack does not wrap");
    require(hyprcapture::ui::stepWindowSelectionPosition(2, 4, 3) == 2, "bottom of stack does not wrap");
    require(hyprcapture::ui::stepWindowSelectionPosition(0, 1, 0) == -1, "empty stack cannot be stepped");

    hyprcapture::ui::WindowWheelStepState wheelState;
    require(hyprcapture::ui::consumeWindowWheelSteps(QPoint(0, -60), {}, false, wheelState) == 0, "partial angle delta is accumulated");
    require(hyprcapture::ui::consumeWindowWheelSteps(QPoint(0, -60), {}, false, wheelState) == 1, "angle wheel down selects a deeper window");
    require(hyprcapture::ui::consumeWindowWheelSteps(QPoint(0, 120), {}, false, wheelState) == -1, "angle direction change resets the remainder");
    require(hyprcapture::ui::consumeWindowWheelSteps(QPoint(0, -240), {}, true, wheelState) == -2, "inverted angle scrolling reverses selection");
    require(hyprcapture::ui::consumeWindowWheelSteps(QPoint(160, 80), {}, false, wheelState) == 0, "horizontal-dominant wheel input is ignored");

    wheelState = {};
    require(hyprcapture::ui::consumeWindowWheelSteps({}, QPoint(0, -20), false, wheelState) == 0, "partial pixel delta is accumulated");
    require(hyprcapture::ui::consumeWindowWheelSteps({}, QPoint(0, -20), false, wheelState) == 1, "pixel wheel down selects a deeper window");
    require(hyprcapture::ui::consumeWindowWheelSteps(QPoint(0, -120), QPoint(0, 80), false, wheelState) == 1,
            "angle delta takes precedence and resets pixel accumulation");

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
