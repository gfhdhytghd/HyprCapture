#include "ui/shot_result.hpp"
#include "shared/shot_channel.hpp"
#include "shared/protocol.hpp"
#include "ui/clipboard_utils.hpp"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QBuffer>
#include <QColorSpace>
#include <QImageWriter>
#include <poll.h>
#include <fcntl.h>

namespace hyprcapture::ui {
namespace { int resultFd = -1; bool finished = false; std::function<bool()> cancellationCheck; }
void enableShotResult(int fd) {
    resultFd = fd;
    finished = false;
    cancellationCheck = {};
    if (fd < 0) return;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}
bool shotResultEnabled() { return resultFd >= 0; }
void setShotCancellationCheck(std::function<bool()> check) { cancellationCheck = std::move(check); }
bool shotPeerCancelled() {
    if (!shotResultEnabled()) return false;
    char byte;
    const auto size = recv(resultFd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return size >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}
void discardShotSession(const QString& json) {
    const auto session = decodeSessionJson(json.toStdString());
    if (!session) return;
    QStringList parents;
    const auto remove = [&](const std::string& path) {
        const auto name = QString::fromStdString(path);
        if (!path.empty() && isPrivateRuntimePath(name)) {
            QFile::remove(name);
            parents.append(QFileInfo(name).absolutePath());
        }
    };
    for (const auto& monitor : session->monitors) { remove(monitor.artifactPath); remove(monitor.cursorArtifactPath); }
    for (const auto& window : session->windows) { remove(window.artifactPath); remove(window.realBackgroundPath); }
    for (const auto& parent : parents)
        if (parent != QString::fromStdString(shot::runtimeRoot())) QDir().rmdir(parent);
}
bool finishShotResult(unsigned char status, const QImage& image, const QString& error) {
    if (!shotResultEnabled() || finished) return false;
    finished = true;
    QByteArray payload;
    if (status == 0) {
        QBuffer buffer(&payload); buffer.open(QIODevice::WriteOnly);
        QImage tagged = image; tagged.setColorSpace(QColorSpace::SRgb);
        QImageWriter writer(&buffer, "PNG"); writer.setQuality(75);
        if (!writer.write(tagged)) { status = 2; payload = "could not encode SDR PNG"; }
    } else if (status == 2) payload = error.toUtf8();
    const auto header = shot::header(status, payload.size());
    // Cancellation/peer loss must interrupt a blocked receiver, including large PNGs.
    const auto sendAll = [status](const char* data, size_t size) {
        while (size) {
            if (status == 0 && cancellationCheck && cancellationCheck()) return false;
            char command;
            if (recv(resultFd, &command, 1, MSG_PEEK | MSG_DONTWAIT) >= 0) return false;
            const auto sent = send(resultFd, data, size, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent > 0) { data += sent; size -= sent; continue; }
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
            pollfd p{resultFd, POLLIN | POLLOUT, 0};
            if (poll(&p, 1, 100) < 0 && errno != EINTR) return false;
        }
        return true;
    };
    const bool ok = sendAll(header.data(), header.size()) && sendAll(payload.constData(), payload.size());
    // EOF comes from helper exit, after overlay destruction and artifact cleanup.
    return ok && status == 0;
}
}
