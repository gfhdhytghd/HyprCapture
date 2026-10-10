#include "ui/hdr_image.hpp"
#include <QImage>
#include <QString>
#include <cassert>
int main() {
    assert(!hyprcapture::ui::hdrHeicAvailable());
    QString error;
    assert(!hyprcapture::ui::saveHdrHeic(QImage(1, 1, QImage::Format_RGBA16FPx4), "/unused.heic", &error));
    assert(error == "HEIC support was not built");
}
