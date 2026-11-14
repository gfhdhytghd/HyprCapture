#include "ui/live_text_layout.hpp"
#include <QImage>
#include <QPainter>
#include <QTest>
#include <cmath>

using hyprcapture::ui::LiveTextLayout;
namespace {
std::vector<hyprcapture::OcrResult> tiltedFixture() {
    // Private fixture geometry only; text preserves width classes, not content.
    return {
        {"a aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa字字字字字字字字字字字字字字字字字字字字字字字字字字字字字字字", {{21.299999237060547, 32.16999816894531}, {1698.050048828125, 30.610000610351562}, {1698.0799560546875, 62.77000045776367}, {21.329999923706055, 64.31999969482422}}},
        {"a 字字字字字字字字字字字字字字字字字字字aaaaaaaaaaaaaaaaaaaaaaaaaaa字aaaaaaaaaaaaaaaaaaaaaaaa字a", {{21.329999923706055, 71.95999908447266}, {1553.8399658203125, 73.5}, {1553.81005859375, 101.06999969482422}, {21.299999237060547, 99.52999877929688}}},
        {"aa字 aaaaaaaaaaaaaaaa 字字字字字字字字字 aaa 字字字字字字字字字字字字字 aa 字字字字字字字字字字字字字字字字字字", {{55.72999954223633, 114.83999633789062}, {1825.9200439453125, 114.83999633789062}, {1825.9200439453125, 140.8800048828125}, {55.72999954223633, 140.8800048828125}}},
        {"aa 字 aaaaaaaaa 字字字字字字字字 aaaaa 字字字字字字字", {{54.09000015258789, 154.66000366210938}, {883.4500122070312, 154.66000366210938}, {883.4500122070312, 180.69000244140625}, {54.09000015258789, 180.69000244140625}}},
        {"a字字字字字字字字字字字字字字字字字字字字字字字", {{55.72999954223633, 192.94000244140625}, {788.3900146484375, 192.94000244140625}, {788.3900146484375, 218.97000122070312}, {55.72999954223633, 218.97000122070312}}},
        {"a字字字字字字字字字字字字字字字", {{24.59000015258789, 234.27999877929688}, {516.2999877929688, 234.27999877929688}, {516.2999877929688, 261.8399963378906}, {24.59000015258789, 261.8399963378906}}},
    };
}
}
class LiveTextLayoutTest final : public QObject {
    Q_OBJECT
  private slots:
    void nonEmptyGlyphs() {
        LiveTextLayout layout; layout.setResults(tiltedFixture());
        QCOMPARE(layout.runs.size(),std::size_t(6));
        int totalEmpty=0;
        for (const auto& run : layout.runs) {
            int empty=0;
            for (const auto& glyph : run.glyphs) empty += glyph.area.isEmpty();
            qInfo("row %d: %d/%zu empty glyphs",run.row,empty,run.glyphs.size());
            totalEmpty+=empty;
        }
        QCOMPARE(totalEmpty,0);
    }
    void sharedEdges() {
        LiveTextLayout layout; layout.setResults(tiltedFixture());
        for (const auto& run : layout.runs) {
            double last=0;
            for (const auto& glyph : run.glyphs) {
                const auto rect=run.fromUnit.inverted().map(glyph.area).boundingRect();
                QVERIFY(std::abs(rect.left()-last)<1e-9);
                QVERIFY(std::abs(rect.top())<1e-9);
                QVERIFY(std::abs(rect.bottom()-1)<1e-9);
                QVERIFY(rect.width()>0);
                QCOMPARE(glyph.x0,last);
                QVERIFY(std::abs(rect.left()-glyph.x0)<1e-9);
                QVERIFY(std::abs(rect.right()-glyph.x1)<1e-9);
                last=rect.right();
            }
            QVERIFY(std::abs(last-1)<1e-9);
        }
    }
    void monotonicHit() {
        LiveTextLayout layout; layout.setResults(tiltedFixture());
        for (const auto& run : layout.runs) for (double v : {.2,.5,.8}) {
            const double width=std::hypot(run.fromUnit.m11(),run.fromUnit.m12());
            int last=run.begin;
            QCOMPARE(layout.hit(run.fromUnit.map(QPointF(-.001,v)),0,true),run.begin);
            for (double px=0;px<=width;px+=.25) {
                const auto p=run.fromUnit.map(QPointF(px/width,v));
                if (px>1 && px<width-1) QVERIFY(layout.contains(p));
                const int offset=layout.hit(p,1);
                QVERIFY(offset>=last); QVERIFY(offset<=run.end); last=offset;
            }
            QCOMPARE(last,run.end);
            QCOMPARE(layout.hit(run.fromUnit.map(QPointF(1.001,v)),0,true),run.end);
        }
    }
    void wholeMiddleLine() {
        LiveTextLayout layout; layout.setResults(tiltedFixture());
        const auto selected=layout.selection(layout.runs[0].begin,layout.runs[2].end);
        const auto& run=layout.runs[1];
        for (int i=0;i<2000;++i)
            QVERIFY(selected.contains(run.fromUnit.map(QPointF((i+.5)/2000,.5))));
    }
    void selectionPixels() {
        LiveTextLayout layout; layout.setResults(tiltedFixture());
        QImage mask(2098,294,QImage::Format_ARGB32_Premultiplied); mask.fill(Qt::transparent);
        QPainter painter(&mask); painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(layout.selection(0,layout.text.size()),Qt::white); painter.end();
        for (const auto& run : layout.runs) for (int y=0;y<mask.height();++y) for (int x=0;x<mask.width();++x) {
            const auto unit=run.fromUnit.inverted().map(QPointF(x+.5,y+.5));
            if (unit.x()>.002 && unit.x()<.998 && unit.y()>.1 && unit.y()<.9)
                QCOMPARE(qAlpha(mask.pixel(x,y)),255);
        }
    }
    void overlappingRunsDoNotCancel() {
        LiveTextLayout layout; layout.setResults(tiltedFixture());
        layout.runs.push_back(layout.runs.front());
        const auto selected=layout.selection(0,layout.text.size());
        QVERIFY(selected.contains(layout.runs.front().fromUnit.map(QPointF(.3,.5))));
    }
    void rotatedHit_data() {
        QTest::addColumn<double>("angle");
        for(double angle : {-70.,-.06,0.,.08,32.,90.,160.})
            QTest::newRow(qPrintable(QString::number(angle))) << angle;
    }
    void rotatedHit() {
        QFETCH(double,angle);
        QTransform transform; transform.translate(150,150); transform.rotate(angle);
        hyprcapture::OcrResult result; result.text="A😀é中";
        for(const auto p:transform.map(QPolygonF(QRectF(0,0,200,30)))) result.poly.push_back({p.x(),p.y()});
        LiveTextLayout layout; layout.setResults({result});
        auto& run=layout.runs.front();
        for(auto& glyph:run.glyphs) {
            const auto p=run.fromUnit.map(QPointF((glyph.x0+glyph.x1)/2,.5));
            QVERIFY(layout.contains(p));
            QCOMPARE(layout.hit(run.fromUnit.map(QPointF(glyph.x0+.001,.5))),glyph.begin);
            QCOMPARE(layout.hit(run.fromUnit.map(QPointF(glyph.x1-.001,.5))),glyph.end);
            // Neither null paths nor arbitrary paint geometry can move carets.
            glyph.area={};
            QCOMPARE(layout.hit(run.fromUnit.map(QPointF(glyph.x0+.001,.5))),glyph.begin);
        }
        QVERIFY(layout.contains(run.fromUnit.map(QPointF(.5,.5))));
        QCOMPARE(layout.hit(run.fromUnit.map(QPointF(.5,-.1)),4),layout.hit(run.fromUnit.map(QPointF(.5,.5))));
        QCOMPARE(layout.hit(run.fromUnit.map(QPointF(.5,-1)),4),-1);
        const auto selected=layout.selection(run.begin,run.end);
        QVERIFY(selected.contains(run.fromUnit.map(QPointF(.5,.5))));
        QCOMPARE(selected,layout.selection(run.end,run.begin));
    }

};
QTEST_MAIN(LiveTextLayoutTest)
#include "live_text_layout_test.moc"
