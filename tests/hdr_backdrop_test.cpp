#include "shared/hdr_color.hpp"
#include "ui/hdr_backdrop.hpp"
#include <LayerShellQt/Window>
#include <QApplication>
#include <QColorSpace>
#include <QPainter>
#include <QPointer>
#include <QWindow>
#include <QTimer>
#include <QWidget>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace hyprcapture;
static float pqNits(unsigned code) {
  constexpr double m1 = 2610.0 / 16384, m2 = 2523.0 / 32, c1 = 3424.0 / 4096,
                   c2 = 2413.0 / 128, c3 = 2392.0 / 128;
  double p = std::pow(code / 1023.0, 1 / m2);
  return 10000 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1 / m1);
}
static QImage fixture() {
  QImage image(512, 256, QImage::Format_RGBA16FPx4);
  image.setColorSpace(QColorSpace::SRgbLinear);
  constexpr float nits[] = {0, 69, 97, 173, 205, 359, 500, 630};
  for (int y = 0; y < image.height(); ++y) {
    auto *row = reinterpret_cast<uint16_t *>(image.scanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      const std::array<std::array<float, 3>, 8> colors{
          {{173, 0, 0},
           {0, 173, 0},
           {0, 0, 173},
           {400, 0, 0},
           {0, 400, 0},
           {0, 0, 400},
           {-38.91F, 180.28F, -13.61F},
           {400, 0, 400}}}; // Display-P3 green in linear sRGB, including
                            // negative channels.
      for (int c = 0; c < 3; ++c)
        row[x * 4 + c] = color::floatToHalf(
            (y < 128 ? nits[x / 64] : colors[x / 64][c]) / 80);
      row[x * 4 + 3] = color::floatToHalf(1);
    }
  }
  return image;
}
class LiveFixture : public QWidget {
public:
  std::unique_ptr<ui::HdrBackdrop> backdrop;
  bool attempted = false;
  LiveFixture() {
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
                   Qt::WindowTransparentForInput);
    resize(512, 256);
    winId();
    auto *layer = LayerShellQt::Window::get(windowHandle());
    layer->setScope("hyprcapture-hdr-live-test");
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    layer->setAnchors(
        LayerShellQt::Window::Anchors{LayerShellQt::Window::AnchorTop} |
        LayerShellQt::Window::AnchorLeft);
    layer->setMargins(QMargins(600, 200, 0, 0));
    layer->setKeyboardInteractivity(
        LayerShellQt::Window::KeyboardInteractivityNone);
    layer->setExclusiveZone(-1);
  }
  void paintEvent(QPaintEvent *) override {
    if (!attempted) {
      attempted = true;
      backdrop = std::make_unique<ui::HdrBackdrop>(windowHandle());
      bool ok = backdrop->setImage(fixture(), rect(), 173);
      printf("HDR_READY=%d\n", ok);
      fflush(stdout);
    }
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);
  }
};
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  // EXT_LINEAR is a normalised linear TF, not the Windows-scRGB protocol
  // shortcut. Its encoding span must describe the actual uploaded units.
  // Mastering peak must remain independent (PQ ignores encoding max).
  for (float reference : {80.F, 173.F, 203.F, 252.F}) {
    for (float peak : {0.F, 69.F, 173.F, 630.F, 1000.F, 10000.F}) {
      const auto fp = ui::hdrPresentationLuminances(true, peak, reference);
      const auto pq = ui::hdrPresentationLuminances(false, peak, reference);
      assert(fp.encodingMaximum == 80 && pq.encodingMaximum == 10000);
      assert(fp.reference == reference && pq.reference == reference);
      assert(fp.masteringMaximum == std::max(peak, reference));
      assert(pq.masteringMaximum == fp.masteringMaximum);
      // Reference-white anchoring back onto the captured output has ratio 1.
      // Neither headless 80-nit output nor HDR 173-nit output gets a gain.
      for (float nits : {.2F, 69.F, 173.F, 630.F})
        assert(std::abs(nits / 80 * fp.encodingMaximum * reference / fp.reference - nits) < .001F);
    }
  }
  const auto invalid = ui::hdrPresentationLuminances(true,
      std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN());
  assert(invalid.encodingMaximum == 80 && invalid.reference == 203 && invalid.masteringMaximum == 203);
  assert(ui::hdrPresentationLuminances(false, 630.01F, 173).masteringMaximum == 631);
  auto image = fixture();
  float peak = 0;
  auto pq = ui::hdrToPq10(image, &peak);
  assert(pq.format() == QImage::Format_RGB30 && peak > 629 && peak < 631);
  const auto *row = reinterpret_cast<const uint32_t *>(pq.constScanLine(100));
  const float nits[] = {0, 69, 97, 173, 205, 359, 500, 630};
  for (int i = 0; i < 8; ++i) {
    auto pixel = row[i * 64 + 32];
    unsigned r = (pixel >> 20) & 1023, g = (pixel >> 10) & 1023,
             b = pixel & 1023;
    assert(r == g && g == b);
    assert(std::abs(pqNits(r) - nits[i]) <= std::max(0.01F, nits[i] * 0.006F));
  }
  if (argc > 1 && std::string_view(argv[1]) == "--live") {
    LiveFixture window;
    window.show();
    QTimer::singleShot(15000, &app, &QApplication::quit);
    return app.exec();
  }
  auto transparent = image.copy();
  auto *pixel = reinterpret_cast<uint16_t *>(transparent.scanLine(100));
  pixel[32 * 4 + 3] = 0;
  const auto checker = ui::hdrToPq10(transparent);
  const unsigned checkerCode =
      (reinterpret_cast<const uint32_t *>(checker.constScanLine(100))[32] >>
       20) &
      1023;
  assert(pqNits(checkerCode) > 10 && pqNits(checkerCode) < 30);
  QWindow parent;
  // Unsupported HDR must leave no repeating timer, even if the consumer keeps
  // its failed backdrop alive alongside the SDR fallback.
  if (auto unavailable = ui::HdrBackdrop::unavailableForTest(&parent)) {
    const auto timers = app.findChildren<QTimer *>("hyprcaptureHdrDrain");
    assert(timers.size() == 1);
    QPointer<QTimer> timer = timers.front();
    assert(timer->isActive());
    assert(!unavailable->setPqImage(pq, QRect(0, 0, 512, 256), peak, 203));
    assert(!timer->isActive());
    unavailable.reset();
    assert(!timer || !timer->isActive());
  }
  if (auto abandoned = ui::HdrBackdrop::unavailableForTest(&parent)) {
    const auto timers = app.findChildren<QTimer *>("hyprcaptureHdrDrain");
    QPointer<QTimer> timer = timers.back();
    assert(timer->isActive());
    abandoned.reset();
    assert(!timer || !timer->isActive());
  }
  auto stalled=ui::HdrUpload::pendingForTest(image);
  assert(stalled->state()==ui::HdrUpload::State::Pending);
  assert(!stalled->waitUntil(std::chrono::steady_clock::now()));
  stalled->failForTest();
  assert(stalled->waitUntil(std::chrono::steady_clock::now()+std::chrono::milliseconds(1)));
  assert(!stalled->result());
  ui::HdrBackdrop fallback(nullptr);
  assert(!fallback.ready());
  assert(!fallback.setImage(image, QRect(0, 0, 512, 256), 173));
  assert(ui::hdrToPq10(QImage(2, 2, QImage::Format_ARGB32)).isNull());
}
