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
            if (bottom - top < 1e-6) continue;
            QTransform transform(dx.x(), dx.y(), dy.x(), dy.y(), at.x(), at.y());
            Run run{begin, int(text.size()), rowIndex, area, transform, {}};
            std::vector<std::pair<int, double>> boundaries{{0, 0}};
            QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, value);
            double total = 0;
            int last = 0;
            for (int next = finder.toNextBoundary(); next >= 0; next = finder.toNextBoundary()) {
                const auto codepoints = value.mid(last, next - last).toUcs4();
                total += !codepoints.empty() && wide(codepoints.front()) ? 1.0 : .5;
                boundaries.push_back({next, total}); last = next;
            }
            if (total > 0) for (auto& boundary : boundaries) boundary.second /= total;
            std::vector<std::pair<double, double>> intervals;
            for (std::size_t i = 1; i < boundaries.size(); ++i)
                intervals.emplace_back(boundaries[i - 1].second, boundaries[i].second);
            if (!result.words.empty()) {
                const auto ranges = ocrWordRanges(result.text, result.words);
                const auto inverse = transform.inverted();
                struct Span { std::size_t first, last; double left, right; };
                std::vector<Span> spans;
                double previous = 0;
                std::size_t byteCursor = 0;
                int utf16Cursor = 0;
                if (ranges) for (std::size_t i = 0; i < ranges->size(); ++i) {
                    const auto [start, end] = (*ranges)[i];
                    const int first = utf16Cursor + QString::fromUtf8(result.text.data() + byteCursor, start - byteCursor).size();
                    const int last = first + QString::fromUtf8(result.text.data() + start, end - start).size();
                    byteCursor = end; utf16Cursor = last;
                    const auto beginIt = std::ranges::lower_bound(boundaries, first, {}, &std::pair<int, double>::first);
                    const auto endIt = std::ranges::lower_bound(boundaries, last, {}, &std::pair<int, double>::first);
                    if (beginIt == boundaries.end() || endIt == boundaries.end() || beginIt->first != first ||
                        endIt->first != last || result.words[i].poly.empty()) {
                        spans.clear(); break; // Never split a grapheme for bad metadata.
                    }
                    double lo = 1, hi = 0;
                    for (const auto p : result.words[i].poly) {
                        const double x = inverse.map(QPointF(p.x, p.y)).x();
                        lo = std::min(lo, x); hi = std::max(hi, x);
                    }
                    lo = std::clamp(lo, previous, 1.0);
                    hi = std::clamp(hi, lo, 1.0);
                    spans.push_back({std::size_t(beginIt - boundaries.begin()), std::size_t(endIt - boundaries.begin()), lo, hi});
                    previous = hi;
                }
                auto distribute = [&](std::size_t first, std::size_t last, double lo, double hi) {
                    const double weight = boundaries[last].second - boundaries[first].second;
                    if (weight <= 0) return;
                    for (auto i = first; i < last; ++i)
                        intervals[i] = {lo + (hi - lo) * (boundaries[i].second - boundaries[first].second) / weight,
                                        lo + (hi - lo) * (boundaries[i + 1].second - boundaries[first].second) / weight};
                };
                if (!spans.empty()) {
                    std::size_t cursor = 0;
                    previous = 0;
                    for (const auto& span : spans) {
                        distribute(cursor, span.first, previous, span.left);
                        distribute(span.first, span.last, span.left, span.right);
                        cursor = span.last; previous = span.right;
                    }
                    distribute(cursor, intervals.size(), previous, 1);
                }
            }
            if (total > 0) for (std::size_t i = 1; i < boundaries.size(); ++i) {
                const auto [x, end] = intervals[i - 1];
                QPainterPath cell;
                cell.addPolygon(transform.map(QPolygonF(QRectF(x, 0, end - x, 1))));
                cell.closeSubpath();
                run.glyphs.push_back({begin + boundaries[i - 1].first, begin + boundaries[i].first,
                                     cell, x, end});
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
    if (x <= 0) return target->begin;
    if (x >= 1) return target->end;
    // Pointer placement uses stored unit-space boundaries, never painted paths
    // (a rotated path's bounding box is not its glyph interval).
    for (const auto& glyph : target->glyphs) {
        if (x < (glyph.x0 + glyph.x1) / 2) return glyph.begin;
    }
    return target->end;
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
    selected.setFillRule(Qt::WindingFill);
    if (anchor < 0 || caret < 0) return selected;
    const int begin = std::min(anchor, caret), end = std::max(anchor, caret);
    for (const auto& run : runs) {
        const Glyph* first = nullptr;
        const Glyph* last = nullptr;
        for (const auto& glyph : run.glyphs) if (glyph.end > begin && glyph.begin < end) {
            if (!first) first = &glyph;
            last = &glyph;
        }
        // One quad per selected run also avoids fractional-pixel seams.
        if (first) {
            selected.addPolygon(run.fromUnit.map(QPolygonF(QRectF(first->x0, 0, last->x1 - first->x0, 1))));
            selected.closeSubpath();
        }
    }
    return selected;
}
} // namespace hyprcapture::ui
