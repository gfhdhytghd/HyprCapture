#include "ui/session_lock_guard.hpp"
#include "shared/session_lock.hpp"

#include <LayerShellQt/Window>
#include <QApplication>
#include <QEvent>
#include <QSocketNotifier>
#include <QWidget>
#include <QWindow>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace hyprcapture::ui {

bool SessionLockApplication::notify(QObject* object, QEvent* event) {
    // Run before application/widget filters installed by editors or scrolling.
    if (m_guard && m_guard->filterEvent(object, event)) return true;
    return QApplication::notify(object, event);
}

int SessionLockGuard::connectToCompositor() {
    const auto path = sessionLockSocketPath();
    if (path.empty() || path.size() >= sizeof(sockaddr_un{}.sun_path)) return -1;
    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    pollfd descriptor{fd, POLLIN, 0};
    char state[64]{};
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 || credentials.uid != geteuid() ||
        poll(&descriptor, 1, 10000) <= 0) {
        close(fd);
        return -1;
    }
    const auto bytes = recv(fd, state, sizeof(state), MSG_DONTWAIT | MSG_TRUNC);
    const QByteArray expected = qgetenv("HYPRCAPTURE_LOCK_GENERATION");
    const auto generation = bytes > 0 && bytes <= static_cast<ssize_t>(sizeof(state))
        ? unlockedSessionGeneration(std::string_view(state, bytes), std::string_view(expected.constData(), expected.size())) : std::nullopt;
    if (!generation) {
        close(fd);
        return -1;
    }
    // Descendants (pins/thumbnails) inherit the generation of their capture.
    // A delayed child cannot become a fresh session after lock or plugin reload.
    qputenv("HYPRCAPTURE_LOCK_GENERATION", QByteArray::fromStdString(*generation));
    return fd;
}

SessionLockGuard::SessionLockGuard(int fd, QObject* parent) : QObject(parent), m_fd(fd) {
    if (auto* app = dynamic_cast<SessionLockApplication*>(qApp)) app->setSessionLockGuard(this);
    if (fd >= 0) {
        m_notifier = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read, this);
        connect(m_notifier.get(), &QSocketNotifier::activated, this, [this] { cancelled(); });
    }
}

SessionLockGuard::~SessionLockGuard() {
    if (auto* app = dynamic_cast<SessionLockApplication*>(qApp)) app->setSessionLockGuard(nullptr);
    m_notifier.reset();
    if (m_fd >= 0) close(m_fd);
}

bool SessionLockGuard::cancelled() {
    if (!m_cancelled && m_startupFd >= 0) {
        char command[8];
        const auto bytes = recv(m_startupFd, command, sizeof(command), MSG_PEEK | MSG_DONTWAIT | MSG_TRUNC);
        // A helper still initializing can connect after a fast lock/unlock.
        // Its original capture channel must retain that cancellation too.
        if (bytes == 6 && std::memcmp(command, "cancel", 6) == 0) cancel();
    }
    if (!m_cancelled) {
        char state[16];
        const auto bytes = m_fd < 0 ? 0 : recv(m_fd, state, sizeof(state), MSG_PEEK | MSG_DONTWAIT);
        // Any packet after the handshake, EOF, or error closes the helper.
        if (bytes >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) cancel();
    }
    return m_cancelled;
}

void SessionLockGuard::cancel() {
    if (m_cancelled) return;
    m_cancelled = true;
    if (m_notifier) m_notifier->setEnabled(false);
    // Do not call close(): an editor close handler can commit/export its image.
    if (auto* grabber = QWidget::mouseGrabber()) grabber->releaseMouse();
    if (auto* grabber = QWidget::keyboardGrabber()) grabber->releaseKeyboard();
    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        if (auto* layer = LayerShellQt::Window::get(window)) {
            layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
            layer->setActivateOnShow(false);
        }
        window->hide();
    }
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        widget->setEnabled(false);
        widget->hide();
        if (auto* participant = dynamic_cast<SessionLockParticipant*>(widget)) participant->abortForSessionLock();
    }
    qApp->quit();
    // Cancellation can arrive while constructing widgets before exec().
    QCoreApplication::postEvent(qApp, new QEvent(QEvent::Quit));
}

bool SessionLockGuard::filterEvent(QObject* object, QEvent* event) {
    // Check the socket before dispatching even already-queued Wayland input.
    // Also cancel before timers/async callbacks can finish a pending action.
    if (!cancelled()) return false;
    switch (event->type()) {
        case QEvent::Show:
            if (auto* widget = qobject_cast<QWidget*>(object)) widget->hide();
            if (auto* window = qobject_cast<QWindow*>(object)) {
                if (auto* layer = LayerShellQt::Window::get(window)) {
                    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
                    layer->setActivateOnShow(false);
                }
                window->hide();
            }
            return true;
        case QEvent::KeyPress: case QEvent::KeyRelease: case QEvent::Shortcut: case QEvent::ShortcutOverride:
        case QEvent::InputMethod: case QEvent::MouseMove: case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease: case QEvent::MouseButtonDblClick: case QEvent::Wheel:
        case QEvent::TouchBegin: case QEvent::TouchUpdate: case QEvent::TouchEnd: case QEvent::TouchCancel:
        case QEvent::NativeGesture: case QEvent::Gesture: case QEvent::GestureOverride:
        case QEvent::TabletMove: case QEvent::TabletPress: case QEvent::TabletRelease:
        case QEvent::TabletEnterProximity: case QEvent::TabletLeaveProximity:
        case QEvent::FocusIn: case QEvent::WindowActivate: case QEvent::Timer: case QEvent::MetaCall:
        case QEvent::SockAct: case QEvent::Close: case QEvent::ContextMenu:
        case QEvent::Enter: case QEvent::Leave: case QEvent::HoverEnter: case QEvent::HoverMove: case QEvent::HoverLeave:
        case QEvent::DragEnter: case QEvent::DragMove: case QEvent::Drop:
            return true;
        default: return false;
    }
}
} // namespace hyprcapture::ui
