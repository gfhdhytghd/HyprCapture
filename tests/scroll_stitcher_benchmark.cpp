#include "ui/scroll_stitcher.hpp"
#include <QGuiApplication>
#include <QPainter>
#include <chrono>
#include <fstream>
#include <iostream>
#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>
#include <sched.h>
#include <sys/resource.h>
using Clock = std::chrono::steady_clock;
int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  cv::setNumThreads(1);
  cv::ocl::setUseOpenCL(false);
  constexpr int width = 2560, height = 16384, viewport = 1440, step = 720;
  QImage document(width, height, QImage::Format_ARGB32);
  document.fill(Qt::white);
  {
    QPainter p(&document);
    p.setFont(QFont("sans-serif", 22));
    for (int y = 40, n = 0; y < height; y += 43, ++n) {
      p.setPen(QColor(25 + n % 80, 40, 60));
      p.drawText(
          42, y,
          QString("Document row %1 — native pixel coordinates %2 — checksum %3")
              .arg(n)
              .arg(y)
              .arg(n * 7919));
      p.fillRect(1900, y - 22, 100 + (n * 73) % 480, 12,
                 QColor((n * 29) % 200, 80, (n * 37) % 200));
    }
  }
  hyprcapture::ui::ScrollStitcher stitcher;
  std::vector<double> times;
  const auto start = Clock::now();
  for (int offset = 0;; offset = std::min(offset + step, height - viewport)) {
    const QImage frame = document.copy(0, offset, width, viewport);
    const auto t = Clock::now();
    const auto result = stitcher.append(frame);
    const auto preview = stitcher.preview({240, 320});
    double ms =
        std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    times.push_back(ms);
    std::cout << "frame," << offset << "," << ms << "," << int(result.status)
              << "\n";
    if (result.status != hyprcapture::ui::ScrollStitcher::Status::Started &&
        result.status != hyprcapture::ui::ScrollStitcher::Status::Appended)
      return 1;
    if (offset == height - viewport)
      break;
  }
  const auto t = Clock::now();
  const auto result = stitcher.image();
  const double assemble =
      std::chrono::duration<double, std::milli>(Clock::now() - t).count();
  const double total =
      std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  bool exact = result == document;
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  std::sort(times.begin(), times.end());
  std::cout << "summary,cpu=" << sched_getcpu()
            << ",threads=" << cv::getNumThreads()
            << ",opencl=" << cv::ocl::useOpenCL() << ",frames=" << times.size()
            << ",median_ms=" << times[times.size() / 2]
            << ",p95_ms=" << times[size_t((times.size() - 1) * .95)]
            << ",max_ms=" << times.back() << ",assemble_ms=" << assemble
            << ",total_ms=" << total << ",maxrss_kib=" << usage.ru_maxrss
            << ",pixel_exact=" << exact << "\n";
  std::ifstream proc("/proc/self/status");
  std::string line;
  while (std::getline(proc, line))
    if (line.starts_with("VmHWM:") || line.starts_with("VmRSS:"))
      std::cout << line << "\n";
  return exact ? 0 : 2;
}
