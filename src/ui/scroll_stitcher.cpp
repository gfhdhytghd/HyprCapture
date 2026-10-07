#include "ui/scroll_stitcher.hpp"
#include <QLinearGradient>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <set>

namespace hyprcapture::ui {
namespace {
cv::Mat gray(const QImage &input) {
  auto image = input.convertToFormat(QImage::Format_RGBA8888);
  cv::Mat result;
  cv::cvtColor(cv::Mat(image.height(), image.width(), CV_8UC4, image.bits(),
                       image.bytesPerLine()),
               result, cv::COLOR_RGBA2GRAY);
  return result;
}
cv::Mat compact(const cv::Mat &input) {
  if (input.cols <= 320)
    return input;
  cv::Mat out;
  cv::resize(input, out, {320, input.rows}, 0, 0, cv::INTER_AREA);
  return out;
}
// Use textured tiles as evidence. White margins never vote for alignment.
// A small changing subregion (caret, clock) cannot outvote the page itself.
double verify(const cv::Mat &a, const cv::Mat &b, int position) {
  const int top = std::max(0, position),
            bottom = std::min(a.rows, position + b.rows);
  if (bottom - top < std::max(64, b.rows / 4))
    return 1e9;
  std::vector<double> errors;
  for (int y = top; y < bottom; y += 32)
    for (int x = 0; x < a.cols; x += 64) {
      cv::Rect ra(x, y, std::min(64, a.cols - x), std::min(32, bottom - y));
      cv::Rect rb = ra;
      rb.y -= position;
      cv::Scalar mean, deviation;
      cv::meanStdDev(a(ra), mean, deviation);
      if (deviation[0] < 3)
        continue;
      cv::Mat diff;
      cv::absdiff(a(ra), b(rb), diff);
      errors.push_back(cv::mean(diff)[0]);
    }
  if (errors.size() < 4)
    return 1e9;
  std::sort(errors.begin(), errors.end());
  const size_t accepted = std::max<size_t>(1, (errors.size() * 4 + 4) / 5);
  if (errors[accepted - 1] > 3.0)
    return 1e9;
  double sum = 0;
  for (size_t i = 0; i < accepted; ++i)
    sum += errors[i];
  return sum / accepted;
}
struct Match {
  int position = 0;
  bool found = false, ambiguous = false;
};
Match match(const cv::Mat &page, const cv::Mat &frame) {
  std::set<int> candidates;
  const int h = std::min(48, frame.rows / 4);
  // Several independent templates, retaining every peak rather than a top-N
  // shortlist: periodic paragraphs must remain ambiguous.
  for (int y : {0, (frame.rows - h) / 2, frame.rows - h}) {
    const int w = std::min(96, frame.cols), x = (frame.cols - w) / 2;
    const cv::Mat pattern = frame(cv::Rect(x, y, w, h));
    cv::Scalar mean, deviation;
    cv::meanStdDev(pattern, mean, deviation);
    if (deviation[0] < 3)
      continue;
    cv::Mat score;
    cv::matchTemplate(page.colRange(x, x + w), pattern, score,
                      cv::TM_CCOEFF_NORMED);
    for (int row = 0; row < score.rows; ++row)
      if (score.at<float>(row, 0) >= .90f)
        candidates.insert(row - y);
  }
  if (candidates.empty()) {
    if (page.size() == frame.size()) {
      cv::Mat a, b, window;
      page.convertTo(a, CV_32F);
      frame.convertTo(b, CV_32F);
      cv::createHanningWindow(window, a.size(), CV_32F);
      double response = 0;
      const auto shift = cv::phaseCorrelate(b, a, window, &response);
      if (response > .1 && std::abs(shift.x) < 2)
        for (int d = -2; d <= 2; ++d)
          candidates.insert(qRound(shift.y) + d);
    }
  }
  if (candidates.size() > 64)
    return {0, false, true};
  std::vector<std::pair<double, int>> verified;
  for (int candidate : candidates) {
    const auto error = verify(page, frame, candidate);
    if (error < 1e8)
      verified.emplace_back(error, candidate);
  }
  if (verified.empty())
    return {};
  std::sort(verified.begin(), verified.end());
  const auto [best, position] = verified.front();
  for (const auto &[error, other] : verified)
    if (std::abs(other - position) > 2 &&
        error - best < std::max(.025, best * .25))
      return {0, false, true};
  return {position, true, false};
}
// A stationary textured backdrop can dominate ordinary registration. Match
// only edges that changed at their screen position, then verify agreement on
// both frames. The accepted strips retain real pixels, including background
// tiles, without blending or synthesizing foreground detail.
Match matchForeground(const cv::Mat &a, const cv::Mat &b) {
  if (a.size() != b.size())
    return {};
  cv::Mat delta;
  cv::absdiff(a, b, delta);
  struct Patch {
    cv::Rect rect;
    double score;
  };
  std::vector<Patch> patches;
  const int width = std::min(96, b.cols), height = 16;
  for (int band = 0; band < 8; ++band) {
    Patch best{{}, 0};
    for (int y = band * b.rows / 8;
         y < std::min((band + 1) * b.rows / 8, b.rows - height); y += 4)
      for (int x : {0, (b.cols - width) / 2, b.cols - width}) {
        const cv::Rect r(x, y, width, height);
        const auto pattern = b(r);
        if (cv::countNonZero(delta(r) > 5) < r.area() / 5)
          continue;
        cv::Mat dx;
        cv::absdiff(pattern.colRange(0, width - 1), pattern.colRange(1, width),
                    dx);
        const double density = double(cv::countNonZero(dx > 12)) / dx.total();
        if (density < .015 || density > .45)
          continue;
        cv::Scalar mean, dev;
        cv::meanStdDev(pattern, mean, dev);
        if (dev[0] > best.score)
          best = {r, dev[0]};
      }
    if (best.score > 4)
      patches.push_back(best);
  }
  std::map<int, std::vector<int>> votes;
  for (const auto &patch : patches) {
    const auto pattern = b(patch.rect);
    cv::Mat scores;
    cv::matchTemplate(a.colRange(patch.rect.x, patch.rect.x + width), pattern,
                      scores, cv::TM_CCOEFF_NORMED);
    for (int row = 0; row < scores.rows; ++row)
      if (scores.at<float>(row, 0) > .97) {
        const int d = row - patch.rect.y;
        if (d == 0 || std::abs(d) > b.rows * 3 / 4)
          continue;
        cv::Mat difference;
        cv::absdiff(a(cv::Rect(patch.rect.x, row, width, height)), pattern,
                    difference);
        if (cv::mean(difference)[0] < .6)
          votes[d].push_back(patch.rect.y);
      }
  }
  std::vector<int> accepted;
  for (const auto &[d, ys] : votes)
    if (ys.size() >= 3 && *std::max_element(ys.begin(), ys.end()) -
                                  *std::min_element(ys.begin(), ys.end()) >=
                              b.rows / 4)
      accepted.push_back(d);
  if (accepted.size() != 1)
    return {0, false, accepted.size() > 1};
  return {accepted.front(), true, false};
}
bool verifyOriginalPixels(const QImage &first, const QImage &second,
                          QRect content, int d) {
  const int start = std::max(0, d),
            end = std::min(content.height(), content.height() + d);
  int votes = 0, agree = 0;
  for (int y = start; y < end; y += 32)
    for (int x = 0; x < content.width(); x += 64) {
      int w = std::min(64, content.width() - x), h = std::min(32, end - y);
      cv::Mat a(h, w, CV_8UC4,
                const_cast<uchar *>(first.constScanLine(content.y() + y)) +
                    4 * (content.x() + x),
                first.bytesPerLine());
      cv::Mat b(h, w, CV_8UC4,
                const_cast<uchar *>(second.constScanLine(content.y() + y - d)) +
                    4 * (content.x() + x),
                second.bytesPerLine());
      cv::Scalar mean, dev;
      cv::meanStdDev(a, mean, dev);
      if (std::max({dev[0], dev[1], dev[2]}) < 3)
        continue;
      ++votes;
      cv::Mat diff;
      cv::absdiff(a, b, diff);
      const auto error = cv::mean(diff);
      if (std::max({error[0], error[1], error[2], error[3]}) <= 3)
        ++agree;
    }
  return votes >= 4 && agree >= votes * .8;
}
QRect detectContent(const cv::Mat &a, const cv::Mat &b) {
  cv::Mat difference;
  cv::absdiff(a, b, difference);
  cv::Mat changed = difference > 5;
  int top = 0, bottom = a.rows, left = 0, right = a.cols;
  while (top < a.rows / 3 &&
         cv::countNonZero(changed.row(top)) < std::max(2, a.cols / 100))
    ++top;
  while (bottom > a.rows * 2 / 3 &&
         cv::countNonZero(changed.row(bottom - 1)) < std::max(2, a.cols / 100))
    --bottom;
  // Untextured white gutters belong to the document, not to pinned chrome.
  auto meaningful = [&](cv::Rect r) {
    if (r.area() == 0)
      return false;
    cv::Scalar mean, dev;
    cv::meanStdDev(a(r), mean, dev);
    return dev[0] > 4 || mean[0] < 240;
  };
  if (!meaningful({0, 0, a.cols, top}))
    top = 0;
  if (!meaningful({0, bottom, a.cols, a.rows - bottom}))
    bottom = a.rows;
  while (left < a.cols / 3 &&
         cv::countNonZero(changed(cv::Rect(left, top, 1, bottom - top))) < 2)
    ++left;
  while (right > a.cols * 2 / 3 &&
         cv::countNonZero(changed(cv::Rect(right - 1, top, 1, bottom - top))) <
             2)
    --right;
  if (!meaningful({0, top, left, bottom - top}))
    left = 0;
  if (!meaningful({right, top, a.cols - right, bottom - top}))
    right = a.cols;
  return {left, top, right - left, bottom - top};
}
// Insert only into a quiet band: never through text, icons, or a footer.
int sidebarSeam(const cv::Mat &image, QRect side) {
  if (side.width() == 0)
    return -1;
  const int margin = std::max(12, side.height() / 30);
  for (int y = side.bottom() - margin - 8; y >= side.top() + side.height() / 2;
       --y) {
    const cv::Mat band = image(cv::Rect(side.x(), y - 8, side.width(), 17));
    double worst = 0;
    for (int row = 1; row < band.rows; ++row) {
      cv::Mat difference;
      cv::absdiff(band.row(row - 1), band.row(row), difference);
      double max = 0;
      cv::minMaxLoc(difference, nullptr, &max);
      worst = std::max(worst, max);
    }
    if (worst <= 3)
      return y;
  }
  return -1;
}
bool texturedBackdrop(const cv::Mat &image, QRect side) {
  if (side.width() < 4 || side.height() < 8)
    return false;
  const auto region =
      image(cv::Rect(side.x(), side.y(), side.width(), side.height()));
  cv::Mat dx;
  cv::absdiff(region.colRange(0, region.cols - 1),
              region.colRange(1, region.cols), dx);
  // Sparse text and icons cannot qualify as a dense background texture.
  return cv::countNonZero(dx > 12) > dx.total() * .70;
}
cv::Mat crop(const cv::Mat &image, const QRect &rect) {
  return image(cv::Rect(rect.x(), rect.y(), rect.width(), rect.height()));
}
} // namespace
ScrollStitcher::ScrollStitcher() = default;
ScrollStitcher::ScrollStitcher(Limits limits) : m_limits(limits) {}
bool ScrollStitcher::stable(const QImage &a, const QImage &b) {
  if (a.isNull() || a.size() != b.size())
    return false;
  const auto first = a.convertToFormat(QImage::Format_ARGB32),
             second = b.convertToFormat(QImage::Format_ARGB32);
  cv::Mat aa(first.height(), first.width(), CV_8UC4,
             const_cast<uchar *>(first.constBits()), first.bytesPerLine());
  cv::Mat bb(second.height(), second.width(), CV_8UC4,
             const_cast<uchar *>(second.constBits()), second.bytesPerLine());
  cv::Mat diff;
  cv::absdiff(aa, bb, diff);
  const auto average = cv::mean(diff);
  return std::max({average[0], average[1], average[2], average[3]}) <= .35 &&
         static_cast<size_t>(cv::countNonZero(diff.reshape(1) > 12)) <=
             diff.total() / 1000;
}
ScrollStitcher::Result ScrollStitcher::append(const QImage &source, const QImage &original) {
  if ((!original.isNull() && original.size() != source.size()) ||
      (!empty() && original.isNull() != m_originalInitial.isNull()))
    return {Status::InvalidFrame};
  if (source.isNull() || source.width() < 64 || source.height() < 96)
    return {Status::InvalidFrame};
  if (source.height() > m_limits.maxHeight ||
      qint64(source.width()) * source.height() > m_limits.maxPixels)
    return {Status::LimitReached};
  const auto frame = source.convertToFormat(QImage::Format_ARGB32);
  if (empty()) {
    m_initial = m_previous = frame;
    m_originalInitial = original.convertToFormat(QImage::Format_ARGB32);
    m_layout = {frame.rect(), frame.size()};
    m_frames = 1;
    return {Status::Started, frame.height()};
  }
  if (frame.size() != m_initial.size())
    return {Status::SizeChanged};
  if (stable(m_previous, frame))
    return {Status::Unchanged};
  const auto previous = gray(m_previous), current = gray(frame);
  QRect content =
      m_layoutLocked ? m_layout.content : detectContent(previous, current);
  if (content.width() < 64 || content.height() < 96)
    return {Status::NoOverlap};
  auto alignment =
      match(compact(crop(previous, content)), compact(crop(current, content)));
  if (alignment.found &&
      !verifyOriginalPixels(m_previous, frame, content, alignment.position))
    alignment.found = false;
  if (!alignment.found && !alignment.ambiguous)
    alignment = matchForeground(compact(crop(previous, content)),
                                compact(crop(current, content)));
  int position = m_layout.viewportY + alignment.position;
  if (!alignment.found && !alignment.ambiguous && m_layoutLocked) {
    alignment =
        match(compact(gray(bodyImage())), compact(crop(current, content)));
    position = m_layout.minimumY + alignment.position;
    // Relocation may only land wholly within retained history. A lost gap
    // must not create an unobserved interval in the output.
    if (position < m_layout.minimumY || position > m_layout.maximumY)
      alignment.found = false;
    if (alignment.found) {
      const auto retained = bodyImage().copy(0, alignment.position,
                                             content.width(), content.height());
      const auto visible = frame.copy(content);
      if (!verifyOriginalPixels(retained, visible, visible.rect(), 0))
        alignment.found = false;
    }
  }
  if (!alignment.found)
    return {alignment.ambiguous ? Status::Ambiguous : Status::NoOverlap};
  if (!m_layoutLocked && alignment.position != 0) {
    // Tiny scrolls can leave a glyph edge unchanged at the viewport boundary.
    // A proposed fixed band that also matches the translated page belongs to
    // the document. Compare every pixel, including alpha, before retaining it
    // as moving content; stationary chrome must keep its original geometry.
    const int d = std::abs(alignment.position);
    const QImage& upper = alignment.position > 0 ? m_previous : frame;
    const QImage& lower = alignment.position > 0 ? frame : m_previous;
    const auto translatedBand = [&](int y, int height) {
      if (height <= 0 || y < 0 || y + height + d > frame.height())
        return false;
      return upper.copy(content.x(), y + d, content.width(), height) ==
             lower.copy(content.x(), y, content.width(), height);
    };
    if (translatedBand(0, content.top()))
      content.setTop(0);
    const int footer = frame.height() - content.bottom() - 1;
    if (translatedBand(frame.height() - footer - d, footer))
      content.setBottom(frame.height() - 1);
  }
  const int minY = std::min(m_layout.minimumY, position),
            maxY = std::max(m_layout.maximumY, position);
  const int added = (maxY - minY) - (m_layout.maximumY - m_layout.minimumY);
  const qint64 height = frame.height() + maxY - minY;
  if (height > m_limits.maxHeight ||
      height * frame.width() > m_limits.maxPixels ||
      (added && m_frames >= m_limits.maxFrames))
    return {Status::LimitReached};
  if (!m_layoutLocked) {
    const int left =
        sidebarSeam(previous, {0, content.y(), content.x(), content.height()});
    const int right = sidebarSeam(
        previous, {content.right() + 1, content.y(),
                   frame.width() - content.right() - 1, content.height()});
    const bool leftTile =
        left < 0 && texturedBackdrop(previous, {0, content.y(), content.x(),
                                                content.height()});
    const bool rightTile =
        right < 0 &&
        texturedBackdrop(previous, {content.right() + 1, content.y(),
                                    frame.width() - content.right() - 1,
                                    content.height()});
    if ((content.x() > 0 && left < 0 && !leftTile) ||
        (content.right() + 1 < frame.width() && right < 0 && !rightTile))
      return {Status::Ambiguous};
    m_layout.leftTiled = leftTile;
    m_layout.rightTiled = rightTile;
    m_layout.content = content;
    m_layout.leftSeam = left;
    m_layout.rightSeam = right;
    m_layoutLocked = true;
    m_strips.push_back({0, m_initial.copy(content)});
    if (!original.isNull())
      m_originalStrips.push_back({0, m_originalInitial.copy(content)});
  }
  if (minY < m_layout.minimumY)
    m_strips.push_back(
        {minY, frame.copy(content.x(), content.y(), content.width(),
                          m_layout.minimumY - minY)});
  if (maxY > m_layout.maximumY) {
    const int count = maxY - m_layout.maximumY;
    m_strips.push_back({m_layout.maximumY + content.height(),
                        frame.copy(content.x(), content.bottom() + 1 - count,
                                   content.width(), count)});
  }
  // Retain uncomposited pixels using the same accepted alignment. This avoids
  // a second matcher and allows background changes after capture.
  if (!original.isNull()) {
    if (minY < m_layout.minimumY)
      m_originalStrips.push_back({minY, original.copy(content.x(), content.y(),
          content.width(), m_layout.minimumY - minY).convertToFormat(QImage::Format_ARGB32)});
    if (maxY > m_layout.maximumY) {
      const int count = maxY - m_layout.maximumY;
      m_originalStrips.push_back({m_layout.maximumY + content.height(),
          original.copy(content.x(), content.bottom() + 1 - count,
                        content.width(), count).convertToFormat(QImage::Format_ARGB32)});
    }
  }
  const int delta = position - m_layout.viewportY;
  m_layout.minimumY = minY;
  m_layout.maximumY = maxY;
  m_layout.viewportY = position;
  m_previous = frame;
  if (added)
    ++m_frames;
  return {added ? Status::Appended : Status::Relocated, added, delta};
}
QImage ScrollStitcher::bodyImage() const {
  QImage result(m_layout.content.width(),
                m_layout.content.height() + m_layout.maximumY -
                    m_layout.minimumY,
                QImage::Format_ARGB32);
  result.fill(Qt::transparent);
  QPainter p(&result);
  for (const auto &strip : m_strips)
    p.drawImage(0, strip.y - m_layout.minimumY, strip.image);
  return result;
}
void ScrollStitcher::paint(QPainter &p, bool original) const {
  const auto& initial = original && !m_originalInitial.isNull() ? m_originalInitial : m_initial;
  const auto& strips = original && !m_originalInitial.isNull() ? m_originalStrips : m_strips;
  if (!m_layoutLocked) {
    p.drawImage(0, 0, initial);
    return;
  }
  const auto c = m_layout.content;
  // Preserve sidebar objects above/below a quiet band. The inserted span
  // is a per-column linear gradient; glyphs and icons are never stretched.
  const int growth = m_layout.maximumY - m_layout.minimumY;
  const auto sidebar = [&](int x, int width, int seam) {
    if (width <= 0)
      return;
    if (seam < 0) {
      // A dense, stationary background may repeat in source-pixel blocks.
      const int tile = std::min(64, c.height());
      p.drawImage(QRect(x, c.y(), width, c.height()), initial,
                  QRect(x, c.y(), width, c.height()));
      for (int y = c.bottom() + 1; y < c.bottom() + 1 + growth; y += tile)
        p.drawImage(
            QRect(x, y, width, std::min(tile, c.bottom() + 1 + growth - y)),
            initial,
            QRect(x, c.bottom() + 1 - tile, width,
                  std::min(tile, c.bottom() + 1 + growth - y)));
      return;
    }
    p.drawImage(QRect(x, c.y(), width, seam - c.y()), initial,
                QRect(x, c.y(), width, seam - c.y()));
    p.drawImage(QRect(x, seam + growth, width, c.bottom() + 1 - seam),
                initial, QRect(x, seam, width, c.bottom() + 1 - seam));
    if (growth <= 0)
      return;
    for (int i = 0; i < width; ++i) {
      QLinearGradient gradient(0, seam, 0, seam + growth);
      gradient.setColorAt(0, initial.pixelColor(x + i, seam - 1));
      gradient.setColorAt(1, initial.pixelColor(x + i, seam));
      p.fillRect(QRect(x + i, seam, 1, growth), gradient);
    }
  };
  sidebar(0, c.x(), m_layout.leftSeam);
  sidebar(c.right() + 1, initial.width() - c.right() - 1, m_layout.rightSeam);
  p.drawImage(QRect(0, 0, initial.width(), c.y()), initial,
              QRect(0, 0, initial.width(), c.y()));
  const int footer = initial.height() - c.bottom() - 1;
  p.drawImage(QRect(0, size().height() - footer, initial.width(), footer),
              initial, QRect(0, c.bottom() + 1, initial.width(), footer));
  for (const auto &strip : strips)
    p.drawImage(c.x(), c.y() + strip.y - m_layout.minimumY, strip.image);
}
QImage ScrollStitcher::image() const {
  if (empty())
    return {};
  QImage result(size(), QImage::Format_ARGB32);
  result.fill(Qt::transparent);
  QPainter p(&result);
  paint(p);
  return result;
}
QImage ScrollStitcher::originalImage() const {
  if (m_originalInitial.isNull())
    return image();
  QImage result(size(), QImage::Format_ARGB32);
  result.fill(Qt::transparent);
  QPainter painter(&result);
  paint(painter, true);
  return result;
}
QImage ScrollStitcher::preview(const QSize &bounds) const {
  if (empty())
    return {};
  QImage result(size().scaled(bounds, Qt::KeepAspectRatio),
                QImage::Format_ARGB32);
  result.fill(Qt::transparent);
  QPainter p(&result);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.scale(qreal(result.width()) / size().width(),
          qreal(result.height()) / size().height());
  paint(p);
  return result;
}
} // namespace hyprcapture::ui
