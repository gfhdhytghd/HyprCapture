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
    void tesseractCjkJoining() {
        const auto response = hyprcapture::parseTesseractTsv(
            "level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n"
            "5\t1\t1\t1\t1\t1\t0\t0\t10\t20\t95\t中\n"
            "5\t1\t1\t1\t1\t2\t30\t0\t10\t20\t95\t文\n");
        QVERIFY(response);
        LiveTextLayout layout; layout.setResults(response.results);
        QCOMPARE(layout.text, QString::fromUtf8("中文"));
        QCOMPARE(layout.runs.size(), std::size_t(1));
        const auto& run = layout.runs.front();
        QCOMPARE(run.glyphs.size(), std::size_t(2));
        QCOMPARE(run.glyphs[0].x0, 0.);
        QCOMPARE(run.glyphs[0].x1, .25);
        QCOMPARE(run.glyphs[1].x0, .75);
        QCOMPARE(run.glyphs[1].x1, 1.);
        QVERIFY(layout.selection(run.begin, run.end).contains(run.fromUnit.map(QPointF(.5,.5))));
    }
    void estimationRatio() {
        hyprcapture::OcrResult line{"A中",{{0,0},{90,0},{90,20},{0,20}}};
        LiveTextLayout layout; layout.setResults({line});
        const auto& plain = layout.runs[0].glyphs;
        QVERIFY(std::abs(plain[0].x1-1./3)<1e-12);
        QVERIFY(std::abs((plain[1].x1-plain[1].x0)/(plain[0].x1-plain[0].x0)-2)<1e-12);
        line.words = {{"A中",{{18,0},{72,0},{72,20},{18,20}}}};
        layout.setResults({line});
        const auto& boxed=layout.runs[0].glyphs;
        QVERIFY(std::abs(boxed[0].x0-.2)<1e-12);
        QVERIFY(std::abs(boxed[0].x1-.4)<1e-12);
        QVERIFY(std::abs(boxed[1].x1-.8)<1e-12);
        line.text="xA中y";
        line.words = {{"x",{{0,0},{9,0},{9,20},{0,20}}}, {"y",{{81,0},{90,0},{90,20},{81,20}}}};
        layout.setResults({line});
        const auto& gap=layout.runs[0].glyphs;
        QVERIFY(std::abs(gap[1].x0-.1)<1e-12);
        QVERIFY(std::abs(gap[2].x1-.9)<1e-12);
        QVERIFY(std::abs((gap[2].x1-gap[2].x0)/(gap[1].x1-gap[1].x0)-2)<1e-12);
    }
    void mixedWordFallback() {
        const auto response=hyprcapture::parseOcrJsonLines(
            R"({"text":"A中","poly":[[0,0],[120,0],[120,20],[0,20]],"words":[{"text":"A","box":[10,0,20,20]},{"text":"中","box":[70,0,110,20]}]})" "\n"
            R"({"text":"A中","poly":[[0,30],[120,30],[120,50],[0,50]]})" "\n"
            R"({"text":"A中","poly":[[0,60],[120,60],[120,80],[0,80]],"words":[{"text":"bad","box":[0,60,50,80]}]})");
        QVERIFY(response); QVERIFY(!response.warning.empty());
        QCOMPARE(response.results[0].words.size(),std::size_t(2));
        QVERIFY(response.results[1].words.empty()); QVERIFY(response.results[2].words.empty());
        LiveTextLayout layout; layout.setResults(response.results);
        QCOMPARE(layout.runs.size(),std::size_t(3));
        QCOMPARE(layout.text,QString::fromUtf8("A中\nA中\nA中"));
        QVERIFY(std::abs(layout.runs[0].glyphs[0].x1-1./6)<1e-12);
        for (std::size_t i=1;i<3;++i) {
            const auto& run=layout.runs[i];
            QVERIFY(std::abs(run.glyphs[0].x1-1./3)<1e-12);
            QCOMPARE(run.glyphs[1].x1,1.);
            QCOMPARE(layout.hit(run.fromUnit.map(QPointF(.1,.5))),run.begin);
            QVERIFY(layout.selection(run.begin,run.end).contains(run.fromUnit.map(QPointF(.9,.5))));
        }
    }
    void simpleApiNativeWordBoxes() {
        const auto response = hyprcapture::parseOcrApiJson(
            R"({"results":[{"text":"A 中","box":[[0,0],[120,0],[120,20],[0,20]],"text_word":["A"," ","中"],"text_word_boxes":[[[10,0],[20,0],[20,20],[10,20]],null,[70,0,110,20]]}]})");
        QVERIFY(response);
        QVERIFY(response.warning.empty());
        LiveTextLayout layout; layout.setResults(response.results);
        QCOMPARE(layout.text, QString::fromUtf8("A 中"));
        QCOMPARE(layout.runs.size(), std::size_t(1));
        const auto& run = layout.runs.front();
        QCOMPARE(run.glyphs.size(), std::size_t(3));
        QVERIFY(std::abs(run.glyphs[0].x0-10./120)<1e-12);
        QVERIFY(std::abs(run.glyphs[0].x1-20./120)<1e-12);
        QVERIFY(std::abs(run.glyphs[2].x0-70./120)<1e-12);
        QVERIFY(std::abs(run.glyphs[2].x1-110./120)<1e-12);
        QCOMPARE(run.glyphs[1].x0, run.glyphs[0].x1);
        QCOMPARE(run.glyphs[1].x1, run.glyphs[2].x0);
        QVERIFY(layout.selection(run.begin,run.end).contains(QPointF(90,10)));
    }
    void wordBoxes_data() {
        QTest::addColumn<double>("angle");
        QTest::addColumn<double>("height");
        for (double angle : {0., -.5, .5}) for (double height : {10., 40.})
            QTest::newRow(qPrintable(QString("%1/%2").arg(angle).arg(height))) << angle << height;
    }
    void wordBoxes() {
        QFETCH(double, angle); QFETCH(double, height);
        hyprcapture::OcrResult result;
        result.text = "中 文!OCR";
        QTransform rotation; rotation.translate(10, 50); rotation.rotate(angle);
        for (const auto p : rotation.map(QPolygonF(QRectF(0,0,200,20)))) result.poly.push_back({p.x(),p.y()});
        // The words are axis-aligned even when the line is rotated, and may
        // extend vertically beyond it. Their projection alone sets caret x.
        auto word = [&](std::string text, double left, double right) {
            return hyprcapture::OcrWord{std::move(text), {{left,40},{right,40},{right,40+height},{left,40+height}}, {}};
        };
        result.words = {word("中",20,40), word("文",80,110), word("OCR",130,190)};
        LiveTextLayout layout; layout.setResults({result});
        QCOMPARE(layout.text, QString::fromUtf8(result.text));
        const auto& run = layout.runs.front();
        const auto inverse = run.fromUnit.inverted();
        auto interval = [&](const hyprcapture::OcrWord& word) {
            double lo = 1, hi = 0;
            for (const auto p : word.poly) {
                const double x = inverse.map(QPointF(p.x,p.y)).x();
                lo = std::min(lo,x); hi = std::max(hi,x);
            }
            return std::pair{std::clamp(lo,0.,1.),std::clamp(hi,0.,1.)};
        };
        const auto a = interval(result.words[0]), b = interval(result.words[1]), c = interval(result.words[2]);
        QVERIFY(std::abs(run.glyphs[0].x0-a.first)<1e-9);
        QVERIFY(std::abs(run.glyphs[0].x1-a.second)<1e-9);
        QVERIFY(std::abs(run.glyphs[2].x0-b.first)<1e-9);
        QVERIFY(std::abs(run.glyphs[2].x1-b.second)<1e-9);
        QCOMPARE(run.glyphs[1].x0,run.glyphs[0].x1); // space occupies gap
        QCOMPARE(run.glyphs[1].x1,run.glyphs[2].x0);
        QCOMPARE(run.glyphs[3].x0,run.glyphs[2].x1); // uncovered punctuation
        QVERIFY(std::abs(run.glyphs[3].x1-c.first)<1e-9);
        for (int i=4;i<7;++i) QVERIFY(std::abs(run.glyphs[i].x1-run.glyphs[i].x0-(c.second-c.first)/3)<1e-9);
        int previous = run.begin;
        for (int i=0;i<=2000;++i) {
            const int hit = layout.hit(run.fromUnit.map(QPointF(i/2000.,.5)),0,true);
            QVERIFY(hit>=previous); previous=hit;
        }
        QCOMPARE(previous,run.end);
        const auto selected=layout.selection(run.begin,run.end);
        QCOMPARE(selected,layout.selection(run.end,run.begin));
        for (const auto& glyph : run.glyphs) {
            QVERIFY(!glyph.area.isEmpty());
            const auto rect=inverse.map(glyph.area).boundingRect();
            QVERIFY(std::abs(rect.left()-glyph.x0)<1e-9);
            QVERIFY(std::abs(rect.right()-glyph.x1)<1e-9);
            QVERIFY(std::abs(rect.height()-1)<1e-9);
            QVERIFY(selected.contains(run.fromUnit.map(QPointF((glyph.x0+glyph.x1)/2,.5))));
        }
    }
    void overlappingWordIntervals() {
        hyprcapture::OcrResult result{"ABC",{{0,0},{100,0},{100,20},{0,20}}};
        result.words = {{"A",{{-10,-10},{60,-10},{60,30},{-10,30}}},
                        {"B",{{40,-10},{80,-10},{80,30},{40,30}}},
                        {"C",{{20,-10},{120,-10},{120,30},{20,30}}}};
        LiveTextLayout layout; layout.setResults({result});
        double last=0;
        for (const auto& glyph : layout.runs[0].glyphs) {
            QVERIFY(glyph.x0>=last); QVERIFY(glyph.x1>=glyph.x0); QVERIFY(glyph.x1<=1); last=glyph.x1;
        }
        QCOMPARE(last,1.);
    }
    void graphemeWordIntervals() {
        hyprcapture::OcrResult result{"😀é中",{{0,0},{100,0},{100,20},{0,20}}};
        result.words = {{"😀",{{0,0},{10,0},{10,20},{0,20}}},
                        {"é",{{10,0},{70,0},{70,20},{10,20}}},
                        {"中",{{70,0},{100,0},{100,20},{70,20}}}};
        LiveTextLayout layout; layout.setResults({result});
        QCOMPARE(layout.runs[0].glyphs.size(),std::size_t(3));
        QVERIFY(std::abs(layout.runs[0].glyphs[0].x1-.1)<1e-9);
        QVERIFY(std::abs(layout.runs[0].glyphs[1].x1-.7)<1e-9);
        QCOMPARE(layout.runs[0].glyphs[1].begin,2);
        QCOMPARE(layout.runs[0].glyphs[1].end,4);
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
