#include "ui/scroll_capture.hpp"
#include "ui/thumbnail_style.hpp"
#include "plugin/window_stream.hpp"
#include "shared/protocol.hpp"
#include "ui/annotation_editor.hpp"
#include "ui/clipboard_utils.hpp"
#include "ui/i18n.hpp"
#include "ui/mapped_image.hpp"
#include <QCloseEvent>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
namespace hyprcapture::ui {
QRect scrollControlsPlacement(const QRect &capture, const QList<QRect> &screens,
                              const QSize &controls) {
  if (!capture.isValid() || !controls.isValid())
    return {};
  for (const auto &screen : screens)
    if (screen.contains(capture))
      return QRect(screen.right() - controls.width() - thumbnail::kThumbnailScreenMargin + 1,
                   screen.bottom() - controls.height() - thumbnail::kThumbnailScreenMargin + 1, controls.width(),
                   controls.height());
  return {};
}
ScrollCaptureController::ScrollCaptureController(QWidget *parent)
    : QWidget(parent) {
  setObjectName("scrollCaptureBar");
  setAttribute(Qt::WA_TranslucentBackground);
  setStyleSheet(thumbnail::imageStyleSheet() + thumbnail::menuStyleSheet(palette()));
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(6);
  m_menu = new QWidget(this);
  m_menu->setObjectName("thumbnailMenu");
  m_menu->setAttribute(Qt::WA_StyledBackground);
  m_menu->setFixedWidth(thumbnail::kThumbnailMaxWidth);
  auto *menu = new QVBoxLayout(m_menu);
  menu->setContentsMargins(6, 6, 6, 6);
  menu->setSpacing(2);
  m_status = new QLabel(m_menu);
  m_status->setWordWrap(true);
  menu->addWidget(m_status);
  m_finish = new QPushButton(uiText("Finish"), m_menu);
  m_finish->setObjectName("scrollCaptureFinish");
  m_finish->setEnabled(false);
  auto *cancelButton = new QPushButton(uiText("Cancel"), m_menu);
  cancelButton->setObjectName("scrollCaptureCancel");
  menu->addWidget(m_finish);
  menu->addWidget(cancelButton);
  m_menu->hide();
  v->addWidget(m_menu, 0, Qt::AlignRight);
  m_preview = new QLabel(this);
  m_preview->setObjectName("thumbnailImage");
  m_preview->setAttribute(Qt::WA_StyledBackground);
  m_preview->setAlignment(Qt::AlignCenter);
  m_preview->setFixedSize(thumbnail::kThumbnailMaxWidth, thumbnail::kThumbnailMaxHeight);
  m_preview->setCursor(Qt::PointingHandCursor);
  m_preview->setAccessibleName(uiText("Long screenshot"));
  m_preview->setAccessibleDescription(tr("Click to finish; right-click for actions. Enter to finish, Escape to cancel."));
  m_preview->installEventFilter(this);
  v->addWidget(m_preview, 0, Qt::AlignRight);
  connect(m_finish, &QPushButton::clicked, this, &ScrollCaptureController::finish);
  connect(cancelButton, &QPushButton::clicked, this, &ScrollCaptureController::cancel);
  m_server = new QLocalServer(this);
  connect(m_server, &QLocalServer::newConnection, this, [this] {
    if (m_control) {
      m_server->nextPendingConnection()->deleteLater();
      return;
    }
    m_control = m_server->nextPendingConnection();
    connect(m_control, &QLocalSocket::readyRead, this,
            &ScrollCaptureController::readControl);
    connect(m_control, &QLocalSocket::disconnected, this, [this] {
      if (!m_done && !m_stopping) {
        setStatus(tr("Capture stopped; finish or cancel"));
        emit failed(tr("Scroll session disconnected"));
      }
    });
    const auto commands = std::exchange(m_commands, {});
    for (const auto &c : commands)
      command(c);
  });
  m_process = new QProcess(this);
  m_process->setProcessEnvironment(trustedProcessEnvironment());
  connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this, [this](int code, QProcess::ExitStatus status) {
            if (!m_done && (code || status != QProcess::NormalExit)) {
              emit failed(tr("Could not start scrolling capture") + ": " +
                          QString::fromUtf8(m_process->readAllStandardOutput() +
                                            m_process->readAllStandardError())
                              .left(2048));
              cancel();
            }
          });
  connect(m_process, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart && !m_done) {
              emit failed(tr("Could not start scrolling capture"));
              cancel();
            }
          });
  m_animation = new QVariantAnimation(this);
  connect(m_animation, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &v) {
            if (!m_editor)
              return;
            const qreal value = v.toReal();
            if (m_hiddenAnnotations)
              m_editor->setAnnotationPresentation(
                  false, 1 - value,
                  m_animation->property("direction").toDouble() * 12 * value);
            else
              m_editor->setAnnotationPresentation(true, value, 0);
          });
  m_idle.setSingleShot(true);
  m_watchdog.setSingleShot(true);
  connect(&m_watchdog, &QTimer::timeout, this, [this] {
    setStatus(tr("Capture timed out; finish or cancel"));
    command({{"command", "pause"}});
  });
  hide();
}
ScrollCaptureController::~ScrollCaptureController() { stop(); }
void ScrollCaptureController::setEditor(AnnotationEditor *editor) {
  m_editor = editor;
  QCoreApplication::instance()->installEventFilter(this);
}
bool ScrollCaptureController::prepare(const QRect &capture, QString &error,
                                      const QString &windowAddress) {
  if (!QGuiApplication::platformName().startsWith("wayland")) {
    error = tr("Scrolling capture requires a Wayland session");
    return false;
  }
  if (capture.width() < 64 || capture.height() < 96) {
    error = tr("Select a larger scrolling area");
    return false;
  }
  for (auto *screen : QGuiApplication::screens())
    if (screen->geometry().contains(capture)) {
      m_screen = screen;
      break;
    }
  m_hyprctl = trustedSystemProgram("hyprctl");
  if (!m_screen || m_hyprctl.isEmpty()) {
    error = tr("Select a scrolling area within one monitor");
    return false;
  }
  m_capture = capture;
  m_windowAddress = windowAddress;
  relayoutPreview();
  return true;
}
void ScrollCaptureController::updateTarget(const QRect &capture) {
  if (m_active || !m_screen || !m_screen->geometry().contains(capture) ||
      capture.width() < 64 || capture.height() < 96)
    return;
  m_capture = capture;
  command({{"command", "target"},
           {"rect", QJsonArray{capture.x(), capture.y(), capture.width(),
                               capture.height()}}});
}
void ScrollCaptureController::start() {
  if (!m_screen || m_done || m_exchange)
    return;
  m_id = QString::fromStdString(makeSessionId());
  m_exchange =
      std::make_unique<QTemporaryDir>(runtimeFile("scroll", "-XXXXXX"));
  if (!m_exchange->isValid()) {
    emit failed(tr("Could not prepare capture"));
    return;
  }
  const QByteArray socketPath =
      m_exchange->filePath("frames.sock").toLocal8Bit();
  m_listener =
      socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socketPath.size() >= int(sizeof(address.sun_path))) {
    cancel();
    return;
  }
  memcpy(address.sun_path, socketPath.constData(), socketPath.size() + 1);
  if (m_listener < 0 ||
      bind(m_listener, reinterpret_cast<sockaddr *>(&address),
           sizeof(address)) ||
      chmod(socketPath.constData(), 0600) || listen(m_listener, 1)) {
    cancel();
    return;
  }
  m_accept = new QSocketNotifier(m_listener, QSocketNotifier::Read, this);
  connect(m_accept, &QSocketNotifier::activated, this,
          &ScrollCaptureController::acceptFrames);
  const QString controlPath = m_exchange->filePath("control.sock");
  m_server->setSocketOptions(QLocalServer::UserAccessOption);
  if (!m_server->listen(controlPath)) {
    cancel();
    return;
  }
  m_worker = std::jthread([this](std::stop_token token) { work(token); });
  m_requestPath = m_exchange->filePath("start.json");
  const QJsonObject request{
      {"version", 1},
      {"id", m_id},
      {"socketPath", QString::fromLocal8Bit(socketPath)},
      {"controlPath", controlPath},
      {"windowAddress", m_windowAddress},
      {"rect", QJsonArray{m_capture.x(), m_capture.y(), m_capture.width(),
                          m_capture.height()}}};
  QFile file(m_requestPath);
  const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
      file.write(bytes) != bytes.size()) {
    cancel();
    return;
  }
  file.close();
  const auto literal =
      QJsonDocument(QJsonArray{m_requestPath}).toJson(QJsonDocument::Compact);
  m_process->start(
      m_hyprctl,
      {"eval",
       QString("hl.plugin.hyprcapture.scroll_session_start(%1)")
           .arg(QString::fromUtf8(literal.mid(1, literal.size() - 2)))});
}
void ScrollCaptureController::acceptFrames() {
  int peer =
      accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
  if (peer < 0)
    return;
  ucred creds{};
  socklen_t length = sizeof(creds);
  if (m_peer >= 0 ||
      getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &creds, &length) ||
      creds.uid != getuid()) {
    ::close(peer);
    return;
  }
  m_peer = peer;
  m_receive = new QSocketNotifier(peer, QSocketNotifier::Read, this);
  connect(m_receive, &QSocketNotifier::activated, this,
          &ScrollCaptureController::readFrames);
}
void ScrollCaptureController::readFrames() {
  for (int count = 0; count < 8; ++count) {
    unsigned char header[WINDOW_STREAM_FRAME_HEADER_BYTES];
    char ancillary[CMSG_SPACE(sizeof(int) * 4)]{};
    iovec vec{header, sizeof(header)};
    msghdr message{};
    message.msg_iov = &vec;
    message.msg_iovlen = 1;
    message.msg_control = ancillary;
    message.msg_controllen = sizeof(ancillary);
    const auto bytes =
        recvmsg(m_peer, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (bytes < 0) {
      if (errno != EAGAIN && errno != EINTR)
        m_receive->setEnabled(false);
      return;
    }
    if (bytes == 0) {
      m_receive->setEnabled(false);
      return;
    }
    QList<int> descriptors;
    for (auto *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c))
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
        const size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int *fds = reinterpret_cast<int *>(CMSG_DATA(c));
        for (size_t i = 0; i < n; ++i)
          descriptors.push_back(fds[i]);
      }
    const auto metadata = decodeWindowStreamFrameHeader(header, bytes);
    QImage frame;
    if (metadata && descriptors.size() == 1 &&
        !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
        metadata->sequence > m_sequence &&
        metadata->payloadBytes <= 128ULL * 1024 * 1024) {
      const int seals = fcntl(descriptors[0], F_GET_SEALS);
      constexpr int required =
          F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
      QFile file;
      if (seals >= 0 && (seals & required) == required &&
          file.open(descriptors[0], QIODevice::ReadOnly,
                    QFileDevice::DontCloseHandle))
        frame = mapRawRgba(file, metadata->pixelWidth, metadata->pixelHeight);
    }
    for (int fd : descriptors)
      ::close(fd);
    if (frame.isNull()) {
      setStatus(tr("Invalid capture frame"));
      continue;
    }
    m_sequence = metadata->sequence;
    m_watchdog.start(5000);
    {
      std::lock_guard lock(m_mutex);
      if (m_latest)
        ++m_dropped;
      m_latest = Frame{frame,
                       QRectF(metadata->logicalX, metadata->logicalY,
                              metadata->logicalWidth, metadata->logicalHeight)
                           .toAlignedRect(),
                       metadata->sequence, metadata->captureMonotonicNs};
    }
    m_ready.notify_one();
  }
}
void ScrollCaptureController::command(const QJsonObject &object) {
  if (m_done)
    return;
  if (!m_control) {
    if (m_commands.size() < 256)
      m_commands.push_back(object);
    return;
  }
  if (m_control->bytesToWrite() > 16384) {
    cancel();
    return;
  }
  m_control->write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
  m_control->flush();
}
void ScrollCaptureController::scroll(const QPointF &p, double delta,
                                     int discrete, bool finger, bool inverted) {
  command({{"command", "axis"},
           {"x", p.x()},
           {"y", p.y()},
           {"delta", delta},
           {"discrete", discrete},
           {"finger", finger},
           {"inverted", inverted}});
}
void ScrollCaptureController::exclude(const QList<QRect> &rects) {
  m_exclusions = rects;
  QJsonArray array;
  auto all = rects;
  if (m_editor)
    for (auto *child : m_editor->findChildren<QWidget *>(
             QString{}, Qt::FindDirectChildrenOnly)) {
      if (child->isVisible() && child->objectName() != "annotationCanvas")
        all.push_back(QRect(child->mapToGlobal(QPoint{}), child->size()));
    }
  if (m_active)
    all.push_back(QRect(mapToGlobal(QPoint{}), size()));
  for (const auto &r : all)
    array.append(QJsonArray{r.x(), r.y(), r.width(), r.height()});
  command({{"command", "exclude"}, {"rects", array}});
}
void ScrollCaptureController::readControl() {
  m_controlBytes += m_control->readAll();
  if (m_controlBytes.size() > 16384) {
    cancel();
    return;
  }
  while (m_controlBytes.contains('\n')) {
    const auto i = m_controlBytes.indexOf('\n');
    const auto j = QJsonDocument::fromJson(m_controlBytes.left(i)).object();
    m_controlBytes.remove(0, i + 1);
    const auto event = j["event"].toString();
    if (event == "begin" && !m_active) {
      m_active = true;
      if (m_editor)
        m_before = m_editor->snapshot();
      show();
      raise();
      exclude(m_exclusions);
      emit began();
    } else if (event == "motion")
      motion(j["delta"].toDouble());
    else if (event == "stopped") {
      setStatus(j["reason"].toString());
      emit failed(j["reason"].toString());
    }
  }
}
void ScrollCaptureController::motion(double delta) {
  if (delta == 0)
    return;
  m_lastInput.restart();
  m_idle.start(180);
  if (m_hiddenAnnotations)
    return;
  m_hiddenAnnotations = true;
  m_animation->stop();
  m_animation->setProperty("direction", delta > 0 ? -1. : 1.);
  m_animation->setDuration(140);
  m_animation->setStartValue(0.);
  m_animation->setEndValue(1.);
  m_animation->start();
  if (m_editor)
    m_editor->toolbarWidget()->hide();
}
void ScrollCaptureController::work(std::stop_token stopToken) {
  cv::setNumThreads(1);
  cv::ocl::setUseOpenCL(false);
  ScrollStitcher stitcher;
  QImage candidate;
  bool aligned = false;
  while (!stopToken.stop_requested()) {
    std::optional<Frame> frame;
    bool final = false;
    {
      std::unique_lock lock(m_mutex);
      m_ready.wait(lock, stopToken,
                   [this] { return m_latest.has_value() || m_finalize; });
      if (stopToken.stop_requested())
        break;
      frame = std::exchange(m_latest, {});
      final = m_finalize;
    }
    if (frame) {
      const bool stable = ScrollStitcher::stable(candidate, frame->image);
      candidate = frame->image;
      // Seed the unscrolled baseline immediately, before input is replayed.
      auto result = stitcher.append(frame->image);
      aligned = result.status == ScrollStitcher::Status::Started ||
                result.status == ScrollStitcher::Status::Appended ||
                result.status == ScrollStitcher::Status::Relocated ||
                result.status == ScrollStitcher::Status::Unchanged;
      auto preview = stitcher.preview({thumbnail::kThumbnailMaxWidth * 4, thumbnail::kThumbnailMaxHeight * 4});
      const auto layout = stitcher.layout();
      const int captures = stitcher.frameCount();
      QMetaObject::invokeMethod(
          this,
          [this, f = *frame, preview, layout, result, captures, stable] {
            present(f.image, f.geometry, preview, layout, result, captures,
                    stable, f.sequence, f.timeNs);
          },
          Qt::QueuedConnection);
    }
    if (final) {
      auto image = stitcher.image();
      const auto layout = stitcher.layout();
      QMetaObject::invokeMethod(
          this,
          [this, image, layout, aligned] {
            if (m_done)
              return;
            if (image.isNull()) {
              cancel();
              return;
            }
            if (!aligned)
              emit failed(tr("Final frame could not be aligned; keeping "
                             "confirmed content"));
            m_layout = layout;
            stop();
            m_done = true;
            emit completed(image);
          },
          Qt::QueuedConnection);
      break;
    }
  }
}
void ScrollCaptureController::present(QImage frame, QRect geometry,
                                      QImage preview, ScrollLayout layout,
                                      ScrollStitcher::Result result,
                                      int captures, bool stable,
                                      quint64 sequence, quint64 timeNs) {
  if (m_done)
    return;
  const bool aligned = result.status == ScrollStitcher::Status::Started ||
                       result.status == ScrollStitcher::Status::Appended ||
                       result.status == ScrollStitcher::Status::Relocated ||
                       result.status == ScrollStitcher::Status::Unchanged;
  if (result.status == ScrollStitcher::Status::Started)
    command({{"command", "baseline"}});
  if (aligned) {
    m_layout = layout;
    m_thumbnail = preview;
    m_finish->setEnabled(true);
    if (m_editor) {
      if (result.status == ScrollStitcher::Status::Started)
        m_editor->replaceCaptureImage(
            frame, QRect(m_editor->mapFromGlobal(geometry.topLeft()),
                         geometry.size()));
      m_editor->setScrollImage(frame, layout, true);
      preview = m_editor->annotatedPreview(preview, layout);
    }
    QPainter p(&preview);
    p.setPen(QPen(QColor("#79bfff"), 2));
    const qreal sx = qreal(preview.width()) / layout.frameSize.width(),
                sy = qreal(preview.height()) / layout.outputHeight();
    p.drawRect(QRectF(
        layout.viewportInImage().x() * sx, layout.viewportInImage().y() * sy,
        layout.content.width() * sx, layout.content.height() * sy));
    p.end();
    const auto pixmap = thumbnail::scaledPixmap(QPixmap::fromImage(preview), m_screen);
    m_preview->setPixmap(pixmap);
    m_preview->setFixedSize(pixmap.deviceIndependentSize().toSize());
    relayoutPreview();
    emit progress(QSize(layout.frameSize.width(), layout.outputHeight()),
                  captures);
    setStatus(tr("%1 × %2 · Scroll either way")
                  .arg(layout.frameSize.width())
                  .arg(layout.outputHeight()));
    // A result captured before the latest input must never restore annotations.
    const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    const bool fresh = !m_lastInput.isValid() ||
                       (m_lastInput.elapsed() >= 180 &&
                        qint64(timeNs) >= nowNs - m_lastInput.nsecsElapsed());
    if (stable && fresh && m_hiddenAnnotations && m_editor) {
      m_hiddenAnnotations = false;
      m_animation->stop();
      m_animation->setDuration(120);
      m_animation->setStartValue(0.);
      m_animation->setEndValue(1.);
      m_animation->start();
      m_editor->toolbarWidget()->show();
    }
    m_lastAcceptedSequence = sequence;
  } else {
    if (m_editor)
      m_editor->setAnnotationPresentation(false, 0, 0);
    m_hiddenAnnotations = true;
    setStatus(tr("Cannot align; scroll back slightly"));
    if (result.status == ScrollStitcher::Status::LimitReached ||
        result.status == ScrollStitcher::Status::SizeChanged) {
      command({{"command", "pause"}});
      setStatus(tr("Capture limit or geometry changed; finish or cancel"));
    }
  }
}
void ScrollCaptureController::setStatus(const QString &text) {
  m_status->setText(text);
  m_status->setAccessibleName(text);
  m_preview->setAccessibleDescription(text + tr(". Click to finish; right-click for actions. Enter to finish, Escape to cancel."));
  if (m_menu->isVisible()) relayoutPreview();
}
void ScrollCaptureController::relayoutPreview() {
  adjustSize();
  if (!m_screen) return;
  const QRect card = scrollControlsPlacement(m_capture, {m_screen->geometry()}, size());
  move(parentWidget() ? parentWidget()->mapFromGlobal(card.topLeft()) : card.topLeft());
  if (m_active) exclude(m_exclusions);
}
QRect ScrollCaptureController::previewGeometry() const {
  return QRect(m_preview->mapToGlobal(QPoint{}), m_preview->size());
}
void ScrollCaptureController::finish() {
  if (m_done || m_finishing || !m_finish->isEnabled())
    return;
  m_finishing = true;
  m_finish->setEnabled(false);
  QTimer::singleShot(250, this, [this] {
    if (m_done)
      return;
    command({{"command", "pause"}});
    {
      std::lock_guard lock(m_mutex);
      m_finalize = true;
    }
    m_ready.notify_one();
  });
}
void ScrollCaptureController::stop() {
  if (m_stopping)
    return;
  m_stopping = true;
  command({{"command", "stop"}});
  m_watchdog.stop();
  m_animation->stop();
  if (m_worker.joinable()) {
    m_worker.request_stop();
    m_ready.notify_all();
    m_worker.join();
  }
  if (m_receive)
    m_receive->setEnabled(false);
  if (m_accept)
    m_accept->setEnabled(false);
  if (m_peer >= 0) {
    ::close(m_peer);
    m_peer = -1;
  }
  if (m_listener >= 0) {
    ::close(m_listener);
    m_listener = -1;
  }
  if (m_control)
    m_control->disconnectFromServer();
  m_server->close();
  m_exchange.reset();
  hide();
}
void ScrollCaptureController::cancel() {
  if (m_done)
    return;
  stop();
  m_done = true;
  if (m_editor && m_before)
    m_editor->restore(m_before);
  emit cancelled();
}
void ScrollCaptureController::closeEvent(QCloseEvent *event) {
  cancel();
  event->accept();
}
bool ScrollCaptureController::eventFilter(QObject *object, QEvent *event) {
  auto *widget = qobject_cast<QWidget *>(object);
  if (m_editor && widget &&
      (widget == m_editor || m_editor->isAncestorOf(widget)) &&
      (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
       event->type() == QEvent::Move || event->type() == QEvent::Resize))
    QTimer::singleShot(0, this, [this] {
      if (!m_done && !m_stopping)
        exclude(m_exclusions);
    });
  if (object == m_preview && event->type() == QEvent::MouseButtonRelease) {
    const auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() == Qt::RightButton) {
      m_menu->setVisible(!m_menu->isVisible());
      relayoutPreview();
    } else if (mouse->button() == Qt::LeftButton) finish();
    return true;
  }
  return QWidget::eventFilter(object, event);
}
} // namespace hyprcapture::ui
