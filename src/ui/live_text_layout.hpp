#pragma once

#include "shared/ocr_result.hpp"
#include <QPainterPath>
#include <QPointF>
#include <QString>
#include <QTransform>
#include <vector>

namespace hyprcapture::ui {

// All offsets are UTF-16 grapheme boundaries in text, including row separators.
class LiveTextLayout {
  public:
    struct Glyph { int begin, end; QPainterPath area; double x0, x1; };
    struct Run {
        int begin, end, row;
        QPainterPath area;
        QTransform fromUnit;
        std::vector<Glyph> glyphs;
    };
    QString copiedText(int anchor, int caret) const;
    QString text;
    std::vector<Run> runs;
    void setResults(const std::vector<OcrResult>& results);
    int hit(QPointF point, double tolerance = 0, bool nearest = false) const;
    bool contains(QPointF point) const;
    std::pair<int, int> wordAt(int position) const;
    std::pair<int, int> lineAt(int position) const;
    QPainterPath selection(int anchor, int caret) const;
  private:
    struct CopyRow { int begin, end; QString prefix, separator; };
    std::vector<CopyRow> m_copyRows;
};

} // namespace hyprcapture::ui
