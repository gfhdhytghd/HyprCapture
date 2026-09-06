#pragma once
#include <QString>
#include <memory>

namespace hyprcapture::audio {
// Owns the isolated DTLN worker. PipeWire monitor mode observes playback without
// moving applications; a worker failure returns capture to the raw microphone.
class EchoSource {
public:
    EchoSource();
    ~EchoSource();
    bool start(const QString& microphone, const QString& output, int model, const QString& backend, QString& error);
    void iterate();
    bool failed() const;
    QString name() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
}
