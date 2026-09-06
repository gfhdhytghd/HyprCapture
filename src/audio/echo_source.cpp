#include "audio/echo_source.hpp"
#include "audio/aec_policy.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QUuid>

namespace hyprcapture::audio {
struct EchoSource::Impl {
    QProcess worker;
    QString sourceName;
    bool broken = false;
    ~Impl() {
        worker.closeWriteChannel();
        if (!worker.waitForFinished(300)) { worker.kill(); worker.waitForFinished(300); }
    }
};
EchoSource::EchoSource() : m(std::make_unique<Impl>()) {}
EchoSource::~EchoSource() = default;
bool EchoSource::start(const QString& microphone, const QString& output, int model, const QString& backend, QString& error) {
    m->sourceName = QString("hyprcapture-aec-%1-%2").arg(QCoreApplication::applicationPid()).arg(QUuid::createUuid().toString(QUuid::Id128));
    m->worker.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    m->worker.start(aec::workerPath(), {"--serve", microphone, output, QString::number(model), backend, m->sourceName});
    if (!m->worker.waitForStarted(500)) { error = "DTLN worker unavailable"; return false; }
    return true;
}
void EchoSource::iterate() {
    if (m->worker.state() == QProcess::NotRunning) m->broken = true;
    const auto bytes = m->worker.readAllStandardOutput();
    for (const auto& line : bytes.split('\n')) if (QJsonDocument::fromJson(line).object().contains("error")) m->broken = true;
}
bool EchoSource::failed() const { return m->broken; }
QString EchoSource::name() const { return m->sourceName; }
}
