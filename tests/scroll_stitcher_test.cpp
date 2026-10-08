#include "ui/scroll_stitcher.hpp"
#include <QGuiApplication>
#include <QPainter>
#include <cmath>
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
    // A small movement at 2x scale leaves some top-row glyph stems unchanged.
    // They still belong to the document, not to a fixed header.
    QImage textPage(1600, 6000, QImage::Format_ARGB32);
    textPage.fill(QColor(250, 249, 246));
    {
        QPainter p(&textPage); p.scale(2, 2); p.setFont(QFont("sans-serif", 13));
        for (int y = 30, row = 0; y < 3000; y += 37, ++row) {
            p.fillRect(12, y-18, 24, 24, QColor(row*71%220,row*43%220,row*113%220));
            p.setPen(QColor(22,30,43));
            p.drawText(48,y,QString("Row %1 | native scroll fixture | value %2").arg(row).arg(row*7919));
        }
    }
    for (int direction : {-1, 1}) {
        ScrollStitcher tiny;
        int lo = 550, hi = 550;
        for (int offset : {550, 550+3*direction, 610, 490, 550, 630, 670, 490}) {
            tiny.append(textPage.copy(360, offset*2, 680, 500));
            lo = std::min(lo, offset); hi = std::max(hi, offset);
            require(tiny.image() == textPage.copy(360,lo*2,680,(250+hi-lo)*2),
                    "tiny scaled scroll retains document edges in both directions");
        }
    }
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

    // A translucent terminal moves glyphs over a stationary, blurred backdrop.
    // Repeated line spacing must not turn a 40px scroll into a -1px match.
    QImage terminalText(1032,4000,QImage::Format_ARGB32);
    for (bool dark : {false,true}) {
        terminalText.fill(Qt::transparent);
        {
            QPainter p(&terminalText); p.setFont(QFont("monospace",22));
            p.setPen(dark ? QColor(238,240,244) : QColor(18,20,24));
            for(int y=35,n=0;y<terminalText.height();y+=41,++n)
                p.drawText(30,y,QString("Row %1: terminal scroll value %2").arg(n).arg(n*7919));
        }
        for (int amplitude : {10,25,50}) {
            QImage backdropImage(1032,898,QImage::Format_ARGB32);
            for(int y=0;y<backdropImage.height();++y)
                for(int x=0;x<backdropImage.width();++x) {
                    const int c=(dark?55:200)+int(amplitude*std::sin(y/190.0+x/510.0));
                    backdropImage.setPixel(x,y,qRgb(c,c,std::min(255,c+8)));
                }
            const auto terminalFrame=[&](int y) {
                auto image=backdropImage; QPainter p(&image);
                p.drawImage(QPoint{},terminalText.copy(0,y,1032,898));
                return image;
            };
            ScrollStitcher terminal;
            const auto initial=terminalFrame(400);
            terminal.append(initial);
            int previousOffset=400, maximumOffset=400;
            for(int offset : {440,520,400,600,1000,1400,400}) {
                const auto current=terminalFrame(offset);
                const auto result=terminal.append(current);
                if(result.status!=Status::Appended && result.status!=Status::Relocated)
                    std::cerr << "dark=" << dark << " backdrop=" << amplitude << " offset=" << offset
                              << " status=" << int(result.status) << '\n';
                require(result.status==Status::Appended || result.status==Status::Relocated,
                        "terminal text registers over a stationary smooth backdrop");
                if(terminal.layout().viewportY!=offset-400 || result.displacement!=offset-previousOffset)
                    std::cerr << "dark=" << dark << " backdrop=" << amplitude << " offset=" << offset
                              << " viewport=" << terminal.layout().viewportY << " delta=" << result.displacement << '\n';
                require(terminal.layout().viewportY==offset-400 && result.displacement==offset-previousOffset,
                        "terminal registration preserves exact displacement and direction");
                maximumOffset=std::max(maximumOffset,offset);
                require(terminal.size().height()==898+maximumOffset-400,
                        "terminal backdrop cannot pin the output height");
                const auto content=terminal.layout().content;
                require(terminal.image().copy(content)==initial.copy(content),
                        "foreground registration retains original captured pixels");
                previousOffset=offset;
            }
            const auto acceptedTerminal=terminal.image();
            require(terminal.append(texture(1032,898,111)).status==Status::NoOverlap,
                    "unrelated terminal scene is not stitched");
            require(terminal.image()==acceptedTerminal,"rejected terminal scene leaves result intact");
        }
    }

    ScrollStitcher retained;
    auto translucent = [&](int offset) {
        auto raw = frame(offset);
        for (int y=0; y<raw.height(); ++y)
            for (int x=0; x<raw.width(); ++x) {
                auto color=raw.pixelColor(x,y); color.setAlpha(128); raw.setPixelColor(x,y,color);
            }
        return raw;
    };
    retained.append(frame(200), translucent(200));
    require(retained.append(frame(350), translucent(350)).status==Status::Appended,
            "paired raw pixels append on the accepted alignment");
    require(retained.append(frame(100), translucent(100)).status==Status::Appended,
            "paired raw pixels prepend on the accepted alignment");
    const auto rawResult=retained.originalImage();
    require(rawResult.size()==retained.image().size(), "raw and composited bounds match");
    require(rawResult.copy(0,0,360,600)==translucent(100), "raw first viewport preserved including alpha");
    require(rawResult.copy(0,250,360,600)==translucent(350), "raw appended viewport preserved including alpha");
    require(retained.append(frame(400)).status==Status::InvalidFrame,
            "cannot silently discard the original pixel channel");

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
