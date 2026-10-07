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
    require(stitcher.append(frame(800)).status == Status::Relocated, "upward revisit preserves canvas");
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
    require(fixedHeader.append(second).status == Status::Appended, "fixed header detected");
    require(fixedHeader.layout().content == QRect(0,70,360,530), "header extent");
    auto expectedHeader = document.copy(0,0,360,730);
    { QPainter p(&expectedHeader); p.fillRect(0,0,360,70,Qt::blue); }
    require(fixedHeader.image() == expectedHeader, "header retained once");

    ScrollStitcher reverse;
    reverse.append(frame(600));
    for (int y : {400, 100, 250, 500, 800, 1100, 100}) {
        const auto r=reverse.append(frame(y));
        require(r.status == Status::Appended || r.status == Status::Relocated, "bidirectional extension and historical relocation");
        require(reverse.layout().viewportY == y-600, "exact document viewport");
    }
    require(reverse.image() == document.copy(0,100,360,1600), "bidirectional pixels");

    const auto chromeFrame = [&](int y) {
        auto image=frame(y);
        QPainter p(&image);
        p.fillRect(0,0,360,40,Qt::blue);
        p.fillRect(0,560,360,40,Qt::green);
        p.fillRect(0,40,50,520,Qt::red);
        p.fillRect(320,40,40,520,Qt::yellow);
        return image;
    };
    ScrollStitcher chrome;
    chrome.append(chromeFrame(500));
    for(int y : {350,200,450,700,800}) {
        const auto r=chrome.append(chromeFrame(y)); require(r.status == Status::Appended || r.status == Status::Relocated, "fixed chrome extension");
    }
    QImage expectedChrome(360,1200,QImage::Format_ARGB32); expectedChrome.fill(Qt::transparent);
    {
        QPainter p(&expectedChrome);
        p.fillRect(0,0,360,40,Qt::blue); p.fillRect(0,1160,360,40,Qt::green);
        p.fillRect(0,40,50,1120,Qt::red); p.fillRect(320,40,40,1120,Qt::yellow);
        p.drawImage(50,40,document.copy(50,240,270,1120));
    }
    require(chrome.image()==expectedChrome,"fixed sidebar gradient extensions preserve flat color");

    // Fixed sidebar labels must survive both sides of the insertion band.
    auto labelledSidebar = [&](int offset) {
        auto image = chromeFrame(offset);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        painter.drawText(4, 110, "Top");
        painter.drawText(4, 530, "End");
        return image;
    };
    ScrollStitcher labels;
    const auto labelledInitial = labelledSidebar(200);
    labels.append(labelledInitial);
    require(labels.append(labelledSidebar(350)).status == Status::Appended, "labelled sidebar extends");
    const int seam = labels.layout().leftSeam;
    require(seam > 300 && seam < 520, "quiet band avoids bottom label and footer");
    const auto labelledOutput = labels.image();
    require(labelledOutput.copy(0,40,50,seam-40) == labelledInitial.copy(0,40,50,seam-40), "sidebar upper text retained exactly");
    require(labelledOutput.copy(0,seam+150,50,560-seam) == labelledInitial.copy(0,seam,50,560-seam), "sidebar lower text shifted intact");
    for (int y=seam; y<seam+150; ++y)
        require(labelledOutput.pixelColor(25,y).alpha()==255, "inserted band is opaque");

    const QImage background=texture(360,600,345);
    QImage foreground(360,2400,QImage::Format_ARGB32); foreground.fill(Qt::transparent);
    { QPainter p(&foreground); for(int y=15,n=0;y<2400;y+=100,++n) {
        p.fillRect(60,y,240,65,QColor(20+n*7%180,30+n*13%180,45));
        p.setPen(Qt::white); p.drawText(70,y+35,QString("Foreground %1").arg(n));
    } }
    auto layered=[&](int offset) { auto image=background; QPainter p(&image); p.drawImage(0,0,foreground.copy(0,offset,360,600)); return image; };
    ScrollStitcher backdrop;
    backdrop.append(layered(300));
    require(backdrop.append(layered(450)).status==Status::Appended,"foreground registers over stationary patterned backdrop");
    const auto layeredResult=backdrop.image();
    require(layeredResult.copy(60,165,240,65)==layered(300).copy(60,165,240,65),"retained foreground pixels never blended");

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
