#pragma once

#include <QApplication>
#include <memory>

class QSocketNotifier;

namespace hyprcapture::ui {

class SessionLockParticipant {
  public:
    virtual ~SessionLockParticipant() = default;
    virtual void abortForSessionLock() = 0;
};

class SessionLockGuard;

class SessionLockApplication : public QApplication {
  public:
    using QApplication::QApplication;
    void setSessionLockGuard(SessionLockGuard* guard) { m_guard = guard; }
    bool notify(QObject* object, QEvent* event) override;

  private:
    SessionLockGuard* m_guard = nullptr;
};

// A lock or lost compositor connection is terminal for this helper process.
class SessionLockGuard : public QObject {
  public:
    explicit SessionLockGuard(int fd, QObject* parent = nullptr);
    ~SessionLockGuard() override;
    static int connectToCompositor();
    void setStartupControl(int fd) { m_startupFd = fd; }
    bool cancelled();
    bool filterEvent(QObject* object, QEvent* event);

  private:
    void cancel();
    int m_fd;
    int m_startupFd = -1;
    bool m_cancelled = false;
    std::unique_ptr<QSocketNotifier> m_notifier;
};

} // namespace hyprcapture::ui
