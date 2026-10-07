#include "ui/clipboard_utils.hpp"
#include "ui/pinned_image.hpp"

#include <QApplication>
#include <LayerShellQt/Window>
#include <QPaintEvent>
#include <QEnterEvent>
#include <QFile>
#include <QImage>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <cstdlib>
#include <cstdio>
#include <memory>

namespace {
struct PaintObserver : QObject {
    QRegion damage;
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Paint) damage |= static_cast<QPaintEvent*>(event)->region();
        return false;
    }
};
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "pinned image test: %s\n", message);
        std::exit(1);
    }
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary directory");
    QImage image(200, 100, QImage::Format_ARGB32);
    image.fill(Qt::red);
    const QString userSource = temporary.filePath(QStringLiteral("source.png"));
    require(image.save(userSource, "PNG"), "save user image");

    QString error;
    std::unique_ptr<QWidget> pin(createPinnedImage(userSource, true, &error));
    require(pin != nullptr && error.isEmpty(), "create user image pin");
    require(QFile::exists(userSource), "consume must preserve arbitrary user image");
    const QRect initialMask = pin->windowHandle()->mask().boundingRect();
    require(initialMask.size() == image.size(), "initial native image dimensions");
    app.processEvents();
    app.processEvents();
    PaintObserver paints;
    pin->installEventFilter(&paints);
    auto* layer = LayerShellQt::Window::get(pin->windowHandle());
    require(layer->keyboardInteractivity() == LayerShellQt::Window::KeyboardInteractivityNone, "pin hover does not request keyboard focus");
    require(pin->toolTip().isEmpty(), "pin does not create a separate tooltip surface");
    const QPointF press = initialMask.center();
    QEnterEvent enter(press, press, press);
    QApplication::sendEvent(pin.get(), &enter);
    QMouseEvent hover(QEvent::MouseMove, press, press, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(pin.get(), &hover);
    app.processEvents();
    require(paints.damage.isEmpty(), "image hover does not repaint the output");
    const QPointF closePoint = initialMask.topRight() + QPoint(-18, 18);
    QMouseEvent hoverClose(QEvent::MouseMove, closePoint, closePoint, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(pin.get(), &hoverClose);
    app.processEvents();
    require(!paints.damage.isEmpty() && paints.damage.boundingRect().width() <= 34 && paints.damage.boundingRect().height() <= 34,
            "close hover repaints only the close button");
    paints.damage = {};
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(pin.get(), &leave);
    app.processEvents();
    require(paints.damage.boundingRect().width() <= 34 && paints.damage.boundingRect().height() <= 34, "leaving pin does not repaint the output");

    QMouseEvent mousePress(QEvent::MouseButtonPress, press, press, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(pin.get(), &mousePress);
    require(layer->keyboardInteractivity() == LayerShellQt::Window::KeyboardInteractivityOnDemand, "explicit click enables Esc keyboard interaction");
    const QPointF moved = press + QPointF(35, 20);
    QMouseEvent mouseMove(QEvent::MouseMove, moved, moved, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(pin.get(), &mouseMove);
    require(pin->windowHandle()->mask().boundingRect().topLeft() == initialMask.topLeft() + QPoint(35, 20), "drag moves image input region");
    QMouseEvent mouseRelease(QEvent::MouseButtonRelease, moved, moved, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(pin.get(), &mouseRelease);
    QWheelEvent wheel(moved, moved, QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(pin.get(), &wheel);
    require(pin->windowHandle()->mask().boundingRect().width() > initialMask.width(), "wheel zoom enlarges image input region");
    pin.reset();

    // PNG does not retain the editor's DPR: use the supplied logical rectangle
    // even when its pixel dimensions are three times larger (issue #33).
    QImage scaledImage(600, 300, QImage::Format_ARGB32);
    scaledImage.fill(Qt::blue);
    const QString scaledSource = temporary.filePath(QStringLiteral("scale3.png"));
    require(scaledImage.save(scaledSource, "PNG"), "save scale-3 image");
    const QRect editorGeometry(43, 67, 200, 100);
    pin.reset(createPinnedImage(scaledSource, false, &error, nullptr, editorGeometry));
    require(pin != nullptr && error.isEmpty(), "create pin at editor geometry");
    require(pin->windowHandle()->mask().boundingRect() == editorGeometry,
            "pin preserves editor origin and logical size instead of centering or scaling PNG again");
    app.processEvents();
    const QImage rendered = pin->grab(editorGeometry).toImage();
    require(rendered.pixelColor(rendered.width() / 2, rendered.height() / 2) == QColor(Qt::blue),
            "image pixels are painted at the requested rectangle");
    pin.reset();

    const QString privateSource = hyprcapture::ui::runtimeFile(QStringLiteral("pin-test"), QStringLiteral(".png"));
    require(!privateSource.isEmpty() && hyprcapture::ui::savePrivatePng(image, privateSource), "save private image");
    pin.reset(createPinnedImage(privateSource, true, &error));
    require(pin != nullptr && !QFile::exists(privateSource), "consume private image after successful decode");
    pin.reset();

    const QString invalidSource = hyprcapture::ui::runtimeFile(QStringLiteral("pin-test-invalid"), QStringLiteral(".png"));
    QFile invalid(invalidSource);
    require(invalid.open(QIODevice::WriteOnly), "create invalid private image");
    invalid.write("not a png");
    invalid.close();
    pin.reset(createPinnedImage(invalidSource, true, &error));
    require(!pin && !error.isEmpty() && QFile::exists(invalidSource), "invalid image is rejected and preserved");
    QFile::remove(invalidSource);

    const QString linkSource = hyprcapture::ui::runtimeFile(QStringLiteral("pin-test-link"), QStringLiteral(".png"));
    require(QFile::link(userSource, linkSource), "create image symlink");
    pin.reset(createPinnedImage(linkSource, true, &error));
    require(pin != nullptr && QFile::exists(linkSource) && QFile::exists(userSource), "consume must preserve symlink source and target");
    pin.reset();
    QFile::remove(linkSource);

    std::puts("pinned image tests passed");
    return 0;
}
