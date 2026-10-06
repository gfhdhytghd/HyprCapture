#include "ui/mapped_image.hpp"
#include <QCoreApplication>
#include <QTemporaryFile>
#include <QPainter>
#include <cstdlib>
#include <iostream>

void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryFile file;
    require(file.open(), "temporary artifact");
    QImage expected(32, 24, QImage::Format_RGBA8888);
    for (int y = 0; y < expected.height(); ++y)
        for (int x = 0; x < expected.width(); ++x)
            expected.setPixelColor(x, y, QColor(x * 7, y * 9, 83, (x + y) * 4));
    require(file.write(reinterpret_cast<const char*>(expected.constBits()), expected.sizeInBytes()) == expected.sizeInBytes(), "write pixels");
    require(file.flush(), "flush pixels");
    require(hyprcapture::ui::mapRawRgba(file, 31, 24).isNull(), "reject trailing pixels");
    require(hyprcapture::ui::mapRawRgba(file, 33, 24).isNull(), "reject truncated pixels");
    require(hyprcapture::ui::mapRawRgba(file, -1, 24).isNull(), "reject negative dimensions");
    require(hyprcapture::ui::mapRawRgba(file, 2147483647, 2147483647).isNull(), "bound dimension multiplication");
    auto image = hyprcapture::ui::mapRawRgba(file, 32, 24);
    require(image == expected, "mapped RGBA including alpha");
    const auto* mappedBits = image.constBits();
    auto shared = image;
    require(shared.constBits() == mappedBits, "sharing does not copy pixels");
    file.close();
    require(file.remove(), "unlink artifact while mapped");
    image = {};
    require(shared == expected, "mapping survives file close, unlink and first owner destruction");
    auto edited = shared;
    {
        QPainter painter(&edited);
        painter.fillRect(edited.rect(), Qt::red);
    }
    require(edited.constBits() != mappedBits && shared == expected, "painting detaches and preserves frozen pixels");
    auto writable = shared;
    writable.bits()[0] ^= 0xff;
    require(writable.constBits() != mappedBits && shared == expected, "bits detaches read-only mapping");
    require(shared.flipped(Qt::Vertical) == expected.flipped(Qt::Vertical), "bottom-up orientation");
    shared = {};
    require(edited.pixelColor(0, 0) == QColor(Qt::red), "edited image survives final mapping owner");
    return 0;
}
