#include "ui/scroll_stitcher.hpp"

#include <QPainter>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace hyprcapture::ui {
namespace {
constexpr int columns = 64;
struct Sample { int r, g, b; bool edge = false; };
using Row = std::array<Sample, columns>;
using Samples = std::vector<Row>;
int difference(const Sample& a, const Sample& b) {
    return (std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b)) / 3;
}
Samples sampleImage(const QImage& image) {
    Samples samples(static_cast<size_t>(image.height()));
    // Scrollbars at the outer edge should not dominate alignment. The returned
    // image still contains the entire user-selected rectangle, without rescaling.
    const int margin = std::clamp(image.width() / 50, 1, 16);
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int c = 0; c < columns; ++c) {
            const int x = margin + c * (image.width() - 2 * margin - 1) / (columns - 1);
            const auto pixel = row[x];
            samples[y][c] = {qRed(pixel), qGreen(pixel), qBlue(pixel)};
        }
    }
    for (int y = 0; y < image.height(); ++y) {
        for (int c = 0; c < columns; ++c) {
            auto& pixel = samples[y][c];
            pixel.edge = difference(pixel, samples[y][std::max(0, c - 1)]) > 10 ||
                         difference(pixel, samples[y][std::min(columns - 1, c + 1)]) > 10 ||
                         difference(pixel, samples[std::max(0, y - 1)][c]) > 10 ||
                         difference(pixel, samples[std::min(image.height() - 1, y + 1)][c]) > 10;
        }
    }
    return samples;
}
struct Score {
    double featureError = std::numeric_limits<double>::infinity();
    double error = std::numeric_limits<double>::infinity();
    double badFraction = 1.0;
    int features = 0;
    int featureRows = 0;
    bool plausible(bool coarse) const {
        return features >= (coarse ? 8 : 40) && featureRows >= (coarse ? 3 : 8) &&
               featureError <= 5.0 && error <= 2.0 && badFraction <= 0.03;
    }
};
Score compareShift(const Samples& previous, const Samples& next, int delta, bool coarse) {
    const int overlap = static_cast<int>(previous.size()) - delta;
    const int rowStep = std::max(1, overlap / (coarse ? 32 : 128));
    const int columnStep = coarse ? 3 : 1;
    qint64 error = 0, featureError = 0;
    int pixels = 0, features = 0, bad = 0, featureRows = 0;
    for (int y = 0; y < overlap; y += rowStep) {
        bool rowHasFeature = false;
        for (int c = 0; c < columns; c += columnStep) {
            const auto& a = previous[y + delta][c];
            const auto& b = next[y][c];
            const int distance = difference(a, b);
            error += distance;
            ++pixels;
            if (a.edge || b.edge) {
                featureError += distance;
                ++features;
                bad += distance > 24;
                rowHasFeature = true;
            }
        }
        featureRows += rowHasFeature;
    }
    if (!features || !pixels)
        return {};
    return {static_cast<double>(featureError) / features, static_cast<double>(error) / pixels,
            static_cast<double>(bad) / features, features, featureRows};
}
bool unchanged(const Samples& a, const Samples& b) {
    qint64 error = 0;
    int pixels = 0, bad = 0;
    const int step = std::max(1, static_cast<int>(a.size()) / 192);
    for (int y = 0; y < static_cast<int>(a.size()); y += step) {
        for (int c = 0; c < columns; ++c) {
            const int distance = difference(a[y][c], b[y][c]);
            error += distance;
            bad += distance > 12;
            ++pixels;
        }
    }
    return pixels && static_cast<double>(error) / pixels <= 0.35 && static_cast<double>(bad) / pixels <= 0.001;
}
} // namespace

ScrollStitcher::ScrollStitcher() : ScrollStitcher(Limits{}) {}
ScrollStitcher::ScrollStitcher(Limits limits) : m_limits(limits) {}

bool ScrollStitcher::stable(const QImage& a, const QImage& b) {
    if (a.isNull() || b.isNull() || a.size() != b.size() || a.width() < columns || a.height() < 64)
        return false;
    return unchanged(sampleImage(a.convertToFormat(QImage::Format_ARGB32)), sampleImage(b.convertToFormat(QImage::Format_ARGB32)));
}

ScrollStitcher::Result ScrollStitcher::append(const QImage& source) {
    if (source.isNull() || source.width() < columns || source.height() < 64)
        return {Status::InvalidFrame};
    if (source.height() > m_limits.maxHeight || static_cast<qint64>(source.width()) * source.height() > m_limits.maxPixels)
        return {Status::LimitReached};
    const QImage frame = source.convertToFormat(QImage::Format_ARGB32);
    if (frame.isNull())
        return {Status::InvalidFrame};
    if (m_previous.isNull()) {
        m_previous = frame;
        m_strips.push_back(frame);
        m_height = frame.height();
        return {Status::Started, frame.height()};
    }
    if (frame.size() != m_previous.size())
        return {Status::SizeChanged};
    const auto previous = sampleImage(m_previous);
    const auto next = sampleImage(frame);
    if (unchanged(previous, next))
        return {Status::Unchanged};
    const int minOverlap = std::max(64, frame.height() / 4);
    std::vector<std::pair<double, int>> candidates;
    for (int delta = 1; delta <= frame.height() - minOverlap; ++delta) {
        const auto score = compareShift(previous, next, delta, true);
        if (score.plausible(true))
            candidates.emplace_back(score.featureError + score.error, delta);
    }
    std::sort(candidates.begin(), candidates.end());
    std::vector<std::pair<double, int>> verified;
    // Verify every plausible offset: repeated rows may otherwise look unique
    // merely because another equally good match was outside a top-N shortlist.
    for (const auto& [unused, delta] : candidates) {
        const auto score = compareShift(previous, next, delta, false);
        if (score.plausible(false))
            verified.emplace_back(score.featureError + score.error, delta);
    }
    if (verified.empty())
        return {Status::NoOverlap};
    std::sort(verified.begin(), verified.end());
    const auto [bestError, delta] = verified.front();
    for (size_t i = 1; i < verified.size(); ++i) {
        if (std::abs(verified[i].second - delta) > 2 && verified[i].first - bestError < std::max(0.8, bestError * 0.25))
            return {Status::Ambiguous};
    }
    const qint64 newHeight = static_cast<qint64>(m_height) + delta;
    if (newHeight > m_limits.maxHeight || newHeight * frame.width() > m_limits.maxPixels || frameCount() >= m_limits.maxFrames)
        return {Status::LimitReached};
    QImage strip = frame.copy(0, frame.height() - delta, frame.width(), delta);
    if (strip.isNull())
        return {Status::InvalidFrame};
    m_strips.push_back(std::move(strip));
    m_previous = frame;
    m_height = static_cast<int>(newHeight);
    return {Status::Appended, delta};
}

QImage ScrollStitcher::image() const {
    if (empty())
        return {};
    QImage result(size(), QImage::Format_ARGB32);
    if (result.isNull())
        return {};
    QPainter painter(&result);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    int y = 0;
    for (const auto& strip : m_strips) {
        painter.drawImage(0, y, strip);
        y += strip.height();
    }
    return result;
}

} // namespace hyprcapture::ui
