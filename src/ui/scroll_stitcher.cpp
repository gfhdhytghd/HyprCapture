#include "ui/scroll_stitcher.hpp"
#include <qfloat16.h>
#include <QColorSpace>
#include <QLinearGradient>
#include <algorithm>
#include <cmath>
#include <cstring>
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
// Glyph contrast changes as it moves over a translucent backdrop. Compare
// detail correlation rather than treating that change as a different glyph.
double detailError(const cv::Mat &a, const cv::Mat &b) {
  cv::Scalar mean, deviation;
  cv::meanStdDev(b, mean, deviation);
  if (std::max({deviation[0], deviation[1], deviation[2]}) < .1)
    return 1e9;
  cv::Mat score;
  cv::matchTemplate(a, b, score, cv::TM_CCOEFF_NORMED);
  return 100.0 * (1.0 - score.at<float>(0, 0));
}
// Use textured tiles as evidence. White margins never vote for alignment.
// A small changing subregion (caret, clock) cannot outvote the page itself.
double verify(const cv::Mat &a, const cv::Mat &b, int position, bool foreground) {
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
      if (foreground)
        errors.push_back(detailError(a(ra), b(rb)));
      else {
        cv::Mat diff;
        cv::absdiff(a(ra), b(rb), diff);
        errors.push_back(cv::mean(diff)[0]);
      }
    }
  if (errors.size() < 4)
    return 1e9;
  std::sort(errors.begin(), errors.end());
  // Detail registration needs broader agreement: a repeated phrase alone
  // must not outweigh the differing line numbers or values beside it.
  const size_t accepted = std::max<size_t>(1, foreground ? (errors.size() * 19 + 19) / 20
                                                       : (errors.size() * 4 + 4) / 5);
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
// Independent templates vote for a *vertical translation*, not an affine
// warp. Like multi-template screenshot stitchers, cap each spatial cell at
// one vote. A detailed GIF cannot buy more votes by containing more corners.
// Native RGBA checks keep downsampling and repeated text from inventing a
// correspondence. Stationary cells form a separate zero-motion layer.
Match matchConsensus(const QImage &first, const QImage &second, QRect content,
                     bool history = false) {
  const auto a = compact(gray(history ? first : first.copy(content)));
  const auto b = compact(gray(second.copy(content)));
  cv::Mat aa(first.height(), first.width(), CV_8UC4,
             const_cast<uchar *>(first.constBits()), first.bytesPerLine());
  cv::Mat bb(second.height(), second.width(), CV_8UC4,
             const_cast<uchar *>(second.constBits()), second.bytesPerLine());
  struct Vote { int x, y; };
  std::map<int, std::vector<Vote>> votes;
  constexpr int columns = 6, rows = 12;
  const int w = std::min(48, b.cols / 2), h = std::min(48, b.rows / 8);
  if (w < 8 || h < 8) return {};
  const auto error = [&](int x, int y, int d) {
    const int left = x * content.width() / b.cols;
    const int right = (x + w) * content.width() / b.cols;
    const cv::Rect rb(content.x() + left, content.y() + y, right - left, h);
    auto ra = rb;
    ra.y += d;
    cv::Mat diff;
    cv::absdiff(aa(ra), bb(rb), diff);
    const auto mean = cv::mean(diff);
    const double worst = std::max({mean[0], mean[1], mean[2], mean[3]});
    // An averaged error alone hides differing line numbers beside repeated
    // text. Native captures are lossless: require nearly identical pixels in
    // each voting patch, leaving translucent detail to the strict matcher.
    return worst <= .35 && size_t(cv::countNonZero(diff.reshape(1) > 3)) <= diff.total() / 1000
               ? worst : 1e9;
  };
  for (int row = 0; row < rows; ++row) {
    const int y = row * (b.rows - h) / (rows - 1);
    for (int column = 0; column < columns; ++column) {
      const int x = column * (b.cols - w) / (columns - 1);
      const auto pattern = b(cv::Rect(x, y, w, h));
      cv::Scalar mean, dev;
      cv::meanStdDev(pattern, mean, dev);
      if (dev[0] < 3) continue; // Blank margins have no motion information.
      if (!history && error(x, y, 0) <= .35) {
        votes[0].push_back({x, y});
        continue;
      }
      cv::Mat score;
      cv::matchTemplate(a.colRange(x, x + w), pattern, score, cv::TM_CCOEFF_NORMED);
      std::vector<std::pair<double, int>> peaks;
      for (int from = 0; from < score.rows; ++from) {
        const int d = from - y;
        const bool inRange = history ? d >= 0 && d + b.rows <= a.rows
            : b.rows - std::abs(d) >= std::max(64, b.rows / 4);
        if (!inRange ||
            score.at<float>(from, 0) < .97f)
          continue;
        const double e = error(x, y, d);
        if (e <= .35) peaks.emplace_back(e, d);
        if (peaks.size() > 32) break; // Repeated history cannot cast a unique vote.
      }
      if (peaks.empty() || peaks.size() > 32) continue; // No unique correspondence.
      std::sort(peaks.begin(), peaks.end());
      const auto [best, d] = peaks.front();
      const bool ambiguous = std::any_of(peaks.begin() + 1, peaks.end(),
          [&](const auto &peak) {
            return std::abs(peak.second - d) > 2 && peak.first <= best + .5;
          });
      if (ambiguous) continue;
      votes[d].push_back({x, y});
    }
  }
  std::vector<int> accepted;
  bool stationary = false;
  for (const auto &[d, support] : votes) {
    if (support.size() < 6) continue;
    int minX = b.cols, maxX = 0, minY = b.rows, maxY = 0;
    std::set<int> bands, lanes;
    for (const auto &v : support) {
      minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
      minY = std::min(minY, v.y); maxY = std::max(maxY, v.y);
      bands.insert(v.y); lanes.insert(v.x);
    }
    // A scrolling video inside a stationary page can itself have perfect
    // matches. Require evidence spread across the overlap, not one widget.
    const int overlap = history ? b.rows : b.rows - std::abs(d);
    const bool broad = lanes.size() >= 2 && maxX - minX >= b.cols * .35;
    // A wide fixed panel can leave only a narrow column of line numbers.
    // Accept that column only with more independently matched vertical bands.
    const bool tall = bands.size() >= 5 && maxY - minY >= overlap * .75;
    if (bands.size() < 3 || (!broad && !tall) || maxY - minY < overlap * .60)
      continue;
    if (d == 0 && !history) stationary = true;
    else accepted.push_back(d);
  }
  if (accepted.size() > 1) return {0, false, true};
  if (accepted.size() == 1) return {accepted.front(), true, false};
  if (stationary) return {0, true, false};
  return {};
}
Match match(const cv::Mat &page, const cv::Mat &frame, bool foreground = false) {
  std::set<int> candidates;
  const int h = std::min(48, frame.rows / 4);
  // Several independent templates, retaining every peak rather than a top-N
  // shortlist: periodic paragraphs must remain ambiguous.
  const int w = std::min(96, frame.cols);
  bool overwhelmed = false;
  for (int x : {0, (frame.cols - w) / 2, frame.cols - w}) {
    std::set<int> columnCandidates;
    for (int y : {0, (frame.rows - h) / 2, frame.rows - h}) {
      const cv::Mat pattern = frame(cv::Rect(x, y, w, h));
      cv::Scalar mean, deviation;
      cv::meanStdDev(pattern, mean, deviation);
      if (deviation[0] < 3) continue;
      cv::Mat score;
      cv::matchTemplate(page.colRange(x, x + w), pattern, score, cv::TM_CCOEFF_NORMED);
      for (int row = 0; row < score.rows; ++row)
        if (score.at<float>(row, 0) >= .90f) columnCandidates.insert(row - y);
    }
    // A flat/repeating central image can produce dozens of equally good
    // peaks. It must not hide distinctive text in the other columns. Keep
    // every peak in each informative column, then verify the whole overlap.
    if (columnCandidates.size() > 64) { overwhelmed = true; continue; }
    candidates.insert(columnCandidates.begin(), columnCandidates.end());
  }
  if (candidates.empty() && overwhelmed) return {0, false, true};
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
  if (candidates.size() > 192)
    return {0, false, true};
  std::vector<std::pair<double, int>> verified;
  for (int candidate : candidates) {
    const auto error = verify(page, frame, candidate, foreground);
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
// Remove slowly varying illumination for registration only. A translucent
// terminal's wallpaper stays fixed while its glyphs move. Comparing their
// composited intensities can reject the true shift or favor a neighboring
// repeated line. Horizontal filtering preserves vertical scroll coordinates.
cv::Mat foregroundDetail(const cv::Mat &input) {
  cv::Mat background, detail;
  cv::blur(input, background, {31, 1});
  cv::subtract(input, background, detail, cv::noArray(), CV_16S);
  detail.convertTo(detail, CV_8U, .5, 128);
  return detail;
}
bool exactOverlap(const QImage &first, const QImage &second, QRect content, int d) {
  const int top = std::max(0, d), bottom = std::min(content.height(), content.height() + d);
  if (bottom - top < std::max(64, content.height() / 4))
    return false;
  for (int y = top; y < bottom; ++y)
    if (std::memcmp(first.constScanLine(content.y() + y) + 4 * content.x(),
                    second.constScanLine(content.y() + y - d) + 4 * content.x(),
                    size_t(content.width()) * 4) != 0)
      return false;
  return true;
}
// Once motion is known, source selection is a separate problem. If a changing
// object crosses the insertion edge, move the cut into an agreeing band in the
// overlap. Copy whole rows from one capture; never blend text or HDR values.
int coherentSeam(const QImage &retained, const QImage &incoming, bool down) {
  const int boundary = down ? retained.height() : 0;
  int run = 0;
  for (int step = 0; step < retained.height(); ++step) {
    const int y = down ? retained.height() - 1 - step : step;
    const auto *a = reinterpret_cast<const QRgb *>(retained.constScanLine(y));
    const auto *b = reinterpret_cast<const QRgb *>(incoming.constScanLine(y));
    int changed = 0;
    for (int x = 0; x < retained.width(); ++x)
      changed += std::max({std::abs(qRed(a[x]) - qRed(b[x])),
                          std::abs(qGreen(a[x]) - qGreen(b[x])),
                          std::abs(qBlue(a[x]) - qBlue(b[x])),
                          std::abs(qAlpha(a[x]) - qAlpha(b[x]))}) > 3;
    run = changed <= retained.width() / 200 ? run + 1 : 0;
    if (run == 8)
      return step == 7 ? boundary : down ? y + 4 : y - 3;
  }
  return boundary; // No observed clean cut: retain the existing overlap.
}
bool verifyOriginalPixels(const QImage &first, const QImage &second,
                          QRect content, int d, bool foreground = false) {
  cv::Mat firstPixels(first.height(), first.width(), CV_8UC4,
                      const_cast<uchar *>(first.constBits()), first.bytesPerLine());
  cv::Mat secondPixels(second.height(), second.width(), CV_8UC4,
                       const_cast<uchar *>(second.constBits()), second.bytesPerLine());
  if (foreground) {
    firstPixels = foregroundDetail(firstPixels);
    secondPixels = foregroundDetail(secondPixels);
  }
  const int start = std::max(0, d),
            end = std::min(content.height(), content.height() + d);
  int votes = 0, agree = 0;
  for (int y = start; y < end; y += 32)
    for (int x = 0; x < content.width(); x += 64) {
      int w = std::min(64, content.width() - x), h = std::min(32, end - y);
      const auto a = firstPixels(cv::Rect(content.x() + x, content.y() + y, w, h));
      const auto b = secondPixels(cv::Rect(content.x() + x, content.y() + y - d, w, h));
      cv::Scalar mean, dev;
      cv::meanStdDev(a, mean, dev);
      if (std::max({dev[0], dev[1], dev[2]}) < 3)
        continue;
      ++votes;
      if (foreground) {
        agree += detailError(a, b) <= 3.0;
        continue;
      }
      cv::Mat diff;
      cv::absdiff(a, b, diff);
      const auto error = cv::mean(diff);
      if (std::max({error[0], error[1], error[2], error[3]}) <= 3.0)
        ++agree;
    }
  return votes >= 4 && agree >= votes * (foreground ? .95 : .8);
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
bool texturedFrames(const cv::Mat &a, const cv::Mat &b) {
  int firstTiles = 0, secondTiles = 0;
  for (int y = 0; y + 16 <= a.rows; y += 16)
    for (int x = 0; x + 32 <= a.cols; x += 32) {
      const QRect tile(x, y, 32, 16);
      firstTiles += texturedBackdrop(a, tile);
      secondTiles += texturedBackdrop(b, tile);
      if (firstTiles >= 3 && secondTiles >= 3)
        return true;
    }
  return false;
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
ScrollStitcher::Result ScrollStitcher::append(const QImage &source, const QImage &original, const QImage &hdr) {
  if ((!hdr.isNull() && (hdr.size() != source.size() || hdr.format() != QImage::Format_RGBA16FPx4_Premultiplied)) ||
      (!empty() && hdr.isNull() != m_hdrInitial.isNull())) return {Status::InvalidFrame};
  if ((!original.isNull() && original.size() != source.size()) ||
      (!empty() && original.isNull() != m_originalInitial.isNull()))
    return {Status::InvalidFrame};
  if (source.isNull() || source.width() < 64 || source.height() < 96)
    return {Status::InvalidFrame};
  if (!hdr.isNull()) m_limits.maxPixels = std::min(m_limits.maxPixels, 16LL * 1024 * 1024);
  if (source.height() > m_limits.maxHeight ||
      qint64(source.width()) * source.height() > m_limits.maxPixels)
    return {Status::LimitReached};
  const auto frame = source.convertToFormat(QImage::Format_ARGB32);
  if (empty()) {
    m_initial = m_previous = frame;
    m_originalInitial = original.convertToFormat(QImage::Format_ARGB32);
    m_hdrInitial = hdr;
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
  const auto previousCompact = compact(crop(previous, content));
  const auto currentCompact = compact(crop(current, content));
  const auto rawAlignment = match(previousCompact, currentCompact);
  auto alignment = rawAlignment;
  bool consensusMotion = false;
  // Opaque documents usually have a byte-identical overlap. This stronger,
  // inexpensive check avoids filtering full-resolution frames on that path.
  const bool smallInferredContent = !m_layoutLocked &&
      (content.width() < frame.width() * .6 || content.height() < frame.height() * .6);
  if (!alignment.found || !exactOverlap(m_previous, frame, content, alignment.position)) {
    alignment = match(foregroundDetail(previousCompact), foregroundDetail(currentCompact), true);
    if (alignment.found &&
        !verifyOriginalPixels(m_previous, frame, content, alignment.position, true))
      alignment.found = false;
    if (!alignment.found) {
      const bool detailAmbiguous = alignment.ambiguous;
      alignment = rawAlignment;
      if (alignment.found &&
          (!verifyOriginalPixels(m_previous, frame, content, alignment.position) ||
           !verifyOriginalPixels(m_previous, frame, content, alignment.position, true)))
        alignment.found = false;
      // The sparse-patch fallback is only evidence for a stationary textured
      // backdrop. Repeated terminal phrases alone can vote for a false shift.
      if (!alignment.found && !alignment.ambiguous &&
          texturedFrames(crop(previous, content), crop(current, content)))
        alignment = matchForeground(previousCompact, currentCompact);
      if (!alignment.found)
        alignment.ambiguous |= detailAmbiguous;
    }
  }
  if (!alignment.found || alignment.position == 0 || smallInferredContent) {
    // Inspect the whole viewport until actual scroll establishes its chrome.
    // An idle GIF otherwise makes detectContent() crop to the animated widget.
    const auto consensus = matchConsensus(m_previous, frame,
        m_layoutLocked ? content : frame.rect());
    if (consensus.found || consensus.ambiguous) {
      alignment = consensus;
      consensusMotion = consensus.found && consensus.position != 0;
    }
    else if (smallInferredContent) alignment.found = false;
  }
  if (alignment.found && alignment.position == 0) {
    // Keep the last registered anchor. Unmatched animation must not replace
    // it (or silently rebase a moving region whose motion was not observed).
    return {Status::Unchanged};
  }
  int position = m_layout.viewportY + alignment.position;
  if (!alignment.found && !alignment.ambiguous && m_layoutLocked) {
    const auto body = bodyImage();
    const auto history = compact(gray(body));
    const auto visible = compact(crop(current, content));
    alignment = match(foregroundDetail(history), foregroundDetail(visible), true);
    const bool foreground = alignment.found;
    if (!alignment.found && !alignment.ambiguous)
      alignment = match(history, visible);
    bool consensus = false;
    const auto visibleFrame = frame.copy(content);
    if (!alignment.found && !alignment.ambiguous) {
      alignment = matchConsensus(body, visibleFrame, visibleFrame.rect(), true);
      consensus = alignment.found;
    }
    position = m_layout.minimumY + alignment.position;
    // Relocation may only land wholly within retained history. A lost gap
    // must not create an unobserved interval in the output.
    if (position < m_layout.minimumY || position > m_layout.maximumY)
      alignment.found = false;
    if (alignment.found && !consensus) {
      const auto retained = body.copy(0, alignment.position,
                                      content.width(), content.height());
      if (!verifyOriginalPixels(retained, visibleFrame, visibleFrame.rect(), 0, foreground))
        alignment.found = false;
    }
  }
  if (!alignment.found)
    return {alignment.ambiguous ? Status::Ambiguous : Status::NoOverlap};
  if (!m_layoutLocked && alignment.position != 0) {
    // Tiny scrolls can leave a glyph edge unchanged at the viewport boundary.
    // A proposed fixed band that also matches the translated page belongs to
    // the document. A stationary smooth backdrop must not pin glyph stems at
    // this boundary either; require actual detail before using that comparison.
    const int d = std::abs(alignment.position);
    const QImage& upper = alignment.position > 0 ? m_previous : frame;
    const QImage& lower = alignment.position > 0 ? frame : m_previous;
    const auto translatedBand = [&](int y, int height) {
      if (height <= 0 || y < 0 || y + height + d > frame.height())
        return false;
      const auto a = upper.copy(content.x(), y + d, content.width(), height);
      const auto b = lower.copy(content.x(), y, content.width(), height);
      if (a == b)
        return true;
      const auto first = foregroundDetail(cv::Mat(a.height(), a.width(), CV_8UC4,
                          const_cast<uchar *>(a.constBits()), a.bytesPerLine()));
      const auto second = foregroundDetail(cv::Mat(b.height(), b.width(), CV_8UC4,
                           const_cast<uchar *>(b.constBits()), b.bytesPerLine()));
      cv::Scalar mean, deviation;
      cv::meanStdDev(first, mean, deviation);
      return std::max({deviation[0], deviation[1], deviation[2]}) >= 3 &&
             detailError(first, second) <= 1.0;
    };
    if (translatedBand(0, content.top()))
      content.setTop(0);
    const int footer = frame.height() - content.bottom() - 1;
    if (translatedBand(frame.height() - footer - d, footer))
      content.setBottom(frame.height() - 1);
    // Sparse moving gutter markers can look stationary on a one-pixel
    // touchpad step. Reclaim only columns that change and whose entire
    // translated interior agrees; fixed sidebar labels must still stay fixed.
    const auto translatedSide = [&](int x, int width, bool left) {
      const int y = content.top() + 32;
      const int height = content.height() - d - 64;
      if (width <= 0 || height < 96) return -1;
      int edge = left ? x + width : x - 1;
      for (int column = x; column < x + width; ++column) {
        bool changed = false, translated = true;
        for (int row = y; row < y + height; ++row) {
          const auto a = upper.pixel(column, row + d), b = lower.pixel(column, row);
          if (std::max({std::abs(qRed(a)-qRed(b)),std::abs(qGreen(a)-qGreen(b)),std::abs(qBlue(a)-qBlue(b)),std::abs(qAlpha(a)-qAlpha(b))}) > 1) { translated = false; break; }
          const auto unshifted = upper.pixel(column, row);
          changed |= std::max({std::abs(qRed(unshifted)-qRed(b)),std::abs(qGreen(unshifted)-qGreen(b)),std::abs(qBlue(unshifted)-qBlue(b))}) > 2;
        }
        if (changed && translated) edge = left ? std::min(edge,column) : std::max(edge,column);
      }
      return edge >= x && edge < x + width ? edge : -1;
    };
    if (const int edge = translatedSide(0, content.left(), true); edge >= 0) content.setLeft(edge);
    if (const int edge = translatedSide(content.right()+1, frame.width()-content.right()-1, false); edge >= 0) content.setRight(edge);
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
    if (!hdr.isNull()) m_hdrStrips.push_back({0, m_hdrInitial.copy(content)});
    if (!original.isNull())
      m_originalStrips.push_back({0, m_originalInitial.copy(content), content.y()});
  }
  int prependTo = m_layout.minimumY;
  int appendFrom = m_layout.maximumY + content.height();
  if (consensusMotion && added) {
    const int from = std::max(position, m_layout.minimumY);
    const int to = std::min(position + content.height(), appendFrom);
    QImage retained(content.width(), to - from, QImage::Format_ARGB32);
    for (const auto &strip : m_strips)
      for (int y = std::max(from, strip.y);
           y < std::min(to, strip.y + strip.image.height()); ++y)
        std::memcpy(retained.scanLine(y - from), strip.image.constScanLine(y - strip.y),
                    size_t(content.width()) * 4);
    const auto incoming = frame.copy(content.x(), content.y() + from - position,
                                      content.width(), to - from);
    const bool down = maxY > m_layout.maximumY;
    const int seam = from + coherentSeam(retained, incoming, down);
    const auto trim = [&](std::vector<Strip> &strips) {
      std::erase_if(strips, [&](const Strip &strip) {
        return down ? strip.y >= seam : strip.y + strip.image.height() <= seam;
      });
      for (auto &strip : strips) {
        if (down && strip.y + strip.image.height() > seam)
          strip.image = strip.image.copy(0, 0, content.width(), seam - strip.y);
        else if (!down && strip.y < seam) {
          const int cut = seam - strip.y;
          strip.image = strip.image.copy(0, cut, content.width(), strip.image.height() - cut);
          strip.sourceTop += cut;
          strip.y = seam;
        }
      }
    };
    trim(m_strips);
    trim(m_originalStrips);
    trim(m_hdrStrips);
    if (down) appendFrom = seam;
    else prependTo = seam;
  }
  // Native window corners are clipped in every source viewport. Once that
  // viewport edge becomes an interior seam, a later overlapping frame can
  // recover the unobscured pixels. Original alpha is the quality mask; an
  // decoration under a rounded corner additionally needs a source row away
  // from viewport edges. Other translucent pixels with unchanged alpha stay.
  if (!original.isNull()) {
    const auto incoming = original.convertToFormat(QImage::Format_ARGB32);
    for (size_t i=0; i<m_originalStrips.size(); ++i) {
      auto& raw = m_originalStrips[i];
      const int from = std::max(raw.y,position), to = std::min(raw.y+raw.image.height(),position+content.height());
      for (int documentY=from; documentY<to; ++documentY) {
        const int storedY=documentY-raw.y, sourceY=content.y()+documentY-position;
        const auto* sourceRaw=reinterpret_cast<const QRgb*>(incoming.constScanLine(sourceY))+content.x();
        const auto* storedRaw=reinterpret_cast<const QRgb*>(raw.image.constScanLine(storedY));
        for (int x=0;x<content.width();++x) {
          const int oldEdge=std::min(raw.sourceTop+storedY,frame.height()-1-raw.sourceTop-storedY);
          const int newEdge=std::min(sourceY,frame.height()-1-sourceY);
          const bool recoverCorner=oldEdge<64 && newEdge>=64 && qAlpha(sourceRaw[x])>=qAlpha(storedRaw[x]);
          if (!recoverCorner && qAlpha(sourceRaw[x])<=qAlpha(storedRaw[x])+2) continue;
          raw.image.setPixel(x,storedY,sourceRaw[x]);
          // setPixel may detach the original plane; refresh its read pointer.
          storedRaw=reinterpret_cast<const QRgb*>(raw.image.constScanLine(storedY));
          m_strips[i].image.setPixel(x,storedY,frame.pixel(content.x()+x,sourceY));
          if (!hdr.isNull())
            std::memcpy(m_hdrStrips[i].image.scanLine(storedY)+x*8,hdr.constScanLine(sourceY)+(content.x()+x)*8,8);
        }
      }
    }
  }
  if (minY < m_layout.minimumY)
    m_strips.push_back(
        {minY, frame.copy(content.x(), content.y(), content.width(),
                          prependTo - minY)});
  if (maxY > m_layout.maximumY) {
    const int count = position + content.height() - appendFrom;
    m_strips.push_back({appendFrom,
                        frame.copy(content.x(), content.bottom() + 1 - count,
                                   content.width(), count)});
  }
  // Retain uncomposited pixels using the same accepted alignment. This avoids
  // a second matcher and allows background changes after capture.
  if (!original.isNull()) {
    if (minY < m_layout.minimumY)
      m_originalStrips.push_back({minY, original.copy(content.x(), content.y(),
          content.width(), prependTo - minY).convertToFormat(QImage::Format_ARGB32), content.y()});
    if (maxY > m_layout.maximumY) {
      const int count = position + content.height() - appendFrom;
      m_originalStrips.push_back({appendFrom,
          original.copy(content.x(), content.bottom() + 1 - count,
                        content.width(), count).convertToFormat(QImage::Format_ARGB32), content.bottom()+1-count});
    }
  }
  if (!hdr.isNull()) {
    if (minY < m_layout.minimumY)
      m_hdrStrips.push_back({minY, hdr.copy(content.x(), content.y(), content.width(), prependTo - minY)});
    if (maxY > m_layout.maximumY) {
      const int count = position + content.height() - appendFrom;
      m_hdrStrips.push_back({appendFrom,
          hdr.copy(content.x(), content.bottom() + 1 - count, content.width(), count)});
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
void ScrollStitcher::paint(QPainter &p, int plane) const {
  const auto& initial = plane == 2 ? m_hdrInitial : plane == 1 && !m_originalInitial.isNull() ? m_originalInitial : m_initial;
  const auto& strips = plane == 2 ? m_hdrStrips : plane == 1 && !m_originalInitial.isNull() ? m_originalStrips : m_strips;
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
    if (plane == 2) {
      // QColor/8-bit gradients clip highlights. Interpolate premultiplied
      // floating-point samples directly, including alpha and out-of-gamut RGB.
      // A thumbnail only needs device-resolution samples of this gradient.
      // Full-size exports keep the original per-pixel interpolation unchanged.
      const auto device=p.worldTransform().mapRect(QRectF(x,seam,width,growth)).size();
      const int bandWidth=std::min(width,std::max(1,int(std::ceil(device.width()))));
      const int bandHeight=std::min(growth,std::max(1,int(std::ceil(device.height()))));
      QImage band(bandWidth,bandHeight,QImage::Format_RGBA16FPx4_Premultiplied);
      const auto* above = reinterpret_cast<const qfloat16*>(initial.constScanLine(seam - 1)) + x * 4;
      const auto* below = reinterpret_cast<const qfloat16*>(initial.constScanLine(seam)) + x * 4;
      for (int y = 0; y < bandHeight; ++y) {
        auto* row = reinterpret_cast<qfloat16*>(band.scanLine(y));
        const float t = (y + .5f) / bandHeight;
        for (int i=0;i<bandWidth;++i) {
          const int source=std::min(width-1,int((i+.5)*width/bandWidth))*4;
          for(int c=0;c<4;++c) row[i*4+c]=qfloat16(float(above[source+c])*(1-t)+float(below[source+c])*t);
        }
      }
      p.drawImage(QRect(x,seam,width,growth),band);
      return;
    }
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
QImage ScrollStitcher::hdrImage() const {
  if (m_hdrInitial.isNull()) return {};
  QImage result(size(), QImage::Format_RGBA16FPx4_Premultiplied);
  if (result.isNull()) return {};
  result.fill(Qt::transparent);
  QPainter painter(&result);
  paint(painter, 2);
  result.setColorSpace(m_hdrInitial.colorSpace());
  return result;
}
QImage ScrollStitcher::preview(const QSize &bounds, bool hdr) const {
  if (empty())
    return {};
  if (hdr && m_hdrInitial.isNull()) return {};
  QImage result(size().scaled(bounds, Qt::KeepAspectRatio),
                hdr ? QImage::Format_RGBA16FPx4_Premultiplied : QImage::Format_ARGB32);
  result.fill(Qt::transparent);
  QPainter p(&result);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.scale(qreal(result.width()) / size().width(),
          qreal(result.height()) / size().height());
  paint(p, hdr ? 2 : 0);
  return result;
}
} // namespace hyprcapture::ui
