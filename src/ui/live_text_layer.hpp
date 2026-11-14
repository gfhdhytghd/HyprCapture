#pragma once

#include "shared/config.hpp"
#include "shared/ocr_result.hpp"
#include "ui/live_text_layout.hpp"
#include <QElapsedTimer>
#include <QImage>
#include <QVariantAnimation>
#include <QWidget>
#include <thread>

namespace hyprcapture::ui {

// A display/input layer only. The annotation canvas and its history stay owned
// by AnnotationEditor; recognition never sees its composited annotation output.
class LiveTextLayer final : public QWidget {
    Q_OBJECT
  public:
    explicit LiveTextLayer(QWidget* canvas);
    ~LiveTextLayer() override;
    void configure(const OcrConfig& config, NotificationBackend backend, bool allowed);
    void setSource(const QImage& base);
    void setView(const QTransform& transform);
    void toggle();
    void turnOff(bool instant = false);
    void stop();
    bool isOn() const { return m_on; }
    bool busy() const { return m_busy; }
    bool available() const { return m_allowed && m_config.mode != "off"; }
    bool copyOnly() const;
    bool fallback() const;
    QString tooltip() const;
    void selectAll();
    void copySelection(bool all = false);
    QString selectedText() const;

  signals:
    void stateChanged();
    void iconShake(qreal offset);
    void recognized();
    void feedback(const QString& message, bool error);

  protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

  private:
    enum class Phase { Idle, Scan, Reveal, Fade, CopyFlash };
    void startJob();
    void cancelJob();
    void receive(quint64 generation, OcrResponse response);
    void finishScan();
    void reveal();
    void animate(QVariantAnimation& animation, int duration, QEasingCurve::Type easing,
                 std::function<void(qreal)> frame, std::function<void()> finished = {});
    void toast(const QString& text);
    void fail(const QString& error);
    void shake();
    void repaintImage();
    QRect imageRect() const;
    QRect toastRect() const;
    void forward(QEvent* event);
    bool reducedMotion() const;
    OcrConfig m_config;
    NotificationBackend m_backend = NotificationBackend::Hyprland;
    bool m_allowed = false, m_on = false, m_busy = false, m_cached = false;
    bool m_pendingReveal = false, m_selecting = false, m_panning = false, m_space = false;
    bool m_fallbackWarned = false;
    QImage m_base;
    QTransform m_transform;
    LiveTextLayout m_layout;
    OcrResponse m_results;
    std::jthread m_worker;
    quint64 m_generation = 0;
    int m_anchor = -1, m_caret = -1;
    QElapsedTimer m_clickClock;
    QPointF m_clickPosition;
    Phase m_phase = Phase::Idle;
    qreal m_dim = 0, m_opacity = 1, m_scan = 0, m_reveal = 1, m_copyPulse = 0;
    QString m_toast;
    qreal m_toastOpacity = 0, m_toastSlide = 0;
    QVariantAnimation m_dimAnimation, m_scanAnimation, m_phaseAnimation, m_copyAnimation, m_toastAnimation, m_shakeAnimation;
};
} // namespace hyprcapture::ui
