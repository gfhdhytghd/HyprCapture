#pragma once
#include <QImage>
#include <QString>
#include <functional>
namespace hyprcapture::ui {
// Process-local output sink. Only explicitly opted-in helpers use it.
void enableShotResult(int fd);
bool shotResultEnabled();
void setShotCancellationCheck(std::function<bool()> check);
bool shotPeerCancelled();
void discardShotSession(const QString& json);
bool finishShotResult(unsigned char status, const QImage& image = {}, const QString& error = {});
}
