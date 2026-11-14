#include "shared/ocr_result.hpp"
#include "ui/live_text_layout.hpp"
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

using namespace hyprcapture;
using hyprcapture::ui::LiveTextLayout;
namespace {
OcrResult line(std::string text,double width,double top,double height=10,double left=0,double degrees=0) {
    OcrResult out; out.text=std::move(text);
    QTransform transform; transform.translate(500,300); transform.rotate(degrees);
    for (const auto p : transform.map(QPolygonF(QRectF(left,top,width,height)))) out.poly.push_back({p.x(),p.y()});
    return out;
}
std::vector<OcrResult> prosePair(const QString& left,const QString& right,double shift,bool words,double degrees,bool singleWords=false) {
    const QString values[]{left,right};
    std::vector<OcrResult> results{line(left.toStdString(),200,0,10,0,degrees),line(right.toStdString(),200,12,10,shift,degrees)};
    if (words) for (int row=0;row<2;++row) for (int begin=0;begin<values[row].size();) {
        if (values[row][begin].isSpace()) { ++begin; continue; }
        int end=begin+1;
        if (!singleWords) while (end<values[row].size()&&!values[row][end].isSpace()) ++end;
        const auto token=values[row].mid(begin,end-begin);
        results[row].words.push_back({token.toStdString(),line("",(end-begin)*10,row*12,10,(row?shift:0)+begin*10,degrees).poly,{}});
        begin=end;
    }
    return results;
}

struct Rendered { std::vector<OcrResult> results; QImage image; };
Rendered render(const QStringList& lines,const QFont& font,bool words,double degrees) {
    QFontMetricsF metrics(font);
    double width=0; for (const auto& text : lines) width=std::max(width,metrics.horizontalAdvance(text));
    const double height=metrics.height(),stride=height*1.25;
    Rendered out;
    out.image=QImage(std::ceil(width+400),std::ceil(lines.size()*stride+300),QImage::Format_RGB32); out.image.fill(Qt::white);
    QPainter painter(&out.image); painter.setFont(font); painter.setPen(Qt::black);
    QTransform transform; transform.translate(150,100); transform.rotate(degrees);
    painter.setTransform(transform);
    auto poly=[&](QRectF rectangle) {
        std::vector<OcrPoint> points;
        for (const auto p : transform.map(QPolygonF(rectangle))) points.push_back({p.x(),p.y()});
        return points;
    };
    for (int i=0;i<lines.size();++i) {
        const auto& original=lines[i];
        painter.drawText(QPointF(0,i*stride+metrics.ascent()),original);
        int begin=0; while (begin<original.size()&&original[begin]==' ') ++begin;
        const auto value=original.mid(begin);
        const double x=metrics.horizontalAdvance(original.left(begin)),y=i*stride;
        OcrResult result; result.text=value.toStdString(); result.poly=poly({x,y,metrics.horizontalAdvance(value),height});
        if (words) for (int first=0;first<value.size();) {
            if (value[first].isSpace()) { ++first; continue; }
            int end=first+1; while (end<value.size()&&!value[end].isSpace()) ++end;
            const auto token=value.mid(first,end-first);
            result.words.push_back({token.toStdString(),poly({x+metrics.horizontalAdvance(value.left(first)),y,metrics.horizontalAdvance(token),height}),{}});
            first=end;
        }
        out.results.push_back(std::move(result));
    }
    painter.end(); return out;
}
}
class OcrCopyTest final : public QObject {
    Q_OBJECT
    int tp=0,fp=0,tn=0,fn=0;
  private slots:
    void paragraphsAndSelection() {
        const std::vector<OcrResult> results{line("alpha",100,0),line("beta",100,12),line("short",50,24),line("tail",100,36),line("next",100,80)};
        const auto copy=ocrCopyText(results);
        QCOMPARE(copy.text,std::string("alpha beta short\ntail\n\nnext"));
        const std::vector<std::string> expected{" "," ","\n","\n\n",""};
        QCOMPARE(copy.rows.size(),expected.size());
        for (std::size_t i=0;i<expected.size();++i) QCOMPARE(copy.rows[i].separator,expected[i]);
        LiveTextLayout layout; layout.setResults(results);
        QCOMPARE(layout.text,QString("alpha\nbeta\nshort\ntail\nnext"));
        QCOMPARE(layout.copiedText(0,layout.text.size()),QString::fromStdString(copy.text));
        QCOMPARE(layout.copiedText(2,layout.text.size()-2),QString("pha beta short\ntail\n\nne"));
        QCOMPARE(layout.copiedText(layout.text.size()-2,2),layout.copiedText(2,layout.text.size()-2));
        QCOMPARE(layout.copiedText(1,4),QString("lph"));
        const int boundary=layout.text.indexOf("tail")+4;
        QCOMPARE(layout.copiedText(boundary,boundary+1),QString("\n\n"));
        QCOMPARE(layout.copiedText(-1,4),QString());
    }
    void softWraps_data() {
        QTest::addColumn<QString>("left"); QTest::addColumn<QString>("right"); QTest::addColumn<QString>("separator");
        for (const auto& values : std::vector<QStringList>{{"hello","world"," "},{"中","文",""},{"中","word",""},{"word","文",""},{"word,","next",""},{"word","。next",""},{"word—","next",""},{"word ","next",""},{"word","\t next",""},{"hello","，next",""}})
            QTest::newRow(qPrintable(values[0]+"/"+values[1])) << values[0] << values[1] << values[2];
    }
    void softWraps() {
        QFETCH(QString,left); QFETCH(QString,right); QFETCH(QString,separator);
        const auto copy=ocrCopyText({line(left.toStdString(),100,0),line(right.toStdString(),100,12)});
        QCOMPARE(QString::fromStdString(copy.rows[0].separator),separator);
        QCOMPARE(QString::fromStdString(copy.text),left+separator+right);
    }
    void rotated_data() {
        QTest::addColumn<double>("degrees");
        for (const double angle : {-.7,0.,.7,35.,90.,160.}) QTest::newRow(qPrintable(QString::number(angle))) << angle;
    }
    void rotated() {
        QFETCH(double,degrees);
        const auto copy=ocrCopyText({line("alpha",100,0,10,0,degrees),line("beta",100,12,10,0,degrees),line("next",100,50,10,0,degrees)});
        QCOMPARE(copy.rows.size(),std::size_t(3));
        // Two gaps use the reference upper median; the large gap is a paragraph.
        QCOMPARE(copy.text,std::string("alpha beta\n\nnext"));
        LiveTextLayout layout; layout.setResults({line("alpha",100,0,10,0,degrees),line("beta",100,12,10,0,degrees),line("next",100,50,10,0,degrees)});
        QCOMPARE(layout.copiedText(0,layout.text.size()),QString::fromStdString(copy.text));
    }
    void changedOrientation() {
        const auto copy=ocrCopyText({line("alpha",100,0),line("beta",100,12,10,0,11)});
        QCOMPARE(copy.rows.size(),std::size_t(2));
        QCOMPARE(copy.rows[0].separator,std::string("\n"));
    }
    void textOnly() {
        QCOMPARE(joinOcrText({{"raw\ntext\n",{}}}),std::string("raw\ntext\n"));
        QVERIFY(ocrCopyText({}).rows.empty());
    }
    void overlapAndWidthThreshold() {
        QCOMPARE(joinOcrText({line("alpha",100,0),line("beta",100,7)}),std::string("alpha beta"));
        QCOMPARE(joinOcrText({line("first",100,0),line("edge",92,12),line("last",100,24)}),std::string("first edge last"));
        QCOMPARE(joinOcrText({line("first",100,0),line("short",91.9,12),line("last",100,24)}),std::string("first short\nlast"));
    }
    void mixedDocument() {
        for (const bool words : {false,true}) for (const bool paragraph : {false,true}) {
            QFont proseFont("Liberation Sans"); proseFont.setPixelSize(24);
            QFont codeFont("Liberation Mono"); codeFont.setPixelSize(24);
            const double degrees=.7;
            const QStringList codeRows{"local value = 1","    print(value)"};
            auto prose=render({"Ordinary prose introduces the following example without changing its words."},proseFont,words,degrees);
            auto code=render(codeRows,codeFont,words,degrees);
            QTransform rotate; rotate.rotate(degrees);
            const QPointF shift=rotate.map(QPointF(0,(paragraph?QFontMetricsF(proseFont).height()+std::ceil(2*QFontMetricsF(proseFont).height()):QFontMetricsF(proseFont).height()*1.25)));
            for (auto& result : code.results) {
                for (auto& p : result.poly) { p.x+=shift.x(); p.y+=shift.y(); }
                for (auto& word : result.words) for (auto& p : word.poly) { p.x+=shift.x(); p.y+=shift.y(); }
                prose.results.push_back(result);
            }
            const auto copy=ocrCopyText(prose.results);
            QCOMPARE(copy.rows.size(),std::size_t(3));
            QVERIFY(!copy.rows[0].code); QVERIFY(copy.rows[1].code); QVERIFY(copy.rows[2].code);
            QCOMPARE(copy.rows[0].separator,std::string(paragraph?"\n\n":"\n"));
            QVERIFY(QString::fromStdString(copy.text).endsWith(codeRows.join('\n')));
            LiveTextLayout layout; layout.setResults(prose.results);
            QCOMPARE(layout.copiedText(0,layout.text.size()),QString::fromStdString(copy.text));
            QCOMPARE(layout.copiedText(layout.runs[1].begin+6,layout.runs[2].end),QString("value = 1\n    print(value)"));
            QCOMPARE(layout.copiedText(layout.runs[2].begin,layout.runs[2].end),QString("print(value)"));
        }
    }
    void existingIndentAndGraphemes() {
        const QStringList rows{QString::fromUtf8("local word = \"é\""),"    print(word)","end"};
        QFont font("Liberation Mono"); font.setPixelSize(24);
        for (const bool words : {false,true}) {
            auto rendered=render(rows,font,words,.7);
            for (int i=0;i<rows.size();++i) rendered.results[i].text=rows[i].toStdString();
            const auto copy=ocrCopyText(rendered.results);
            for (const auto& row : copy.rows) QVERIFY(row.code);
            QCOMPARE(QString::fromStdString(copy.text),rows.join('\n'));
        }
    }
    void codeParagraphGap() {
        const QStringList rows{"def calculate_the_value():","    value = 1","    return value"};
        QFont font("Liberation Mono"); font.setPixelSize(24);
        for (const bool words : {false,true}) {
            auto rendered=render(rows,font,words,0);
            for (std::size_t i=1;i<rendered.results.size();++i) {
                const double gap=std::ceil(1.5*QFontMetricsF(font).height())-.25*QFontMetricsF(font).height();
                for (auto& p : rendered.results[i].poly) p.y+=gap;
                for (auto& word : rendered.results[i].words) for (auto& p : word.poly) p.y+=gap;
            }
            const auto copy=ocrCopyText(rendered.results);
            for (const auto& row : copy.rows) QVERIFY(row.code);
            QCOMPARE(copy.text,std::string("def calculate_the_value():\n\n    value = 1\n    return value"));
            LiveTextLayout layout; layout.setResults(rendered.results);
            QCOMPARE(layout.copiedText(0,layout.text.size()),QString::fromStdString(copy.text));
        }
    }
    void nonGridWordGaps() {
        QFont font("Liberation Mono"); font.setPixelSize(24);
        const QStringList rows{"local value = 1","if value > 0 then","    print(value)","end"};
        for (const double cells : {.45,-10.}) {
            auto rendered=render(rows,font,true,0);
            const double shift=cells*QFontMetricsF(font).horizontalAdvance("x");
            for (auto& p : rendered.results[0].words[1].poly) p.x+=shift;
            const auto copy=ocrCopyText(rendered.results);
            QVERIFY(!copy.rows.front().code);
            QVERIFY(copy.rows[2].code); QVERIFY(copy.rows[3].code); // Preserve the valid consecutive sub-run.
        }
    }
    void listMarkers_data() {
        QTest::addColumn<QString>("marker"); QTest::addColumn<bool>("words"); QTest::addColumn<double>("degrees");
        const QStringList markers{
            "- 背景","-背景","* 说明","*说明","• 选项","•选项","· 段落","·段落","+ 内容","+内容",
            "a. 用法","a.用法","b) 事项","b)事项","A. 说明","A.说明",
            "1. 项目","1.项目","12) 条目","12)条目","(1) 项目","(1)项目","１. 项目","１.项目",
            "① 说明","①说明","⑳ 说明","⑳说明","一、条目","一、 条目","十一、事项","（一）条目","（一） 条目",
            "[ ] 事项","[ ]事项","[x] 内容","[x]内容","[X]内容","　-背景","  a.用法","a.English item","1.English item"
        };
        for (const auto& marker : markers) for (const bool words : {false,true}) for (const double degrees : {0.,.7,90.})
            QTest::newRow(qPrintable(marker+"/"+QString::number(words)+"/"+QString::number(degrees))) << marker << words << degrees;
    }
    void listMarkers() {
        QFETCH(QString,marker); QFETCH(bool,words); QFETCH(double,degrees);
        const QString previous="这是前一行的正文。";
        const auto results=prosePair(previous,marker,0,words,degrees);
        const auto copy=ocrCopyText(results);
        QCOMPARE(copy.rows.size(),std::size_t(2));
        QVERIFY(!copy.rows[0].code); QVERIFY(!copy.rows[1].code);
        QCOMPARE(copy.rows[0].separator,std::string("\n"));
        QCOMPARE(QString::fromStdString(copy.text),previous+"\n"+marker);
        LiveTextLayout layout; layout.setResults(results);
        QCOMPARE(layout.text,previous+"\n"+marker);
        QCOMPARE(layout.copiedText(0,layout.text.size()),QString::fromStdString(copy.text));
        QCOMPARE(layout.copiedText(2,layout.text.size()-1),previous.mid(2)+"\n"+marker.left(marker.size()-1));
        QCOMPARE(layout.copiedText(layout.text.size()-1,2),layout.copiedText(2,layout.text.size()-1));
    }
    void indentBoundaries_data() {
        QTest::addColumn<bool>("words"); QTest::addColumn<double>("degrees"); QTest::addColumn<double>("cells");
        for (const bool words : {false,true}) for (const double degrees : {0.,.7,90.}) for (const double cells : {-1.,-.999, .999,1.})
            QTest::newRow(qPrintable(QString("%1/%2/%3").arg(words).arg(degrees).arg(cells))) << words << degrees << cells;
    }
    void indentBoundaries() {
        QFETCH(bool,words); QFETCH(double,degrees); QFETCH(double,cells);
        const QString previous="这是前一行的正文。",next="后续正文保持原样内容";
        const double advance=words?10:200./next.size();
        const auto results=prosePair(previous,next,cells*advance,words,degrees);
        const auto copy=ocrCopyText(results);
        const std::string separator=std::abs(cells)>=1?"\n":"";
        QCOMPARE(copy.rows[0].separator,separator);
        QCOMPARE(QString::fromStdString(copy.text),previous+QString::fromStdString(separator)+next);
        LiveTextLayout layout; layout.setResults(results);
        QCOMPARE(layout.copiedText(0,layout.text.size()),QString::fromStdString(copy.text));
    }
    void singleCharacterWordAdvance() {
        const auto copy=ocrCopyText(prosePair("这是前一行的正文。","后续正文保持原样内容",10,true,.7,true));
        QCOMPARE(copy.rows[0].separator,std::string("\n"));
    }
    void previousWordAdvance() {
        auto results=prosePair("这是前一行的正文。","后续正文保持原样内容",10,true,.7);
        results[1].words.clear();
        QCOMPARE(ocrCopyText(results).rows[0].separator,std::string("\n"));
    }
    void legacyWordAdvance() {
        auto first=line("这是前一行的正文。",200,0);
        auto second=line("后续",20,12,10,10);
        auto third=line("正文",20,12,10,50);
        second.level=third.level=OcrLevel::Word;
        const auto copy=ocrCopyText({first,second,third});
        QCOMPARE(copy.rows.size(),std::size_t(2));
        QCOMPARE(copy.rows[0].separator,std::string("\n"));
    }
    void ordinaryRowStarts_data() {
        QTest::addColumn<QString>("next"); QTest::addColumn<bool>("words");
        for (const auto& value : {"-word","-2.5 is negative","e.g. normal prose","i.e. normal prose","1.25 is a decimal","普通正文继续说明"})
            for (const bool words : {false,true}) QTest::newRow(qPrintable(QString(value)+"/"+QString::number(words))) << QString(value) << words;
    }
    void ordinaryRowStarts() {
        QFETCH(QString,next); QFETCH(bool,words);
        const auto copy=ocrCopyText(prosePair("这是前一行的正文。",next,0,words,0));
        QCOMPARE(copy.rows[0].separator,std::string());
    }
    void leadInBoundaries_data() {
        QTest::addColumn<QString>("previous"); QTest::addColumn<bool>("words");
        for (const auto& value : {"上一行是提示:","上一行是提示：","上一行是提示: ","上一行是提示：　","上一行是提示：\t"})
            for (const bool words : {false,true}) QTest::newRow(qPrintable(QString(value)+"/"+QString::number(words))) << QString(value) << words;
    }
    void leadInBoundaries() {
        QFETCH(QString,previous); QFETCH(bool,words);
        const QString next="后续正文保持原样内容";
        const auto results=prosePair(previous,next,0,words,0);
        const auto copy=ocrCopyText(results);
        QCOMPARE(copy.rows[0].separator,std::string("\n"));
        QCOMPARE(QString::fromStdString(copy.text),previous+"\n"+next);
        LiveTextLayout layout; layout.setResults(results);
        QCOMPARE(layout.copiedText(0,layout.text.size()),QString::fromStdString(copy.text));
    }
    void wrappedSentenceEnd_data() {
        QTest::addColumn<QString>("previous"); QTest::addColumn<QString>("next"); QTest::addColumn<bool>("words");
        for (const auto& value : {"上一行以句号结束。","The preceding prose sentence ends.","The preceding prose ends!","上一行以叹号结束！"})
            for (const bool words : {false,true}) QTest::newRow(qPrintable(QString(value)+"/"+QString::number(words))) << QString(value) << QString("下一行仍是普通正文") << words;
    }
    void wrappedSentenceEnd() {
        QFETCH(QString,previous); QFETCH(QString,next); QFETCH(bool,words);
        const auto results=prosePair(previous,next,0,words,0);
        const auto copy=ocrCopyText(results);
        QCOMPARE(copy.rows[0].separator,std::string());
        QCOMPARE(QString::fromStdString(copy.text),previous+next);
        LiveTextLayout layout; layout.setResults(results);
        QCOMPARE(layout.text,previous+"\n"+next);
        QCOMPARE(layout.copiedText(0,layout.text.size()),previous+next);
    }
    void listParagraphGap() {
        const auto copy=ocrCopyText({line("这是前一段的正文。",200,0),line("-背景",200,50)});
        QCOMPARE(copy.rows[0].separator,std::string("\n\n"));
    }
    void renderedFixtures_data() {
        QTest::addColumn<QString>("family"); QTest::addColumn<int>("size"); QTest::addColumn<bool>("words");
        QTest::addColumn<double>("degrees"); QTest::addColumn<QStringList>("rows"); QTest::addColumn<bool>("expectedCode"); QTest::addColumn<QString>("kind");
        const std::vector<std::pair<QString,QStringList>> fixtures{
            {"lua",{"local value = 1","if value > 0 then","    print(value)","end"}},
            {"cpp",{"int main() {","    auto value = 42;","    if (value > 0) {","        return value;","    }","}"}},
            {"shell",{"#!/bin/sh","count=0","for path in \"$@\"; do","    printf '%s\\n' \"$path\"","done"}},
            {"json",{"{","    \"name\": \"capture\",","    \"enabled\": true,","    \"count\": 42","}"}},
            {"python",{"def render(value):","    if value > 0:","        return value + 1","    return 0"}},
            {"english",{"We describe a practical approach to building reliable interfaces with clear text and consistent behavior.","Readers can compare the available choices, follow the explanation, and finish their work without extra steps.","This paragraph discusses ordinary language and illustrates how long proportional lines can have similar averages."}},
            {"chinese",{"这是一段普通中文，用来检查段落复制行为。","下一行继续说明文字的内容和阅读顺序。","这些文字不是程序代码，也不应识别为代码。"}},
            {"mixed",{"中文内容包含 English words，仍属于普通段落。","继续讨论 API 接口与用户体验，不是源程序。","最后一行包含 Latin 和中文混合说明。"}},
            {"caption",{"Morning light over water","A quiet harbor at dawn"}},
            {"prose-short",{"Note: this is ordinary prose","A short description follows","The explanation ends here."}},
        };
        const auto families=QFontDatabase::families();
        int available=0;
        for (const auto& family : {"Liberation Mono","Noto Sans Mono","Adwaita Mono","Liberation Sans","Liberation Serif","Adwaita Sans"}) {
            if (!families.contains(family)) continue;
            ++available; const bool mono=QString(family).contains("Mono");
            for (const int size : {16,24,32}) for (const double degrees : {0.,.7,-1.2}) for (const bool words : {false,true})
                for (std::size_t fixture=0;fixture<fixtures.size();++fixture) {
                    const auto& [kind,rows]=fixtures[fixture];
                    QTest::newRow(qPrintable(QString("%1/%2/%3/%4/%5").arg(family).arg(size).arg(degrees).arg(words).arg(kind)))
                        << QString(family) << size << words << degrees << rows << (mono&&fixture<5) << kind;
                }
        }
        QVERIFY2(available>=4,"rendered matrix requires several installed monospace and proportional fonts");
    }
    void renderedFixtures() {
        QFETCH(QString,family); QFETCH(int,size); QFETCH(bool,words); QFETCH(double,degrees); QFETCH(QStringList,rows); QFETCH(bool,expectedCode); QFETCH(QString,kind);
        QFont font(family); font.setPixelSize(size);
        QFontMetricsF metrics(font);
        if (family.contains("Mono")) QVERIFY(std::abs(metrics.horizontalAdvance("i")-metrics.horizontalAdvance("W"))<1e-6);
        const auto rendered=render(rows,font,words,degrees);
        const auto copy=ocrCopyText(rendered.results);
        bool allCode=copy.rows.size()==std::size_t(rows.size()),anyCode=false;
        for (const auto& row : copy.rows) { allCode&=row.code; anyCode|=row.code; }
        const bool classified=expectedCode?allCode:anyCode;
        if (expectedCode) classified?++tp:++fn; else classified?++fp:++tn;
        if (size==24&&degrees==.7) {
            const auto name=(family+"-"+kind+"-"+QString::number(words)).replace(' ','_');
            rendered.image.save(QDir::current().filePath("ocr-copy-"+name+".png"));
        }
        QCOMPARE(classified,expectedCode);
        if (expectedCode) {
            QCOMPARE(QString::fromStdString(copy.text),rows.join('\n'));
            LiveTextLayout layout; layout.setResults(rendered.results);
            QCOMPARE(layout.copiedText(0,layout.text.size()),rows.join('\n'));
            const auto first=layout.runs.front();
            QCOMPARE(layout.copiedText(first.begin+1,first.end),layout.text.mid(first.begin+1,first.end-first.begin-1));
        }
    }
    void cleanupTestCase() {
        qInfo("COPY_CONFUSION TP=%d FP=%d TN=%d FN=%d",tp,fp,tn,fn);
        QFile file(QDir::current().filePath("ocr-copy-confusion.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"tp",tp},{"fp",fp},{"tn",tn},{"fn",fn}}).toJson());
    }
};
QTEST_MAIN(OcrCopyTest)
#include "ocr_copy_test.moc"
