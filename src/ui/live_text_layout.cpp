#include "ui/live_text_layout.hpp"
#include <QPolygonF>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <cmath>
#include <limits>

namespace hyprcapture::ui {
namespace {
bool wide(char32_t c) {
    return (c >= 0x1100 && c <= 0x11ff) || (c >= 0x2e80 && c <= 0xa4cf) ||
           (c >= 0xac00 && c <= 0xd7af) || (c >= 0xf900 && c <= 0xfaff) ||
           (c >= 0xfe10 && c <= 0xfe6f) || (c >= 0xff01 && c <= 0xff60) || c >= 0x1f000;
}
double distance(QPointF p, const QPainterPath& path) {
    if (path.contains(p)) return 0;
    double best = std::numeric_limits<double>::max();
    for (const auto& polygon : path.toSubpathPolygons()) for (int i = 0; i < polygon.size(); ++i) {
        const QPointF a = polygon[i], v = polygon[(i + 1) % polygon.size()] - a;
        const double length2 = QPointF::dotProduct(v, v);
        const double t = length2 ? std::clamp(QPointF::dotProduct(p - a, v) / length2, 0.0, 1.0) : 0;
        const QPointF delta = p - a - t * v;
        best = std::min(best, std::hypot(delta.x(), delta.y()));
    }
    return best;
}
}

void LiveTextLayout::setResults(const std::vector<OcrResult>& results) {
    text.clear(); runs.clear();
    int rowIndex = 0;
    for (const auto& row : ocrReadingRows(results)) {
        if (!text.isEmpty()) text += '\n';
        std::string_view previous;
        for (const auto index : row) {
            const auto& result = results[index];
            text += QString::fromStdString(ocrTextSeparator(previous, result.text));
            previous = result.text;
            const QString value = QString::fromStdString(result.text);
            const int begin = text.size(); text += value;
            if (result.poly.size() < 3) continue;
            QPolygonF polygon;
            for (const auto p : result.poly) polygon << QPointF(p.x, p.y);
            QPainterPath area; area.addPolygon(polygon); area.closeSubpath();
            const QPointF origin = polygon[0], edge = polygon[1] - origin;
            const double length = std::hypot(edge.x(), edge.y());
            if (length < 1e-6) continue;
            const QPointF u = edge / length, v(-u.y(), u.x());
            double left = 0, right = length, top = 0, bottom = 0;
            for (const auto p : polygon) {
                const auto d = p - origin;
                const double x = QPointF::dotProduct(d, u), y = QPointF::dotProduct(d, v);
                left = std::min(left, x); right = std::max(right, x);
                top = std::min(top, y); bottom = std::max(bottom, y);
            }
            const auto at = origin + left * u + top * v;
            const auto dx = (right - left) * u, dy = (bottom - top) * v;
            QTransform transform(dx.x(), dx.y(), dy.x(), dy.y(), at.x(), at.y());
            Run run{begin, int(text.size()), rowIndex, area, transform, {}};
            std::vector<std::pair<int, double>> boundaries{{0, 0}};
            QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, value);
            double total = 0;
            int last = 0;
            for (int next = finder.toNextBoundary(); next >= 0; next = finder.toNextBoundary()) {
                const auto codepoints = value.mid(last, next - last).toUcs4();
                total += !codepoints.empty() && wide(codepoints.front()) ? 1.0 : .55;
                boundaries.push_back({next, total}); last = next;
            }
            if (total > 0) for (std::size_t i = 1; i < boundaries.size(); ++i) {
                const double x = boundaries[i - 1].second / total, end = boundaries[i].second / total;
                QPainterPath cell; cell.addRect(QRectF(x, 0, end - x, 1));
                run.glyphs.push_back({begin + boundaries[i - 1].first, begin + boundaries[i].first,
                                     transform.map(cell).intersected(area)});
            }
            runs.push_back(std::move(run));
        }
        ++rowIndex;
    }
}

int LiveTextLayout::hit(QPointF point, double tolerance, bool nearest) const {
    const Run* target = nullptr;
    double closest = std::numeric_limits<double>::max();
    for (const auto& run : runs) {
        const double d = distance(point, run.area);
        if (d < closest) { closest = d; target = &run; }
    }
    if (!target || (!nearest && closest > tolerance)) return -1;
    const double x = target->fromUnit.inverted().map(point).x();
    const double width = target->fromUnit.m11() * target->fromUnit.m11() + target->fromUnit.m12() * target->fromUnit.m12();
    // Glyph boundaries are stored geometrically, so rotated and nonuniform text
    // use the same interpolation for painting and pointer placement.
    double best = std::numeric_limits<double>::max();
    int position = target->begin;
    for (const auto& glyph : target->glyphs) {
        const auto unit = target->fromUnit.inverted().map(glyph.area).boundingRect();
        for (const auto& [edge, offset] : {std::pair{unit.left(), glyph.begin}, {unit.right(), glyph.end}}) {
            const double d = std::abs(edge - x) * std::sqrt(width);
            if (d < best) { best = d; position = offset; }
        }
    }
    return position;
}
bool LiveTextLayout::contains(QPointF point) const {
    return std::ranges::any_of(runs, [&](const auto& run) { return run.area.contains(point); });
}
std::pair<int,int> LiveTextLayout::wordAt(int position) const {
    if (text.isEmpty()) return {0, 0};
    position = std::clamp(position, 0, int(text.size()) - 1);
    if (!text[position].isLetterOrNumber() && position && text[position - 1].isLetterOrNumber()) --position;
    int begin = position, end = position + 1;
    if (text[position].isLetterOrNumber()) {
        while (begin && text[begin - 1].isLetterOrNumber()) --begin;
        while (end < text.size() && text[end].isLetterOrNumber()) ++end;
    }
    // Do not split surrogate pairs or combining character clusters.
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.setPosition(begin); if (!finder.isAtBoundary()) begin = finder.toPreviousBoundary();
    finder.setPosition(end); if (!finder.isAtBoundary()) end = finder.toNextBoundary();
    return {begin, end};
}
std::pair<int,int> LiveTextLayout::lineAt(int position) const {
    int row = -1;
    for (const auto& run : runs) if (position >= run.begin && position <= run.end) { row = run.row; break; }
    int begin = text.size(), end = 0;
    for (const auto& run : runs) if (run.row == row) { begin = std::min(begin, run.begin); end = std::max(end, run.end); }
    return begin <= end ? std::pair{begin, end} : std::pair{0, 0};
}
QPainterPath LiveTextLayout::selection(int anchor, int caret) const {
    QPainterPath selected;
    if (anchor < 0 || caret < 0) return selected;
    const int begin = std::min(anchor, caret), end = std::max(anchor, caret);
    for (const auto& run : runs) for (const auto& glyph : run.glyphs)
        if (glyph.end > begin && glyph.begin < end) selected.addPath(glyph.area);
    return selected;
}
} // namespace hyprcapture::ui
