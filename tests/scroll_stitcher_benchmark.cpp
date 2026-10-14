#include "ui/scroll_stitcher.hpp"
#include <QGuiApplication>
#include <QPainter>
#include <chrono>
#include <qfloat16.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string_view>
#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>
#include <sched.h>
#include <sys/resource.h>
using Clock = std::chrono::steady_clock;
int benchmark4k(bool hdr, bool single) {
  if(single) cv::setNumThreads(1);
  constexpr int width=3840,viewport=2160,step=120,frames=7;
  QImage doc(width,viewport+step*(frames-1),QImage::Format_RGBA8888);
  unsigned seed=727;
  for(int y=0;y<doc.height();++y) {
    auto* row=doc.scanLine(y);
    for(int x=0;x<width*4;++x) {seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;row[x]=x%4==3?255:seed&255;}
  }
  QImage linear;
  if(hdr) {
    linear=QImage(doc.size(),QImage::Format_RGBA16FPx4_Premultiplied);
    for(int y=0;y<doc.height();++y) {auto* out=reinterpret_cast<qfloat16*>(linear.scanLine(y));
      const auto* in=doc.constScanLine(y);
      for(int x=0;x<width;++x) {for(int c=0;c<3;++c)out[4*x+c]=qfloat16(2.f+in[4*x+c]/255.f);out[4*x+3]=qfloat16(1.f);}}
  }
  std::vector<double> times;
  for(int repeat=0;repeat<3;++repeat) {
    hyprcapture::ui::ScrollStitcher s;
    for(int n=0;n<frames;++n) {
      const auto frame=doc.copy(0,n*step,width,viewport);
      const auto h=hdr?linear.copy(0,n*step,width,viewport):QImage{};
      const auto start=Clock::now();
      const auto r=s.append(frame,hdr?frame:QImage{},h);
      const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
      if(n)times.push_back(ms);
      if(r.status!=(n?hyprcapture::ui::ScrollStitcher::Status::Appended:hyprcapture::ui::ScrollStitcher::Status::Started))return 1;
    }
    if(s.image().convertToFormat(doc.format())!=doc)return 2;
    if(hdr && s.hdrImage()!=linear)return 3;
  }
  std::sort(times.begin(),times.end());
  double total=0;for(double t:times)total+=t;
  std::cout<<"4k,"<<(hdr?"RGBA8+original+RGBA16F":"RGBA8")<<",threads="<<cv::getNumThreads()
    <<",samples="<<times.size()<<",mean_ms="<<total/times.size()<<",median_ms="<<times[times.size()/2]
    <<",p95_ms="<<times[size_t((times.size()-1)*.95)]<<",exact=1\n";
  return 0;
}
int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  if(argc>1 && (std::string_view(argv[1])=="--4k8" || std::string_view(argv[1])=="--4k16"))
    return benchmark4k(std::string_view(argv[1])=="--4k16",argc>2 && std::string_view(argv[2])=="--single");
  cv::ocl::setUseOpenCL(false);
  const bool dynamic = argc > 1 && std::string_view(argv[1]) == "--dynamic";
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
    QImage frame = document.copy(0, offset, width, viewport);
    if (dynamic) {
      QImage animation(640, 400, QImage::Format_ARGB32);
      unsigned seed = unsigned(offset) + 1;
      for (int y = 0; y < animation.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(animation.scanLine(y));
        for (int x = 0; x < animation.width(); ++x) {
          seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
          row[x] = qRgb(seed & 255, (seed >> 8) & 255, (seed >> 16) & 255);
        }
      }
      QPainter p(&frame);
      p.drawImage(880, 500, animation);
    }
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
  bool exact = dynamic ? result.size() == document.size() &&
      result.copy(0, 0, 800, height) == document.copy(0, 0, 800, height)
      : result == document;
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
            << (dynamic ? ",unobscured_pixels_exact=" : ",pixel_exact=") << exact << "\n";
  std::ifstream proc("/proc/self/status");
  std::string line;
  while (std::getline(proc, line))
    if (line.starts_with("VmHWM:") || line.starts_with("VmRSS:"))
      std::cout << line << "\n";
  return exact ? 0 : 2;
}
