#include "ui/scroll_stitcher.hpp"
#include <QGuiApplication>
#include <QPainter>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <opencv2/core.hpp>
#include <qfloat16.h>

using hyprcapture::ui::ScrollStitcher;
using Status = ScrollStitcher::Status;
namespace {
void require(bool ok, const char *message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
QImage floatPlane(const QImage &raw) {
    QImage hdr(raw.size(), QImage::Format_RGBA16FPx4_Premultiplied);
    for (int y = 0; y < raw.height(); ++y) {
        auto *row = reinterpret_cast<qfloat16 *>(hdr.scanLine(y));
        for (int x = 0; x < raw.width(); ++x) {
            const auto c = raw.pixelColor(x, y);
            const float alpha = c.alphaF();
            row[x*4] = qfloat16((2.f + c.redF()) * alpha);
            row[x*4+1] = qfloat16((2.f + c.greenF()) * alpha);
            row[x*4+2] = qfloat16((2.f + c.blueF()) * alpha);
            row[x*4+3] = qfloat16(alpha);
        }
    }
    return hdr;
}
void pixelsNear(const QImage &a, const QImage &b, const char *message) {
    require(a.size() == b.size(), message);
    for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width(); ++x) {
        const auto p = a.pixelColor(x,y), q = b.pixelColor(x,y);
        const bool ok=p.alpha() == q.alpha() &&
                std::max({std::abs(p.red()-q.red()), std::abs(p.green()-q.green()),
                          std::abs(p.blue()-q.blue())}) <= 2;
        if(!ok) std::cerr<<"pixel "<<x<<","<<y<<" actual="<<p.name(QColor::HexArgb).toStdString()<<" expected="<<q.name(QColor::HexArgb).toStdString()<<'\n';
        require(ok, message);
    }
}
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    cv::setNumThreads(1);
    QImage page(600, 7000, QImage::Format_ARGB32);
    page.fill(QColor(240,240,235));
    {
        QPainter p(&page);
        for (int y=0; y<page.height(); y+=40) {
            p.setPen(QColor(20,30,40));
            p.drawText(12,y+24,QString("Comment %1: unique value %2").arg(y/40).arg(y*7919));
            for (int x=0; x<600; x+=40)
                p.fillRect(x,y,24,6,QColor((y*7919+x*317)%251,(y*379+x*173)%249,90));
            p.fillRect(440,y,120,16,QColor((y*7)%255,80,170));
        }
    }
    for (bool translucent : {false, true}) for (bool sticky : {false, true}) {
        ScrollStitcher stitcher;
        auto frame = [&](int offset, bool loaded=true) {
            QImage raw(600,720,QImage::Format_ARGB32);
            raw.fill(QColor(40,40,45));
            {
                QPainter p(&raw);
                p.setPen(Qt::white); p.drawText(15,25,"Browser toolbar");
                p.drawImage(QRect(0,40,600,660),page,QRect(0,offset+600,600,660));
                if (!loaded) p.fillRect(16,260,380,128,QColor(240,240,235));
                if (sticky) {
                    p.drawImage(QRect(440,40,160,660),page,QRect(440,std::min(offset,320)+600,160,660));
                    p.setBrush(Qt::blue); p.drawEllipse(QRect(280,640,48,48));
                    if (offset>=260) {
                        p.fillRect(0,40,440,56,Qt::white);
                        p.drawText(15,70,"Back: compact sticky title");
                    }
                }
            }
            if (translucent) for (int y=40; y<700; ++y) {
                auto *row = reinterpret_cast<QRgb *>(raw.scanLine(y));
                for (int x=0; x<600; ++x) row[x]=(row[x]&0xffffff)|0xd9000000;
            }
            QImage sdr(raw.size(),QImage::Format_ARGB32);
            for (int y=0; y<sdr.height(); ++y) {
                const int value=180+int(60*std::sin(y/55.));
                auto *row=reinterpret_cast<QRgb *>(sdr.scanLine(y));
                std::fill(row,row+sdr.width(),qRgb(value,value,std::min(255,value+8)));
            }
            { QPainter p(&sdr); p.drawImage(0,0,raw); }
            return std::pair{sdr,raw};
        };
        int previous=0;
        // Fast sidebar-stop transition, flick/bounce, long reverse jumps across
        // header disappearance, then forward again across its reappearance.
        for (int y : {0,80,320,640,1040,1390,1800,1680,1655,1671,1649,
                      2050,2420,2600,2580,2620,2100,1580,1060,540,180,160,-80,-60,-76,80,
                      400,950,1500,2050,2600}) {
            const auto [sdr,raw]=frame(y);
            const auto r=stitcher.append(sdr,raw,floatPlane(raw));
            if ((r.status!=Status::Started && r.status!=Status::Appended &&
                 r.status!=Status::Relocated && r.status!=Status::Unchanged) ||
                 stitcher.layout().viewportY!=y) {
                std::cerr<<"translucent="<<translucent<<" sticky="<<sticky
                         <<" previous="<<previous<<" next="<<y<<" status="<<int(r.status)
                         <<" actual="<<stitcher.layout().viewportY<<" reason="<<r.reason<<'\n';
                return 1;
            }
            previous=y;
        }
        auto expected=page.copy(0,700,400,2500);
        if (translucent) for (int y=0; y<expected.height(); ++y) {
            auto *row=reinterpret_cast<QRgb *>(expected.scanLine(y));
            for (int x=0; x<expected.width(); ++x) row[x]=(row[x]&0xffffff)|0xd9000000;
        }
        pixelsNear(stitcher.originalImage().copy(0,140-stitcher.layout().minimumY,400,2500),expected,"main column retains correct rows and alpha");
        require(stitcher.hdrImage().copy(0,140-stitcher.layout().minimumY,400,2500)==floatPlane(expected),"HDR strips use the same row geometry");
        if (sticky) {
            require(!stitcher.layout().panels.empty(),"sticky sidebar classified");
            const auto [sdr,pinned]=frame(320);
            pixelsNear(stitcher.originalImage().copy(440,360-stitcher.layout().minimumY,160,600),pinned.copy(440,40,160,600),
                       "sidebar appears once from pinned viewport rather than repeated strips");
        }
        // An unobserved gap must leave all planes unchanged; history can still
        // reacquire a previously visited position and continue afterward.
        const auto before=stitcher.originalImage(), beforeHdr=stitcher.hdrImage();
        const auto [farSdr,farRaw]=frame(4800);
        const auto rejected=stitcher.append(farSdr,farRaw,floatPlane(farRaw));
        require(rejected.status==Status::NoOverlap || rejected.status==Status::Ambiguous,"insufficient overlap is rejected");
        require(stitcher.originalImage()==before && stitcher.hdrImage()==beforeHdr,"rejection preserves all confirmed planes");
        const auto [backSdr,backRaw]=frame(2400);
        const auto recovered=stitcher.append(backSdr,backRaw,floatPlane(backRaw));
        require((recovered.status==Status::Relocated || recovered.status==Status::Unchanged) &&
                stitcher.layout().viewportY==2400,"recover after rejected frame");

        // Late loading at a confirmed position changes formerly blank tiles.
        // Keep known page anchors and replace the blank consistently in planes.
        ScrollStitcher loading;
        auto [firstSdr,firstRaw]=frame(0);
        loading.append(firstSdr,firstRaw,floatPlane(firstRaw));
        auto [blankSdr,blankRaw]=frame(120,false);
        require(loading.append(blankSdr,blankRaw,floatPlane(blankRaw)).status==Status::Appended,"scroll into lazy placeholder");
        auto [loadedSdr,loadedRaw]=frame(120,true);
        const auto loaded=loading.append(loadedSdr,loadedRaw,floatPlane(loadedRaw));
        require(loaded.status==Status::Unchanged && loading.layout().viewportY==120,"loading keeps document position");
        pixelsNear(loading.originalImage().copy(16,380,380,128),loadedRaw.copy(16,260,380,128),"late content repairs retained blank");
        require(loading.hdrImage().copy(16,380,380,128)==floatPlane(loadedRaw).copy(16,260,380,128),"late content repairs HDR blank");
    }
    // A compact header shrinks; its still-fixed upper rows stay masked. When
    // it disappears entirely, disoccluded rows must not retain a stale mask.
    {
        ScrollStitcher s;
        const auto header=page.copy(0,100,600,80);
        auto f=[&](int y,int h) {
            auto image=page.copy(0,600+y,600,660);
            if(h) {QPainter p(&image);p.drawImage(0,0,header.copy(0,0,600,h));}
            return image;
        };
        for(int y : {0,80,110,140,170}) {
            auto image=f(y,y?80:0);s.append(image,image);
        }
        const int before=s.layout().topOcclusion;
        require(before>=80,"full compact header masked");
        auto image=f(220,40);
        const auto r=s.append(image,image);
        require(r.status==Status::Appended,"shrinking header keeps tracking");
        require(s.layout().topOcclusion>=40 && s.layout().topOcclusion<before,"shrinking header preserves fixed upper rows");
        image=f(270,0);s.append(image,image);
        require(s.layout().topOcclusion==0,"disappearing header releases entire mask");
    }

    {
        ScrollStitcher s;
        for(int offset : {0,80,160,240,480,720,960,1200,1440,1680,1920,2160,2400}) {
            auto frame=page.copy(0,600+offset,600,660);
            {QPainter p(&frame);p.fillRect(0,280,48,48,Qt::white);
             p.setPen(Qt::black);p.drawText(4,310,"A");}
            const auto r=s.append(frame,frame);
            require(r.status==(offset?Status::Appended:Status::Started),"middle fixed control does not stop tracking");
            require(s.layout().viewportY==offset,"middle control cannot anchor scrolling document");
            require(s.layout().topOcclusion==0,"middle control is not top chrome");
        }
        pixelsNear(s.originalImage().copy(100,0,400,3060),page.copy(100,600,400,3060),"generic fast content remains complete beside middle control");

    }

    for(bool transparentMargin : {false,true}) {
        auto marginPage=page;
        {QPainter p(&marginPage);p.setCompositionMode(QPainter::CompositionMode_Source);p.fillRect(0,0,80,marginPage.height(),QColor(0,0,0,transparentMargin?0:255));
         for(int y=11;y<marginPage.height();y+=37)
             p.fillRect(0,y,600,1,QColor(y*31%255,y*17%255,y*7%255));
         for(int y=0;y<marginPage.height();++y)
             p.fillRect(100,y,8,1,QColor(y*17%251,y*31%255,0));
}
        for(bool toolbar : {false,true}) for(int widgetY : {180,280}) {
        ScrollStitcher s;
        auto fixture=marginPage;
        if(!toolbar) {
            // Exercise native border profiles independently of fixed chrome.
            QPainter p(&fixture);p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(0,0,8,fixture.height(),QColor(50,50,50,32));
            p.fillRect(8,0,3,fixture.height(),QColor(204,213,244));
        }
        auto f=[&](int offset) {
            auto image=fixture.copy(0,offset,600,660);
            QPainter p(&image);p.fillRect(12,widgetY,48,48,Qt::white);
            p.setPen(Qt::black);p.drawText(20,widgetY+30,"A");
            if(toolbar) {
                p.fillRect(0,0,80,90,Qt::white);
                p.drawText(15,38,"Back");p.drawText(15,72,"Tabs");
            }
            return image;
        };
        const std::vector<int> offsets=toolbar?std::vector<int>{0,80,240,360,700,1180,1660,2140,2620}:std::vector<int>{0,80,240,360,700,1200,1750,2300,2850};
        for(int offset : offsets) {
            const auto raw=f(offset);
            const auto r=s.append(raw,raw,floatPlane(raw));
            if(r.status!=(offset?Status::Appended:Status::Started) || s.layout().viewportY!=offset)
                std::cerr<<"margin toolbar="<<toolbar<<" offset="<<offset<<" status="<<ScrollStitcher::statusName(r.status)<<" reason="<<r.reason<<" viewport="<<s.layout().viewportY<<" top="<<s.layout().topOcclusion<<" bottom="<<s.layout().bottomOcclusion<<'\n';
            require(r.status==(offset?Status::Appended:Status::Started) && s.layout().viewportY==offset,"fast uniform-margin control keeps full motion region");
            if(s.layout().bottomOcclusion) std::cerr<<"margin offset="<<offset<<" bottom="<<s.layout().bottomOcclusion<<'\n';
            require(s.layout().bottomOcclusion==0,"plain margin widget cannot mask the scrolling photos");
        }
        const int end=offsets.back();
        auto expected=fixture.copy(0,0,600,end+660);
        {QPainter p(&expected);
         if(toolbar) p.drawImage(0,0,f(0).copy(0,0,80,90));
         p.drawImage(12,end+widgetY,f(end).copy(12,widgetY,48,48));}
        pixelsNear(s.originalImage(),expected,"uniform-margin widget retained once without losing rules or document rows");
        require(s.hdrImage()==floatPlane(expected),"uniform-margin local source keeps HDR geometry and values");
        }
    }

    {
        QImage dense(600,4000,QImage::Format_ARGB32);
        unsigned seed=817;
        for(int y=0;y<dense.height();++y) {
            auto *row=reinterpret_cast<QRgb*>(dense.scanLine(y));
            for(int x=0;x<dense.width();++x) {
                seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
                row[x]=qRgb(seed&255,(seed>>8)&255,(seed>>16)&255);
            }
        }
        ScrollStitcher scrollbar;
        for(int offset : {0,80,160,240,480,960,1440,1920,2400}) {
            auto raw=dense.copy(0,offset,600,660);
            {QPainter p(&raw);p.fillRect(582,0,18,660,Qt::white);
             p.fillRect(586,80+offset/8,12,40,QColor(140,140,140));}
            const auto r=scrollbar.append(raw,raw,floatPlane(raw));
            require(r.status==(offset?Status::Appended:Status::Started) && scrollbar.layout().viewportY==offset,"independently moving edge chrome cannot stall fast content");
            require(scrollbar.layout().bottomOcclusion==0,"scrollbar thumb cannot mask lower scrolling content");
        }
        pixelsNear(scrollbar.originalImage().copy(0,0,570,3060),dense.copy(0,0,570,3060),"scrollbar chrome preserves every main row");
        require(scrollbar.hdrImage().copy(0,0,570,3060)==floatPlane(dense.copy(0,0,570,3060)),"scrollbar chrome preserves HDR source geometry");
        auto f=[&](int offset) {
            auto image=dense.copy(0,offset,600,660);
            QPainter p(&image);p.fillRect(0,280,48,48,Qt::white);
            p.setPen(Qt::black);p.drawText(4,310,"A");return image;
        };
        ScrollStitcher s;
        for(int offset : {0,80,160,240}) {
            const auto raw=f(offset);s.append(raw,raw,floatPlane(raw));
        }
        require(s.layout().bottomOcclusion>330,"textured middle control requires a safe source band");
        const auto confirmed=s.originalImage(),confirmedHdr=s.hdrImage(),raw=f(600);
        const auto gap=s.append(raw,raw,floatPlane(raw));
        require(gap.status==Status::NoOverlap && std::string_view(gap.reason)=="occlusion-leaves-no-safe-overlap", "unsafe source gap is rejected explicitly despite matching pixels");
        require(s.originalImage()==confirmed && s.hdrImage()==confirmedHdr,"unsafe source cannot introduce transparent bands in any plane");
    }

}
