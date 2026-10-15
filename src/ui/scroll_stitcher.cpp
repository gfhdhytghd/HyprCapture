#include "ui/scroll_stitcher.hpp"
#include <qfloat16.h>
#include <QColorSpace>
#include <QLinearGradient>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <QByteArray>
#include <QDebug>
#include <chrono>
#include <limits>
#include <map>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <set>

namespace hyprcapture::ui {
namespace {
class TrackerTiming {
public:
  explicit TrackerTiming(const char *stage):m_stage(stage),m_start(std::chrono::steady_clock::now()),
      m_enabled(qEnvironmentVariableIsSet("HYPRCAPTURE_TIMING_FILE")) {}
  ~TrackerTiming() {if(m_enabled) qInfo("scroll.stage name=%s elapsed_us=%lld",m_stage,
      static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-m_start).count()));}
private:
  const char *m_stage;
  std::chrono::steady_clock::time_point m_start;
  bool m_enabled;
};
int quietMarginRow(const QImage &image, QRect body, QRect object) {
  std::map<QByteArray,int> rows;
  int best=0, result=body.top();
  const int bytes=image.depth()/8;
  for(int y=body.top()+16;y<body.bottom()-16;y+=8) {
    if(y>=object.top() && y<=object.bottom()) continue;
    const QByteArray key(reinterpret_cast<const char*>(image.constScanLine(y)+object.left()*bytes),object.width()*bytes);
    auto &entry=rows[key];
    if(++entry>best) {best=entry;result=y;}
  }
  return result;
}
cv::Mat gray(const QImage &input) {
  auto image = input.convertToFormat(QImage::Format_RGBA8888);
  cv::Mat result;
  cv::cvtColor(cv::Mat(image.height(), image.width(), CV_8UC4, image.bits(),
                       image.bytesPerLine()),
               result, cv::COLOR_RGBA2GRAY);
  return result;
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
  if (bottom - top < 64)
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
  double confidence = 0;
  bool priorUsed = false;
  int nativeFirst=-1,nativeLast=-1;
};
// Only break a pixel tie when exactly one supported peak lies inside a
// calibrated prediction interval. A strong conflict or two plausible periods
// remains ambiguous; input cannot invent a correspondence.
Match selectPeaks(const std::vector<std::pair<double, int>> &peaks,
                  const ScrollPrediction &prior, double confidence) {
  if (peaks.empty())
    return {};
  if (peaks.size() == 1)
    return {peaks.front().second, true, false, confidence};
  if (prior.valid && prior.resolvesTies) {
    std::optional<int> chosen;
    for (const auto &[score, d] : peaks)
      if (std::abs(d - prior.offset) <= prior.tolerance) {
        if (chosen && std::abs(*chosen - d) > 2)
          return {0, false, true};
        chosen = d;
      }
    if (chosen)
      return {*chosen, true, false, confidence, true};
  }
  return {0, false, true};
}
// Native row-gradient signatures examine the complete vertical search range
// and every horizontal pixel. They preserve distinguishing glyphs even on a
// very wide page. Blank/constant rows do not vote. Hash hits are verified.
Match nativeRows(const cv::Mat &page, const cv::Mat &frame,
                 const ScrollPrediction &prior, bool edgeOcclusion = false) {
  struct Bucket {std::vector<int> positions;bool consistent=true;};
  std::map<uint64_t, Bucket> rows;
  const auto sameGradients=[](const cv::Mat &a,int ay,const cv::Mat &b,int by) {
    const auto *x=a.ptr<uchar>(ay),*y=b.ptr<uchar>(by);
    const int light=int(x[0])-int(y[0]);
    for(int i=0;i<a.cols;++i) if(int(x[i])-int(y[i])!=light) return false;
    return true;
  };
  const auto signature = [](const cv::Mat &image, int y) {
    uint64_t hash = 14695981039346656037ULL;
    int low = 255, high = 0, previous = image.at<uchar>(y, 0);
    for (int x = 0; x < image.cols; ++x) {
      const int v = image.at<uchar>(y, x);
      low = std::min(low, v);
      high = std::max(high, v);
      hash = (hash ^ uint64_t(v - previous + 255)) * 1099511628211ULL;
      previous = v;
    }
    return std::pair{hash, high - low > 8};
  };
  for (int y = 0; y < page.rows; ++y) {
    auto [hash, detail] = signature(page, y);
    if (detail) {
      auto &bucket=rows[hash];
      // Verify repeated rows once against their canonical row. A rare hash
      // collision retains per-hit native checks; repeated text is not O(H²W).
      if(bucket.consistent && !bucket.positions.empty() &&
         !sameGradients(page,bucket.positions.front(),page,y)) bucket.consistent=false;
      bucket.positions.push_back(y);
    }
  }
  struct Support {
    int count = 0, first = INT_MAX, last = 0;
  };
  std::vector<Support> support(page.rows+frame.rows);
  std::vector<int> informative(frame.rows + 1);
  for (int y = 0; y < frame.rows; ++y) {
    const auto [hash, detail] = signature(frame, y);
    const auto it = rows.find(hash);
    const bool repetitive=it!=rows.end() && it->second.positions.size()>size_t(std::max(8,page.rows/4));
    // Repeated border-only rows carry no offset information. Every remaining
    // informative row must agree, including a single differing text ID.
    informative[y + 1] = informative[y] + (detail && !repetitive);
    if (!detail || repetitive || it == rows.end()) continue;
    const auto &bucket=it->second;
    if(bucket.consistent && !sameGradients(page,bucket.positions.front(),frame,y)) continue;
    for (int from : bucket.positions) {
      const int d = from - y;
      if (std::min(frame.rows, page.rows - d) - std::max(0, -d) < 64)
        continue;
      if(!bucket.consistent && !sameGradients(page,from,frame,y)) continue;
      auto &vote = support[d+frame.rows];
      ++vote.count;
      vote.first = std::min(vote.first, y);
      vote.last = std::max(vote.last, y);
    }
  }
  std::vector<std::pair<double, int>> peaks;
  for (int d=1-frame.rows;d<page.rows;++d) {
    const auto &v=support[d+frame.rows];
    if(!v.count) continue;
    int start = std::max(0, -d), end = std::min(frame.rows, page.rows - d);
    const int overlap = end - start;
    if (edgeOcclusion) {
      if (v.first - start > frame.rows / 3 ||
          end - 1 - v.last > frame.rows / 3 ||
          v.last - v.first + 16 < overlap * .60)
        continue;
      start = v.first;
      end = v.last + 1;
    }
    const int total = informative[end] - informative[start];
    if (v.count < 8 || v.last - v.first < 48 || !total || v.count != total)
      continue;
    if(overlap<std::max(96,frame.rows/4) &&
       !(prior.valid && prior.resolvesTies && std::abs(d-prior.offset)<=prior.tolerance)) {
      // An exact repeated card heading is still a weak position anchor when
      // it is nearly the entire overlap. Short native overlaps need dense
      // vertical information (photos/noise/barcodes), not sparse text chrome.
      const int first=std::max(0,-d),last=std::min(frame.rows,page.rows-d);
      bool rich=false;
      for(int x=0;x<frame.cols && !rich;++x) {
        int changes=0;
        for(int y=first+1;y<last;++y) {
          changes+=std::abs(int(frame.at<uchar>(y,x))-int(frame.at<uchar>(y-1,x)))>8;
          if(changes+last-y-1<(last-first-1)*.8) break;
        }
        rich=changes>=(last-first-1)*.8;
      }
      if(!rich) continue;
    }
    peaks.emplace_back(1. - double(v.count) / total, d);
  }
  if (peaks.empty())
    return {};
  std::sort(peaks.begin(), peaks.end());
  std::erase_if(peaks, [&](const auto &p) {
    return p.first > peaks.front().first + .001;
  });
  auto result = selectPeaks(peaks, prior, 1. - peaks.front().first);
  if (result.found) {
    result.nativeFirst = support[result.position+frame.rows].first;
    result.nativeLast = support[result.position+frame.rows].last;
  }
  return result;
}
// Independent templates vote for a *vertical translation*, not an affine
// warp. Like multi-template screenshot stitchers, cap each spatial cell at
// one vote. A detailed GIF cannot buy more votes by containing more corners.
// Native RGBA checks keep downsampling and repeated text from inventing a
// correspondence. Stationary cells form a separate zero-motion layer.
Match matchConsensus(const QImage &first, const QImage &second, QRect content,
                     bool history = false, const ScrollPrediction& prior = {}) {
  const auto a = gray(history ? first : first.copy(content));
  const auto b = gray(second.copy(content));
  cv::Mat aa(first.height(), first.width(), CV_8UC4,
             const_cast<uchar *>(first.constBits()), first.bytesPerLine());
  cv::Mat bb(second.height(), second.width(), CV_8UC4,
             const_cast<uchar *>(second.constBits()), second.bytesPerLine());
  struct Vote { int x, y; };
  std::map<int, std::vector<Vote>> votes;
  constexpr int columns = 6, rows = 12;
  const int w = std::min(std::max(48,b.cols*3/20), b.cols / 2), h = std::min(48, b.rows / 8);
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
            : b.rows - std::abs(d) >= 64;
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
  if (!accepted.empty()) {
    std::vector<std::pair<double,int>> peaks;for(int d:accepted) peaks.emplace_back(0,d);
    return selectPeaks(peaks,prior,1.);
  }
  if (stationary) return {0, true, false, 1.0};
  return {};
}
Match match(const cv::Mat &page, const cv::Mat &frame, bool foreground = false, const ScrollPrediction& prior = {}) {
  const auto exact=nativeRows(page,frame,prior);
  if(exact.found || exact.ambiguous) return exact;
  std::set<int> candidates;
  const int h = std::min(48, frame.rows / 4);
  // Several independent templates, retaining every peak rather than a top-N
  // shortlist: periodic paragraphs must remain ambiguous.
  bool overwhelmed = false;
  // Adjacent native columns retain every glyph, while bounding template FFT
  // size on wide/translucent windows. All vertical peaks remain candidates.
  for (int x=0;x<frame.cols;x+=256) {
    const int w=std::min(256,frame.cols-x);
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
  std::vector<int> search(candidates.begin(),candidates.end());
  // Centre evaluation on the prior, but still verify the complete native
  // search window so a unique contradictory pixel match always wins.
  if(prior.valid) std::stable_sort(search.begin(),search.end(),[&](int a,int b) {return std::abs(a-prior.offset)<std::abs(b-prior.offset);});
  for (int candidate : search) {
    // A tiny overlap containing one repeated card header cannot establish a
    // large reverse jump. Exact native row matches above may prove smaller
    // overlaps; correlation alone needs at least a quarter of the viewport.
    const int overlap=std::min(page.rows,candidate+frame.rows)-std::max(0,candidate);
    if(overlap<std::max(96,frame.rows/4)) continue;
    const auto error = verify(page, frame, candidate, foreground);
    if (error < 1e8)
      verified.emplace_back(error, candidate);
  }
  if (verified.empty())
    return {};
  std::sort(verified.begin(), verified.end());
  const auto [best, position] = verified.front();
  std::vector<std::pair<double,int>> peaks{{best,position}};
  for (const auto &[error, other] : verified)
    if (std::abs(other-position)>2 && error-best<std::max(.025,best*.25)) peaks.emplace_back(error,other);
  return selectPeaks(peaks,prior,std::clamp(1.-best/100.,0.,1.));
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
bool exactOverlap(const QImage &first, const QImage &second, QRect content, int d, int tolerance=0) {
  const int top = std::max(0, d), bottom = std::min(content.height(), content.height() + d);
  if (bottom - top < 64)
    return false;
  for (int y=top;y<bottom;++y) {
    const auto* a=first.constScanLine(content.y()+y)+4*content.x();
    const auto* b=second.constScanLine(content.y()+y-d)+4*content.x();
    if(!std::memcmp(a,b,size_t(content.width())*4)) continue;
    if(!tolerance) return false;
    for(int x=0;x<content.width()*4;++x) if(std::abs(int(a[x])-int(b[x]))>tolerance) return false;
  }
  return true;
}
// A dynamic/fixed edge may obscure part of the pairwise overlap. Pixel-prove
// a contiguous native run containing the informative row votes, not merely a
// sparse repeated prefix. Coordinates are relative to the current viewport.
QRect nativeRun(const QImage &previous, const QImage &current, QRect region,
                const Match &candidate) {
  const int d = candidate.position;
  int start = std::max(0, -d),
      end = std::min(region.height(), region.height() - d), run = start;
  for (int y = start; y <= end; ++y) {
    bool same = y < end;
    if (same) {
      const auto *a =
          previous.constScanLine(region.y() + y + d) + region.x() * 4;
      const auto *b = current.constScanLine(region.y() + y) + region.x() * 4;
      if (std::memcmp(a, b, region.width() * 4))
        for (int x = 0; x < region.width() * 4; ++x)
          if (std::abs(int(a[x]) - int(b[x])) > 1) {
            same = false;
            break;
          }
    }
    if (!same) {
      if (y - run >= 64 && run <= candidate.nativeFirst &&
          y > candidate.nativeLast)
        return {0, run, region.width(), y - run};
      run = y + 1;
    }
  }
  return {};
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
                          QRect content, int d, bool foreground = false, double fraction = .8) {
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
  return votes >= 4 && agree >= votes * (foreground ? .95 : fraction);
}
// Fixed side columns prevent a full-row hash from matching. Generate native
// candidates from adjacent columns covering every x, then prove each candidate
// over a broad body. No glyph is resampled and every vertical offset is searched.
// Two independently scrolling halves cannot satisfy a two-thirds-width proof.
Match nativeColumnMotion(const QImage &a, const QImage &b,
                         const cv::Mat &grayA, const cv::Mat &grayB,
                         QRect content, const ScrollPrediction &prior, QRect &provenRun) {
  TrackerTiming timing("native_columns");
  std::set<int> offsets;
  for(int x=content.left();x<=content.right();x+=256) {
    const QRect column(x,content.y(),std::min(256,content.right()+1-x),content.height());
    if(column.width()<16) continue;
    const cv::Rect roi(column.x(),column.y(),column.width(),column.height());
    // A periodic prefix must not nominate a prior-selected offset before
    // the column containing the distinguishing glyph has been considered.
    const auto candidate=nativeRows(grayA(roi),grayB(roi),{},true);
    if(candidate.found && candidate.position) offsets.insert(candidate.position);
  }
  std::vector<std::pair<double,int>> verified;
  std::map<int,QRect> runs;
  for(int d:offsets) {
    bool accepted=false;
    for(const auto region:{content,content.adjusted(0,0,-content.width()/3,0),
                         content.adjusted(content.width()/3,0,0,0)}) {
      const cv::Rect roi(region.x(),region.y(),region.width(),region.height());
      const auto proof=nativeRows(grayA(roi),grayB(roi),prior,true);
      if(proof.found && proof.position==d) {
        const auto run=nativeRun(a,b,region,proof);
        if(!run.isEmpty()) {
          accepted=true;runs[d]=run.translated(region.topLeft());break;
        }
      }
      // This fast path reports exact native evidence. Majority tile agreement
      // can hide a differing ID beside repeated cards; leave such frames to
      // the correlation/ambiguity path instead of reporting confidence one.
      if(exactOverlap(a,b,region,d)) accepted=true;

    }
    if(accepted) verified.emplace_back(0,d);
  }
  const auto result=verified.empty()?Match{}:selectPeaks(verified,prior,1.);
  if(result.found) provenRun=runs[result.position];
  return result;
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
  const auto background=[&](cv::Rect r) {
    if(r.area()==0) return -1;
    std::array<int,256> counts{};
    for(int y=r.y;y<r.y+r.height;++y) for(int x=r.x;x<r.x+r.width;++x)
      ++counts[a.at<uchar>(y,x)];
    const auto mode=std::max_element(counts.begin(),counts.end());
    return *mode>=r.area()/2?int(mode-counts.begin()):-1;
  };
  const auto sameBackground=[&](cv::Rect side,cv::Rect body) {
    const int value=background(side);
    return value>=0 && value==background(body);
  };
  const auto detail=[&](cv::Rect r) {
    cv::Scalar mean,dev;cv::meanStdDev(a(r),mean,dev);return dev[0]>4;
  };
  const auto distributedBand=[&](int y,int height) {
    int bands=0;
    const bool gutter=sameBackground({0,y,a.cols,height},{0,top,a.cols,bottom-top});
    for(int i=0;i<3;++i) {
      const int x=a.cols*i/3,end=a.cols*(i+1)/3;
      bands+=gutter?detail({x,y,end-x,height}):meaningful({x,y,end-x,height});
    }
    return bands>=2;
  };
  if(top && !distributedBand(0,top)) top=0;
  if(bottom<a.rows && !distributedBand(bottom,a.rows-bottom)) bottom=a.rows;
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
  // A compact fixed widget in an otherwise white gutter is not a sidebar.
  // Require stationary side appearance in multiple vertical bands before
  // freezing an edge on the first translation (before column history exists).
  const auto distributedSide=[&](int x,int width) {
    int bands=0;
    const bool gutter=sameBackground({x,top,width,bottom-top},{left,top,right-left,bottom-top});
    for(int i=0;i<3;++i) {
      const int y=top+(bottom-top)*i/3, end=top+(bottom-top)*(i+1)/3;
      bands+=gutter?detail({x,y,width,end-y}):meaningful({x,y,width,end-y});
    }
    return bands>=2;
  };
  if(left && !distributedSide(0,left)) left=0;
  if(right<a.cols && !distributedSide(right,a.cols-right)) right=a.cols;
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
                                          QRect content, int d, const std::vector<QRect>& objects) {
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
      if(std::any_of(objects.begin(),objects.end(),[&](const QRect& r) {
           return r.contains(x,y) || r.contains(x,y+d);
         })) continue;
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
Match registerBody(const QImage &a, const QImage &b, QRect rect, bool native = false, const ScrollPrediction& prior = {}) {
  const auto first = gray(a.copy(rect)), second = gray(b.copy(rect));
  auto raw = match(first, second, false, prior);
  if (raw.found && (exactOverlap(a, b, rect, raw.position) ||
      (native && verifyOriginalPixels(a,b,rect,raw.position,false,.95)))) return raw;
  auto detail = match(foregroundDetail(first), foregroundDetail(second), true, prior);
  if (detail.found && verifyBody(a, b, rect, detail.position) &&
      (!native || verifyOriginalPixels(a,b,rect,detail.position))) return detail;
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
  result.bottomOcclusion=m_bottomOcclusion;
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
  std::vector<QRect> objects;
  for(const auto& object:m_marginFloats) objects.push_back(object.rect);
  const auto evidence = columnEvidence(original.isNull()?m_previous:m_originalPrevious,
                                       original.isNull()?frame:original, evidenceRect, displacement, objects);
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
    const auto visible = gray(side.image[0]);
    // Prefer the first viewport: late history can already contain a few
    // duplicated bottom slices while the stationary vote is accumulating.
    const auto initialSide = m_initial.copy(newlyPinned);
    auto baseline = gray(initialSide);
    std::vector<int> exactPositions;
    for (int d = std::max(0, m_layout.minimumY)+side.sourceTop; d <= std::min(start, old.height()*3/4)+side.sourceTop; ++d)
      if (exactOverlap(initialSide, side.image[0], initialSide.rect(), d)) exactPositions.push_back(d);
    auto alignment = exactPositions.size() == 1 ? Match{exactPositions.front(),true,false}
        : match(foregroundDetail(baseline), foregroundDetail(visible), true);
    if (!alignment.found) alignment = match(baseline, visible);
    bool initialAnchor = alignment.found;
    if (!alignment.found) {
      baseline = gray(history[0]);
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
  const auto &previous = m_originalPrevious.isNull() ? m_previous : m_originalPrevious;
  if (d > 0 && m_topOcclusion && d < c.height()-m_topOcclusion) {
    int moving=0, translated=0, stationary=0, firstBody=c.top()+m_topOcclusion, bodyRun=0;
    for(int y=c.top();y<c.top()+m_topOcclusion;++y) {
      const auto *a=reinterpret_cast<const QRgb*>(previous.constScanLine(y));
      const auto *shifted=reinterpret_cast<const QRgb*>(previous.constScanLine(y+d));
      const auto *b=reinterpret_cast<const QRgb*>(frame.constScanLine(y));
      int rowMoving=0, rowTranslated=0;
      for(int x=c.left();x<=c.right();++x) {
        const auto error=[](QRgb a,QRgb b) {return std::max({std::abs(qRed(a)-qRed(b)),std::abs(qGreen(a)-qGreen(b)),std::abs(qBlue(a)-qBlue(b))});};
        stationary += error(a[x],b[x])<=3 && error(a[x],shifted[x])>6;
        rowMoving += error(a[x],b[x])>6;
        rowTranslated += error(a[x],b[x])>6 && error(shifted[x],b[x])<=3;
      }
      moving+=rowMoving;translated+=rowTranslated;
      bodyRun = rowMoving>std::max(2,c.width()/100) && rowTranslated>=rowMoving*.8 ? bodyRun+1 : 0;
      if(bodyRun>=8) firstBody=std::min(firstBody,y-7);
    }
    if(moving>c.width()*m_topOcclusion/20 && translated>=moving*.6 && firstBody<c.top()+m_topOcclusion) {
      // A compact header can shrink without disappearing. Moving padding at
      // its old lower edge must not unmask the still-fixed upper chrome.
      const int old=m_topOcclusion;
      m_topOcclusion=stationary>std::max(8,c.width()/10) ? firstBody-c.top() : 0;
      for(int y=c.top()+m_topOcclusion;y<c.top()+old && y<int(m_rows.size());++y) m_rows[y]={};
    }
  }
  if (d < 0 && m_topOcclusion && m_layout.viewportY + d < m_topPinnedAt) {
    int changed = 0;
    for (int y=c.top();y<c.top()+m_topOcclusion;++y) {
      const auto *a=reinterpret_cast<const QRgb *>(previous.constScanLine(y));
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
    const auto *a = reinterpret_cast<const QRgb *>(previous.constScanLine(y));
    const auto *shifted = reinterpret_cast<const QRgb *>(previous.constScanLine(y + d));
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
// Small screen-fixed occluders need source masks, not column classification.
// In translated coordinates the residual contains the object and its shifted
// ghost. Screen-fixed detail identifies the real component; moving document
// detail agrees with the translation and cannot qualify. No shape assumptions.
void ScrollStitcher::updateFloating(const QImage &frame, const QImage &original, const QImage &hdr, int d, bool edgeMasks) {
  if (d <= 0) return;
  TrackerTiming timing("floating");
  // An exact native body translation excludes any new body occluder. Known
  // edge masks retain their current-frame tail sources below; fixed side
  // panels outside this body must not trigger a full floating-object scan.
  const auto body=m_layout.content.adjusted(0,m_topOcclusion,0,-m_bottomOcclusion);
  if(!original.isNull() && body.height()>std::abs(d)+64 &&
     exactOverlap(m_originalPrevious,original,body,d)) return;
  // Compact fixed objects can live in a gutter excluded by initial layout
  // detection. Their local sources are independent of the moving column.
  const QRect c(0,m_layout.content.y(),frame.width(),m_layout.content.height());
  if (d>=c.height()-96) return;
  const auto error=[](QRgb a,QRgb b) {
    return std::max({std::abs(qRed(a)-qRed(b)),std::abs(qGreen(a)-qGreen(b)),
                     std::abs(qBlue(a)-qBlue(b)),std::abs(qAlpha(a)-qAlpha(b))});
  };
  // Prefer native uncomposited pixels: backdrop illumination cannot provide
  // evidence for a document occluder. Without them, require a clean SDR body.
  QImage first=original.isNull()?m_previous:m_originalPrevious;
  QImage second=original.isNull()?frame:original;
  const auto clean=[&]() {
    int changed=0,samples=0;
    const int stride=std::max(1,c.width()/512);
    for(int y=c.top();y<=c.bottom()-d;++y) {
      const auto *a=reinterpret_cast<const QRgb*>(first.constScanLine(y+d));
      const auto *b=reinterpret_cast<const QRgb*>(second.constScanLine(y));
      for(int x=c.left();x<=c.right();x+=stride) {
        if(y<c.top()+c.height()/4 && error(first.pixel(x,y),b[x])<=3) continue;
        if(!m_columns.empty() && m_columns[x].stationaryFrames) continue;
        ++samples;changed+=error(a[x],b[x])>3;
      }
    }
    return samples && changed<=samples*.08;
  };
  const bool cleanBody=clean();
  if(!cleanBody) {m_floatingCandidates.clear();return;}
  std::vector<FloatingCandidate> candidates;
  // Compact top sources must touch the viewport edge; interior translated
  // ghosts cannot establish a header. Broad chrome uses updateTop row motion.
  for(bool top : {true,false}) {
    // The lower scan starts where compact top chrome stops being eligible;
    // do not leave a middle band in which fixed controls are invisible.
    const int begin=top?c.top():c.top()+c.height()/12;
    const int end=top?c.top()+c.height()/4:c.bottom()+1;
    cv::Mat residual(end-begin,c.width(),CV_8U,cv::Scalar(0));
    cv::Mat fixed(residual.size(),CV_8U,cv::Scalar(0));
    for(int y=begin;y<end;++y) {
      const int shiftedY=top?y+d:y-d;
      if(shiftedY<c.top() || shiftedY>c.bottom()) continue;
      if(!top && shiftedY<c.top()+m_topOcclusion) continue;
      const auto *a=reinterpret_cast<const QRgb*>(first.constScanLine(y));
      const auto *b=reinterpret_cast<const QRgb*>(second.constScanLine(y));
      const auto *shifted=reinterpret_cast<const QRgb*>(
          (top?first:second).constScanLine(shiftedY));
      for(int x=c.left();x<=c.right();++x) {
        // Find contrast support first; padding below covers antialiasing and
        // soft shadows without making wallpaper noise an occluder.
        const bool mismatch=error(top?b[x]:a[x],shifted[x])>6;
        residual.at<uchar>(y-begin,x-c.left())=mismatch?255:0;
        // Both endpoints of an edge must stay fixed. A background pixel next
        // to a moving rule can otherwise masquerade as a stationary widget
        // inside the translated ghost of the real control.
        const int next=std::min(x+1,c.right()),below=std::min(y+1,c.bottom());
        const bool detail=(error(a[x],a[next])>8 && error(a[next],b[next])<=3) ||
            (error(a[x],first.pixel(x,below))>8 && error(first.pixel(x,below),second.pixel(x,below))<=3);
        fixed.at<uchar>(y-begin,x-c.left())=mismatch && detail && error(a[x],b[x])<=3?255:0;
      }
    }
    // Join antialiased edges/icon fragments, without making sparse detail a
    // full-height sidebar. Padding keeps soft shadows out of strip sources.
    // A moving rule can join the residual to most of the page. Recognize
    // compact invariant edges independently before inspecting merged ghosts.
    for(bool localOnly : {true,false}) {
    cv::Mat joined,labels,stats,centers;
    cv::dilate(localOnly?fixed:residual,joined,cv::getStructuringElement(cv::MORPH_RECT,{5,5}));
    const int count=cv::connectedComponentsWithStats(joined,labels,stats,centers);
    for(int id=1;id<count;++id) {
      QRect r(c.x()+stats.at<int>(id,cv::CC_STAT_LEFT),begin+stats.at<int>(id,cv::CC_STAT_TOP),
              stats.at<int>(id,cv::CC_STAT_WIDTH),stats.at<int>(id,cv::CC_STAT_HEIGHT));
      if(!top && r.top()<=begin) continue; // A clipped header fragment is not a footer.
      if(r.width()<16 || r.height()<16 || r.width()>c.width()/3 || r.height()>c.height()/3+d) continue;
      int columnSupport=0;
      for(int x=r.left();x<=r.right();++x)
        columnSupport+=!m_columns.empty() && m_columns[x].stationaryFrames>0;
      // Independently distributed side evidence belongs to the panel layer,
      // even while it is still accumulating its three-frame pin threshold.
      // At the first sticky transition glyph columns may still be moving or
      // have too little per-column history. Distributed stationary detail in
      // the same lane belongs to a side panel, not a compact floating object.
      int firstFixed = c.bottom(), lastFixed = c.top(), fixedBands = 0;
      int laneLeft=std::max(c.left(),r.left()-16),laneRight=std::min(c.right(),r.right()+16);
      // Short labels at the far end of a sticky side panel need the evidence
      // from their whole side lane. A narrow glyph column alone can look like
      // a floating button before per-column pin history has accumulated.
      if(r.left()>=c.left()+c.width()*2/3) laneLeft=c.left()+c.width()*2/3;
      if(r.right()<c.left()+c.width()/3) laneRight=c.left()+c.width()/3;
      for (int y=r.top()>c.top()+c.height()/4?c.top()+c.height()/4:c.top(); y<=std::min(c.bottom()-d,c.bottom()-96); ++y) {
        int fixedDetail=0, changed=0, samples=0;
        for (int x=laneLeft; x<=laneRight; x+=2) {
          const auto a=first.pixel(x,y), b=second.pixel(x,y), shifted=first.pixel(x,y+d);
          // A side lane may include part of the adjacent document. Pixels
          // already proven to translate with it cannot veto fixed panel detail.
          if(error(a,b)>3 && error(shifted,b)<=3) continue;
          ++samples; changed += error(a,b)>3;
          // A vertical window border spans every row, but supplies no
          // distributed foreground. Count variation along the scroll axis.
          const bool detail=error(a,first.pixel(x,std::min(y+1,c.bottom())))>8;
          fixedDetail += detail && error(a,b)<=3 && error(shifted,b)>8;
        }
        if (fixedDetail>=3 && changed<=std::max(1,samples/100)) { ++fixedBands;firstFixed=std::min(firstFixed,y);lastFixed=y; }
        if(fixedBands>=3 && lastFixed-firstFixed>std::max(r.height()*3,c.height()/5)) break;
      }
      if (fixedBands>=3 && lastFixed-firstFixed>std::max(r.height()*3,c.height()/5)) continue;
      int support=0;
      QRect fixedBounds;
      for(int y=r.top();y<=r.bottom();++y) for(int x=r.left();x<=r.right();++x)
        if(labels.at<int>(y-begin,x-c.left())==id && fixed.at<uchar>(y-begin,x-c.left())) {
          ++support; fixedBounds=fixedBounds.united(QRect(x,y,1,1));
        }
      if(support>=8) {
        // Translation ghosts can touch the real object. Refine with connected
        // stationary edges instead of including an unrelated coincident edge
        // or the whole merged residual in its representative source.
        const cv::Rect roi(r.x()-c.x(),r.y()-begin,r.width(),r.height());
        cv::Mat evidence,componentMask,joinedEdges,edgeLabels,edgeStats,edgeCenters;
        cv::compare(labels(roi),id,componentMask,cv::CMP_EQ);
        cv::bitwise_and(fixed(roi),componentMask,evidence);
        cv::dilate(evidence,joinedEdges,cv::getStructuringElement(cv::MORPH_RECT,{5,5}));
        const int edgeCount=cv::connectedComponentsWithStats(joinedEdges,edgeLabels,edgeStats,edgeCenters);
        QRect best;
        for(int edge=1;edge<edgeCount;++edge) {
          const cv::Rect box(edgeStats.at<int>(edge,cv::CC_STAT_LEFT),edgeStats.at<int>(edge,cv::CC_STAT_TOP),
                             edgeStats.at<int>(edge,cv::CC_STAT_WIDTH),edgeStats.at<int>(edge,cv::CC_STAT_HEIGHT));
          if(cv::countNonZero(evidence(box))<8 || box.width*box.height<=best.width()*best.height()) continue;
          best=QRect(box.x,box.y,box.width,box.height);
        }
        if(best.width()<8 || best.height()<8) continue;
        fixedBounds=best.translated(r.topLeft());
      }
      // A translated toolbar ghost may contain a coincidentally fixed edge
      // at its far side. That tiny edge cannot establish the whole residual
      // as an independent stationary object.
      if(support>=8 && fixedBounds.width()<std::max(4,r.width()/8)) continue;
      const auto observed=support>=8?fixedBounds:r;
      int observations=1;
      for(const auto& previous:m_floatingCandidates)
        if(previous.top==top && std::abs(previous.rect.left()-observed.left())<=2 &&
           std::abs(previous.rect.right()-observed.right())<=2 && std::abs(previous.rect.top()-observed.top())<=2 &&
           std::abs(previous.rect.bottom()-observed.bottom())<=2)
          observations=std::max(observations,previous.observations+1);
      candidates.push_back({observed,top,observations});
      // An animated fill may have no invariant pixels. Require its compact
      // residual to recur at the same screen bounds on independent scrolls.
      if(support<8 && observations<2) continue;
      if(top && r.top() > c.top()+16) continue;
      if (!top && (support>=8 || observations>=2) && !original.isNull() && r.bottom()<c.bottom()-c.height()/8) {
        // A fixed control in a plain margin needs a local source, not a band
        // hiding half the document. Allow uniform horizontal rules, but any
        // glyph/texture elsewhere in this lane disqualifies background fill.
        QRect object=(support>=8?fixedBounds:r).adjusted(-8,-8,8,8).intersected(c);
        auto found=std::find_if(m_marginFloats.begin(),m_marginFloats.end(),
            [&](const MarginFloat &f){return f.rect.intersects(object);});
        if(found!=m_marginFloats.end()) object=object.united(found->rect).intersected(c);
        const int quiet=quietMarginRow(original,c,object);
        const auto uniform=[&](const QImage &image) {
          if(image.isNull()) return true;
          const int bytes=image.depth()/8;
          const auto near=[&](const uchar* a,const uchar* b) {
            if(bytes==4) {for(int i=0;i<4;++i) if(std::abs(int(a[i])-int(b[i]))>3) return false;}
            else {const auto* x=reinterpret_cast<const qfloat16*>(a);const auto* z=reinterpret_cast<const qfloat16*>(b);
              for(int i=0;i<4;++i) if(std::abs(float(x[i])-float(z[i]))>.015f*std::max(1.f,std::abs(float(z[i])))) return false;}
            return true;
          };
          for(int y=c.top()+16;y<c.bottom()-16;y+=2) {
            if(y>=object.top() && y<=object.bottom()) continue;
            // Window toolbar controls above a plain document margin do not
            // disqualify a local source. Require native three-frame stability
            // before ignoring any of those upper rows.
            if((y<c.top()+c.height()/4 || y>c.bottom()-96) && !m_originalInitial.isNull() && !m_originalPrevious.isNull()) {
              bool chrome=true;
              for(int x=object.left();x<=object.right();++x)
                chrome &= original.pixel(x,y)==m_originalInitial.pixel(x,y) &&
                          original.pixel(x,y)==m_originalPrevious.pixel(x,y);
              if(chrome) continue;
            }
            bool same=true,rule=true;
            const int ref=object.right()+8<image.width()?object.right()+8:object.left()-8;
            if(ref<0) return false;
            for(int x=object.left();x<=object.right();++x) {
              const auto *pixel=image.constScanLine(y)+x*bytes;
              const bool background=near(pixel,image.constScanLine(quiet)+x*bytes);
              same &= background;
              rule &= background || near(pixel,image.constScanLine(y)+ref*bytes);
            }
            if(!same && !rule) return false;
          }
          return true;
        };
        if(!top && object.height()<c.height()/3 && uniform(original) && uniform(m_originalPrevious) &&
           uniform(m_originalInitial) && uniform(hdr) && uniform(m_hdrInitial)) {
          const std::array<QImage,3> planes{frame,original,hdr};
          MarginFloat local;local.rect=object;
          for(int plane=0;plane<3;++plane) if(!planes[plane].isNull()) {
            local.image[plane]=planes[plane].copy(object);
          }
          if(m_layout.viewportY+d>=m_layout.maximumY) {
            if(found==m_marginFloats.end()) m_marginFloats.push_back(std::move(local));
            else *found=std::move(local);
          }
          continue;
        }
      }
      // Exact translation inside the moving body cannot justify masking it
      // because a fixed object or repeated glyph exists in an outside gutter.
      if(localOnly || !edgeMasks || !r.intersects(m_layout.content)) continue;
      if(std::any_of(m_marginFloats.begin(),m_marginFloats.end(),[&](const MarginFloat& f) {
           return f.rect.intersects(r) || f.rect.translated(0,d).intersects(r);
         })) continue;
      if(columnSupport>=1) continue;
      // A coincident glyph/card edge during a fast scroll is not a persistent
      // middle-screen control. Confirm its native fixed bounds in another
      // translated frame before reserving a large source band. Bottom-attached
      // controls can still mask their insertion edge immediately.
      if(!top && r.bottom()<c.bottom()-c.height()/8 && observations<2) continue;
      // Narrow edge chrome (for example a scrollbar thumb) moves at a
      // different rate from the document. Its overlapping edge pixels must
      // not turn the whole lower viewport into a fixed footer source band.
      const int edge=std::max(8,c.width()/100);
      if(!top && r.bottom()<c.bottom()-c.height()/8 && r.width()<std::max(32,c.width()/40) &&
         (r.left()<=c.left()+edge || r.right()>=c.right()-edge)) continue;
      if(qEnvironmentVariableIsSet("HYPRCAPTURE_TIMING_FILE"))
        qInfo("scroll.mask top=%d rect=%d,%d,%d,%d support=%d observations=%d delta=%d",top,
            r.x(),r.y(),r.width(),r.height(),support,observations,d);
      if(top) {
        m_topOcclusion=std::max(m_topOcclusion,std::min(c.height()/4,r.bottom()+7-c.top()));
      } else {
        m_bottomOcclusion=std::max(m_bottomOcclusion,c.bottom()+1-std::max(c.top(),r.top()-6));
      }
    }
  }
  }
  m_floatingCandidates=std::move(candidates);
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
ScrollStitcher::Result
ScrollStitcher::append(const QImage &frame, const QImage &original,
                       const QImage &hdr, const ScrollMotionSnapshot *motion,
                       uint64_t timeNs) {
  m_prediction = m_motion.begin(motion, timeNs);
  auto result = appendPixels(frame, original, hdr);
  const bool accepted =
      result.status == Status::Started || result.status == Status::Appended ||
      result.status == Status::Relocated || result.status == Status::Unchanged;
  const bool identical =
      std::strcmp(result.reason, "identical-pixels-with-pending-input") == 0;
  const bool settled =
      identical && motion && timeNs >= motion->lastInputNs + 200000000;
  m_motion.confirm(result.displacement, accepted || settled,
                   !result.priorUsed && !identical);
  if (m_prediction.valid) {
    result.predictedOffset = m_prediction.offset;
    result.priorTolerance = m_prediction.tolerance;
  }
  result.input = m_prediction.input;
  result.gain = m_prediction.gain;
  if (result.status == Status::Ambiguous && m_prediction.valid && !identical)
    result.reason = "ambiguous-pixels-outside-prior";
  return result;
}
ScrollStitcher::Result ScrollStitcher::appendPixels(const QImage &source, const QImage &sourceOriginal, const QImage &sourceHdr) {
  QImage original=sourceOriginal, hdr=sourceHdr;
  original.setDevicePixelRatio(1); hdr.setDevicePixelRatio(1);
  if ((!hdr.isNull() && (hdr.size() != source.size() || hdr.format() != QImage::Format_RGBA16FPx4_Premultiplied)) ||
      (!empty() && hdr.isNull() != m_hdrInitial.isNull())) return {Status::InvalidFrame, 0, 0, 0, "invalid-frame-or-plane"};
  if ((!original.isNull() && original.size() != source.size()) ||
      (!empty() && original.isNull() != m_originalInitial.isNull()))
    return {Status::InvalidFrame, 0, 0, 0, "invalid-frame-or-plane"};
  if (source.isNull() || source.width() < 64 || source.height() < 96)
    return {Status::InvalidFrame, 0, 0, 0, "invalid-frame-or-plane"};
  if (!hdr.isNull()) m_limits.maxPixels = std::min(m_limits.maxPixels, 32LL * 1024 * 1024);
  if (source.height() > m_limits.maxHeight ||
      qint64(source.width()) * source.height() > m_limits.maxPixels)
    return {Status::LimitReached, 0, 0, 0, "canvas-or-frame-limit"};
  auto frame = source.convertToFormat(QImage::Format_ARGB32);
  frame.setDevicePixelRatio(1);
  // Window captures without a backdrop often share the exact source image.
  // Normalize that storage once and retain the identity for geometry checks.
  original = !original.isNull() && sourceOriginal.cacheKey()==source.cacheKey()
      ? frame : original.convertToFormat(QImage::Format_ARGB32);
  if (empty()) {
    m_initial = m_previous = frame;
    m_originalPrevious = m_originalInitial = original;
    m_hdrInitial = hdr;
    m_layout = {frame.rect(), frame.size()};
    m_frames = 1;
    return {Status::Started, frame.height(), 0, 1, "baseline"};
  }
  if (frame.size() != m_initial.size())
    return {Status::SizeChanged, 0, 0, 0, "frame-size-changed"};
  // A tiny changed ID can be the only evidence of a whole line-period
  // scroll on a wide log. Do not spend a changed-pixel budget here.
  if (original.isNull() ? m_previous==frame : m_originalPrevious==original) {
    // A whole-period scroll and a clamped edge can produce identical pixels.
    // Input alone cannot tell them apart. Retain history and retire that
    // unobserved input only after settling, without calibrating from it.
    if(m_prediction.valid && m_prediction.resolvesTies && std::abs(m_prediction.offset)>2)
      return {Status::Ambiguous,0,0,0,"identical-pixels-with-pending-input"};
    return {Status::Unchanged, 0, 0, 1, "stationary-frame"};
  }
  const QImage &registrationPrevious = original.isNull() ? m_previous : m_originalPrevious;
  const QImage &registrationCurrent = original.isNull() ? frame : original;
  const auto previous = gray(registrationPrevious), current = gray(registrationCurrent);
  // Uncomposited geometry keeps screen-fixed backdrop gradients from making
  // translucent document gutters look like fixed sidebars or footers.
  // The bottom source band also retains middle-screen floating controls once
  // in the final viewport. It is not a full-width motion exclusion: cropping
  // the whole band can leave only a fixed control or empty margin as anchors.
  const int matchingBottom = std::min(m_bottomOcclusion,m_layout.content.height()/8);
  QRect content = m_layoutLocked
      ? m_layout.content.adjusted(0, m_topOcclusion, 0, -matchingBottom)
      : original.isNull() || (m_originalInitial.cacheKey()==m_previous.cacheKey() &&
                              original.cacheKey()==frame.cacheKey())
          ? detectContent(previous, current)
          : detectContent(gray(m_originalInitial), gray(original));
  if (content.width() < 64 || content.height() < 96)
    return {Status::NoOverlap, 0, 0, 0, "insufficient-content-region"};
  Match wholeNative;
  const auto probe=m_layoutLocked?content:frame.rect();
  const auto edge=nativeRows(crop(previous,probe),crop(current,probe),m_prediction,true);
  const auto run=edge.found && edge.position?nativeRun(registrationPrevious,registrationCurrent,probe,edge):QRect{};
  if(!run.isEmpty()) {
    wholeNative=edge;
    if(!m_layoutLocked &&
       (content.width()<frame.width()*.6 || content.height()<frame.height()*.6)) {
      const int first=std::min(run.top(),run.top()+edge.position);
      const int last=std::max(run.bottom()+1,run.bottom()+1+edge.position);
      // Only unchanged screen-edge chrome is an initial fixed source. A bar
      // which just appeared stays inside the body for dynamic row detection.
      int top=0,bottom=frame.height();
      while(top<first && std::memcmp(registrationPrevious.constScanLine(top),registrationCurrent.constScanLine(top),frame.width()*4)==0) ++top;
      while(bottom>last && std::memcmp(registrationPrevious.constScanLine(bottom-1),registrationCurrent.constScanLine(bottom-1),frame.width()*4)==0) --bottom;
      content=QRect(0,top,frame.width(),bottom-top);
    }
  }
  if(!wholeNative.found) {
    QRect columnRun;
    const auto columns=nativeColumnMotion(registrationPrevious,registrationCurrent,previous,current,probe,m_prediction,columnRun);
    const bool small=!m_layoutLocked && (content.width()<frame.width()*.6 || content.height()<frame.height()*.6);
    if(columns.found && (!small || !columnRun.isEmpty())) {
      wholeNative=columns;
      if(small) {
        const int first=std::min(columnRun.top(),columnRun.top()+columns.position);
        const int last=std::max(columnRun.bottom()+1,columnRun.bottom()+1+columns.position);
        int top=0,bottom=frame.height();
        const auto sameRow=[&](int y) {return std::memcmp(registrationPrevious.constScanLine(y)+columnRun.x()*4,
            registrationCurrent.constScanLine(y)+columnRun.x()*4,columnRun.width()*4)==0;};
        while(top<first && sameRow(top)) ++top;
        while(bottom>last && sameRow(bottom-1)) --bottom;
        content.setTop(top);content.setBottom(bottom-1);
      }
    }
  }
  const auto previousNative = crop(previous, content);
  const auto currentNative = crop(current, content);
  const auto rawAlignment = wholeNative.found?wholeNative:match(previousNative, currentNative, false, m_prediction);
  auto alignment = rawAlignment;
  const char *method = "pairwise-overlap";
  bool consensusMotion = false;
  bool usedPrior = rawAlignment.priorUsed;
  bool sparseMotion = false;
  // Opaque documents usually have a byte-identical overlap. This stronger,
  // inexpensive check avoids filtering full-resolution frames on that path.
  const bool smallInferredContent = !m_layoutLocked &&
      (content.width() < frame.width() * .6 || content.height() < frame.height() * .6);
  const bool exactMotion = alignment.found &&
      exactOverlap(m_previous, frame, content, alignment.position);
  const bool exactRegistration = alignment.found &&
      exactOverlap(registrationPrevious, registrationCurrent, content, alignment.position);
  const bool verifiedNative = !original.isNull() && rawAlignment.found &&
      verifyOriginalPixels(registrationPrevious,registrationCurrent,content,rawAlignment.position,false,.95);
  if (!exactRegistration && !verifiedNative && !wholeNative.found) {
    alignment = match(foregroundDetail(previousNative), foregroundDetail(currentNative), true, m_prediction);
    if (alignment.found &&
        (!verifyOriginalPixels(registrationPrevious, registrationCurrent, content, alignment.position, true) ||
         (!original.isNull() && !verifyOriginalPixels(registrationPrevious, registrationCurrent, content, alignment.position))))
      alignment.found = false;
    if (!alignment.found) {
      const bool detailAmbiguous = alignment.ambiguous;
      alignment = rawAlignment;
      if (alignment.found &&
          (!verifyOriginalPixels(registrationPrevious, registrationCurrent, content, alignment.position) ||
           !verifyOriginalPixels(registrationPrevious, registrationCurrent, content, alignment.position, true)))
        alignment.found = false;
      // The sparse-patch fallback is only evidence for a stationary textured
      // backdrop. Repeated terminal phrases alone can vote for a false shift.
      if (!alignment.found && !alignment.ambiguous &&
          texturedFrames(crop(previous, content), crop(current, content))) {
        alignment = matchForeground(previousNative, currentNative);
        sparseMotion = alignment.found;
      }
      if (!alignment.found)
        alignment.ambiguous |= detailAmbiguous;
    }
  }
  // A unique, natively verified zero in a locked body already establishes
  // document position. Illumination/alpha animation must not run every motion
  // fallback again. Keep the zero-position repair path for late-loaded tiles.
  const bool verifiedStationary=m_layoutLocked && alignment.found && alignment.position==0 &&
      verifyOriginalPixels(registrationPrevious,registrationCurrent,content,0,true);
  // When chrome appears mid-capture, registration's old rectangle still
  // includes it. Retry only the broadly changing body. Unlike byte-consensus,
  // this verifies native foreground detail as well as template alignment, and never
  // overrides a successfully verified foreground alignment.
  if (!alignment.found || (!verifiedStationary && alignment.position == 0) || sparseMotion) {
    const auto body = detectContent(previous, current).intersected(content);
    if (body != content && body.width() >= std::max(64, content.width() / 3) &&
        body.height() >= std::max(96, content.height() * 3 / 5)) {
      const bool sparseRejected = sparseMotion &&
          !verifyOriginalPixels(registrationPrevious, registrationCurrent, body, alignment.position, true);
      if (!alignment.found || alignment.position == 0 || sparseRejected) {
        const auto registration = registerBody(registrationPrevious, registrationCurrent, body, !original.isNull(), m_prediction);
        if (registration.found && registration.position != 0) {
          alignment = registration; method = "changing-body-overlap";
          sparseMotion = false;
        } else if (sparseRejected) alignment.found = false;
      }
    }
  }
  if (!alignment.found || (!verifiedStationary && alignment.position == 0) || sparseMotion) {
    // The very first frame of newly appearing chrome is not stationary yet.
    // Broad interior bodies exclude top chrome, one side, or bottom chrome.
    // Require independently verified translations; conflicts stay ambiguous.
    std::map<int,double> translations;
    for (const auto rect : {content.adjusted(0, content.height()/4, -content.width()/3, 0),
                            content.adjusted(content.width()/3, content.height()/4, 0, 0),
                            content.adjusted(0, content.height()/4, 0, -content.height()/4),
                            content.adjusted(0, 0, -content.width()/3, -content.height()/4),
                            content.adjusted(content.width()/3, 0, 0, -content.height()/4)}) {
      if (rect.width() < 64 || rect.height() < 96) continue;
      const auto candidate = registerBody(registrationPrevious, registrationCurrent, rect, !original.isNull(), m_prediction);
      if (candidate.found && candidate.position) {translations[candidate.position]=candidate.confidence;usedPrior|=candidate.priorUsed;}
    }
    if (translations.size() == 1) {alignment = {translations.begin()->first, true, false, translations.begin()->second}; method="interior-body-overlap";}
    else if (translations.size() > 1) alignment = {0, false, true};
  }
  // A period-aligned scroll can change only the differing message IDs. A
  // byte-exact translation of the entire native viewport proves that those
  // IDs belong to a moving page, not an animated widget in a stationary page.
  const bool broadNativeMotion=smallInferredContent && alignment.found && alignment.position &&
      (wholeNative.found || exactOverlap(registrationPrevious,registrationCurrent,frame.rect(),alignment.position));
  if (!alignment.found || (!verifiedStationary && alignment.position == 0) || (smallInferredContent && !broadNativeMotion)) {
    // Preserve master's foreground-registration precedence. Native consensus
    // is a fallback; it cannot override found translucent detail alignment.
    auto consensus = matchConsensus(registrationPrevious, registrationCurrent,
        m_layoutLocked ? content : frame.rect(), false, m_prediction);
    if(consensus.found && consensus.position==0 && alignment.ambiguous &&
       m_prediction.valid && std::abs(m_prediction.offset)>m_prediction.tolerance) {
      consensus.found=false;consensus.ambiguous=true;
    }
    if(consensus.found && consensus.position) {
      bool coherent=false;
      for(const auto region : {content, content.adjusted(0,0,-content.width()/3,0), content.adjusted(content.width()/3,0,0,0)})
        coherent |= verifyOriginalPixels(registrationPrevious,registrationCurrent,region,consensus.position,true);
      // An interior animation/panel may cover most body tiles. Two native
      // edge bands can still prove the document translation independently;
      // neither a central video nor a single periodic prefix can supply both.
      if(!coherent && content.width()>=320) {
        const QRect left(content.x(),content.y(),content.width()/5,content.height());
        const QRect right(content.right()+1-content.width()/5,content.y(),content.width()/5,content.height());
        coherent=verifyOriginalPixels(registrationPrevious,registrationCurrent,left,consensus.position,true) &&
                 verifyOriginalPixels(registrationPrevious,registrationCurrent,right,consensus.position,true);
      }
      if(!coherent) consensus.found=false;
    }
    if (consensus.found || consensus.ambiguous) {
      alignment = consensus; method="native-consensus";
      consensusMotion = consensus.found && consensus.position != 0;
    }
    else if (smallInferredContent) alignment.found = false;
  }
  if ((!alignment.found || (!verifiedStationary && alignment.position == 0)) && !original.isNull()) {
    std::map<int,double> offsets;
    for (const auto rect : {content.adjusted(0,0,-content.width()/3,0),
                            content.adjusted(content.width()/3,0,0,0)}) {
      const auto raw = match(gray(registrationPrevious.copy(rect)),
                             gray(registrationCurrent.copy(rect)), false, m_prediction);
      if (raw.found && raw.position && verifyOriginalPixels(registrationPrevious,registrationCurrent,rect,raw.position))
        {offsets[raw.position]=raw.confidence;usedPrior|=raw.priorUsed;}
    }
    if (offsets.size() == 1) { alignment = {offsets.begin()->first,true,false,offsets.begin()->second}; consensusMotion=true; method="full-height-body-overlap"; }
    else if (offsets.size() > 1) alignment = {0,false,true};
  }
  if (alignment.found && alignment.position == 0 && m_layoutLocked && !original.isNull() &&
      verifyOriginalPixels(registrationPrevious,registrationCurrent,content,0)) {
    // A lazy image may replace an already observed blank at the same document
    // position. Require unchanged native page anchors; never infer position
    // from the loading rectangle itself or from forwarded input.
    bool filledBlank=false;
    const auto oldPixels=gray(registrationPrevious.copy(content));
    const auto newPixels=gray(registrationCurrent.copy(content));
    for (int y=0;y+32<=oldPixels.rows && !filledBlank;y+=32)
      for (int x=0;x+64<=oldPixels.cols && !filledBlank;x+=64) {
        cv::Scalar mean,oldDev,newDev;
        cv::meanStdDev(oldPixels(cv::Rect(x,y,64,32)),mean,oldDev);
        cv::meanStdDev(newPixels(cv::Rect(x,y,64,32)),mean,newDev);
        filledBlank=oldDev[0]<1 && newDev[0]>8;
      }
    if (filledBlank) {
      const int from=m_layout.viewportY+m_topOcclusion;
      const int to=m_layout.viewportY+m_layout.content.height()-m_bottomOcclusion;
      const auto refresh=[&](std::vector<Strip>& strips,const QImage& incoming) {
        if(incoming.isNull() || to<=from) return;
        std::vector<Strip> updated;
        for(const auto& strip:strips) {
          const int end=strip.y+strip.image.height();
          if(end<=from || strip.y>=to) {updated.push_back(strip);continue;}
          if(strip.y<from) updated.push_back({strip.y,strip.image.copy(0,0,strip.image.width(),from-strip.y),strip.sourceTop});
          if(end>to) updated.push_back({to,strip.image.copy(0,to-strip.y,strip.image.width(),end-to),strip.sourceTop+to-strip.y});
        }
        const auto rect=m_layout.content.adjusted(0,m_topOcclusion,0,-m_bottomOcclusion);
        updated.push_back({from,incoming.copy(rect),rect.top()});
        strips=std::move(updated);
      };
      refresh(m_strips,frame);refresh(m_originalStrips,original);refresh(m_hdrStrips,hdr);
      cleanMarginFloats();
      m_previous=frame;m_originalPrevious=original;
      Result result{Status::Unchanged,0,0,alignment.confidence,"loaded-blank-refresh"};
      result.priorUsed=usedPrior || alignment.priorUsed;return result;
    }
  }
  if (alignment.found && alignment.position == 0) {
    // Keep the last registered anchor. Unmatched animation must not replace
    // it (or silently rebase a moving region whose motion was not observed).
    Result result{Status::Unchanged, 0, 0, alignment.confidence, "stationary-anchor"};
    result.priorUsed=usedPrior || alignment.priorUsed;return result;
  }
  int position = m_layout.viewportY + alignment.position;
  if (alignment.found && m_layoutLocked && position > m_layout.maximumY &&
      alignment.position > m_layout.content.height()-m_topOcclusion-m_bottomOcclusion)
    return {Status::NoOverlap,0,0,alignment.confidence,"occlusion-leaves-no-safe-overlap"};
  if (!alignment.found && m_layoutLocked) {
    method="retained-history";
    auto historyPrior=m_prediction;historyPrior.offset+=m_layout.viewportY-m_layout.minimumY+m_topOcclusion;
    const auto body = bodyImage(!original.isNull());
    const auto history = gray(body);
    const auto visible = crop(current, content);
    alignment = match(foregroundDetail(history), foregroundDetail(visible), true, historyPrior);
    const bool foreground = alignment.found;
    if (!alignment.found && !alignment.ambiguous)
      alignment = match(history, visible, false, historyPrior);
    bool consensus = false;
    const auto visibleFrame = registrationCurrent.copy(content);
    if (!alignment.found && !alignment.ambiguous) {
      alignment = matchConsensus(body, visibleFrame, visibleFrame.rect(), true, historyPrior);
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
    return {alignment.ambiguous ? Status::Ambiguous : Status::NoOverlap, 0, 0, alignment.confidence,
            alignment.ambiguous ? "conflicting-translations" : "no-verified-overlap"};
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
    // Reclaim adjacent page gutters/prefixes whose entire translated
    // interior agrees; independently fixed labels remain a barrier.
    const auto translatedSide = [&](int x, int width, bool left) {
      const int y=content.top()+32, height=content.height()-d-64;
      if(width<=0 || height<96) return -1;
      int edge=-1;
      // Walk outwards from proven document motion. Periodic text prefixes and
      // blank gutters can agree with both zero and document motion; retain
      // them until an actually non-translating fixed column forms a barrier.
      for(int i=0;i<width;++i) {
        const int column=left?x+width-1-i:x+i;
        bool translated=true;
        for(int row=y;row<y+height;++row) {
          const auto a=upper.pixel(column,row+d),b=lower.pixel(column,row);
          if(std::max({std::abs(qRed(a)-qRed(b)),std::abs(qGreen(a)-qGreen(b)),std::abs(qBlue(a)-qBlue(b)),std::abs(qAlpha(a)-qAlpha(b))})>1) {translated=false;break;}
        }
        if(!translated) break;
        edge=column;
      }
      return edge;
    };
    if (const int edge = translatedSide(0, content.left(), true); edge >= 0) content.setLeft(edge);
    if (const int edge = translatedSide(content.right()+1, frame.width()-content.right()-1, false); edge >= 0) content.setRight(edge);
  }
  int minY = std::min(m_layout.minimumY, position + m_topOcclusion);
  const int maxY = std::max(m_layout.maximumY, position);
  int added = (maxY - minY) - (m_layout.maximumY - m_layout.minimumY);
  const qint64 height = frame.height() + maxY - minY;
  if (height > m_limits.maxHeight)
    return {Status::LimitReached,0,0,0,"canvas-height-limit"};
  if (height * frame.width() > m_limits.maxPixels)
    return {Status::LimitReached,0,0,0,"canvas-pixel-budget"};
  if (added && m_frames >= m_limits.maxFrames)
    return {Status::LimitReached,0,0,0,"accepted-frame-budget"};
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
      return {Status::Ambiguous, 0, alignment.position, alignment.confidence, "sidebar-has-no-safe-seam"};
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
  const int oldBottomOcclusion=m_bottomOcclusion;
  content = m_layout.content;
  if (displacement && std::abs(displacement) <= content.height() - 96) {
    updateTop(original.isNull()?frame:original, displacement);
    updateColumns(frame, original, hdr, displacement);
    updateFloating(frame, original, hdr, displacement, !exactMotion);
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
  int appendFrom = m_layout.maximumY + content.height() - m_bottomOcclusion;
  if(m_bottomOcclusion>oldBottomOcclusion && maxY>m_layout.maximumY)
    appendFrom=std::min(appendFrom,position+m_topOcclusion);
  if (m_bottomOcclusion && maxY > m_layout.maximumY) {
    const auto trimTail=[&](std::vector<Strip>& strips) {
      std::erase_if(strips,[&](const Strip& s){return s.y>=appendFrom;});
      for(auto& s:strips) if(s.y+s.image.height()>appendFrom)
        s.image=s.image.copy(0,0,s.image.width(),appendFrom-s.y);
    };
    trimTail(m_strips); trimTail(m_originalStrips); trimTail(m_hdrStrips);
  }
  if ((consensusMotion || (!original.isNull() && !exactMotion)) && added) {
    const int from = std::max(position + m_topOcclusion, m_layout.minimumY);
    const int to = std::min(position + content.height() - m_bottomOcclusion, appendFrom);
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
    std::vector<int> backgroundAlpha(content.width());
    for(int x=0;x<content.width();++x) {
      std::array<int,256> histogram{};
      for(int y=content.top()+64;y<content.bottom()-64;y+=16)
        ++histogram[qAlpha(incoming.pixel(content.x()+x,y))];
      const auto mode=std::max_element(histogram.begin(),histogram.end());
      backgroundAlpha[x]=*mode?mode-histogram.begin():255;
    }
    for (size_t i=0; i<m_originalStrips.size(); ++i) {
      auto& raw = m_originalStrips[i];
      const int from = std::max(raw.y,position), to = std::min(raw.y+raw.image.height(),position+content.height());
      for (int documentY=from; documentY<to; ++documentY) {
        const int storedY=documentY-raw.y, sourceY=content.y()+documentY-position;
        if (sourceY < content.y() + m_topOcclusion || sourceY > content.bottom()-m_bottomOcclusion) continue;
        const auto* sourceRaw=reinterpret_cast<const QRgb*>(incoming.constScanLine(sourceY))+content.x();
        const auto* storedRaw=reinterpret_cast<const QRgb*>(raw.image.constScanLine(storedY));
        for (int x=0;x<content.width();++x) {
          if(qAlpha(sourceRaw[x])<=qAlpha(storedRaw[x])) continue;
          // Transparent page backgrounds are intrinsic, not missing corners.
          // A more opaque, screen-stationary foreground layer cannot repair
          // a different document row merely because that row is transparent.
          const auto stationary=m_originalPrevious.pixel(content.x()+x,sourceY);
          if(qAlpha(sourceRaw[x])>backgroundAlpha[x]+2 &&
             std::max({std::abs(qRed(sourceRaw[x])-qRed(stationary)),
                       std::abs(qGreen(sourceRaw[x])-qGreen(stationary)),
                       std::abs(qBlue(sourceRaw[x])-qBlue(stationary)),
                       std::abs(qAlpha(sourceRaw[x])-qAlpha(stationary))})<=3) continue;
          const int oldEdge=std::min(raw.sourceTop+storedY,frame.height()-1-raw.sourceTop-storedY);
          const int newEdge=std::min(sourceY,frame.height()-1-sourceY);
          const bool recoverCorner=oldEdge<64 && newEdge>=64 && qAlpha(sourceRaw[x])>qAlpha(storedRaw[x]);
          if(std::any_of(m_marginFloats.begin(),m_marginFloats.end(),[&](const MarginFloat& object) {
               return object.rect.contains(content.x()+x,sourceY);
             })) continue;
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
    const int count = position + content.height() - m_bottomOcclusion - appendFrom;
    m_strips.push_back({appendFrom,
                        frame.copy(content.x(), content.bottom() + 1 - m_bottomOcclusion - count,
                                   content.width(), count)});
  }
  // Retain uncomposited pixels using the same accepted alignment. This avoids
  // a second matcher and allows background changes after capture.
  if (!original.isNull()) {
    if (minY < m_layout.minimumY)
      m_originalStrips.push_back({minY, original.copy(content.x(), content.y() + minY - position,
          content.width(), prependTo - minY).convertToFormat(QImage::Format_ARGB32), content.y()});
    if (maxY > m_layout.maximumY) {
      const int count = position + content.height() - m_bottomOcclusion - appendFrom;
      m_originalStrips.push_back({appendFrom,
          original.copy(content.x(), content.bottom() + 1 - m_bottomOcclusion - count,
                        content.width(), count).convertToFormat(QImage::Format_ARGB32), content.bottom()+1-m_bottomOcclusion-count});
    }
  }
  if (!hdr.isNull()) {
    if (minY < m_layout.minimumY)
      m_hdrStrips.push_back({minY, hdr.copy(content.x(), content.y() + minY - position, content.width(), prependTo - minY)});
    if (maxY > m_layout.maximumY) {
      const int count = position + content.height() - m_bottomOcclusion - appendFrom;
      m_hdrStrips.push_back({appendFrom,
          hdr.copy(content.x(), content.bottom() + 1 - m_bottomOcclusion - count, content.width(), count)});
    }
  }
  if (m_bottomOcclusion && position >= m_layout.maximumY) {
    const QRect tail(content.x(),content.bottom()+1-m_bottomOcclusion,content.width(),m_bottomOcclusion);
    m_bottomTail={frame.copy(tail),original.convertToFormat(QImage::Format_ARGB32).copy(tail),hdr.copy(tail)};
  }
  if(position>=m_layout.maximumY) {
    // Large steps can put a known middle control outside pairwise overlap.
    // Keep its proven geometry, but refresh its representative pixels from
    // the current viewport in every plane instead of reusing an older page.
    const std::array<QImage,3> planes{frame,original,hdr};
    for(auto &object:m_marginFloats)
      for(int plane=0;plane<3;++plane) if(!planes[plane].isNull())
        object.image[plane]=planes[plane].copy(object.rect);
  }
  cleanMarginFloats();
  const int delta = position - m_layout.viewportY;
  m_layout.minimumY = minY;
  m_layout.maximumY = maxY;
  m_layout.viewportY = position;
  m_previous = frame;
  m_originalPrevious = original;
  if (added)
    ++m_frames;
  Result result{added ? Status::Appended : Status::Relocated, added, delta, alignment.confidence, method};
  result.priorUsed=usedPrior || alignment.priorUsed;
  if(result.priorUsed) result.reason="motion-prior-native-peaks";
  return result;
}
void ScrollStitcher::cleanMarginFloats() {
  const auto c=m_layout.content;
  const std::array<std::vector<Strip>*,3> planes{&m_strips,&m_originalStrips,&m_hdrStrips};
  for(const auto &object:m_marginFloats) {
    const auto clear=[&](QImage &image,int sourceTop,int plane) {
      if(image.isNull() || object.image[plane].isNull()) return;
      const int bytes=image.depth()/8;
      const int left=std::max(c.left(),object.rect.left()),right=std::min(c.right(),object.rect.right());
      if(right<left) return;
      const int ref=object.rect.right()+8<=c.right()?object.rect.right()+8:object.rect.left()-8;
      if(ref<c.left() || ref>c.right()) return;
      const int top=std::max(0,object.rect.top()-sourceTop),bottom=std::min(image.height()-1,object.rect.bottom()-sourceTop);
      const QImage& baseline=plane==2?m_hdrInitial:plane==1?m_originalInitial:m_initial;
      const int quiet=quietMarginRow(baseline,c,object.rect);
      for(int y=top;y<=bottom;++y) {
        // A neighboring margin pixel from this same captured row also retains
        // full-width horizontal rules; do not replace them with a flat color.
        auto *row=image.scanLine(y);
        const auto *background=row+(ref-c.x())*bytes;
        const auto *quietNeighbor=baseline.constScanLine(quiet)+ref*bytes;
        for(int x=left;x<=right;++x) {
          const auto *profile=baseline.constScanLine(quiet)+x*bytes;
          std::memcpy(row+(x-c.x())*bytes,std::memcmp(profile,quietNeighbor,bytes)==0?background:profile,bytes);
        }
      }
    };
    for(size_t i=0;i<m_originalStrips.size();++i) {
      const int sourceTop=m_originalStrips[i].sourceTop;
      for(int plane=0;plane<3;++plane) if(i<planes[plane]->size()) clear((*planes[plane])[i].image,sourceTop,plane);
    }
    for(int plane=0;plane<3;++plane) clear(m_bottomTail[plane],c.bottom()+1-m_bottomOcclusion,plane);
  }
}
QImage ScrollStitcher::bodyImage(bool original) const {
  QImage result(m_layout.content.width(),
                m_layout.content.height() + m_layout.maximumY -
                    m_layout.minimumY,
                QImage::Format_ARGB32);
  result.fill(Qt::transparent);
  QPainter p(&result);
  p.setCompositionMode(QPainter::CompositionMode_Source);
  for (const auto &strip : original ? m_originalStrips : m_strips)
    p.drawImage(0, strip.y - m_layout.minimumY, strip.image);
  return result;
}
void ScrollStitcher::paint(QPainter &p, int plane) const {
  const int channel = plane == 1 && m_originalInitial.isNull() ? 0 : plane;
  QImage initial = channel == 2 ? m_hdrInitial : channel == 1 ? m_originalInitial : m_initial;
  // Keep the registration baseline intact. Paint initial side sources with
  // recognized margin objects removed, then draw their representative once.
  for(const auto& object:m_marginFloats) {
    const int bytes=initial.depth()/8;
    const int ref=object.rect.right()+8<initial.width()?object.rect.right()+8:object.rect.left()-8;
    if(ref<0 || ref>=initial.width()) continue;
    const int quiet=quietMarginRow(initial,m_layout.content,object.rect);
    const QImage source=initial;
    for(int y=object.rect.top();y<=object.rect.bottom();++y) {
      auto *row=initial.scanLine(y);
      const auto *neighbor=source.constScanLine(y)+ref*bytes;
      const auto *quietNeighbor=source.constScanLine(quiet)+ref*bytes;
      for(int x=object.rect.left();x<=object.rect.right();++x) {
        const auto *base=source.constScanLine(quiet)+x*bytes;
        // Preserve the margin's vertical alpha/border profile, while allowing
        // unoccluded full-width rules from this exact source row.
        const auto *pixel=std::memcmp(base,quietNeighbor,bytes)==0?neighbor:base;
        std::memcpy(row+x*bytes,pixel,bytes);
      }
    }
  }
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
  if(m_bottomOcclusion)
    p.drawImage(c.x(),c.y()+c.height()+growth-m_bottomOcclusion,m_bottomTail[channel]);
  for(const auto &object:m_marginFloats) {
    p.save();p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(object.rect.x(),object.rect.y()+growth,object.image[channel]);p.restore();
  }
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
