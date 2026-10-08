#pragma once
#include "ui/scroll_stitcher.hpp"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
class QTemporaryDir;
class QLabel;
class QPushButton;
class QProcess;
class QScreen;
class QLocalServer;
class QLocalSocket;
class QSocketNotifier;
class QVariantAnimation;
class AnnotationEditor;
struct AnnotationSnapshot;
namespace hyprcapture::ui {
QRect scrollControlsPlacement(const QRect &capture, const QList<QRect> &screens,
                              const QSize &controls);
class ScrollCaptureController final : public QWidget {
  Q_OBJECT
public:
  explicit ScrollCaptureController(QWidget *parent = nullptr);
  ~ScrollCaptureController() override;
  bool prepare(const QRect &capture, QString &error,
               const QString &windowAddress = {});
  void setEditor(AnnotationEditor *editor);
  void setWindowBackgroundProvider(std::function<QImage(const QImage&, const QRect&)> provider);
  void updateTarget(const QRect &capture);
  QRect captureGeometry() const { return m_capture; }
  void start(); // arm; does not begin capture until a real scroll gesture
  void scroll(const QPointF &global, double delta, int discrete, bool finger,
              bool inverted);
  void exclude(const QList<QRect> &rects);
  void finish();
  void cancel();
  ScrollLayout resultLayout() const { return m_layout; }
  QImage originalResult() const { return m_originalResult; }
  QImage originalFirstFrame() const { return m_originalFirstFrame; }
  QRect originalGeometry() const { return m_originalGeometry; }
  QRect previewGeometry() const;
signals:
  void began();
  void completed(const QImage &image);
  void cancelled();
  void failed(const QString &reason);
  void progress(const QSize &imageSize, int captures);

protected:
  void closeEvent(QCloseEvent *) override;
  bool eventFilter(QObject *, QEvent *) override;

private:
  struct Frame {
    QImage image;
    QRect geometry;
    quint64 sequence = 0, timeNs = 0;
    QImage background;
  };
  void acceptFrames();
  void readFrames();
  void command(const QJsonObject &object);
  void readControl();
  void work(std::stop_token stop);
  void motion(double delta);
  void restoreAnnotationsIfReady();
  void present(QImage frame, QRect geometry, QImage preview,
               ScrollLayout layout, ScrollStitcher::Result result, int captures,
               bool stable, quint64 sequence, quint64 timeNs);
  void stop();
  void setStatus(const QString &);
  void relayoutPreview();
  QPointer<AnnotationEditor> m_editor;
  std::shared_ptr<const AnnotationSnapshot> m_before;
  QPointer<QScreen> m_screen;
  QRect m_capture;
  QString m_windowAddress, m_hyprctl, m_id, m_requestPath;
  std::unique_ptr<QTemporaryDir> m_exchange;
  QLocalServer *m_server = nullptr;
  QLocalSocket *m_control = nullptr;
  QProcess *m_process = nullptr;
  QSocketNotifier *m_accept = nullptr;
  QSocketNotifier *m_receive = nullptr;
  int m_listener = -1, m_peer = -1;
  QWidget *m_menu = nullptr;
  QLabel *m_status = nullptr;
  QLabel *m_preview = nullptr;
  QPushButton *m_finish = nullptr;
  QVariantAnimation *m_animation = nullptr;
  QTimer m_idle, m_watchdog;
  QElapsedTimer m_lastInput;
  qreal m_annotationOpacity = 1, m_annotationOffset = 0;
  quint64 m_lastInputNs = 0, m_latestCaptureNs = 0;
  bool m_latestAligned = false, m_latestStable = false;
  std::function<QImage(const QImage&, const QRect&)> m_backgroundProvider;
  QImage m_background;
  QRect m_backgroundGeometry, m_originalGeometry;
  QList<QRect> m_exclusions;
  QList<QJsonObject> m_commands;
  QByteArray m_controlBytes;
  ScrollLayout m_layout;
  QImage m_thumbnail, m_originalResult, m_originalFirstFrame;
  std::mutex m_mutex;
  std::condition_variable_any m_ready;
  std::optional<Frame> m_latest;
  bool m_finalize = false;
  std::jthread m_worker;
  quint64 m_sequence = 0, m_lastAcceptedSequence = 0, m_dropped = 0;
  bool m_active = false, m_done = false, m_finishing = false,
       m_hiddenAnnotations = false, m_stopping = false;
};
} // namespace hyprcapture::ui
