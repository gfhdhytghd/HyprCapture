#pragma once

#include "ui/clipboard_utils.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QString>
#include <chrono>
#include <cstdio>
#include <utility>

namespace hyprcapture::ui {

inline bool timingEnabled() {
    return qEnvironmentVariableIsSet("HYPRCAPTURE_TIMING") || qEnvironmentVariableIsSet("HYPRCAPTURE_TIMING_FILE");
}

inline void traceTiming(const QString& event, qint64 elapsedMs = -1, qint64 elapsedUs = -1) {
    if (!timingEnabled())
        return;
    const auto monotonicUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    QString line = QStringLiteral("%1 pid=%2 %3 monotonic_us=%4")
                       .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))
                       .arg(QCoreApplication::applicationPid())
                       .arg(event)
                       .arg(monotonicUs);
    if (elapsedMs >= 0)
        line += QStringLiteral(" elapsed_ms=%1").arg(elapsedMs);
    if (elapsedUs >= 0)
        line += QStringLiteral(" elapsed_us=%1").arg(elapsedUs);
    line += QLatin1Char('\n');
    const QString path = qEnvironmentVariable("HYPRCAPTURE_TIMING_FILE");
    if (!path.isEmpty()) {
        QFile file(path);
        if (isPrivateRuntimePath(path) && file.open(QIODevice::WriteOnly | QIODevice::Append))
            file.write(line.toUtf8());
        return;
    }
    fputs(line.toLocal8Bit().constData(), stderr);
}

class ScopedUiTiming {
  public:
    explicit ScopedUiTiming(QString event, bool enabled = true) : m_event(std::move(event)), m_enabled(enabled && timingEnabled()) {
        if (m_enabled)
            m_started = std::chrono::steady_clock::now();
    }
    ~ScopedUiTiming() {
        if (m_enabled) {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_started).count();
            traceTiming(m_event, us / 1000, us);
        }
    }
  private:
    QString m_event;
    bool m_enabled;
    std::chrono::steady_clock::time_point m_started;
};

} // namespace hyprcapture::ui
