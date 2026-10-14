#include "ui/scroll_stitcher.hpp"
#include <QGuiApplication>
#include <QPainter>
#include <cmath>
#include <qfloat16.h>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <opencv2/core.hpp>

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
    cv::setNumThreads(1);
    // A changing image and an interior fixed panel must not become the page's
    // motion model. Exercise forward, reverse, idle and recovery transitions.
    const auto dynamicDocument = texture(640, 2400, 730);
    const auto panel = texture(360, 280, 91);
    const auto dynamicFrame = [&](int offset, unsigned tick, bool fixed) {
        auto image = dynamicDocument.copy(0, offset, 640, 600);
        QPainter p(&image);
        if (fixed)
            p.drawImage(140, 160, panel);
        else
            p.drawImage(130, 650-offset, texture(380, 220, tick));
        return image;
    };
    for (bool fixed : {false, true}) {
        ScrollStitcher dynamic;
        unsigned tick = 1;
        dynamic.append(dynamicFrame(400, tick++, fixed));
        for (int offset : {520, 640, 460, 300, 400, 700}) {
            const auto result = dynamic.append(dynamicFrame(offset, tick++, fixed));
            require(result.status == Status::Appended || result.status == Status::Relocated,
                    "animation and interior fixed panel allow bidirectional scrolling");
            require(dynamic.layout().viewportY == offset-400,
                    "dynamic regions cannot change the measured page displacement");
        }
        const auto accepted = dynamic.image();
        require(accepted.copy(0, 0, 100, 1000) == dynamicDocument.copy(0, 300, 100, 1000),
                "unobscured document pixels remain exact through dynamic captures");
        require(dynamic.append(dynamicFrame(700, tick++, fixed)).status == Status::Unchanged,
                "animation at rest is unchanged document position");
        require(dynamic.image() == accepted, "idle animation never changes the retained image");
    }
    ScrollStitcher idleAnimation;
    idleAnimation.append(dynamicFrame(400, 101, false));
    require(idleAnimation.append(dynamicFrame(400, 102, false)).status == Status::Unchanged,
            "animation before first scroll does not lock a false content rectangle");
    require(idleAnimation.layout().content == QRect(0, 0, 640, 600),
            "idle animation preserves the full viewport layout");
    require(idleAnimation.append(dynamicFrame(520, 103, false)).displacement == 120,
            "scrolling starts correctly after an idle animated baseline");
    ScrollStitcher animatedHistory;
    unsigned historyTick = 201;
    for (int offset : {300, 450, 600, 850, 1100, 1350, 300}) {
        const auto result = animatedHistory.append(dynamicFrame(offset, historyTick++, false));
        require(result.status == Status::Started || result.status == Status::Appended ||
                    result.status == Status::Relocated,
                "animation does not prevent relocation into distant retained history");
        require(animatedHistory.layout().viewportY == offset-300,
                "historical animation relocation has an exact document coordinate");
    }

    // Motion contained in one widget is not evidence that the document moved.
    ScrollStitcher movingWidget;
    const auto widget = texture(300, 1000, 57);
    for (int tick = 0; tick < 4; ++tick) {
        auto image = dynamicDocument.copy(0, 400, 640, 600);
        { QPainter p(&image); p.drawImage(160, 180, widget.copy(0, tick*30, 300, 220)); }
        const auto result = movingWidget.append(image);
        if (result.status != (tick == 0 ? Status::Started : Status::Unchanged))
            std::cerr << "widget tick=" << tick << " status=" << int(result.status)
                      << " delta=" << result.displacement << '\n';
        require(result.status == (tick == 0 ? Status::Started : Status::Unchanged),
                "motion inside a video never grows a stationary page");
        require(movingWidget.size() == QSize(640, 600), "video motion cannot grow the canvas");
    }
    // Two independently scrolling panes have no single document coordinate.
    const auto splitFrame = [&](int left, int right) {
        auto image = dynamicDocument.copy(0, left, 640, 600);
        QPainter p(&image);
        p.drawImage(320, 0, dynamicDocument.copy(320, right, 320, 600));
        return image;
    };
    ScrollStitcher conflicting;
    const auto splitInitial = splitFrame(400, 1000);
    conflicting.append(splitInitial);
    require(conflicting.append(splitFrame(520, 880)).status == Status::Ambiguous,
            "two spatially supported motion layers are rejected");
    require(conflicting.image() == splitInitial, "conflicting motion preserves accepted pixels");

    // Without persistent document detail, fresh animation frames provide no
    // measured offset. Rejection must preserve every retained output plane.
    ScrollStitcher noAnchors;
    const auto noAnchorInitial = texture(640, 600, 999);
    noAnchors.append(noAnchorInitial);
    require(noAnchors.append(texture(640, 600, 1000)).status == Status::NoOverlap,
            "full viewport animation is not assigned a guessed displacement");
    require(noAnchors.image() == noAnchorInitial, "no-anchor rejection preserves the canvas");
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
    // Sparse colored markers in an otherwise blank gutter are moving content,
    // even when a native touchpad starts with a single pixel of displacement.
    ScrollStitcher gutter;
    for (int offset : {800,801,920,680,800,960,1040,680}) {
        gutter.append(textPage.copy(0,offset,1600,1000));
    }
    require(gutter.image()==textPage.copy(0,680,1600,1360), "tiny first scroll preserves sparse moving gutters");
    ScrollStitcher decoratedGutter;
    for (int offset : {800,801,920,680,800,960,1040,680}) {
        QImage decorated(1644,1044,QImage::Format_ARGB32);
        for(int y=0;y<decorated.height();++y) {
            QPainter p(&decorated); p.fillRect(0,y,1644,1,QColor(y%220,y%220,y%220));
        }
        { QPainter p(&decorated); p.drawImage(22,22,textPage.copy(0,offset,1600,1000)); }
        decoratedGutter.append(decorated);
    }
    require(decoratedGutter.image().copy(22,54,1600,1296)==textPage.copy(0,712,1600,1296),"fixed native border cannot conceal moving sparse gutter markers");
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

    // Repeating high-contrast artwork in the middle cannot suppress unique
    // evidence elsewhere in the viewport (as found by the HDR live fixture).
    auto artwork=texture(360,2400);
    for(int y=0;y<artwork.height();++y)
        for(int x=90;x<270;++x) artwork.setPixel(x,y,x<170 && y%113<35 ? qRgb(230,50,180) : qRgb(250,250,250));
    ScrollStitcher artworkStitcher;
    artworkStitcher.append(artwork.copy(0,400,360,600));
    for(int y : {460,340,400,480,520,340}) {
        const auto r=artworkStitcher.append(artwork.copy(0,y,360,600));
        require(r.status==Status::Appended || r.status==Status::Relocated,"distinctive side columns register despite repeating central artwork");
    }
    require(artworkStitcher.image()==artwork.copy(0,340,360,780),"central artwork cannot change exact displacement");

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

    const auto floatPlane=[](const QImage& sdr) {
        QImage hdr(sdr.size(),QImage::Format_RGBA16FPx4_Premultiplied);
        for(int y=0;y<hdr.height();++y) {
            auto* row=reinterpret_cast<qfloat16*>(hdr.scanLine(y));
            for(int x=0;x<hdr.width();++x) {
                const auto c=sdr.pixelColor(x,y);
                row[x*4]=qfloat16(2.f+c.red()/255.f); row[x*4+1]=qfloat16(2.f+c.green()/255.f);
                row[x*4+2]=qfloat16(2.f+c.blue()/255.f); row[x*4+3]=qfloat16(1.f);
            }
        }
        return hdr;
    };
    for (bool down : {true, false}) {
        // The animated object straddles the old viewport edge. All three
        // planes must choose the same new capture for the complete object.
        const int objectY = down ? 840 : 340;
        const auto seamFrame = [&](int offset, unsigned tick) {
            auto image = dynamicDocument.copy(0, offset, 640, 600);
            QPainter p(&image);
            p.drawImage(130, objectY-offset, texture(380, 220, tick));
            return image;
        };
        ScrollStitcher seam;
        const auto a = seamFrame(400, 11), b = seamFrame(down ? 520 : 280, 12);
        seam.append(a, a, floatPlane(a));
        require(seam.append(b, b, floatPlane(b)).status == Status::Appended,
                "animated boundary extends in either direction");
        const int minimum = down ? 400 : 280;
        require(seam.image().copy(130, objectY-minimum, 380, 220) == texture(380, 220, 12),
                "seam avoids cutting an animated object between two times");
        require(seam.originalImage() == seam.image(), "raw pixels use the same dynamic seam");
        require(seam.hdrImage() == floatPlane(seam.image()),
                "HDR pixels use the same dynamic seam without blending");
    }
    ScrollStitcher clippedWindow;
    for(int offset : {200,350,100}) {
        auto raw=frame(offset); auto displayed=raw;
        auto linear=floatPlane(raw);
        for(int y=0;y<24;++y) for(int x=0;x<24;++x) {
            raw.setPixel(x,y,qRgba(0,0,0,0)); displayed.setPixel(x,y,qRgb(255,255,255));
            auto* pixel=reinterpret_cast<qfloat16*>(linear.scanLine(y))+x*4;
            for(int c=0;c<4;++c) pixel[c]=qfloat16(0.f);
        }
        // An almost opaque corner can still contain strongly colored border
        // pixels; alpha rounding must not strand it at an interior seam.
        raw.setPixel(0,0,qRgba(255,255,255,254));
        clippedWindow.append(displayed,raw,linear);
    }
    require(clippedWindow.image().copy(0,100,24,24)==document.copy(0,200,24,24),"overlap recovers native corner pixels at interior seams");
    require(clippedWindow.originalImage().copy(0,100,24,24)==document.copy(0,200,24,24),"corner repair restores original alpha");
    const auto repairedHdr=clippedWindow.hdrImage().copy(0,100,24,24), expectedHdr=floatPlane(document.copy(0,200,24,24));
    require(repairedHdr==expectedHdr,"corner repair preserves paired HDR values");
    ScrollStitcher hdrChrome;
    hdrChrome.append(chromeFrame(500), {}, floatPlane(chromeFrame(500)));
    for(int y : {350,200,450,700,800}) hdrChrome.append(chromeFrame(y), {}, floatPlane(chromeFrame(y)));
    const auto hdrChromeResult=hdrChrome.hdrImage(), hdrChromeExpected=floatPlane(expectedChrome);
    require(hdrChromeResult.size()==hdrChromeExpected.size(), "HDR fixed header/footer dimensions");
    for(int y=0;y<hdrChromeResult.height();++y)
        require(std::memcmp(hdrChromeResult.constScanLine(y),hdrChromeExpected.constScanLine(y),360*8)==0,
                "HDR fixed headers, footers and sidebar interpolation preserve highlights");

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

    // HDR registration must follow SDR displacement exactly while retaining
    // highlights above 1.0, negative wide-gamut channels and transparency.
    QImage hdrDocument(360, 1800, QImage::Format_RGBA16FPx4_Premultiplied);
    for (int y=0; y<hdrDocument.height(); ++y) {
        auto* row=reinterpret_cast<qfloat16*>(hdrDocument.scanLine(y));
        for (int x=0; x<360; ++x) {
            row[x*4]=qfloat16(2.f + y/1000.f); row[x*4+1]=qfloat16(-.125f);
            row[x*4+2]=qfloat16(x/200.f); row[x*4+3]=qfloat16(.5f);
        }
    }
    ScrollStitcher hdrStitcher;
    const auto hdrFrame=[&](int y) { return hdrDocument.copy(0,y,360,600); };
    require(hdrStitcher.append(frame(200), {}, hdrFrame(200)).status==Status::Started, "HDR baseline");
    require(hdrStitcher.append(frame(350), {}, hdrFrame(350)).status==Status::Appended, "HDR append");
    require(hdrStitcher.append(frame(100), {}, hdrFrame(100)).status==Status::Appended, "HDR prepend");
    require(hdrStitcher.append(frame(250), {}, hdrFrame(250)).status==Status::Relocated, "HDR relocation");
    const auto hdrResult=hdrStitcher.hdrImage(), hdrExpected=hdrDocument.copy(0,100,360,850);
    require(hdrResult.size()==hdrStitcher.image().size() && hdrResult.format()==hdrExpected.format(), "paired HDR dimensions/format");
    for (int y=0; y<hdrResult.height(); ++y)
        require(std::memcmp(hdrResult.constScanLine(y),hdrExpected.constScanLine(y),360*8)==0, "float HDR channels and alpha preserved exactly");
    const auto hdrPreview=hdrStitcher.preview({180,425},true);
    require(hdrPreview.format()==QImage::Format_RGBA16FPx4_Premultiplied && hdrPreview.size()==QSize(180,425),"HDR thumbnail retains float format at bounded size");
    const auto* sample=reinterpret_cast<const qfloat16*>(hdrPreview.constScanLine(200));
    require(float(sample[0])>2 && float(sample[1])<0 && float(sample[3])==.5f,"HDR thumbnail retains highlights, wide gamut and alpha");
    require(hdrStitcher.append(frame(400)).status==Status::InvalidFrame, "missing HDR plane rejected");
    require(hdrStitcher.append(frame(400), {}, frame(400)).status==Status::InvalidFrame, "byte HDR plane rejected");

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
    for (bool fixed : {false, true}) {
        ScrollStitcher textWithWidget;
        unsigned tick = 500;
        for (int offset : {300, 420, 510, 380, 210, 450, 600}) {
            auto image = text.copy(0, offset, 800, 700);
            { QPainter p(&image);
              p.drawImage(220, fixed ? 190 : 660-offset,
                          texture(400, 300, fixed ? 17 : tick++)); }
            const auto result = textWithWidget.append(image);
            if (result.status != Status::Started && result.status != Status::Appended &&
                result.status != Status::Relocated)
                std::cerr << "text widget fixed=" << fixed << " offset=" << offset
                          << " status=" << int(result.status) << '\n';
            require(result.status == Status::Started || result.status == Status::Appended ||
                        result.status == Status::Relocated,
                    "sparse text remains matchable with a large animated or fixed widget");
            require(textWithWidget.layout().viewportY == offset-300,
                    "repeated text around a widget preserves exact document position");
        }
    }
    return 0;
}
