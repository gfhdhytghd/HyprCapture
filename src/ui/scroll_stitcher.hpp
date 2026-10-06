#pragma once

#include <QImage>
#include <QtTypes>
#include <vector>

namespace hyprcapture::ui {

// A forward-only document stitcher. Uncertain frames never alter the accepted
// image, so scrolling back to the last overlap can recover without losing data.
class ScrollStitcher {
  public:
    enum class Status { Started, Appended, Unchanged, NoOverlap, Ambiguous, SizeChanged, LimitReached, InvalidFrame };
    struct Limits {
        int maxHeight = 32768;
        qint64 maxPixels = 64LL * 1024 * 1024;
        int maxFrames = 200;
    };
    struct Result {
        Status status;
        int addedRows = 0;
    };
    ScrollStitcher();
    explicit ScrollStitcher(Limits limits);
    Result append(const QImage& frame);
    QImage image() const;
    QSize size() const { return {m_previous.width(), m_height}; }
    int frameCount() const { return static_cast<int>(m_strips.size()); }
    bool empty() const { return m_previous.isNull(); }
    static bool stable(const QImage& a, const QImage& b);

  private:
    Limits m_limits;
    QImage m_previous;
    std::vector<QImage> m_strips;
    int m_height = 0;
};

} // namespace hyprcapture::ui
