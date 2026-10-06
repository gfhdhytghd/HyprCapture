#pragma once

#include "ui/scroll_stitcher.hpp"
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <memory>
class QTemporaryDir;

class QLabel;
class QPushButton;
class QProcess;
class QScreen;

namespace hyprcapture::ui {

// The controls must never overlap the pixels being captured. Coordinates are
// global logical desktop coordinates; negative monitor origins are supported.
QRect scrollControlsPlacement(const QRect& capture, const QList<QRect>& screens, const QSize& controls);

class ScrollCaptureController final : public QWidget {
    Q_OBJECT
  public:
    explicit ScrollCaptureController();
    ~ScrollCaptureController() override;
    bool prepare(const QRect& capture, QString& error);
    void start();

  signals:
    void completed(const QImage& image);
    void cancelled();
    void progress(const QSize& imageSize, int captures);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void sample();
    void receiveFrame(const QImage& frame);
    void setStatus(const QString& text);
    void stop(const QString& reason = {});
    void finish();
    void cancel();
    void configureLayer();

    QPointer<QScreen> m_captureScreen;
    QPointer<QScreen> m_controlsScreen;
    QRect m_capture;
    QRect m_controls;
    QSize m_expectedSize;
    QSize m_frameSize;
    qreal m_scale = 1.0;
    QString m_hyprctl;
    std::unique_ptr<QTemporaryDir> m_exchange;
    QString m_requestPath;
    QLabel* m_status = nullptr;
    QPushButton* m_finish = nullptr;
    QProcess* m_process = nullptr;
    QTimer m_interval;
    QTimer m_timeout;
    QTimer m_sessionTimeout;
    QByteArray m_pixels;
    QByteArray m_stderr;
    QImage m_candidate;
    ScrollStitcher m_stitcher;
    bool m_running = false;
    bool m_done = false;
};

} // namespace hyprcapture::ui
