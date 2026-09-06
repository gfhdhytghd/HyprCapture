#pragma once
#include <QString>
#include <memory>

namespace hyprcapture::audio {
// Owns PipeWire's mature WebRTC echo-cancel module in this helper process.
// monitor.mode observes existing playback; no applications are rerouted.
class EchoSource {
public:
    EchoSource();
    ~EchoSource();
    bool start(const QString& microphone, const QString& output, QString& error);
    void iterate();
    bool failed() const;
    QString name() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
}
