#pragma once
#include "shared/scroll_motion.hpp"
#include <QImage>
#include <QPainter>
#include <QRect>
#include <array>
#include <vector>

namespace hyprcapture::ui {
// Native pixel coordinates: the first viewport is document Y zero. Prepending
// changes minimumY, never the coordinates attached to an annotation.
struct ScrollLayout {
  struct Panel {
    QRect rect;
    int documentY = 0, sourceTop = 0, sourceHeight = 0, seam = -1;
    int maximumY = 0; // Frozen when released; active panels use current maximumY.
    bool active = true;
    bool operator==(const Panel &) const = default;
  };
  QRect content;
  QSize frameSize;
  int minimumY = 0, maximumY = 0, viewportY = 0;
  int leftSeam = -1, rightSeam = -1;
  bool leftTiled = false, rightTiled = false;
  QRect initialContent;
  int topOcclusion = 0, bottomOcclusion = 0;
  std::vector<Panel> panels;
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
  static const char *statusName(Status status) {
    switch (status) {
    case Status::Started: return "Started";
    case Status::Appended: return "Appended";
    case Status::Relocated: return "Relocated";
    case Status::Unchanged: return "Unchanged";
    case Status::NoOverlap: return "NoOverlap";
    case Status::Ambiguous: return "Ambiguous";
    case Status::SizeChanged: return "SizeChanged";
    case Status::LimitReached: return "LimitReached";
    case Status::InvalidFrame: return "InvalidFrame";
    }
    return "Unknown";
  }
  struct Limits {
    int maxHeight = 32768;
    qint64 maxPixels = 64LL * 1024 * 1024;
    int maxFrames = 200;
  };
  struct Result {
    Status status;
    int addedRows = 0;
    int displacement = 0;
    double confidence = 0; // Verified correlation; zero when no unique match exists.
    const char *reason = "frame-validation";
    double predictedOffset = std::numeric_limits<double>::quiet_NaN(), priorTolerance = 0;
    std::array<double,4> input{}, gain{};
    bool priorUsed = false;
  };
  ScrollStitcher();
  explicit ScrollStitcher(Limits limits);
  Result append(const QImage &frame, const QImage &original = {}, const QImage &hdr = {}, const ScrollMotionSnapshot* motion = nullptr, uint64_t timeNs = 0);
  QImage image() const;
  QImage originalImage() const;
  QImage hdrImage() const;
  QImage preview(const QSize &bounds, bool hdr = false) const;
  QSize size() const {
    return empty() ? QSize{}
                   : QSize(m_initial.width(), m_layout.outputHeight());
  }
  int frameCount() const { return m_frames; }
  bool empty() const { return m_initial.isNull(); }
  ScrollLayout layout() const;
  static bool stable(const QImage &a, const QImage &b);

private:
  Result appendPixels(const QImage &, const QImage &, const QImage &);
  ScrollMotionPrior m_motion;
  ScrollPrediction m_prediction;
  struct Strip {
    int y;
    QImage image;
    int sourceTop = 0; // source viewport row, for native corner recovery
  };
  QImage bodyImage(bool original = false) const;
  void paint(QPainter &painter, int plane = 0) const;
  void updateColumns(const QImage &frame, const QImage &original,
                     const QImage &hdr, int displacement);
  void updateTop(const QImage &frame, int displacement);
  void updateFloating(const QImage &frame, const QImage &original, const QImage &hdr, int displacement, bool edgeMasks);
  void cleanMarginFloats();
  struct ColumnMotion {
    int stationaryFrames = 0, stationaryTravel = 0;
    int stationaryStart = 0;
    bool changed = false, translated = false;
    int changeAge = 65;
  };
  struct PinnedSide {
    QRect rect;
    int documentY = 0, prefixY = 0, sourceTop = 0, seam = -1;
    std::array<QImage, 3> image, prefix, fill, underlay;
  };
  Limits m_limits;
  QImage m_initial, m_previous, m_originalInitial, m_originalPrevious, m_hdrInitial;
  ScrollLayout m_layout;
  std::vector<Strip> m_strips, m_originalStrips, m_hdrStrips;
  std::vector<ColumnMotion> m_columns;
  std::vector<ColumnMotion> m_rows;
  std::vector<PinnedSide> m_pinnedSides;
  std::vector<ScrollLayout::Panel> m_releasedPanels;
  QRect m_initialContent;
  int m_topOcclusion = 0; // Registration/source mask, not a document rebase.
  int m_topPinnedAt = 0;
  int m_bottomOcclusion = 0;
  std::array<QImage, 3> m_bottomTail;
  struct FloatingCandidate { QRect rect; bool top; int observations; };
  std::vector<FloatingCandidate> m_floatingCandidates;
  struct MarginFloat {
    QRect rect;
    std::array<QImage,3> image;
  };
  std::vector<MarginFloat> m_marginFloats;
  int m_frames = 0;
  bool m_layoutLocked = false;
};
} // namespace hyprcapture::ui
