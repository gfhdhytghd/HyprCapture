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
struct ColumnEvidence {
  bool stationary = false, changed = false, translating = false;
};
// Compare zero motion with the already registered document translation at
// native resolution. Flat/repeating pixels cannot vote for either layer.
// Require detail distributed vertically, rather than a clock/caret or one
// horizontal line. This also works for one-pixel precision-touchpad steps.
std::vector<ColumnEvidence> columnEvidence(const QImage &a, const QImage &b,
                                          QRect content, int d) {
  std::vector<ColumnEvidence> result(a.width());
  const int top = content.top() + std::max(0, -d),
            bottom = content.bottom() + 1 - std::max(0, d);
  if (bottom - top < 96) return result;
  const auto difference = [](QRgb x, QRgb y) {
    return std::max({std::abs(qRed(x) - qRed(y)),
                     std::abs(qGreen(x) - qGreen(y)),
                     std::abs(qBlue(x) - qBlue(y)),
                     std::abs(qAlpha(x) - qAlpha(y))});
  };
  struct Counts { int changed = 0, stationary = 0, translating = 0, first, last; };
  std::vector<Counts> counts(a.width(), {0, 0, 0, bottom, top});
  // Walk scanlines contiguously; touchpad captures run this for many frames.
  // At most 256 distributed scanlines: decisions need spatial support, not
  // a full image traversal. Retain native pixels for tiny touchpad movement.
  const int rowStep=std::max(1,(bottom-top+255)/256);
  for (int sample=0,begin=top;begin<bottom;begin+=rowStep,++sample) {
    // Rotate the phase so repeated rules/glyph edges cannot alias the stride.
    const int y=std::min(bottom-1,begin+sample%rowStep);
    const auto *current = reinterpret_cast<const QRgb *>(b.constScanLine(y));
    const auto *next = reinterpret_cast<const QRgb *>(b.constScanLine(std::min(y+1,b.height()-1)));
    const auto *screen = reinterpret_cast<const QRgb *>(a.constScanLine(y));
    const auto *translated = reinterpret_cast<const QRgb *>(a.constScanLine(y + d));
    for (int x = content.left(); x <= content.right(); ++x) {
      auto &count = counts[x];
      const int screenError = difference(current[x], screen[x]);
      const bool detail = difference(current[x],next[x]) > 8 ||
          difference(current[x],current[std::min(x+1,content.right())]) > 8;
      count.changed += detail && screenError > 6;
      count.translating += detail && screenError > 6 && difference(current[x], translated[x]) <= 3;
      if (detail && screenError <= 3 && difference(current[x], translated[x]) > 8) {
        ++count.stationary;
        count.first = std::min(count.first, y);
        count.last = y;
      }
    }
  }
  for (int x = content.left(); x <= content.right(); ++x) {
    const auto &count = counts[x];
    result[x] = {count.changed <= std::max(2, (bottom - top) / rowStep / 200) &&
                     count.stationary >= 8 && count.last - count.first >= (bottom - top) / 5,
                 count.changed > 2, count.translating >= 8 && count.translating >= count.changed * .9 && count.stationary < count.translating / 4};
  }
  return result;
}
// Verify foreground at native resolution after removing slow illumination.
// Cropping away newly opaque chrome must not turn the remaining translucent
// wallpaper tiles into false mismatches or matches for a repeated comment.
bool verifyBody(const QImage &a, const QImage &b, QRect rect, int d) {
  return verify(foregroundDetail(gray(a.copy(rect))),
                foregroundDetail(gray(b.copy(rect))),d,true)<1e8;
}
Match registerBody(const QImage &a, const QImage &b, QRect rect) {
  const auto first = compact(gray(a.copy(rect))), second = compact(gray(b.copy(rect)));
  auto raw = match(first, second);
  if (raw.found && exactOverlap(a, b, rect, raw.position)) return raw;
  auto detail = match(foregroundDetail(first), foregroundDetail(second), true);
  if (detail.found && verifyBody(a, b, rect, detail.position)) return detail;
  if (raw.found && verifyOriginalPixels(a, b, rect, raw.position) &&
      verifyBody(a, b, rect, raw.position)) return raw;
  return {0, false, raw.ambiguous || detail.ambiguous};
}
} // namespace
ScrollStitcher::ScrollStitcher() = default;
ScrollStitcher::ScrollStitcher(Limits limits) : m_limits(limits) {}
ScrollLayout ScrollStitcher::layout() const {
  auto result=m_layout;
  result.initialContent=m_layoutLocked?m_initialContent:m_layout.content;
  result.topOcclusion=m_topOcclusion;
  result.panels=m_releasedPanels;
  for (const auto &side:m_pinnedSides)
    result.panels.push_back({side.rect,side.documentY,side.sourceTop,
                            side.image[0].height(),side.seam,m_layout.maximumY,true});
  return result;
}
void ScrollStitcher::updateColumns(const QImage &frame, const QImage &original,
                                   const QImage &hdr, int displacement) {
  const auto old = m_layout.content;
  // Emerging top chrome can change every column once, including otherwise
  // blank document gutters. Keep edge chrome out of side-motion history even
  // before its row mask has accumulated enough stationary observations.
  const int inset = std::max(m_topOcclusion, std::min(64, old.height()/4));
  const auto evidenceRect = m_initialContent.adjusted(0, inset, 0, 0);
  const auto evidence = columnEvidence(m_previous, frame, evidenceRect, displacement);
  // Only forward growth can release a section panel. Revisiting old, pre-pin
  // captures must not reinterpret their motion as a new unsticky section.
  if (m_layout.viewportY + displacement > m_layout.maximumY) {
    QRect widened = old;
    std::vector<QRect> released;
    for (const auto &side : m_pinnedSides) {
      int translating = 0, changed = 0;
      for (int x=side.rect.left(); x<=side.rect.right(); ++x) {
        translating += evidence[x].translating;
        changed += evidence[x].changed;
      }
      if (translating >= 8 && translating >= changed * .8) {
        widened = widened.united(side.rect);
        released.push_back(side.rect);
        m_releasedPanels.push_back({side.rect,side.documentY,side.sourceTop,
                                   side.image[0].height(),side.seam,m_layout.maximumY,false});
      }
    }
    if (widened != old) {
      // Materialize the composed history before releasing its overlay. This
      // retains the prefix and single pinned panel in all native planes.
      const std::array<QImage,3> history{image(),originalImage(),hdrImage()};
      const std::array<QImage,3> incoming{frame,original.convertToFormat(QImage::Format_ARGB32),hdr};
      const std::array<std::vector<Strip>*,3> strips{&m_strips,&m_originalStrips,&m_hdrStrips};
      for (int plane=0;plane<3;++plane) {
        if (strips[plane]->empty()) continue;
        QImage body = history[plane].copy(widened.x(),old.y(),widened.width(),
                          old.height()+m_layout.maximumY-m_layout.minimumY);
        QPainter p(&body); p.setCompositionMode(QPainter::CompositionMode_Source);
        const int position=m_layout.viewportY+displacement;
        for (const auto &rect : released)
          p.drawImage(rect.x()-widened.x(),position-m_layout.minimumY,
                      incoming[plane].copy(rect));
        p.end();
        // Keep source-row provenance for native corner recovery. A single
        // document-height strip would mislabel interior rows as viewport edges.
        for (auto &strip : *strips[plane])
          strip.image=body.copy(0,strip.y-m_layout.minimumY,widened.width(),strip.image.height());
      }
      std::erase_if(m_pinnedSides,[&](const PinnedSide& side) {
        return std::find(released.begin(),released.end(),side.rect)!=released.end();
      });
      m_layout.content = widened;
      for (const auto &rect : released)
        for (int x=rect.left();x<=rect.right();++x) m_columns[x] = {};
      return;
    }
  }
  if (m_columns.empty()) m_columns.resize(frame.width());
  int firstMoving = old.right() + 1, lastMoving = old.left() - 1;
  for (int x = old.left(); x <= old.right(); ++x) {
    auto &column = m_columns[x];
    // Observed screen changes distinguish a newly pinned panel from an
    // always-stationary backdrop beneath scrolling translucent foreground.
    column.changeAge = evidence[x].changed ? 0 : std::min(65,column.changeAge+1);
    column.changed = column.changeAge <= std::max(6,64/std::max(1,std::abs(displacement)));
    column.translated |= evidence[x].translating;
    if (evidence[x].stationary) {
      if (!column.stationaryFrames) column.stationaryStart = m_layout.viewportY;
      ++column.stationaryFrames;
      column.stationaryTravel += std::abs(displacement);
    } else {
      column.stationaryFrames = column.stationaryTravel = 0;
    }
  }
  // A thin moving scrollbar beside a pinned panel is not a scrolling
  // document column. Require local horizontal support before treating motion
  // as a barrier; preserve the actual classified edge and its blank gutter.
  for (int x = old.left(); x <= old.right(); ++x) {
    if (!evidence[x].changed) continue;
    int support = 0;
    for (int i = std::max(old.left(), x-16); i <= std::min(old.right(), x+16); ++i)
      support += evidence[i].changed;
    if (support >= 8) {
      firstMoving = std::min(firstMoving, x);
      lastMoving = x;
    }
  }
  if (firstMoving > lastMoving || lastMoving - firstMoving + 1 < 64) return;
  cv::Mat frameGray;
  const auto pinned = [&](int from, int to, bool left) {
    int votes = 0, edge = left ? from - 1 : to;
    for (int x = from; x < to; ++x) {
      const auto &column = m_columns[x];
      if (!(column.changed && column.stationaryFrames >= 3 && column.stationaryTravel >= 6)) continue;
      if (!column.translated) {
        if (frameGray.empty()) frameGray=gray(frame);
        if (texturedBackdrop(frameGray,QRect(std::max(from,std::min(x,to-4)),old.y()+inset,std::min(4,to-from),old.height()-inset))) continue;
      }
      {
        ++votes;
        edge = left ? std::max(edge,x) : std::min(edge,x);
      }
    }
    return votes >= std::max(8, frame.width() / 100) ? edge : -1;
  };
  auto content = old;
  // Unknown blank gutters remain document pixels. A gap between text and a
  // pinned panel must not become part of the panel merely for lacking motion.
  if (const int edge = pinned(old.left(), firstMoving, true); edge >= 0)
    content.setLeft(edge + 1);
  if (const int edge = pinned(lastMoving + 1, old.right() + 1, false); edge >= 0)
    content.setRight(edge - 1);
  if (content == old) return;

  const std::array<QImage, 3> incoming{
      frame, original.convertToFormat(QImage::Format_ARGB32), hdr};
  const auto remember = [&](QRect newlyPinned) {
    PinnedSide side;
    side.rect = newlyPinned;
    side.prefixY = m_layout.minimumY;
    int start = m_layout.viewportY;
    for (int x = newlyPinned.left(); x <= newlyPinned.right(); ++x)
      if (m_columns[x].stationaryFrames >= 3)
        start = std::min(start, m_columns[x].stationaryStart);
    const std::array<const std::vector<Strip> *, 3> planes{
        &m_strips, &m_originalStrips, &m_hdrStrips};
    // Register the pinned viewport against observed pre-pin sidebar history.
    // Its scroll offset is independent of the main column after it pins (a
    // wheel step may even cross the threshold part-way). Keep the prefix.
    const int historyHeight = old.height() + std::max(0, start - m_layout.minimumY);
    std::array<QImage, 3> history;
    for (int plane = 0; plane < 3; ++plane) {
      if (incoming[plane].isNull()) continue;
      history[plane] = QImage(newlyPinned.width(), historyHeight, incoming[plane].format());
      history[plane].fill(Qt::transparent);
      QPainter painter(&history[plane]);
      painter.setCompositionMode(QPainter::CompositionMode_Source);
      for (const auto &strip : *planes[plane])
        painter.drawImage(0, strip.y - m_layout.minimumY,
            strip.image.copy(newlyPinned.x() - old.x(), 0,
                             newlyPinned.width(), strip.image.height()));
      side.image[plane] = incoming[plane].copy(newlyPinned);
    }
    side.sourceTop = m_topOcclusion;
    for (auto &image : side.image)
      if (!image.isNull() && side.sourceTop)
        image = image.copy(0,side.sourceTop,image.width(),image.height()-side.sourceTop);
    const auto visible = compact(gray(side.image[0]));
    // Prefer the first viewport: late history can already contain a few
    // duplicated bottom slices while the stationary vote is accumulating.
    const auto initialSide = m_initial.copy(newlyPinned);
    auto baseline = compact(gray(initialSide));
    std::vector<int> exactPositions;
    for (int d = std::max(0, m_layout.minimumY)+side.sourceTop; d <= std::min(start, old.height()*3/4)+side.sourceTop; ++d)
      if (exactOverlap(initialSide, side.image[0], initialSide.rect(), d)) exactPositions.push_back(d);
    auto alignment = exactPositions.size() == 1 ? Match{exactPositions.front(),true,false}
        : match(foregroundDetail(baseline), foregroundDetail(visible), true);
    if (!alignment.found) alignment = match(baseline, visible);
    bool initialAnchor = alignment.found;
    if (!alignment.found) {
      baseline = compact(gray(history[0]));
      alignment = match(foregroundDetail(baseline), foregroundDetail(visible), true);
      if (!alignment.found) alignment = match(baseline, visible);
    }
    const int documentY = (initialAnchor ? 0 : m_layout.minimumY) + alignment.position - side.sourceTop;
    if (alignment.found && documentY >= m_layout.minimumY && documentY <= start) {
      side.documentY = documentY;
      // Recover occluded top rows from their earlier, unobstructed captures.
      const int prefixHeight = documentY + side.sourceTop - m_layout.minimumY;
      for (int plane = 0; plane < 3; ++plane)
        if (!history[plane].isNull()) {
          if (prefixHeight)
            side.prefix[plane] = history[plane].copy(0, 0, newlyPinned.width(), prefixHeight);
          // A top bar can settle later than this sidebar. Keep only the
          // earlier unobstructed rows it could subsequently occlude.
          side.underlay[plane] = history[plane].copy(0,documentY-m_layout.minimumY,
              newlyPinned.width(),old.height()/4);
        }
    } else {
      // Missing/ambiguous side motion must never discard the captured top
      // items. Keep the earliest complete observed sidebar as a safe source.
      side.documentY = side.sourceTop = 0;
      // The initial viewport starts at zero; negative captured rows precede it.
      for (int plane=0;plane<3;++plane)
        if (!history[plane].isNull() && m_layout.minimumY<0)
          side.prefix[plane]=history[plane].copy(0,0,newlyPinned.width(),-m_layout.minimumY);
      const std::array<QImage, 3> initial{m_initial, m_originalInitial, m_hdrInitial};
      for (int plane = 0; plane < 3; ++plane)
        if (!initial[plane].isNull()) side.image[plane] = initial[plane].copy(newlyPinned);
    }
    side.seam = sidebarSeam(gray(side.image[0]), side.image[0].rect());
    if (side.seam < 0) {
      // No quiet band: extend background after the complete panel, not labels.
      for (int plane = 0; plane < 3; ++plane)
        if (!side.image[plane].isNull())
          side.fill[plane] = QImage(newlyPinned.width(), 1, side.image[plane].format());
      for (int x = 0; x < newlyPinned.width(); ++x) {
        std::map<QRgb, int> colors;
        int count = 0, sample = 0;
        for (int y = 0; y < side.image[0].height(); ++y) {
          auto &color = colors[side.image[0].pixel(x, y)];
          if (++color > count) { count = color; sample = y; }
        }
        for (int plane = 0; plane < 3; ++plane) {
          if (side.image[plane].isNull()) continue;
          const int bytes = plane == 2 ? 8 : 4;
          std::memcpy(side.fill[plane].scanLine(0) + x * bytes,
                      side.image[plane].constScanLine(sample) + x * bytes, bytes);
        }
      }
    }
    m_pinnedSides.push_back(std::move(side));
  };
  if (content.left() != old.left())
    remember({old.left(), old.y(), content.left() - old.left(), old.height()});
  if (content.right() != old.right())
    remember({content.right() + 1, old.y(), old.right() - content.right(), old.height()});
  // Remove even the pre-pin slices from history. All planes keep identical
  // strip coordinates/heights/source rows, including native corner recovery.
  const auto narrow = [&](std::vector<Strip> &strips) {
    for (auto &strip : strips)
      strip.image = strip.image.copy(content.x() - old.x(), 0,
                                      content.width(), strip.image.height());
  };
  narrow(m_strips);
  narrow(m_originalStrips);
  narrow(m_hdrStrips);
  m_layout.content = content;
}
void ScrollStitcher::updateTop(const QImage &frame, int d) {
  const auto c = m_layout.content;
  if (d > 0 && m_topOcclusion && d < c.height()-m_topOcclusion) {
    int moving=0, translated=0;
    for(int y=c.top();y<c.top()+m_topOcclusion;++y) {
      const auto *a=reinterpret_cast<const QRgb*>(m_previous.constScanLine(y));
      const auto *shifted=reinterpret_cast<const QRgb*>(m_previous.constScanLine(y+d));
      const auto *b=reinterpret_cast<const QRgb*>(frame.constScanLine(y));
      for(int x=c.left();x<=c.right();++x) {
        const auto error=[](QRgb a,QRgb b) {return std::max({std::abs(qRed(a)-qRed(b)),std::abs(qGreen(a)-qGreen(b)),std::abs(qBlue(a)-qBlue(b))});};
        moving += error(a[x],b[x])>6;
        translated += error(a[x],b[x])>6 && error(shifted[x],b[x])<=3;
      }
    }
    if(moving>c.width()*m_topOcclusion/20 && translated>=moving*.6) {
      m_topOcclusion=0;
      for(auto& row:m_rows) row={};
    }
  }
  if (d < 0 && m_topOcclusion && m_layout.viewportY + d < m_topPinnedAt) {
    int changed = 0;
    for (int y=c.top();y<c.top()+m_topOcclusion;++y) {
      const auto *a=reinterpret_cast<const QRgb *>(m_previous.constScanLine(y));
      const auto *b=reinterpret_cast<const QRgb *>(frame.constScanLine(y));
      for (int x=c.left();x<=c.right();++x)
        changed += std::max({std::abs(qRed(a[x])-qRed(b[x])),std::abs(qGreen(a[x])-qGreen(b[x])),
                             std::abs(qBlue(a[x])-qBlue(b[x])),std::abs(qAlpha(a[x])-qAlpha(b[x]))})>6;
    }
    // The full title has re-entered the page; previously hidden top rows are
    // visible document data again, including prepends above the first capture.
    if (changed > c.width()*m_topOcclusion/20) {
      m_topOcclusion=0;
      for (auto &row:m_rows) row.stationaryFrames=row.stationaryTravel=0;
    }
  }
  if (d <= 0) return;
  if (m_rows.empty()) m_rows.resize(frame.height());
  const int columnStep=std::max(1,(c.width()+511)/512);
  const int samples=(c.width()+columnStep-1)/columnStep;
  const int bottom = std::min(c.top() + c.height() / 4, c.bottom() + 1 - d);
  int fixedRows = 0, lastFixed = c.top() - 1, firstMoving = bottom;
  for (int y = c.top(); y < bottom; ++y) {
    const auto *a = reinterpret_cast<const QRgb *>(m_previous.constScanLine(y));
    const auto *shifted = reinterpret_cast<const QRgb *>(m_previous.constScanLine(y + d));
    const auto *b = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
    int changed = 0, stationary = 0, translating = 0, first = c.right(), last = c.left();
    const auto error = [](QRgb a, QRgb b) {
      return std::max({std::abs(qRed(a)-qRed(b)), std::abs(qGreen(a)-qGreen(b)),
                       std::abs(qBlue(a)-qBlue(b)), std::abs(qAlpha(a)-qAlpha(b))});
    };
    for (int sample=0,begin=c.left();begin<=c.right();begin+=columnStep,++sample) {
      const int x=std::min(c.right(),begin+sample%columnStep);
      const int screenError = error(a[x], b[x]);
      changed += screenError > 6;
      // Smooth wallpaper/illumination is not stationary *document* detail.
      const bool detail = error(b[x], b[std::min(x+1,c.right())]) > 8;
      translating += detail && screenError>6 && error(shifted[x],b[x])<=3;
      if (detail && screenError <= 3 && error(shifted[x], b[x]) > 8) {
        ++stationary; first = std::min(first, x); last = x;
      }
    }
    auto &row = m_rows[y];
    row.changeAge=changed>std::max(2,samples/100)?0:std::min(65,row.changeAge+1);
    row.changed=row.changeAge<=std::max(6,64/d);
    row.translated |= translating>=8 && translating>=changed*.9;
    if (changed <= std::max(2, samples / 100) && stationary >= 8 &&
        last - first >= c.width() / 3) {
      if (!row.stationaryFrames) row.stationaryStart=m_layout.viewportY;
      ++row.stationaryFrames; row.stationaryTravel += d;
    } else row.stationaryFrames = row.stationaryTravel = 0;
    if (changed > std::max(2, samples / 100) && y >= c.top() + m_topOcclusion)
      firstMoving = std::min(firstMoving, y);
    if (y < firstMoving && row.changed && row.stationaryFrames >= 3 && row.stationaryTravel >= 6 &&
        (row.translated || stationary<samples*.7)) {
      ++fixedRows; lastFixed = y;
    }
    // A moving body row is a barrier: an interior stationary widget cannot
    // turn the preceding scrolling document into header chrome.
    // Still update lower rows' history, so a subsequently appearing header
    // can be identified as a change from formerly scrolling pixels.
  }
  if (fixedRows >= 8 && lastFixed - c.top() >= 12) {
    if (!m_topOcclusion) m_topPinnedAt=m_rows[lastFixed].stationaryStart;
    // Include quiet padding/background beneath the stationary glyphs, up to
    // the first positively moving body row. Otherwise opaque header padding
    // would still be copied into a pinned sidebar's source/native planes.
    m_topOcclusion = std::max(m_topOcclusion, firstMoving - c.top());
    for (auto &side : m_pinnedSides) {
      // Revisit already selected sources too: the side panel may have pinned
      // while the top bar was still animating and had not earned a row mask.
      if (side.underlay[0].isNull()) continue;
      const int top=std::min(m_topOcclusion,side.underlay[0].height());
      const int extra=top-side.sourceTop;
      if (extra<=0) continue;
      for(int plane=0;plane<3;++plane) {
        if(side.image[plane].isNull()) continue;
        QImage prefix(side.rect.width(),side.prefix[plane].height()+extra,side.image[plane].format());
        prefix.fill(Qt::transparent);
        QPainter painter(&prefix);painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(0,0,side.prefix[plane]);
        painter.drawImage(0,side.prefix[plane].height(),side.underlay[plane].copy(
            0,side.sourceTop,side.rect.width(),extra));
        painter.end();
        side.prefix[plane]=std::move(prefix);
        side.image[plane]=side.image[plane].copy(0,extra,side.rect.width(),side.image[plane].height()-extra);
      }
      if(side.seam>=0) side.seam-=extra;
      side.sourceTop=top;
    }
  }
}
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
ScrollStitcher::Result ScrollStitcher::append(const QImage &source, const QImage &sourceOriginal, const QImage &sourceHdr) {
  QImage original=sourceOriginal, hdr=sourceHdr;
  original.setDevicePixelRatio(1); hdr.setDevicePixelRatio(1);
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
  auto frame = source.convertToFormat(QImage::Format_ARGB32);
  frame.setDevicePixelRatio(1);
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
      m_layoutLocked ? m_layout.content.adjusted(0, m_topOcclusion, 0, 0) : detectContent(previous, current);
  if (content.width() < 64 || content.height() < 96)
    return {Status::NoOverlap};
  const auto previousCompact = compact(crop(previous, content));
  const auto currentCompact = compact(crop(current, content));
  const auto rawAlignment = match(previousCompact, currentCompact);
  auto alignment = rawAlignment;
  bool consensusMotion = false;
  bool sparseMotion = false;
  // Opaque documents usually have a byte-identical overlap. This stronger,
  // inexpensive check avoids filtering full-resolution frames on that path.
  const bool smallInferredContent = !m_layoutLocked &&
      (content.width() < frame.width() * .6 || content.height() < frame.height() * .6);
  const bool exactMotion = alignment.found &&
      exactOverlap(m_previous, frame, content, alignment.position);
  if (!exactMotion) {
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
          texturedFrames(crop(previous, content), crop(current, content))) {
        alignment = matchForeground(previousCompact, currentCompact);
        sparseMotion = alignment.found;
      }
      if (!alignment.found)
        alignment.ambiguous |= detailAmbiguous;
    }
  }
  // When chrome appears mid-capture, registration's old rectangle still
  // includes it. Retry only the broadly changing body. Unlike byte-consensus,
  // this verifies native foreground detail as well as template alignment, and never
  // overrides a successfully verified foreground alignment.
  if (!alignment.found || alignment.position == 0 || sparseMotion) {
    const auto body = detectContent(previous, current).intersected(content);
    if (body != content && body.width() >= std::max(64, content.width() / 3) &&
        body.height() >= std::max(96, content.height() * 3 / 5)) {
      const bool sparseRejected = sparseMotion &&
          !verifyOriginalPixels(m_previous, frame, body, alignment.position, true);
      if (!alignment.found || alignment.position == 0 || sparseRejected) {
        const auto registration = registerBody(m_previous, frame, body);
        if (registration.found && registration.position != 0) {
          alignment = registration;
          sparseMotion = false;
        } else if (sparseRejected) alignment.found = false;
      }
    }
  }
  if (!alignment.found || alignment.position == 0 || sparseMotion) {
    // The very first frame of newly appearing chrome is not stationary yet.
    // Two broad interior bodies exclude top chrome and one side edge. Require
    // independently verified translations; conflicting bodies remain ambiguous.
    std::set<int> translations;
    for (const auto rect : {content.adjusted(0, content.height()/4, -content.width()/3, 0),
                            content.adjusted(content.width()/3, content.height()/4, 0, 0)}) {
      if (rect.width() < 64 || rect.height() < 96) continue;
      const auto candidate = registerBody(m_previous, frame, rect);
      if (candidate.found && candidate.position) translations.insert(candidate.position);
    }
    if (translations.size() == 1) alignment = {*translations.begin(), true, false};
    else if (translations.size() > 1) alignment = {0, false, true};
  }
  if (!alignment.found || alignment.position == 0 || smallInferredContent) {
    // Preserve master's foreground-registration precedence. Native consensus
    // is a fallback; it cannot override found translucent detail alignment.
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
    position = m_layout.minimumY + alignment.position - m_topOcclusion;
    // Relocation may only land wholly within retained history. A lost gap
    // must not create an unobserved interval in the output.
    if (position + m_topOcclusion < m_layout.minimumY || position > m_layout.maximumY)
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
  int minY = std::min(m_layout.minimumY, position + m_topOcclusion);
  const int maxY = std::max(m_layout.maximumY, position);
  int added = (maxY - minY) - (m_layout.maximumY - m_layout.minimumY);
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
    m_initialContent = content;
    m_layout.leftSeam = left;
    m_layout.rightSeam = right;
    m_layoutLocked = true;
    m_strips.push_back({0, m_initial.copy(content)});
    if (!hdr.isNull()) m_hdrStrips.push_back({0, m_hdrInitial.copy(content)});
    if (!original.isNull())
      m_originalStrips.push_back({0, m_originalInitial.copy(content), content.y()});
  }
  // Only accepted, overlapping motion contributes evidence. Rejects, idle
  // frames and lost-overlap history jumps cannot change sidebar sources.
  const int displacement = position - m_layout.viewportY;
  content = m_layout.content;
  if (displacement && std::abs(displacement) <= content.height() - 96) {
    updateTop(frame, displacement);
    updateColumns(frame, original, hdr, displacement);
    content = m_layout.content;
  }
  minY = std::min(m_layout.minimumY, position + m_topOcclusion);
  added = (maxY - minY) - (m_layout.maximumY - m_layout.minimumY);
  if (minY < m_layout.minimumY) {
    const std::array<QImage,3> incoming{frame,original.convertToFormat(QImage::Format_ARGB32),hdr};
    for (auto &side:m_pinnedSides) {
      const int count=side.prefixY-minY;
      if (count<=0) continue;
      for (int plane=0;plane<3;++plane) {
        if (incoming[plane].isNull()) continue;
        QImage prefix(side.rect.width(),count+side.prefix[plane].height(),incoming[plane].format());
        prefix.fill(Qt::transparent);
        QPainter painter(&prefix);painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(0,0,incoming[plane].copy(side.rect.x(),content.y()+minY-position,side.rect.width(),count));
        painter.drawImage(0,count,side.prefix[plane]);
        side.prefix[plane]=std::move(prefix);
      }
      side.prefixY=minY;
    }
  }
  int prependTo = m_layout.minimumY;
  int appendFrom = m_layout.maximumY + content.height();
  if (consensusMotion && added) {
    const int from = std::max(position + m_topOcclusion, m_layout.minimumY);
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
        if (sourceY < content.y() + m_topOcclusion) continue;
        const auto* sourceRaw=reinterpret_cast<const QRgb*>(incoming.constScanLine(sourceY))+content.x();
        const auto* storedRaw=reinterpret_cast<const QRgb*>(raw.image.constScanLine(storedY));
        for (int x=0;x<content.width();++x) {
          const int oldEdge=std::min(raw.sourceTop+storedY,frame.height()-1-raw.sourceTop-storedY);
          const int newEdge=std::min(sourceY,frame.height()-1-sourceY);
          const bool recoverCorner=oldEdge<64 && newEdge>=64 && qAlpha(sourceRaw[x])>=qAlpha(storedRaw[x]);
          if (!recoverCorner) {
            if (qAlpha(sourceRaw[x])<=qAlpha(storedRaw[x])+2) continue;
            // Byte-exact document motion proves that an edge sample belongs
            // to the page too. For uncertain translucent alignment, retain
            // the edge guard until stationary chrome has earned its mask.
            if (!exactMotion && (newEdge<64 || sourceY<content.y()+std::max(64,content.height()/4))) continue;
          }
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
        {minY, frame.copy(content.x(), content.y() + minY - position, content.width(),
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
      m_originalStrips.push_back({minY, original.copy(content.x(), content.y() + minY - position,
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
      m_hdrStrips.push_back({minY, hdr.copy(content.x(), content.y() + minY - position, content.width(), prependTo - minY)});
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
  const int channel = plane == 1 && m_originalInitial.isNull() ? 0 : plane;
  const auto &initial = channel == 2 ? m_hdrInitial : channel == 1 ? m_originalInitial : m_initial;
  const auto &strips = channel == 2 ? m_hdrStrips : channel == 1 ? m_originalStrips : m_strips;
  if (!m_layoutLocked) { p.drawImage(0, 0, initial); return; }
  const auto c = m_layout.content;
  const int growth = m_layout.maximumY - m_layout.minimumY;
  const auto expand = [&](const QImage &source, QRect rect, int x, int y,
                          int extra, int seam, bool tiled, const QImage &fill = QImage{}) {
    if (rect.isEmpty()) return;
    const int width = rect.width(), height = rect.height();
    if (seam < 0) {
      p.drawImage(QRect(x, y, width, height), source, rect);
      if (extra <= 0) return;
      if (!fill.isNull()) {
        p.drawImage(QRect(x, y + height, width, extra), fill);
      } else if (tiled) {
        const int tile = std::min(64, height);
        for (int i = 0; i < extra; i += tile)
          p.drawImage(QRect(x, y + height + i, width, std::min(tile, extra-i)), source,
                      QRect(rect.x(), rect.bottom()+1-tile, width, std::min(tile, extra-i)));
      }
      return;
    }
    p.drawImage(QRect(x, y, width, seam), source,
                QRect(rect.x(), rect.y(), width, seam));
    p.drawImage(QRect(x, y + seam + extra, width, height - seam), source,
                QRect(rect.x(), rect.y() + seam, width, height - seam));
    if (extra <= 0) return;
    if (channel == 2) {
      const auto device = p.worldTransform().mapRect(QRectF(x,y+seam,width,extra)).size();
      const int bandWidth = std::min(width,std::max(1,int(std::ceil(device.width()))));
      const int bandHeight = std::min(extra,std::max(1,int(std::ceil(device.height()))));
      QImage band(bandWidth,bandHeight,QImage::Format_RGBA16FPx4_Premultiplied);
      const auto *above = reinterpret_cast<const qfloat16 *>(source.constScanLine(rect.y()+seam-1)) + rect.x()*4;
      const auto *below = reinterpret_cast<const qfloat16 *>(source.constScanLine(rect.y()+seam)) + rect.x()*4;
      for (int row = 0; row < bandHeight; ++row) {
        auto *out = reinterpret_cast<qfloat16 *>(band.scanLine(row));
        const float t = (row+.5f)/bandHeight;
        for (int i = 0; i < bandWidth; ++i) {
          const int sample = std::min(width-1,int((i+.5)*width/bandWidth))*4;
          for (int component = 0; component < 4; ++component)
            out[i*4+component] = qfloat16(float(above[sample+component])*(1-t)+float(below[sample+component])*t);
        }
      }
      p.drawImage(QRect(x,y+seam,width,extra),band);
    } else {
      for (int i = 0; i < width; ++i) {
        QLinearGradient gradient(0,y+seam,0,y+seam+extra);
        gradient.setColorAt(0,source.pixelColor(rect.x()+i,rect.y()+seam-1));
        gradient.setColorAt(1,source.pixelColor(rect.x()+i,rect.y()+seam));
        p.fillRect(QRect(x+i,y+seam,1,extra),gradient);
      }
    }
  };
  // Initial fixed sidebars retain their original geometry and sources.
  const auto original = m_initialContent;
  expand(initial, {0,c.y(),original.x(),c.height()}, 0,c.y(),growth,
         m_layout.leftSeam < 0 ? -1 : m_layout.leftSeam-c.y(), m_layout.leftTiled);
  expand(initial, {original.right()+1,c.y(),initial.width()-original.right()-1,c.height()},
         original.right()+1,c.y(),growth,
         m_layout.rightSeam < 0 ? -1 : m_layout.rightSeam-c.y(), m_layout.rightTiled);
  for (const auto &side : m_pinnedSides) {
    // Pre-pin document rows + one pinned viewport; expand only the background
    // below/between them. No Assignees/Labels/Projects rows are discarded.
    p.drawImage(side.rect.x(),c.y()+side.prefixY-m_layout.minimumY,side.prefix[channel]);
    expand(side.image[channel],side.image[channel].rect(),side.rect.x(),
           c.y()+side.documentY+side.sourceTop-m_layout.minimumY,
           m_layout.maximumY-side.documentY,side.seam,false,side.fill[channel]);
  }
  p.drawImage(QRect(0,0,initial.width(),c.y()),initial,QRect(0,0,initial.width(),c.y()));
  const int footer = initial.height()-c.bottom()-1;
  p.drawImage(QRect(0,size().height()-footer,initial.width(),footer),initial,
              QRect(0,c.bottom()+1,initial.width(),footer));
  for (const auto &strip : strips)
    p.drawImage(c.x(),c.y()+strip.y-m_layout.minimumY,strip.image);
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
