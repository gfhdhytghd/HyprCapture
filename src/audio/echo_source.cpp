#include "audio/echo_source.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>
#include <cerrno>
#include <cstring>

namespace hyprcapture::audio {
struct EchoSource::Impl {
    pw_main_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_impl_module* module = nullptr;
    spa_hook listener{};
    QString sourceName;
    bool broken = false;
    ~Impl() {
        if (module) { spa_hook_remove(&listener); pw_impl_module_destroy(module); }
        if (context) pw_context_destroy(context);
        if (loop) pw_main_loop_destroy(loop);
    }
};
EchoSource::EchoSource() : m(std::make_unique<Impl>()) {}
EchoSource::~EchoSource() = default;
bool EchoSource::start(const QString& microphone, const QString& output, QString& error) {
    pw_init(nullptr, nullptr);
    m->loop = pw_main_loop_new(nullptr);
    if (m->loop) m->context = pw_context_new(pw_main_loop_get_loop(m->loop), pw_properties_new(PW_KEY_CONFIG_NAME, "client-rt.conf", nullptr), 0);
    if (!m->context) { error = "PipeWire client initialization failed"; return false; }
    m->sourceName = QString("hyprcapture-aec-%1").arg(QCoreApplication::applicationPid());
    const QJsonObject capture{{"target.object", microphone}, {"node.dont-fallback", true}, {"node.dont-reconnect", true}, {"node.passive", true}};
    const QJsonObject reference{{"target.object", output}, {"node.dont-fallback", true}, {"node.dont-reconnect", true}, {"node.passive", true}};
    const QJsonObject source{{"node.name", m->sourceName}, {"node.description", "HyprCapture echo-cancelled microphone"},
                             {"node.virtual", true}, {"priority.session", 0}, {"priority.driver", 0}};
    // Keep level control in HyprCapture's existing presets; the AEC switch
    // must not silently enable WebRTC's aggressive noise suppression or AGC.
    const QJsonObject processing{{"webrtc.noise_suppression", false}, {"webrtc.gain_control", false},
                                 {"webrtc.voice_detection", false}, {"webrtc.transient_suppression", false}};
    const QJsonObject config{{"aec.args", processing}, {"library.name", "aec/libspa-aec-webrtc"}, {"monitor.mode", true}, {"audio.rate", 48000},
                             {"capture.props", capture}, {"sink.props", reference}, {"source.props", source}};
    const auto args = QJsonDocument(config).toJson(QJsonDocument::Compact);
    m->module = pw_context_load_module(m->context, "libpipewire-module-echo-cancel", args.constData(), nullptr);
    if (!m->module) { error = "PipeWire WebRTC module: " + QString::fromLocal8Bit(strerror(errno)); return false; }
    static const pw_impl_module_events events = {
        .version = PW_VERSION_IMPL_MODULE_EVENTS,
        .destroy = [](void* data) { auto& impl = *static_cast<Impl*>(data); spa_hook_remove(&impl.listener); impl.module = nullptr; impl.broken = true; },
    };
    pw_impl_module_add_listener(m->module, &m->listener, &events, m.get());
    return true;
}
void EchoSource::iterate() {
    if (m->loop && pw_loop_iterate(pw_main_loop_get_loop(m->loop), 0) < 0) m->broken = true;
}
bool EchoSource::failed() const { return m->broken; }
QString EchoSource::name() const { return m->sourceName; }
}
