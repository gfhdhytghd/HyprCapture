#pragma once
#include <QWidget>
#include <QPainter>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>

// Post-fader sample peak and RMS meter. No audio monitoring/playback is performed.
class AudioMeter final : public QWidget {
public:
    explicit AudioMeter(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(90, 38); clock.start();
        setToolTip("Post-gain sample peak / RMS in dBFS; red indicates clipping before the final mix limiter");
    }
    void setGain(int db) { gain = db; held = 0; clipUntil = 0; refresh(); }
    void setLevels(double p, double r, bool ready) {
        peak = std::isfinite(p) ? std::max(0., p) : 0;
        rms = std::isfinite(r) ? std::max(0., r) : 0;
        available = ready; refresh();
    }
    void reset() { peak = rms = held = 0; clipUntil = 0; available = false; update(); }
private:
    void refresh() {
        const double value = peak * factor();
        if (value >= held || clock.elapsed() > holdUntil) { held = value; holdUntil = clock.elapsed() + 1200; }
        if (value >= 1) clipUntil = clock.elapsed() + 1500;
        setProperty("postGainPeak", value);
        setProperty("postGainRms", rms * factor());
        update();
    }
    double factor() const { return gain <= -61 ? 0 : std::pow(10., gain / 20.); }
    static double db(double v) { return v > 0 ? 20 * std::log10(v) : -100; }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const double post = peak * factor();
        const int w = width();
        auto x = [w](double v) { return std::clamp((v + 60.) / 60., 0., 1.) * w; };
        p.fillRect(QRectF(0, 15, w, 8), QColor("#29333c"));
        for (auto range : {std::pair{-60., -18.}, {-18., -6.}, {-6., 0.}}) {
            const double end = std::min(db(post), range.second);
            if (end > range.first && available)
                p.fillRect(QRectF(x(range.first), 15, x(end) - x(range.first), 8), QColor(range.first == -60 ? "#45bc86" : range.first == -18 ? "#e2b85b" : "#ef6868"));
        }
        if (available) {
            p.fillRect(QRectF(0, 24, x(db(rms * factor())), 2), QColor("#b5ccd9"));
            p.setPen(QColor("#ffffff")); p.drawLine(QPointF(x(db(held)), 14), QPointF(x(db(held)), 24));
        }
        auto font = p.font(); font.setPixelSize(9); p.setFont(font);
        p.setPen(clock.elapsed() < clipUntil ? QColor("#ff7070") : palette().color(QPalette::WindowText));
        p.drawText(QRect(0, 0, w, 13), Qt::AlignRight, !available ? "No signal" : clock.elapsed() < clipUntil ? "CLIP" : post == 0 ? "−∞ dBFS" : QString::number(db(post), 'f', 1) + " dBFS");
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(QRect(0, 27, w, 11), Qt::AlignLeft, "−60");
        p.drawText(QRect(0, 27, w, 11), Qt::AlignCenter, "−30");
        p.drawText(QRect(0, 27, w, 11), Qt::AlignRight, "0");
    }
    QElapsedTimer clock;
    double peak = 0, rms = 0, held = 0;
    qint64 holdUntil = 0, clipUntil = 0;
    int gain = 0;
    bool available = false;
};
