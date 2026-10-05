#include "ui/clipboard_utils.hpp"
#include "ui/pinned_image.hpp"

#include <QApplication>
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
    const QPointF press = initialMask.center();
    QMouseEvent mousePress(QEvent::MouseButtonPress, press, press, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(pin.get(), &mousePress);
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
