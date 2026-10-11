#include "ui/session_lock_guard.hpp"

#include <QEvent>
#include <QWidget>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace {
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

class Observer : public QObject {
  public:
    int seen = 0;
    bool event(QEvent*) override { ++seen; return true; }
    bool eventFilter(QObject*, QEvent*) override { ++seen; return false; }
};

class Overlay : public QWidget, public hyprcapture::ui::SessionLockParticipant {
  public:
    bool aborted = false;
    void abortForSessionLock() override { aborted = true; }
};
}

int main(int argc, char** argv) {
    using namespace hyprcapture::ui;
    SessionLockApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    for (bool disconnect : {false, true}) {
        int fds[2];
        require(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) == 0, "create control channel");
        SessionLockGuard guard(fds[1]);
        Observer target, laterFilter;
        app.installEventFilter(&laterFilter);
        Overlay overlay;
        overlay.show();
        QEvent before(QEvent::KeyPress);
        QCoreApplication::sendEvent(&target, &before);
        require(target.seen == 1 && laterFilter.seen > 0, "unlocked input reaches application filters and target");
        target.seen = laterFilter.seen = 0;
        if (!disconnect) {
            const auto sent = send(fds[0], "locked", 6, MSG_NOSIGNAL);
            if (sent < 0 && errno == EPERM)
                std::cerr << "Lock packet delivery blocked by sandbox; testing terminal EOF instead\n";
            else
                require(sent == 6, "queue lock before notifier dispatch");
        }
        close(fds[0]);
        for (auto type : {QEvent::KeyPress, QEvent::KeyRelease, QEvent::Shortcut, QEvent::InputMethod,
                          QEvent::MouseMove, QEvent::MouseButtonPress, QEvent::MouseButtonRelease, QEvent::Wheel,
                          QEvent::TouchBegin, QEvent::TouchUpdate, QEvent::TouchEnd, QEvent::NativeGesture,
                          QEvent::Gesture, QEvent::TabletMove, QEvent::TabletPress, QEvent::TabletRelease,
                          QEvent::FocusIn, QEvent::WindowActivate, QEvent::Timer, QEvent::MetaCall, QEvent::SockAct,
                          QEvent::Close, QEvent::ContextMenu, QEvent::Drop}) {
            QEvent event(type);
            QCoreApplication::sendEvent(&target, &event);
        }
        require(guard.cancelled() && target.seen == 0, "queued input and action callbacks are discarded on lock/EOF");
        // Cancellation itself emits hide/disable events. A later filter must
        // still never get the locked input delivered to our target.
        const int seen = laterFilter.seen;
        QEvent wheel(QEvent::Wheel);
        QCoreApplication::sendEvent(&target, &wheel);
        require(laterFilter.seen == seen, "lock gate runs before later application filters");
        require(!overlay.isVisible() && !overlay.isEnabled(), "overlay is hidden and disabled without closing/committing");
        require(overlay.aborted, "in-progress overlay work is aborted synchronously");
        overlay.show();
        require(!overlay.isVisible(), "cancelled helper cannot remap after unlock");
        app.removeEventFilter(&laterFilter);
    }
    {
        SessionLockGuard unavailable(-1);
        require(unavailable.cancelled(), "missing compositor channel fails closed");
    }
}
