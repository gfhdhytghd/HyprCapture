#include "ui/hdr_image.hpp"
#include <QColorSpace>
#include <QElapsedTimer>
#include <QImage>
#include <cstdio>

// Isolate the full-resolution SDR -> HDR pass removed for native real backgrounds.
int main() {
    QImage background(2386, 1656, QImage::Format_RGBA8888);
    background.setColorSpace(QColorSpace::SRgb);
    for (int y = 0; y < background.height(); ++y) {
        auto* row = background.scanLine(y);
        for (int x = 0; x < background.width(); ++x) {
            row[4*x] = (x/24 + y/24)%2 ? 17 : 94;
            row[4*x+1] = 29; row[4*x+2] = 53;
            row[4*x+3] = (x < 10 || y < 10) ? 0 : 255;
        }
    }
    puts("trial,redundant_background_sdr_to_hdr_ms");
    for (int trial = 0; trial < 5; ++trial) {
        QElapsedTimer clock; clock.start();
        const auto image = hyprcapture::ui::sdrToHdr(background);
        const auto ms = clock.nsecsElapsed()/1e6;
        if (image.isNull()) return 1;
        printf("%d,%.3f\n", trial, ms);
    }
}
