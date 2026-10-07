#pragma once
#include <QImage>
#include <QPainter>
#include <QRect>
#include <vector>

namespace hyprcapture::ui {
// Native pixel coordinates: the first viewport is document Y zero. Prepending
// changes minimumY, never the coordinates attached to an annotation.
struct ScrollLayout {
  QRect content;
  QSize frameSize;
  int minimumY = 0, maximumY = 0, viewportY = 0;
  int leftSeam = -1, rightSeam = -1;
  bool leftTiled = false, rightTiled = false;
  QRect viewportInImage() const {
    return {content.x(), content.y() + viewportY - minimumY, content.width(),
            content.height()};
  }
  int outputHeight() const { return frameSize.height() + maximumY - minimumY; }
  bool operator==(const ScrollLayout &) const = default;
};
class ScrollStitcher {
public:
  enum class Status {
    Started,
    Appended,
    Relocated,
    Unchanged,
    NoOverlap,
    Ambiguous,
    SizeChanged,
    LimitReached,
    InvalidFrame
  };
  struct Limits {
    int maxHeight = 32768;
    qint64 maxPixels = 64LL * 1024 * 1024;
    int maxFrames = 200;
  };
  struct Result {
    Status status;
    int addedRows = 0;
    int displacement = 0;
  };
  ScrollStitcher();
  explicit ScrollStitcher(Limits limits);
  Result append(const QImage &frame);
  QImage image() const;
  QImage preview(const QSize &bounds) const;
  QSize size() const {
    return empty() ? QSize{}
                   : QSize(m_initial.width(), m_layout.outputHeight());
  }
  int frameCount() const { return m_frames; }
  bool empty() const { return m_initial.isNull(); }
  ScrollLayout layout() const { return m_layout; }
  static bool stable(const QImage &a, const QImage &b);

private:
  struct Strip {
    int y;
    QImage image;
  };
  QImage bodyImage() const;
  void paint(QPainter &painter) const;
  Limits m_limits;
  QImage m_initial, m_previous;
  ScrollLayout m_layout;
  std::vector<Strip> m_strips;
  int m_frames = 0;
  bool m_layoutLocked = false;
};
} // namespace hyprcapture::ui
