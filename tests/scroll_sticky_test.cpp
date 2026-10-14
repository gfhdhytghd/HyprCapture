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
#include "scroll_review_cases.hpp"
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    reviewCases(argc>1 ? argv[1] : "");
    if(argc>1) return 0;
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
    // A sticky side column initially participates in page motion, then pins.
    // Tiny touchpad strips used to repeat its viewport rows through the canvas.
    const auto stickyDocument = texture(240, 1400, 817);
    QImage sidebarDocument(120, 500, QImage::Format_ARGB32);
    sidebarDocument.fill(QColor(221, 226, 235));
    {
        QPainter p(&sidebarDocument);
        for (int y : {50, 110, 170, 350})
            p.drawImage(0, y, texture(120, 12, unsigned(y)));
    }
    const auto rawPlane = [](QImage image) {
        return image.rgbSwapped(); // Deliberately distinct from the SDR plane.
    };
    for (bool quiet : {true, false}) for (bool left : {false, true}) for (int step : {1, 3, 120}) {
        if (!quiet && step != 3) continue;
        constexpr int pin = 30, header = 24, viewport = 360;
        const int end = step == 1 ? 70 : step == 3 ? 150 : 600;
        const int mainX = left ? 120 : 0, sideX = left ? 0 : 240;
        auto sideDocument = sidebarDocument;
        if (!quiet) {
            // A label crosses every possible insertion band, but the modal
            // background is unambiguous. Objects must still appear only once.
            sideDocument.fill(QColor(221, 226, 235));
            QPainter p(&sideDocument);
            for (int y = 0; y < sideDocument.height(); y += 6)
                p.fillRect(0, y, 120, 2, QColor(25 + y % 100, 40, 60));
        }
        const auto stickyFrame = [&](int offset) {
            QImage image(360, 408, QImage::Format_ARGB32);
            image.fill(Qt::blue);
            QPainter p(&image);
            p.fillRect(0, 384, 360, 24, Qt::green);
            p.drawImage(mainX, header, stickyDocument.copy(0, offset, 240, viewport));
            p.drawImage(sideX, header, sideDocument.copy(0, std::min(offset, pin), 120, viewport));
            return image;
        };
        ScrollStitcher sticky;
        for (int offset = 0; offset <= end; offset += step) {
            const auto source = stickyFrame(offset);
            const auto result = sticky.append(source, rawPlane(source), floatPlane(source));
            require(result.status == (offset ? Status::Appended : Status::Started),
                    "sticky column accepts small touchpad and large wheel steps");
            if (sticky.layout().viewportY != offset)
                std::cerr << "sticky left=" << left << " step=" << step << " offset=" << offset
                          << " viewport=" << sticky.layout().viewportY
                          << " displacement=" << result.displacement << '\n';
            require(sticky.layout().viewportY == offset,
                    "sticky sidebar cannot change the measured document offset");
            if (offset >= pin + std::max(6, 3 * step)) {
                if(sticky.layout().content!=QRect(mainX,header,240,viewport))
                    std::cerr<<"pin classification quiet="<<quiet<<" left="<<left<<" step="<<step<<" offset="<<offset
                        <<" x="<<sticky.layout().content.x()<<" width="<<sticky.layout().content.width()<<'\n';
                require(sticky.layout().content == QRect(mainX, header, 240, viewport),
                        "sticky column is reclassified after sustained stationary motion");
            }
        }
        require(sticky.layout().content == QRect(mainX, header, 240, viewport),
                "column that pins mid-capture is excluded from the locked content");
        const auto pinned = stickyFrame(end);
        // Preserve pre-pin rows, then append the pinned viewport once. The
        // expansion begins in a quiet band near its bottom, or after the
        // complete viewport when no quiet band exists.
        const int localSeam = quiet ? viewport/2+40 : viewport;
        QImage expected(360, 408 + end, QImage::Format_ARGB32);
        {
            QPainter p(&expected);
            p.drawImage(0, 0, pinned.copy(0, 0, 360, header));
            p.drawImage(0, 384 + end, pinned.copy(0, 384, 360, 24));
            p.drawImage(mainX, header, stickyDocument.copy(0, 0, 240, viewport + end));
            p.drawImage(sideX, header, sideDocument.copy(0, 0, 120, pin + localSeam));
            p.fillRect(sideX, header + pin + localSeam, 120, end - pin, QColor(221,226,235));
            p.drawImage(sideX, header + localSeam + end,
                        sideDocument.copy(0, pin + localSeam, 120, viewport - localSeam));
        }
        if (sticky.image() != expected) {
            std::cerr << "sidebar mismatch quiet=" << quiet << " left=" << left << " step=" << step << "\n";
        }
        require(sticky.image() == expected,
                "pinned sidebar replaces every old slice and main document stays exact");
        require(sticky.originalImage() == rawPlane(expected),
                "sticky recrop and pinned source preserve the distinct original plane");
        require(sticky.hdrImage() == floatPlane(expected),
                "sticky recrop and pinned source preserve floating HDR highlights");
        for (int offset : {end - step, pin + step, 0, end}) {
            const auto source = stickyFrame(offset);
            require(sticky.append(source, rawPlane(source), floatPlane(source)).status == Status::Relocated,
                    "reverse motion and historical relocation keep the narrowed document coordinates");
            require(sticky.image() == expected && sticky.originalImage() == rawPlane(expected) &&
                        sticky.hdrImage() == floatPlane(expected),
                    "revisiting the unpinned sidebar cannot resurrect old slices in any plane");
        }
    }
    // Narrow independently moving chrome at the outside edge must not block
    // a wide stationary sidebar. A scrollbar changes even after the panel pins.
    for (int step : {3,120}) {
        const int end = step==3 ? 150 : 600;
        ScrollStitcher scrollbar;
        for (int offset=0;offset<=end;offset+=step) {
            QImage frame(360,408,QImage::Format_ARGB32);frame.fill(Qt::blue);
            QPainter p(&frame);
            p.fillRect(0,384,360,24,Qt::green);
            p.drawImage(0,24,stickyDocument.copy(0,offset,240,360));
            p.drawImage(240,24,sidebarDocument.copy(0,std::min(offset,30),120,360));
            p.drawImage(355,24,texture(5,360,unsigned(offset+13)));p.end();
            require(scrollbar.append(frame).status==(offset ? Status::Appended : Status::Started),
                    "moving narrow scrollbar does not interfere with document registration");
        }
        require(scrollbar.layout().content==QRect(0,24,240,360),
                "independent scrollbar cannot hold a sticky sidebar in the document column");
        require(scrollbar.image().copy(0,24,240,360+end)==stickyDocument.copy(0,0,240,360+end),
                "scrollbar classification preserves full scrolling main column");
        require(scrollbar.image().copy(240,24,115,200)==sidebarDocument.copy(0,0,115,200),
                "sidebar labels remain single and retain pre-pin rows beside a moving scrollbar");
    }
    // Repeated GitHub-like cards over a translucent, noisy backdrop. Both a
    // side panel and a compact title appear/pin after capture has started.
    QImage cards(240, 1400, QImage::Format_ARGB32);
    cards.fill(QColor(246,248,250));
    {
        QPainter p(&cards); p.setFont(QFont("sans-serif",10));
        for (int y = 0, n = 0; y < cards.height(); y += 80, ++n) {
            p.fillRect(4,y+4,232,21,QColor(214,234,245));
            p.setPen(QColor(30,40,50));
            p.drawText(22,y+19,"Owner commented 2d ago");
            p.drawText(8,y+43,"After the update, the background");
            p.drawText(8,y+60,QString("Row %1: still black, value %2").arg(n).arg(n*7919));
            p.fillRect(7,y+9,10,10,QColor(n*71%200,n*43%200,n*113%200));
        }
    }
    for (int y=0;y<cards.height();++y) for(int x=0;x<cards.width();++x)
        cards.setPixel(x,y,(cards.pixel(x,y)&0x00ffffff)|0xd9000000);
    QImage issueSide(120, 420, QImage::Format_ARGB32);
    issueSide.fill(QColor(221,226,235,217));
    {
        QPainter p(&issueSide); p.setFont(QFont("sans-serif",9)); p.setPen(QColor(35,45,60));
        int y=10;
        for (const char *label : {"Assignees","Labels","Projects","Milestone","Notifications"}) {
            p.drawText(3,y,label); p.drawText(3,y+17,"None assigned"); y+=65;
        }
    }
    QImage sidePage(120,500,QImage::Format_ARGB32);
    sidePage.fill(QColor(221,226,235,217));
    for(int y=0;y<issueSide.height();++y)
        std::memcpy(sidePage.scanLine(y+40),issueSide.constScanLine(y),120*4);
    {QPainter p(&sidePage);p.setPen(Qt::black);p.drawText(3,20,"Earlier sidebar");}
    for (int step : {1,3,120}) {
        const int end = step==1 ? 80 : step==3 ? 180 : 960;
        unsigned tick=0;
        const auto issueFrame = [&](int offset) {
            QImage raw(360,408,QImage::Format_ARGB32);raw.fill(Qt::blue);
            for(int y=0;y<360;++y) {
                std::memcpy(raw.scanLine(y+24),cards.constScanLine(y+offset+80),240*4);
                std::memcpy(raw.scanLine(y+24)+240*4,sidePage.constScanLine(y+40+std::min(offset,12)),120*4);
            }
            {
                QPainter p(&raw);p.setCompositionMode(QPainter::CompositionMode_Source);
                p.fillRect(0,384,360,24,Qt::green);
                if(offset>=24) {
                    p.fillRect(0,24,360,32,QColor(240,243,247));
                    p.fillRect(343,32,9,12,Qt::black); // Fixed side chrome must not enter the pinned source.
                    if(step==120 && offset<600)
                        p.fillRect(0,24,240,32,QColor(190+(offset/120%2)*40,220,245));
                    p.fillRect(3,31,42,18,QColor(30,140,60));
                    p.setFont(QFont("sans-serif",10));p.setPen(Qt::black);
                    p.drawText(49,44,"Issue compact sticky title #34");
                }
            }
            QImage displayed(raw.size(),QImage::Format_ARGB32);
            for(int y=0;y<displayed.height();++y) for(int x=0;x<displayed.width();++x) {
                const int value=200+int(25*std::sin(y/75.0+x/110.0)) + int(tick%3)-1;
                displayed.setPixel(x,y,qRgb(value,value,std::min(255,value+8)));
            }
            ++tick;
            {QPainter p(&displayed);p.drawImage(0,0,raw);}
            return std::pair{displayed,raw};
        };
        ScrollStitcher issue;
        for(int offset=0;offset<=end;offset+=step) {
            const auto [displayed,raw]=issueFrame(offset);
            const auto result=issue.append(displayed,raw,floatPlane(raw));
            if(result.displacement!=(offset?step:0) || (offset && result.status!=Status::Appended))
                std::cerr<<"issue step="<<step<<" offset="<<offset<<" status="<<int(result.status)
                         <<" delta="<<result.displacement<<'\n';
            require(result.status==(offset?Status::Appended:Status::Started),
                    "translucent repeated cards with newly sticky top/side accept every scroll");
            require(issue.layout().viewportY==offset,"sticky title cannot misregister repeated comments");
        }
        const auto hdrBody=issue.hdrImage().copy(0,24,240,360+end);
        const auto hdrExpected=floatPlane(cards.copy(0,80,240,360+end));
        if(hdrBody!=hdrExpected) {
            bool found=false;
            for(int y=0;y<hdrBody.height() && !found;++y) for(int x=0;x<240 && !found;++x) {
                const auto *a=hdrBody.constScanLine(y)+x*8,*b=hdrExpected.constScanLine(y)+x*8;
                if(std::memcmp(a,b,8)) {std::cerr<<"HDR issue mismatch step="<<step<<" x="<<x<<" y="<<y<<'\n';found=true;}
            }
        }
        require(hdrBody==hdrExpected,
                "sticky compact title is absent from the entire exact HDR document column");
        const auto original=issue.originalImage();
        const auto composed=issue.image();
        for(int y=0;y<360+end;++y) for(int x=0;x<240;++x) {
            const auto expected=cards.pixelColor(x,y+80), actual=original.pixelColor(x,y+24);
            require(actual.alpha()==expected.alpha() &&
                        std::max({std::abs(actual.red()-expected.red()),std::abs(actual.green()-expected.green()),
                                  std::abs(actual.blue()-expected.blue())})<=2,
                    "raw translucent comments have no duplicated, missing or compact-title rows");
            const auto display=composed.pixelColor(x,y+24);
            require(std::max({std::abs(display.red()-(expected.red()*217+200*38+127)/255),
                              std::abs(display.green()-(expected.green()*217+200*38+127)/255),
                              std::abs(display.blue()-(expected.blue()*217+208*38+127)/255)})<=12,
                    "SDR comments preserve glyph positions despite non-identical background pixels");
        }
        const auto sideActual=issue.hdrImage().copy(240,24,120,330);
        const auto sideExpected=floatPlane(sidePage.copy(0,40,120,330));
        if(sideActual!=sideExpected) {
            bool found=false;
            for(int y=0;y<330 && !found;++y) for(int x=0;x<120 && !found;++x)
                if(std::memcmp(sideActual.constScanLine(y)+x*8,sideExpected.constScanLine(y)+x*8,8)) {
                    std::cerr<<"side HDR step="<<step<<" x="<<x<<" y="<<y<<" contentWidth="<<issue.layout().content.width()<<'\n';found=true;
                }
        }
        require(sideActual==sideExpected,
                "sidebar keeps all Assignees/Labels/Projects/Notifications rows including the pre-pin prefix");
        for(int offset : {12,-30}) {
            const auto [displayed,raw]=issueFrame(offset);
            const auto result=issue.append(displayed,raw,floatPlane(raw));
            require(result.status==(offset==12 ? Status::Relocated : Status::Appended),
                    "unpinning top chrome allows new rows above the initial capture");
        }
        require(issue.layout().minimumY==-30 && issue.layout().viewportY==-30,
                "unpinning restores the full prepend extent without rebasing annotations");
        require(issue.hdrImage().copy(0,24,240,390+end)==floatPlane(cards.copy(0,50,240,390+end)),
                "unpinning/prepending preserves every main row in the paired HDR history");
        require(issue.hdrImage().copy(240,24,120,360)==floatPlane(sidePage.copy(0,10,120,360)),
                "new pre-pin sidebar prefix is recovered when scrolling above the initial capture");

    }
}
