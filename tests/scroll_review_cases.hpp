#include <string>
#include <utility>
// Native-pixel regressions for changing scroll layers. Included by scroll_sticky_test.
QImage reviewFloat(const QImage& image) {
    QImage out(image.size(), QImage::Format_RGBA16FPx4_Premultiplied);
    for (int y=0;y<image.height();++y) {
        auto* row=reinterpret_cast<qfloat16*>(out.scanLine(y));
        for (int x=0;x<image.width();++x) {
            const auto c=image.pixelColor(x,y);
            row[4*x]=qfloat16(2.f+c.red()/255.f); row[4*x+1]=qfloat16(c.green()/255.f);
            row[4*x+2]=qfloat16(-.125f+c.blue()/255.f); row[4*x+3]=qfloat16(c.alpha()/255.f);
        }
    }
    return out;
}
void reviewAppend(ScrollStitcher& s,const QImage& f) {
    const auto r=s.append(f,f.rgbSwapped(),reviewFloat(f));
    require(r.status==Status::Started || r.status==Status::Appended || r.status==Status::Relocated,
            "review frame registers");
}
void reviewExact(const ScrollStitcher& s,const QImage& expected,const char* label) {
    require(s.image()==expected,label);
    require(s.originalImage()==expected.rgbSwapped(),"review original plane exact");
    require(s.hdrImage()==reviewFloat(expected),"review HDR plane exact");
}
void reviewCases(const std::string& which) {
    const auto run=[&](const char* name){return which.empty() || which==name;};
    if(run("wallpaper")) {
        const auto page=texture(240,1600,819), wallpaper=texture(120,360,732);
        const auto frame=[&](int offset) {
            QImage f(360,360,QImage::Format_ARGB32); QPainter p(&f);
            p.drawImage(0,0,page.copy(0,offset,240,360)); p.drawImage(240,0,wallpaper);
            // One earlier long output line, then several quiet frames, then
            // full-width output again. Dense stationary detail is wallpaper.
            if(offset==30 || offset>=210) p.drawImage(240,offset==30?100:340,texture(120,20,unsigned(offset)));
            return f;
        };
        ScrollStitcher s;
        for(int y : {0,30,60,90,120,150,180,210,240}) {
            reviewAppend(s,frame(y));
            require(s.layout().content.right()==359,"intermittent text over wallpaper must not pin columns");
        }
        require(s.image().copy(240,580,120,20)==frame(240).copy(240,340,120,20),
                "later long output is never truncated");
        require(s.originalImage()==s.image().rgbSwapped(),"wallpaper paired original pixels");
        require(s.hdrImage()==reviewFloat(s.image()),"wallpaper paired HDR pixels");
    }
    if(run("unsticky")) for(bool left : {false,true}) {
        const auto page=texture(240,1800,333), panel=texture(120,1800,515);
        const int sideX=left?0:240,mainX=left?120:0;
        const auto frame=[&](int offset) {
            QImage f(360,360,QImage::Format_ARGB32); QPainter p(&f);
            p.drawImage(mainX,0,page.copy(0,offset,240,360));
            const int sideY=offset<=30?offset:offset<=150?30:offset-120;
            p.drawImage(sideX,0,panel.copy(0,sideY,120,360)); return f;
        };
        ScrollStitcher s;
        for(int y : {0,30,60,90,120,150}) reviewAppend(s,frame(y));
        require(s.layout().content.width()==240,"section panel pins");
        const auto before=s.image();
        for(int y : {180,210,240}) reviewAppend(s,frame(y));
        require(s.layout().content==QRect(0,0,360,360),"section panel resumes scrolling and reclaims width");
        require(s.image().copy(mainX,0,240,600)==page.copy(0,0,240,600),"reclaim keeps main document exact");
        require(s.image().copy(sideX,390,120,210)==panel.copy(0,270,120,210),"unsticky tail follows document translation");
        require(s.image().copy(sideX,0,120,180)==before.copy(sideX,0,120,180),"reclaim preserves pre-pin history");
        require(s.originalImage()==s.image().rgbSwapped(),"reclaim original planes agree");
        require(s.hdrImage()==reviewFloat(s.image()),"reclaim HDR planes agree");
    }
    if(run("fallback")) {
        const auto page=texture(240,1600,432), side=texture(120,1600,642), replacement=texture(120,360,743);
        const auto frame=[&](int offset) {
            QImage f(360,360,QImage::Format_ARGB32); QPainter p(&f);
            p.drawImage(0,0,page.copy(0,offset+300,240,360));
            p.drawImage(240,0,offset>=30?replacement:side.copy(0,offset+300,120,360)); return f;
        };
        ScrollStitcher s;
        for(int y : {0,-60,0,30,60,90,120}) reviewAppend(s,frame(y));
        require(s.layout().minimumY==-60 && s.layout().content.width()==240,"negative history then unalignable panel pins");
        require(s.image().copy(240,0,120,60)==side.copy(0,240,120,60),"fallback preserves negative prefix without a hole");
        require(s.originalImage().copy(240,0,120,60)==side.copy(0,240,120,60).rgbSwapped(),"fallback original prefix exact");
        require(s.hdrImage().copy(240,0,120,60)==reviewFloat(side.copy(0,240,120,60)),"fallback HDR prefix exact");
    }
    if(run("top-wallpaper")) {
        const auto page=texture(360,1600,447),wallpaper=texture(360,80,638);
        const auto frame=[&](int offset) {QImage f(360,360,QImage::Format_ARGB32);QPainter p(&f);
            p.drawImage(0,0,wallpaper);p.drawImage(0,80,page.copy(0,offset,360,280));
            if(offset==30) p.drawImage(0,20,texture(360,20,914));
            return f;};
        ScrollStitcher s;
        for(int y : {0,30,60,90,120,150,180}) {
            reviewAppend(s,frame(y));
            require(s.layout().topOcclusion==0,"intermittent content over top wallpaper cannot become a persistent header mask");
        }
        const auto expected=page.copy(0,0,360,460);
        require(s.image().copy(0,80,360,460)==expected,"top wallpaper leaves moving body exact");
        require(s.originalImage().copy(0,80,360,460)==expected.rgbSwapped(),"top wallpaper original body exact");
        require(s.hdrImage().copy(0,80,360,460)==reviewFloat(expected),"top wallpaper HDR body exact");
    }
    if(run("top-release")) {
        const auto page=texture(360,1800,321);
        const auto frame=[&](int offset) {
            auto f=page.copy(0,offset+60,360,360);
            if(offset>=30 && offset<=150) {QPainter p(&f);p.drawImage(0,0,page.copy(0,90,360,40));}
            return f;
        };
        ScrollStitcher s;
        for(int y : {0,30,60,90,120,150}) reviewAppend(s,frame(y));
        require(s.layout().topOcclusion>=40,"emerging top panel earns source mask");
        reviewAppend(s,frame(180));
        require(s.layout().topOcclusion==0,"translating top rows release the old source mask");
        reviewAppend(s,frame(-30));
        require(s.layout().minimumY==-30,"released mask permits complete upward extension");
        const auto prefix=page.copy(0,30,360,30);
        require(s.image().copy(0,0,360,30)==prefix,"released top mask captures prefix");
        require(s.originalImage().copy(0,0,360,30)==prefix.rgbSwapped(),"released mask original prefix exact");
        require(s.hdrImage().copy(0,0,360,30)==reviewFloat(prefix),"released mask HDR prefix exact");
    }
    if(run("limits")) {
        const auto page=texture(360,1800,929), title=texture(360,40,218);
        const auto frame=[&](int offset) {auto f=page.copy(0,300+offset,360,360);
            if(offset>=60 || offset<0) {QPainter p(&f);p.drawImage(0,0,title);} return f;};
        ScrollStitcher s({32768,64LL*1024*1024,6});
        for(int y : {0,30,60,90,120,150}) reviewAppend(s,frame(y));
        require(s.layout().topOcclusion==40,"limit test pins top mask");
        auto f=frame(-10);
        const auto r=s.append(f,f.rgbSwapped(),reviewFloat(f));
        require(r.status==Status::Relocated && r.addedRows==0,"header-only upward rows do not hit maxFrames");
        require(s.layout().minimumY==0 && s.layout().viewportY==-10 && s.frameCount()==6,"header-only relocation keeps retained extent and frame count");
        auto far=frame(180);const auto before=s.image();
        require(s.append(far,far.rgbSwapped(),reviewFloat(far)).status==Status::LimitReached,"real growth still hits frame limit");
        require(s.image()==before,"limit rejection preserves output");
    }
    if(run("both")) for(bool header : {false,true}) {
        const auto page=texture(240,1600,929);
        QImage panel(120,1600,QImage::Format_ARGB32);panel.fill(QColor(220,225,230));
        {QPainter p(&panel);for(int y : {50,100,200})p.drawImage(0,y,texture(120,12,unsigned(y)));}
        const int top=header?24:0;
        const auto frame=[&](int offset) {QImage f(480,360+top,QImage::Format_ARGB32);f.fill(Qt::blue);
            QPainter p(&f);p.drawImage(120,top,page.copy(0,offset,240,360));
            for(int x : {0,360}) p.drawImage(x,top,panel.copy(0,std::min(offset,30),120,360));
            return f;};
        ScrollStitcher s;for(int y : {0,30,60,90,120,150})reviewAppend(s,frame(y));
        require(s.layout().content==QRect(120,top,240,360),"both sticky sides preserve shared main rectangle");
        QImage expected(480,510+top,QImage::Format_ARGB32);expected.fill(Qt::blue);
        {QPainter p(&expected);p.drawImage(120,top,page.copy(0,0,240,510));
         for(int x : {0,360}) {p.drawImage(x,top,panel.copy(0,0,120,370));
             p.fillRect(x,top+370,120,120,QColor(220,225,230));
             p.drawImage(x,top+490,panel.copy(0,370,120,20));}}
        reviewExact(s,expected,"both sides and header compose exactly");
    }
    if(run("thresholds")) for(int sideWidth : {7,8,9,63,64}) {
        const auto page=texture(257-sideWidth,1500,417),panel=texture(sideWidth,1500,518);
        ScrollStitcher s;
        for(int offset : {0,30,60,90,120}) {QImage f(257,361,QImage::Format_ARGB32);QPainter p(&f);
            p.drawImage(0,0,page.copy(0,offset,page.width(),361));
            p.drawImage(page.width(),0,panel.copy(0,std::min(offset,30),sideWidth,361));p.end();reviewAppend(s,f);}
        require(s.layout().content.width()==(sideWidth<8?257:page.width()),"eight-column pin support threshold");
        require(s.image().copy(0,0,page.width(),481)==page.copy(0,0,page.width(),481),"threshold leaves main output exact");
        require(s.originalImage().copy(0,0,page.width(),481)==page.copy(0,0,page.width(),481).rgbSwapped(),"threshold original body exact");
        require(s.hdrImage().copy(0,0,page.width(),481)==reviewFloat(page.copy(0,0,page.width(),481)),"threshold HDR body exact");
    }
    if(run("sampling")) for(int height : {361,722,1083,2161}) for(int period : {3,4,6,8,10}) {
        const auto page=texture(240,height+200,112);
        QImage panel(120,height+200,QImage::Format_ARGB32);panel.fill(QColor(220,225,230));
        {QPainter p(&panel);for(int y=0;y<panel.height();y+=period)
            p.fillRect(0,y,120,1,QColor(20+(y/period)%100,40,60));}
        ScrollStitcher s;
        for(int offset : {0,17,36,55,74}) {QImage f(360,height,QImage::Format_ARGB32);QPainter p(&f);
            p.drawImage(0,0,page.copy(0,offset,240,height));
            p.drawImage(240,0,panel.copy(0,std::min(offset,17),120,height));p.end();reviewAppend(s,f);}
        require(s.layout().content.width()==240,"distributed samples do not alias periodic sidebar rules");
        require(s.image().copy(0,0,240,height+74)==page.copy(0,0,240,height+74),"sampled registration main body exact");
        require(s.originalImage().copy(0,0,240,height+74)==page.copy(0,0,240,height+74).rgbSwapped(),"sampled original body exact");
        require(s.hdrImage().copy(0,0,240,height+74)==reviewFloat(page.copy(0,0,240,height+74)),"sampled HDR body exact");
    }
    if(run("sizes")) for(const QSize size : {QSize(64,96),QSize(65,129),QSize(127,193),QSize(257,361),QSize(514,722)}) {
        auto page=texture(size.width(),size.height()+300,918);
        page.setDevicePixelRatio(2); // Stitching is always in native pixels.
        ScrollStitcher s;
        for(int y : {0,1,3,17,30,20}) {
            const auto f=page.copy(0,y,size.width(),size.height());
            const auto r=s.append(f,f.rgbSwapped(),reviewFloat(f));
            if(r.status!=Status::Started && r.status!=Status::Appended && r.status!=Status::Relocated)
                std::cerr<<"small viewport "<<size.width()<<"x"<<size.height()<<" offset="<<y<<" status="<<int(r.status)<<'\n';
            require(r.status==Status::Started || r.status==Status::Appended || r.status==Status::Relocated,"small/odd frame registers");
        }
        page.setDevicePixelRatio(1);
        reviewExact(s,page.copy(0,0,size.width(),size.height()+30),"odd/small/HiDPI viewport exact");
    }
    if(run("gutters")) for(bool relative : {false,true}) for(bool dark : {false,true}) {
        QImage page(401,1500,QImage::Format_ARGB32); page.fill(dark?QColor(20,22,24):QColor(250,250,250));
        {QPainter p(&page);p.drawImage(100,0,texture(201,1500,622));
         p.setPen(dark?Qt::white:Qt::black);for(int y=15;y<1500;y+=27)p.drawText(65,y,QString::number(y/27));}
        ScrollStitcher s;
        for(int offset : {0,27,54,81,108,135,162,189}) {
            auto f=page.copy(0,offset,401,361);
            if(relative) {QPainter p(&f);p.fillRect(64,0,30,361,dark?QColor(20,22,24):QColor(250,250,250));
                p.setPen(dark?Qt::white:Qt::black);for(int y=15;y<361;y+=27)p.drawText(65,y,QString::number(std::abs(y/27-6)));}
            reviewAppend(s,f);
            require(s.layout().content.left()<=100 && s.layout().content.right()>=300,"line gutter cannot truncate code or uniform margins");
        }
        const auto body=page.copy(100,0,201,550);
        require(s.image().copy(100,0,201,550)==body,"code body exact with absolute/relative gutter");
        require(s.originalImage().copy(100,0,201,550)==body.rgbSwapped(),"code gutter original body exact");
        require(s.hdrImage().copy(100,0,201,550)==reviewFloat(body),"code gutter HDR body exact");
        if(!relative) reviewExact(s,page.copy(0,0,401,550),"dark/light uniform margins and absolute gutter exact");
    }
    if(run("alpha")) for(const auto& sample : {std::pair{150,250},std::pair{150,320},std::pair{510,160}}) {
        auto page=texture(360,1400,914);
        ScrollStitcher s;
        auto a=page.copy(0,200,360,600), raw=a;
        raw.setPixelColor(170,sample.first,QColor(20,40,60,100));
        s.append(a,raw,reviewFloat(raw));
        const auto b=page.copy(0,sample.second,360,600);
        require(s.append(b,b,reviewFloat(b)).status==Status::Appended,"alpha recovery scroll accepted");
        const int y=sample.first-s.layout().minimumY;
        require(s.originalImage().pixel(170,y)==page.pixel(170,200+sample.first),
                "non-corner alpha upgrade in top quarter or within 64px of either edge is recovered");
        require(s.hdrImage().copy(170,y,1,1)==reviewFloat(page.copy(170,200+sample.first,1,1)),"alpha recovery HDR sample exact");
    }
}
