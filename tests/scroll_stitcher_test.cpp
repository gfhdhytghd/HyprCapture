#include "ui/scroll_stitcher.hpp"
#include <QGuiApplication>
#include <QPainter>
#include <cstdlib>
#include <iostream>

using hyprcapture::ui::ScrollStitcher;
using Status = ScrollStitcher::Status;
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
QImage texture(int width, int height, unsigned seed = 42) {
    QImage image(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            row[x] = qRgb(seed & 255, (seed >> 8) & 255, (seed >> 16) & 255);
        }
    }
    return image;
}
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const auto document = texture(360, 2400);
    const auto frame = [&](int offset) { return document.copy(0, offset, 360, 600); };
    ScrollStitcher stitcher;
    require(stitcher.append(frame(0)).status == Status::Started, "first frame");
    require(stitcher.append(frame(0)).status == Status::Unchanged, "duplicate frame");
    for (int offset : {1, 110, 270, 590, 950}) {
        require(stitcher.append(frame(offset)).status == Status::Appended, "variable forward scroll incl one pixel");
        require(stitcher.image() == document.copy(0, 0, 360, offset + 600), "pixel-exact stitched result");
    }
    const auto accepted = stitcher.image();
    require(stitcher.append(frame(800)).status == Status::NoOverlap, "upward scroll rejected");
    require(stitcher.append(frame(1600)).status == Status::NoOverlap, "lost overlap rejected");
    require(stitcher.append(texture(360, 600, 9)).status == Status::NoOverlap, "unrelated scene rejected");
    require(stitcher.append(texture(361, 600)).status == Status::SizeChanged, "size change rejected");
    require(stitcher.image() == accepted, "all rejected frames leave accepted pixels unchanged");
    require(stitcher.append(frame(1200)).status == Status::Appended, "recover after scrolling back into overlap");

    auto repeated = texture(360, 24);
    QImage stripes(360, 900, QImage::Format_ARGB32);
    for (int y = 0; y < stripes.height(); ++y)
        memcpy(stripes.scanLine(y), repeated.constScanLine(y % 24), stripes.bytesPerLine());
    ScrollStitcher ambiguous;
    ambiguous.append(stripes.copy(0, 0, 360, 600));
    require(ambiguous.append(stripes.copy(0, 7, 360, 600)).status == Status::Ambiguous, "periodic content rejected rather than guessed");
    require(ambiguous.size().height() == 600, "ambiguity does not append");

    ScrollStitcher fixedHeader;
    auto first = frame(0), second = frame(130);
    for (QImage* image : {&first, &second}) {
        QPainter painter(image); painter.fillRect(0, 0, 360, 70, Qt::blue);
    }
    fixedHeader.append(first);
    require(fixedHeader.append(second).status == Status::NoOverlap, "fixed header must not be silently duplicated");

    ScrollStitcher limited({700, 360LL * 700, 2});
    limited.append(frame(0));
    require(limited.append(frame(110)).status == Status::LimitReached, "height and pixel budget");
    require(limited.size().height() == 600, "limit retains partial image");
    require(limited.append(frame(50)).status == Status::Appended, "still accepts within budget");
    require(limited.append(frame(70)).status == Status::LimitReached, "frame budget");
    require(ScrollStitcher::stable(frame(0), frame(0)), "stable frame");
    require(!ScrollStitcher::stable(frame(0), frame(20)), "moving frame");
    require(!ScrollStitcher::stable({}, frame(0)), "invalid frame");

    // Document-like sparse text on white, instead of relying only on dense noise.
    QImage text(800, 2500, QImage::Format_ARGB32); text.fill(Qt::white);
    {
        QPainter painter(&text);
        painter.setPen(Qt::black);
        QFont font("sans-serif", 14); painter.setFont(font);
        for (int y = 40, line = 0; y < text.height(); y += 37, ++line)
            painter.drawText(24, y, QString("Line %1: scrolling capture keeps the original pixels, value %2.").arg(line).arg(line * 7919));
    }
    ScrollStitcher textStitcher;
    textStitcher.append(text.copy(0, 0, 800, 700));
    for (int y : {147, 361, 583, 932}) {
        const auto result = textStitcher.append(text.copy(0, y, 800, 700));
        require(result.status == Status::Appended, "sparse text alignment");
        require(textStitcher.image() == text.copy(0, 0, 800, y + 700), "sparse text pixels preserved");
    }
    return 0;
}
