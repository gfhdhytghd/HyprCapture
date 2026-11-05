#include "ui/scroll_stitcher.hpp"
#include <QFont>
#include <QGuiApplication>
#include <QPainter>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <string_view>
using namespace hyprcapture;
using ui::ScrollStitcher;
using Status = ScrollStitcher::Status;
void require(bool ok, const char *message) {
  if (!ok) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
QImage textPage(int kind, bool periodic = false) {
  QImage page(3072, 4200, QImage::Format_ARGB32);
  page.fill(QColor(248, 248, 248));
  QPainter p(&page);
  QFont font("monospace");
  font.setStyleHint(QFont::Monospace);
  font.setPixelSize(11);
  p.setFont(font);
  for (int y = 0, line = 0; y < page.height(); y += 20, ++line) {
    const auto key = periodic ? QString("000000000000")
                              : QString::number(uint64_t(line + 73) * 7919, 16)
                                    .rightJustified(12, '0');
    p.setPen(QColor(24, 24, 24));
    if (kind == 0)
      p.drawText(96, y + 14,
                 "2026-10-14 19:21:02 INFO completed request id=" + key +
                     " result=ok duration=12ms");
    if (kind == 1)
      p.drawText(128, y + 14,
                 "    const auto value = lookup(\"" + key +
                     "\"); // same layout, different identifiers");
    if (kind == 3) {
      p.drawText(96, y + 14, "Entry message: identical repeated wording");
      p.drawText(1537, y + 14, QString(QChar(33 + line % 90)));
    }
    if (kind == 2) {
      p.drawText(96, y + 14,
                 "User: same display name     Message " + key +
                     ": operation completed successfully");
      p.setPen(QColor(228, 228, 228));
      p.drawLine(80, y + 19, 2900, y + 19);
    }
  }
  return page;
}
int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  cv::setNumThreads(1);
  for (int kind = 0; kind < 4; ++kind)
    for (int step : {3, 160, 480}) {
      const auto page = textPage(kind);
      ScrollStitcher stitcher;
      int end = 0;
      for (int offset :
           {0, step, step * 2, step * 3, step * 2, step * 3, step * 4}) {
        const auto raw = page.copy(0, offset, 3072, 640);
        const auto result = stitcher.append(raw, raw);
        if (stitcher.layout().viewportY != offset)
          std::cerr << "text kind=" << kind << " step=" << step
                    << " offset=" << offset
                    << " measured=" << stitcher.layout().viewportY
                    << " status=" << ScrollStitcher::statusName(result.status)
                    << " reason=" << result.reason << '\n';
        require(stitcher.layout().viewportY == offset,
                "native glyphs disambiguate fast/reverse uniform text layouts");
        require(result.status == Status::Started ||
                    result.status == Status::Appended ||
                    result.status == Status::Relocated,
                "text frame is independently pixel registered");
        end = std::max(end, offset);
      }
      require(stitcher.originalImage() == page.copy(0, 0, 3072, 640 + end),
              "text rows are neither repeated nor omitted");
    }
  {
    // A moving scrollbar prevents whole-row equality. The small native ID in
    // the middle is still the only distinction between periodic text rows.
    const auto page=textPage(3);
    ScrollStitcher columns;
    for(int offset:{0,160,640,1120,640,160,640}) {
      auto raw=page.copy(0,offset,3072,640);
      {QPainter p(&raw);p.fillRect(3056,0,16,640,QColor(180,180,180));
       p.fillRect(3056,20+offset/8,16,40,QColor(80,80,80));}
      const auto r=columns.append(raw,raw);
      if(columns.layout().viewportY!=offset) std::cerr<<"columns offset="<<offset<<" viewport="<<columns.layout().viewportY<<" status="<<int(r.status)<<" reason="<<r.reason<<'\n';
      require((r.status==Status::Started || r.status==Status::Appended || r.status==Status::Relocated) &&
                  columns.layout().viewportY==offset,
              "native columns preserve tiny glyph identity beside independent chrome");
    }
    require(columns.originalImage().copy(0,0,3000,1760)==page.copy(0,0,3000,1760),
            "column candidate generation retains exact fast and reverse text");
  }
  {
    // A short label at the far end of a sticky panel must borrow evidence
    // from its whole side lane, before three-frame column pinning completes.
    QImage page(480,2600,QImage::Format_ARGB32),side(160,1000,QImage::Format_ARGB32);
    unsigned seed=739;
    for(int y=0;y<page.height();++y) for(int x=0;x<page.width();++x) {
      seed=seed*1664525u+1013904223u;
      page.setPixel(x,y,qRgba(seed&255,(seed>>8)&255,(seed>>16)&255,217));
    }
    side.fill(QColor(244,246,249,217));
    {QPainter p(&side);
     for(int y=4;y<1000;y+=37) p.fillRect(12,y,62,12,QColor((y*7)%200,40,70));
     p.fillRect(112,500,28,24,Qt::blue);}
    ScrollStitcher sticky;
    for(int offset:{0,120,240,360,480,600,720,840,960,840,600,960}) {
      QImage raw(640,600,QImage::Format_ARGB32);
      for(int y=0;y<600;++y) {
        std::memcpy(raw.scanLine(y),page.constScanLine(y+offset),480*4);
        std::memcpy(raw.scanLine(y)+480*4,side.constScanLine(y+std::min(offset,200)),160*4);
      }
      {QPainter p(&raw);p.setPen(Qt::NoPen);p.setBrush(Qt::blue);p.drawEllipse(220,572,28,28);}
      const auto r=sticky.append(raw,raw);
      require(r.status==Status::Started || r.status==Status::Appended || r.status==Status::Relocated,
              "fast sticky transition and reverse remain tracked on translucent pixels");
      if(sticky.layout().viewportY!=offset || sticky.layout().bottomOcclusion>=80)
        std::cerr<<"label offset="<<offset<<" viewport="<<sticky.layout().viewportY<<" bottom="<<sticky.layout().bottomOcclusion<<" width="<<sticky.layout().content.width()<<'\n';
      require(sticky.layout().viewportY==offset && sticky.layout().bottomOcclusion<80,
              "short sidebar label cannot become a full-width middle occlusion");
    }
    require(sticky.originalImage().copy(0,0,470,1500)==page.copy(0,0,470,1500),
            "sticky label and bottom control preserve all native main rows");
  }
  {
    // Small illumination changes force native correlation rather than exact
    // row hashes. Repeated card/text chrome must not create a short-overlap
    // reverse jump. A rejected ambiguous repaint is retried with clear pixels.
    const auto page=textPage(2);
    ScrollStitcher fading;
    fading.append(page.copy(0,0,3072,1600));
    for(int offset:{160,640,1120,1600,2080,1600,1120,1600,2080}) {
      auto raw=page.copy(0,offset,3072,1600),dim=raw;
      for(int y=0;y<dim.height();++y) for(int x=0;x<dim.width();++x) {
        const auto c=dim.pixel(x,y);
        dim.setPixel(x,y,qRgb(int(qRed(c)*.98),int(qGreen(c)*.98),int(qBlue(c)*.98)));
      }
      const auto r=fading.append(dim);
      if(r.status==Status::Appended || r.status==Status::Relocated || r.status==Status::Unchanged)
        require(fading.layout().viewportY==offset,
                "correlation cannot guess a repeated short-overlap reverse jump");
      fading.append(raw);
      require(fading.layout().viewportY==offset,
              "clear native pixels reacquire after an ambiguous illumination frame");
    }
    const auto stitched=fading.image(),expected=page.copy(0,0,3072,3680);
    require(stitched.size()==expected.size(),"illumination cannot change text canvas geometry");
    for(int y=0;y<expected.height();++y) for(int x=0;x<expected.width();++x)
      require(std::abs(qRed(stitched.pixel(x,y))-qRed(expected.pixel(x,y)))<=8,
              "faded repeated text preserves native glyph positions");
  }
  {
    // The new compact header is byte-identical to a previous card heading
    // near the bottom. A tiny exact overlap must not beat the scrolling body.
    QImage page(640,3000,QImage::Format_ARGB32);
    unsigned seed=911;
    for(int y=0;y<page.height();++y) for(int x=0;x<page.width();++x) {
      seed=seed*1664525u+1013904223u;
      page.setPixel(x,y,qRgb(seed&255,(seed>>8)&255,(seed>>16)&255));
    }
    QImage header(640,145,QImage::Format_ARGB32);header.fill(Qt::white);
    {QPainter p(&header);p.setPen(Qt::black);p.drawText(50,35,"Same author and repeated card header");
     p.fillRect(10,15,28,28,Qt::blue);p.setPen(Qt::gray);p.drawRect(0,0,639,144);}
    {QPainter p(&page);p.drawImage(0,1455,header);}
    ScrollStitcher compact;
    const auto first=page.copy(0,0,640,1600);
    compact.append(first,first);
    for(int offset:{200,400,200}) {
      auto raw=page.copy(0,offset,640,1600);
      if(offset!=200 || compact.layout().viewportY==0) {QPainter p(&raw);p.drawImage(0,0,header);}
      const auto r=compact.append(raw,raw);
      if(compact.layout().viewportY!=offset) std::cerr<<"compact offset="<<offset<<" actual="<<compact.layout().viewportY<<" status="<<int(r.status)<<" reason="<<r.reason<<" top="<<compact.layout().topOcclusion<<'\n';
      require((r.status==Status::Appended || r.status==Status::Relocated) && compact.layout().viewportY==offset,
              "sparse exact header overlap cannot invent a large forward jump");
    }
    require(compact.originalImage()==page.copy(0,0,640,2000),
            "appearing and hiding repeated chrome preserves complete native rows");
  }
  {
    // Mirrored native glyphs have equal area in each10px cell. A320px-wide
    // INTER_AREA image loses every identifier even though native rows differ.
    QImage page(3200, 3000, QImage::Format_ARGB32);
    page.fill(QColor(248, 248, 248));
    QPainter painter(&page);
    painter.setPen(QColor(24, 24, 24));
    QFont font("monospace");
    font.setPixelSize(11);
    painter.setFont(font);
    for (int y = 0, line = 0; y < page.height(); y += 20, ++line) {
      painter.drawText(96, y + 14, "INFO status OK value=");
      const unsigned id = (line + 1) * 7919;
      for (int cell = 0; cell < 12; ++cell)
        for (int row = 0; row < 5; ++row) {
          const int b[5] = {4, 4, 6, 5, 6}, d[5] = {1, 1, 3, 5, 3};
          const int bits = (id >> cell) & 1 ? b[row] : d[row];
          for (int x = 0; x < 3; ++x)
            if (bits & (1 << x))
              painter.fillRect(1000 + cell * 10 + x, y + 8 + row, 1, 1,
                               QColor(24, 24, 24));
        }
    }
    painter.end();
    const auto first = page.copy(0, 0, 3200, 640),
               next = page.copy(0, 240, 3200, 640);
    cv::Mat coarseFirst, coarseNext;
    cv::resize(
        cv::Mat(640, 3200, CV_8UC4, const_cast<uchar *>(first.constBits())),
        coarseFirst, {320, 640}, 0, 0, cv::INTER_AREA);
    cv::resize(
        cv::Mat(640, 3200, CV_8UC4, const_cast<uchar *>(next.constBits())),
        coarseNext, {320, 640}, 0, 0, cv::INTER_AREA);
    require(cv::countNonZero(coarseFirst.reshape(1) != coarseNext.reshape(1)) ==
                0,
            "fixture proves coarse candidate generation erases glyph identity");
    for (bool lighting : {false, true}) {
      auto current = next;
      if (lighting)
        for (int y = 0; y < current.height(); ++y)
          for (int x = 0; x < current.width(); ++x) {
            const auto c = current.pixel(x, y);
            current.setPixel(x, y,
                             qRgb(qRed(c) + 1, qGreen(c) + 1, qBlue(c) + 1));
          }
      ScrollStitcher native;
      native.append(first, first);
      const auto result = native.append(current, current);
      require(result.status == Status::Appended && result.displacement == 240,
              "native whole-window candidates survive exact coarse aliasing "
              "and luminance drift");
      require(native.originalImage().height() == 880,
              "alias-resistant match retains the complete page");
    }
  }
  for (bool dynamic : {false, true}) {
    const auto page = textPage(2);
    ScrollStitcher browser;
    int end = 0;
    for (int offset : {0, 160, 640, 1120, 1600, 1120, 640, 160, 640}) {
      QImage raw(3072, 736, QImage::Format_ARGB32);
      raw.fill(QColor(55, 60, 70));
      QPainter painter(&raw);
      painter.drawImage(0, 80, page.copy(0, offset, 3072, 640));
      painter.setPen(Qt::white);
      painter.drawText(24, 40, "Browser toolbar, URL, tabs");
      painter.drawText(24, 730, "Fixed footer");
      if (dynamic && offset >= 300) {
        painter.fillRect(0, 80, 3072, 48, QColor(240, 240, 240));
        painter.setPen(Qt::black);
        painter.drawText(24, 110, "Back / compact header");
      }
      painter.end();
      const auto result = browser.append(raw, raw);
      if (browser.layout().viewportY != offset)
        std::cerr << "browser dynamic=" << dynamic << " offset=" << offset
                  << " viewport=" << browser.layout().viewportY
                  << " reason=" << result.reason << '\n';
      require(browser.layout().viewportY == offset,
              "native text search survives fixed browser chrome and fast "
              "dynamic headers");
      end = std::max(end, offset);
    }
    require(browser.originalImage().copy(0, 80, 3072, end + 640) ==
                page.copy(0, 0, 3072, end + 640),
            "browser chrome does not duplicate, hide or shift any text row");
  }
  const auto periodic = textPage(0, true);
  ScrollStitcher ambiguous;
  ambiguous.append(periodic.copy(0, 0, 3072, 640));
  const auto before = ambiguous.image();
  const auto uncertain = ambiguous.append(periodic.copy(0, 270, 3072, 640));
  require(
      uncertain.status == Status::Ambiguous ||
          uncertain.status == Status::NoOverlap,
      "truly repeated text cannot select an arbitrary period without a prior");
  require(ambiguous.image() == before,
          "ambiguous text leaves confirmed output unchanged");

  auto mixed = textPage(1);
  {
    QPainter p(&mixed);
    p.drawImage(0, 700, periodic.copy(0, 0, 3072, 3500));
  }
  ScrollStitcher guided;
  ScrollMotionSnapshot input;
  uint64_t time = 1000000000;
  auto feed = [&](int offset, double delta, bool settled = false) {
    time += settled ? 300000000 : 100000000;
    if (delta) {
      input.amount[0] += delta;
      input.travel[0] += std::abs(delta);
      input.lastInputNs = time;
    }
    const auto raw = mixed.copy(0, offset, 3072, 640);
    return guided.append(raw, raw, {}, &input, time);
  };
  feed(0, 0);
  for (int offset : {100, 200}) {
    feed(offset, 1);
    feed(offset, 0, true);
  }
  bool used = false;
  for (int offset : {400, 600, 800, 1290}) {
    const auto result =
        feed(offset, (offset - guided.layout().viewportY) / 100.);
    if (guided.layout().viewportY != offset)
      std::cerr << "prior text offset=" << offset
                << " measured=" << guided.layout().viewportY
                << " reason=" << result.reason
                << " predicted=" << result.predictedOffset
                << " tolerance=" << result.priorTolerance << '\n';
    require(
        guided.layout().viewportY == offset,
        "learned input gain selects one native-supported repeated-text peak");
    used |= result.priorUsed;
  }
  require(used, "repeated-text fixture exercises prior-based tie resolution");
  require(guided.originalImage() == mixed.copy(0, 0, 3072, 1930),
          "motion prior preserves exact observed text pixels");
  const auto confirmed = guided.originalImage();
  const auto identical = feed(1490, 2);
  require(identical.status == Status::Ambiguous &&
              std::string_view(identical.reason) ==
                  "identical-pixels-with-pending-input",
          "input cannot distinguish a clamped edge from an invisible "
          "whole-period scroll");
  require(guided.originalImage() == confirmed,
          "identical pixels never manufacture repeated rows from input alone");
  feed(1290, 0, true); // settle ambiguous input without training from it

  const auto conflict =
      feed(1560, 2); // prior200px, native periods are190,210,...
  require(
      conflict.status == Status::Ambiguous ||
          conflict.status == Status::NoOverlap,
      "strongly conflicting prior cannot choose an unsupported periodic peak");
  require(guided.originalImage() == confirmed,
          "conflicting input cannot extend confirmed content");
  ScrollStitcher contrary;
  ScrollMotionSnapshot amounts;
  uint64_t clock = 1000000000;
  const auto unique = textPage(2);
  const auto observe = [&](int offset, double delta, bool settle = false) {
    clock += settle ? 300000000 : 100000000;
    if (delta) {
      amounts.amount[0] += delta;
      amounts.travel[0] += std::abs(delta);
      amounts.lastInputNs = clock;
    }
    const auto raw = unique.copy(0, offset, 3072, 640);
    return contrary.append(raw, raw, {}, &amounts, clock);
  };
  observe(0, 0);
  for (int offset : {100, 200}) {
    observe(offset, 1);
    observe(offset, 0, true);
  }
  const auto pixelsWin = observe(500, 1);
  require(std::abs(pixelsWin.predictedOffset - 100) < 1 &&
              pixelsWin.displacement == 300 && !pixelsWin.priorUsed,
          "unique native pixels override a badly wrong calibrated prediction");
}
