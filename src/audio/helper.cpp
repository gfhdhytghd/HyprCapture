#include "audio/helper.hpp"
#include "audio/echo_source.hpp"
#include "audio/aec_policy.hpp"
#include "shared/audio_timeline.hpp"
#include "shared/trusted_path.hpp"
#include <pulse/pulseaudio.h>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSocketNotifier>
#include <QTimer>
#include <array>
#include <cmath>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <map>
#include <set>
#include <QRegularExpression>
#include <sys/stat.h>
#include <unistd.h>

namespace hyprcapture::audio {
namespace {
void report(const QString& message) {
    const auto data = QJsonDocument(QJsonObject{{"error", message}}).toJson(QJsonDocument::Compact) + '\n';
    (void)!write(STDOUT_FILENO, data.constData(), data.size());
}
bool privateDirectory(const QString& path) {
    struct stat st{};
    const auto bytes = QFile::encodeName(path);
    return lstat(bytes.constData(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() && (st.st_mode & 0077) == 0;
}
bool regularOwnedFile(const QString& path) {
    struct stat st{};
    const auto bytes = QFile::encodeName(path);
    return lstat(bytes.constData(), &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid();
}
QString program(const char* name) {
    for (const auto& dir : qEnvironmentVariable("PATH").split(':')) {
        if (!QDir::isAbsolutePath(dir)) continue;
        if (auto path = security::trustedExecutablePath((dir + "/" + name).toStdString()))
            return QString::fromStdString(*path);
    }
    return {};
}
QJsonArray windowSources() {
    const auto hyprctl = program("hyprctl");
    if (hyprctl.isEmpty()) return {};
    QProcess query;
    query.start(hyprctl, {"-j", "clients"});
    if (!query.waitForFinished(2000) || query.exitCode() != 0 || query.exitStatus() != QProcess::NormalExit) {
        query.kill(); query.waitForFinished(1000); return {};
    }
    const auto bytes = query.readAllStandardOutput();
    if (bytes.size() > 8 * 1024 * 1024) return {};
    QJsonArray result;
    for (const auto& value : QJsonDocument::fromJson(bytes).array()) {
        const auto window = value.toObject();
        const auto address = window["address"].toString();
        const auto pid = window["pid"].toInt();
        if (!QRegularExpression("^0x[0-9a-fA-F]+$").match(address).hasMatch() || pid <= 0) continue;
        auto title = window["title"].toString().left(256);
        const auto app = window["class"].toString().left(128);
        if (title.isEmpty()) title = app;
        result.append(QJsonObject{{"name", "window:" + address}, {"description", "Window · " + title + " — " + app}, {"pid", pid}});
        if (result.size() >= 512) break;
    }
    return result;
}
struct ProcessIdentity { int parent = 0; QByteArray start; };
ProcessIdentity processIdentity(int pid) {
    if (pid <= 1) return {};
    const auto path = QString("/proc/%1/stat").arg(pid);
    if (QFileInfo(path).ownerId() != getuid()) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.read(8192);
    const auto fields = bytes.mid(bytes.lastIndexOf(')') + 2).simplified().split(' ');
    return {fields.value(1).toInt(), fields.value(19)};
}
bool processBelongsTo(int pid, int target) {
    // Audio often runs in a browser/application subprocess. Do not match by a
    // human-readable application name: unrelated applications can share names.
    for (int depth = 0; pid > 1 && depth < 64; ++depth) {
        if (pid == target) return true;
        const auto identity = processIdentity(pid);
        if (identity.parent == pid) break;
        pid = identity.parent;
    }
    return false;
}
struct Device {
    QString name, description, monitor;
    uint32_t index = PA_INVALID_INDEX;
};
struct Recorder;
struct Track {
    Recorder* owner = nullptr;
    pa_stream* stream = nullptr;
    QString device, role;
    int fd = -1;
    uint32_t sourceIndex = PA_INVALID_INDEX, sinkInput = PA_INVALID_INDEX, sinkIndex = PA_INVALID_INDEX;
    std::int64_t next = -1, lastTrace = 0;
    double peak = 0, energy = 0;
    size_t samples = 0;
    bool failed = false;
};
struct Recorder {
    pa_mainloop* loop = nullptr;
    pa_context* context = nullptr;
    std::vector<Device> sinks, sources;
    QString defaultSink, defaultSource, directory, mode, output, input;
    std::array<Track, 2> tracks;
    std::int64_t origin = monotonicUs();
    bool meterOnly = false;
    QTimer meterTimer, applicationTimer, echoPoll;
    std::unique_ptr<EchoSource> echo;
    bool echoEnabled = false, echoStarting = false, echoActive = false;
    int echoPolicy = 0, echoModel = 0;
    QString echoBackend = "cpu";
    uint32_t echoMicrophoneIndex = PA_INVALID_INDEX, echoOutputIndex = PA_INVALID_INDEX;
    QString originalMicrophone;

    QJsonArray windows;
    int applicationPid = 0;
    QByteArray applicationStart;
    bool applicationSource = false, scanning = false, scanAgain = false;
    std::map<uint32_t, std::unique_ptr<Track>> applicationTracks;
    std::set<uint32_t> scannedInputs;
    // One second of timestamped stereo audio for the summed live meter.
    struct MeterFrame { std::int64_t position = -1; float left = 0, right = 0; };
    std::vector<MeterFrame> meterFrames = std::vector<MeterFrame>(48000);
    std::int64_t meterNext = 0;

    bool listOnly = false, initialized = false, serverFailed = false;
    int pending = 3;
    QTimer pump;

    ~Recorder() {
        pump.stop();
        if (context) pa_context_set_state_callback(context, nullptr, nullptr);
        for (auto& [_, t] : applicationTracks) closeTrack(*t);
        for (auto& t : tracks) closeTrack(t);
        if (context) { pa_context_disconnect(context); pa_context_unref(context); }
        if (loop) pa_mainloop_free(loop);
    }
    void closeTrack(Track& t) {
        if (t.stream) {
            pa_stream_set_state_callback(t.stream, nullptr, nullptr);
            pa_stream_set_read_callback(t.stream, nullptr, nullptr);
            pa_stream_set_moved_callback(t.stream, nullptr, nullptr);
            pa_stream_disconnect(t.stream); pa_stream_unref(t.stream); t.stream = nullptr;
        }
        if (t.fd >= 0) { close(t.fd); t.fd = -1; }
    }
    void scanApplication() {
        if (!initialized || !applicationSource || applicationPid <= 1 || serverFailed || tracks[0].failed) return;
        if (scanning) { scanAgain = true; return; }
        if (processIdentity(applicationPid).start != applicationStart) {
            for (auto& [_, t] : applicationTracks) closeTrack(*t);
            applicationTracks.clear(); fail(tracks[0], "window application exited"); return;
        }
        scanning = true; scannedInputs.clear();
        auto* op = pa_context_get_sink_input_info_list(context, [](pa_context*, const pa_sink_input_info* info, int eol, void* data) {
            auto& r = *static_cast<Recorder*>(data);
            if (eol) {
                if (eol > 0) {
                    for (auto it = r.applicationTracks.begin(); it != r.applicationTracks.end();) {
                        if (!r.scannedInputs.contains(it->first)) { r.closeTrack(*it->second); it = r.applicationTracks.erase(it); }
                        else ++it;
                    }
                }
                r.scanning = false;
                if (r.scanAgain) { r.scanAgain = false; r.scanApplication(); }
                return;
            }
            if (!info) return;
            const auto* property = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_ID);
            const int pid = property ? QByteArray(property).toInt() : 0;
            if (!processBelongsTo(pid, r.applicationPid)) return;
            r.scannedInputs.insert(info->index);
            auto found = r.applicationTracks.find(info->index);
            if (found != r.applicationTracks.end()) {
                if (found->second->sinkIndex == info->sink) return;
                r.closeTrack(*found->second); r.applicationTracks.erase(found);
            }
            if (r.applicationTracks.size() >= 64) return;
            auto track = std::make_unique<Track>();
            track->owner = &r; track->role = "System"; track->sinkInput = info->index; track->sinkIndex = info->sink;
            r.applicationTracks[info->index] = std::move(track);
            // Resolve the actual sink monitor; never substitute the default sink.
            auto* sinkOp = pa_context_get_sink_info_by_index(r.context, info->sink, [](pa_context*, const pa_sink_info* sink, int, void* data) {
                auto& r = *static_cast<Recorder*>(data);
                if (!sink) return;
                for (auto& [_, t] : r.applicationTracks) {
                    if (t->sinkIndex == sink->index && !t->stream && !t->failed) {
                        r.openTrack(*t, "System", QString::fromUtf8(sink->monitor_source_name));
                        t->sourceIndex = sink->monitor_source;
                    }
                }
            }, &r);
            if (sinkOp) pa_operation_unref(sinkOp);
        }, this);
        if (op) pa_operation_unref(op); else scanning = false;
    }
    void addApplicationSamples(Track& t, const float* values, size_t count, std::int64_t position) {
        if (meterOnly) {
            for (size_t i = 0; i + 1 < count; i += 2) {
                const auto frame = position + i / 2;
                if (frame < meterNext || frame >= meterNext + static_cast<std::int64_t>(meterFrames.size())) continue;
                auto& slot = meterFrames[frame % meterFrames.size()];
                if (slot.position != frame) slot = {frame, 0, 0};
                slot.left += std::isfinite(values[i]) ? values[i] : 0;
                slot.right += std::isfinite(values[i+1]) ? values[i+1] : 0;
            }
            return;
        }
        // Callbacks run serially on this helper's main loop. Add streams into one
        // private sparse PCM file without moving or muting the real playback.
        std::array<float, 2048> mixed;
        size_t consumed = 0;
        while (consumed < count) {
            const auto n = std::min(mixed.size(), count - consumed);
            std::fill(mixed.begin(), mixed.end(), 0);
            const auto offset = position * frameBytes + consumed * sizeof(float);
            size_t readBytes = 0;
            while (readBytes < n * sizeof(float)) {
                const auto got = pread(t.fd, reinterpret_cast<char*>(mixed.data()) + readBytes, n * sizeof(float) - readBytes, offset + readBytes);
                if (got < 0 && errno == EINTR) continue;
                if (got < 0) { fail(t, "audio mix read failed"); return; }
                if (got == 0) break;
                readBytes += got;
            }
            for (size_t i = 0; i < n; ++i) mixed[i] += std::isfinite(values[consumed+i]) ? values[consumed+i] : 0;
            size_t written = 0;
            while (written < n * sizeof(float)) {
                const auto put = pwrite(t.fd, reinterpret_cast<char*>(mixed.data()) + written, n * sizeof(float) - written, offset + written);
                if (put < 0 && errno == EINTR) continue;
                if (put <= 0) { fail(t, "audio mix write failed"); return; }
                written += put;
            }
            consumed += n;
        }
    }
    void echoStatus(const QString& state) {
        const auto data = QJsonDocument(QJsonObject{{"aec", state}, {"model", echoModel}, {"backend", echoBackend}}).toJson(QJsonDocument::Compact) + '\n';
        (void)!write(STDOUT_FILENO, data.constData(), data.size());
    }
    void fallbackEcho(const QString& reason) {
        if (!echoStarting && !echoActive) return;
        echoPoll.stop(); echoStarting = echoActive = false;
        auto& mic = tracks[1];
        const int fd = mic.fd; mic.fd = -1; closeTrack(mic); mic = Track{}; mic.fd = fd;
        echo.reset();
        report("Echo cancellation unavailable (" + reason + "); microphone continues without AEC");
        echoStatus("unavailable");
        openTrack(mic, "Microphone", originalMicrophone);
    }
    bool startEcho(const QString& microphone) {
        originalMicrophone = microphone;
        const auto decision = aec::selection(echoPolicy, echoBackend);
        echoModel = decision["model"].toInt();
        if (!echoModel) {
            auto status = decision; status["description"] = aec::description(decision, echoPolicy);
            const auto bytes = QJsonDocument(status).toJson(QJsonDocument::Compact) + '\n';
            (void)!write(STDOUT_FILENO, bytes.constData(), bytes.size());
            if (decision["aec"] == "pending") {
                // Quick recordings may never create an overlay. Validate for the
                // next recording without changing this recording's raw path.
                auto* check = new QProcess(QCoreApplication::instance());
                QObject::connect(check, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), check, &QObject::deleteLater);
                QObject::connect(check, &QProcess::errorOccurred, check, [check](QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart) check->deleteLater();
                });
                QStringList arguments{"--install"}; if (echoBackend == "npu") arguments << "--npu";
                check->start(aec::workerPath(), arguments);
            }
            return false;
        }
        QString outputName = (applicationSource || output == "auto" || output == "default") ? defaultSink : output;
        bool found = false;
        for (const auto& sink : sinks) if (sink.name == outputName) { found = true; echoOutputIndex = sink.index; }
        for (const auto& source : sources) if (source.name == microphone) echoMicrophoneIndex = source.index;
        QString error;
        echo = std::make_unique<EchoSource>();
        if (!found || !echo->start(microphone, outputName, echoModel, echoBackend, error)) {
            echo.reset(); report("Echo cancellation unavailable: " + (found ? error : "playback output not found") + "; microphone continues without AEC");
            echoStatus("unavailable"); return false;
        }
        echoStarting = true; echoStatus("starting");
        QObject::connect(&echoPoll, &QTimer::timeout, [&] {
            if (!echoStarting || !echo || !context) return;
            auto* op = pa_context_get_source_info_by_name(context, echo->name().toUtf8().constData(), [](pa_context*, const pa_source_info* info, int, void* data) {
                auto& r = *static_cast<Recorder*>(data);
                if (!info || !r.echoStarting) return;
                r.echoPoll.stop(); r.echoStarting = false; r.echoActive = true;
                r.openTrack(r.tracks[1], "Microphone", QString::fromUtf8(info->name));
                r.tracks[1].sourceIndex = info->index;
                r.echoStatus("active");
            }, this);
            if (op) pa_operation_unref(op);
        });
        echoPoll.start(30);
        QTimer::singleShot(3000, [&] { if (echoStarting) fallbackEcho("source startup timed out"); });
        return true;
    }
    void fail(Track& t, const QString& reason) {
        if (t.failed) return;
        t.failed = true;
        if (&t == &tracks[1] && echoActive) {
            QTimer::singleShot(0, [this, reason] { fallbackEcho(reason); }); return;
        }
        report(t.role + ": " + reason + "; video recording continues");
    }
    void serverError() {
        if (serverFailed) return;
        serverFailed = true;
        if (listOnly) { report("Audio server unavailable"); QCoreApplication::exit(1); return; }
        for (auto& t : tracks) if (!t.role.isEmpty()) fail(t, "audio server unavailable");
        if (!initialized) report("Audio server unavailable; video recording continues");
    }
    void done() {
        if (--pending || initialized) return;
        initialized = true;
        if (listOnly) {
            QJsonArray outputs, inputs;
            for (const auto& d : sinks) outputs.append(QJsonObject{{"name", d.name}, {"description", d.description}});
            for (const auto& d : sources) if (d.monitor.isEmpty() && !d.name.startsWith("hyprcapture-aec-")) inputs.append(QJsonObject{{"name", d.name}, {"description", d.description}});
            auto bytes = QJsonDocument(QJsonObject{{"outputs", outputs}, {"inputs", inputs}, {"windows", windows}}).toJson(QJsonDocument::Compact) + '\n';
            (void)!write(STDOUT_FILENO, bytes.constData(), bytes.size());
            QCoreApplication::quit();
            return;
        }
        if (mode == "system" || mode == "mix") {
            if (applicationSource) {
                tracks[0].role = "System";
                if (applicationPid <= 1 || applicationStart.isEmpty()) fail(tracks[0], "window unavailable; no desktop audio fallback");
                else {
                    if (!meterOnly) {
                        const auto path = QFile::encodeName(directory + "/system.f32");
                        tracks[0].fd = open(path.constData(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
                        if (tracks[0].fd < 0) fail(tracks[0], "cannot create window audio recovery file");
                    }
                    scanApplication();
                    QObject::connect(&applicationTimer, &QTimer::timeout, [&] { scanApplication(); });
                    applicationTimer.start(1000);
                }
            } else {
            auto name = (output == "default" || output == "auto") ? defaultSink : output;
            QString source;
            for (const auto& d : sinks) if (d.name == name) source = d.monitor;
            openTrack(tracks[0], "System", source);
            }
        }
        if (mode == "microphone" || mode == "mix") {
            auto name = input == "default" ? defaultSource : input;
            bool found = false;
            for (const auto& d : sources) if (d.name == name && d.monitor.isEmpty()) found = true;
            if (!found || !echoEnabled || !startEcho(name)) openTrack(tracks[1], "Microphone", found ? name : QString{});
            if (!echoEnabled) echoStatus("off");
        }
    }
    void openTrack(Track& t, const QString& role, const QString& source) {
        t.owner = this; t.role = role; t.device = source;
        if (source.isEmpty()) { fail(t, "selected device unavailable"); return; }
        for (const auto& d : sources) if (d.name == source) t.sourceIndex = d.index;
        if (!meterOnly && t.fd < 0) {
        if (t.sinkInput != PA_INVALID_INDEX) t.fd = fcntl(tracks[0].fd, F_DUPFD_CLOEXEC, 3);
        else {
        const auto path = QFile::encodeName(directory + (role == "System" ? "/system.f32" : "/microphone.f32"));
        t.fd = open(path.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        }
        if (t.fd < 0) { fail(t, "cannot create audio recovery file"); return; }
        }
        pa_sample_spec spec{PA_SAMPLE_FLOAT32LE, 48000, 2};
        pa_channel_map map; pa_channel_map_init_stereo(&map);
        t.stream = pa_stream_new(context, role.toUtf8().constData(), &spec, &map);
        if (!t.stream) { fail(t, "cannot create capture stream"); return; }
        pa_stream_set_state_callback(t.stream, [](pa_stream* s, void* data) {
            auto& t = *static_cast<Track*>(data);
            if (pa_stream_get_state(s) == PA_STREAM_READY && t.role == "Microphone" && !t.owner->meterOnly) {
                // Let the fixed AEC reservoir fill before the first video frame.
                QTimer::singleShot(t.owner->echoActive ? 120 : 0, QCoreApplication::instance(), [] {
                    constexpr char ready[] = "{\"audioReady\":true}\n";
                    (void)!write(STDOUT_FILENO, ready, sizeof(ready)-1);
                });
            }
            if (pa_stream_get_state(s) == PA_STREAM_FAILED || pa_stream_get_state(s) == PA_STREAM_TERMINATED)
                t.owner->fail(t, "device disconnected");
        }, &t);
        pa_stream_set_moved_callback(t.stream, [](pa_stream*, void* data) {
            auto& t = *static_cast<Track*>(data);
            t.owner->fail(t, "device changed; automatic switching is disabled");
        }, &t);
        pa_stream_set_read_callback(t.stream, [](pa_stream* s, size_t, void* data) {
            auto& t = *static_cast<Track*>(data);
            for (size_t available = pa_stream_readable_size(s); available != 0 && available != static_cast<size_t>(-1); available = pa_stream_readable_size(s)) {
            const void* samples = nullptr; size_t bytes = 0;
            if (pa_stream_peek(s, &samples, &bytes) < 0) { t.owner->fail(t, "capture read failed"); return; }
            if (!bytes) return;
            if (!t.failed && samples) {
                if (t.owner->meterOnly && t.sinkInput == PA_INVALID_INDEX) {
                    const auto* values = static_cast<const float*>(samples);
                    for (size_t i = 0; i < bytes / sizeof(float); ++i) {
                        const double v = std::isfinite(values[i]) ? values[i] : 0;
                        t.peak = std::max(t.peak, std::abs(v)); t.energy += v * v; ++t.samples;
                    }
                    pa_stream_drop(s);
                    continue;
                }
                pa_usec_t latency = 0; int negative = 0;
                const bool timed = pa_stream_get_latency(s, &latency, &negative) == 0;
                const auto processingDelay = t.role == "Microphone" && t.owner->echoActive ? aec::processingDelayUs : 0;
                const auto timestamp = monotonicUs() + (timed ? (negative ? static_cast<std::int64_t>(latency) : -static_cast<std::int64_t>(latency)) : 0) - processingDelay;
                auto position = alignedSample(sampleAt(timestamp - t.owner->origin), t.next);
                if (t.sinkInput != PA_INVALID_INDEX) {
                    // Never add an overlapping packet twice for one playback stream.
                    if (t.next >= 0) position = std::max(position, t.next);
                    t.owner->addApplicationSamples(t, static_cast<const float*>(samples), bytes / sizeof(float), position);
                    t.next = position + bytes / frameBytes;
                    pa_stream_drop(s); continue;
                }
                if (qEnvironmentVariableIsSet("HYPRCAPTURE_TIMING") && monotonicUs() - t.lastTrace > 500000) {
                    std::fprintf(stderr, "sound %s elapsed=%lld latency=%llu negative=%d timed=%d next=%lld position=%lld bytes=%zu\n",
                                 qPrintable(t.role), static_cast<long long>(monotonicUs() - t.owner->origin),
                                 static_cast<unsigned long long>(latency), negative, timed, static_cast<long long>(t.next), static_cast<long long>(position), bytes);
                    t.lastTrace = monotonicUs();
                }

                auto remaining = bytes - bytes % frameBytes;
                const auto* ptr = static_cast<const char*>(samples);
                auto offset = position * frameBytes;
                while (remaining) {
                    const auto n = pwrite(t.fd, ptr, remaining, offset);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) { t.owner->fail(t, "audio file write failed (check disk space)"); break; }
                    remaining -= n; ptr += n; offset += n;
                }
                t.next = position + bytes / frameBytes;
            } else if (!t.failed) {
                // PulseAudio holes represent lost samples, not the end of the stream.
                t.next = -1;
            }
            pa_stream_drop(s);
            }
        }, &t);
        pa_buffer_attr attr{static_cast<uint32_t>(-1), static_cast<uint32_t>(-1), static_cast<uint32_t>(-1), static_cast<uint32_t>(-1), 480 * 8};
        const auto flags = static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_DONT_MOVE);
        if (t.sinkInput != PA_INVALID_INDEX && pa_stream_set_monitor_stream(t.stream, t.sinkInput) < 0) {
            fail(t, "audio server does not support application capture"); return;
        }
        if (pa_stream_connect_record(t.stream, source.toUtf8().constData(), &attr, flags) < 0) fail(t, "cannot open selected device");
    }
    void start() {
        loop = pa_mainloop_new();
        context = pa_context_new(pa_mainloop_get_api(loop), "HyprCapture Sound");
        pa_context_set_state_callback(context, [](pa_context* c, void* data) {
            auto& r = *static_cast<Recorder*>(data);
            if (pa_context_get_state(c) == PA_CONTEXT_FAILED || pa_context_get_state(c) == PA_CONTEXT_TERMINATED) { r.serverError(); return; }
            if (pa_context_get_state(c) != PA_CONTEXT_READY) return;
            auto release = [](pa_operation* op) { if (op) pa_operation_unref(op); };
            release(pa_context_get_server_info(c, [](pa_context*, const pa_server_info* info, void* d) {
                auto& r = *static_cast<Recorder*>(d);
                if (info) { r.defaultSink = QString::fromUtf8(info->default_sink_name); r.defaultSource = QString::fromUtf8(info->default_source_name); }
                r.done();
            }, &r));
            release(pa_context_get_sink_info_list(c, [](pa_context*, const pa_sink_info* info, int eol, void* d) {
                auto& r = *static_cast<Recorder*>(d);
                if (eol) { r.done(); return; }
                if (info && r.sinks.size() < 256) r.sinks.push_back({QString::fromUtf8(info->name), QString::fromUtf8(info->description), QString::fromUtf8(info->monitor_source_name), info->index});
            }, &r));
            release(pa_context_get_source_info_list(c, [](pa_context*, const pa_source_info* info, int eol, void* d) {
                auto& r = *static_cast<Recorder*>(d);
                if (eol) { r.done(); return; }
                if (info && r.sources.size() < 512) r.sources.push_back({QString::fromUtf8(info->name), QString::fromUtf8(info->description),
                    info->monitor_of_sink != PA_INVALID_INDEX ? QStringLiteral("monitor") : QString{}, info->index});
            }, &r));
            if (!r.listOnly) {
                pa_context_set_subscribe_callback(c, [](pa_context*, pa_subscription_event_type_t event, uint32_t index, void* data) {
                    auto& r = *static_cast<Recorder*>(data);
                    if ((event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK_INPUT) r.scanApplication();
                    if ((event & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE &&
                        (((event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE && index == r.echoMicrophoneIndex) ||
                         ((event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK && index == r.echoOutputIndex)))
                        QTimer::singleShot(0, QCoreApplication::instance(), [&r] { r.fallbackEcho("microphone or playback reference removed"); });
                    if ((event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE &&
                        (event & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE)
                        for (auto& t : r.tracks) if (!t.role.isEmpty() && t.sourceIndex == index) r.fail(t, "device removed");
                }, &r);
                release(pa_context_subscribe(c, static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SINK_INPUT), nullptr, nullptr));
            }
        }, this);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) serverError();
        QObject::connect(&pump, &QTimer::timeout, [&] {
            if (echo) {
                echo->iterate();
                if (echo->failed()) fallbackEcho("audio processing module stopped");
            }
            for (int i = 0; i < 64; ++i) {
                int result = 0;
                const int dispatched = pa_mainloop_iterate(loop, 0, &result);
                if (dispatched < 0) { serverError(); break; }
                if (dispatched == 0) break;
            }
        });
        pump.start(2);
        if (meterOnly) {
            // Never allow a stalled UI to block the audio callback or accumulate telemetry.
            const int flags = fcntl(STDOUT_FILENO, F_GETFL);
            fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK);
            QObject::connect(&meterTimer, &QTimer::timeout, [&] {
                QJsonObject levels;
                if (applicationSource) {
                    const auto end = std::max<std::int64_t>(0, sampleAt(monotonicUs() - origin) - 4800);
                    meterNext = std::max(meterNext, end - static_cast<std::int64_t>(meterFrames.size()));
                    auto& t = tracks[0];
                    for (; meterNext < end; ++meterNext) {
                        const auto& frame = meterFrames[meterNext % meterFrames.size()];
                        if (frame.position == meterNext) {
                            t.peak = std::max({t.peak, std::abs(static_cast<double>(frame.left)), std::abs(static_cast<double>(frame.right))});
                            t.energy += frame.left * frame.left + frame.right * frame.right;
                        }
                        t.samples += 2;
                    }
                }
                for (auto& t : tracks) {
                    bool available = !t.failed && t.stream && pa_stream_get_state(t.stream) == PA_STREAM_READY;
                    if (applicationSource && t.role == "System" && !t.failed && !serverFailed)
                        for (const auto& [_, stream] : applicationTracks) available |= !stream->failed && stream->stream && pa_stream_get_state(stream->stream) == PA_STREAM_READY;
                    levels[t.role] = QJsonObject{{"peak", t.peak}, {"rms", t.samples ? std::sqrt(t.energy / t.samples) : 0},
                                                {"available", available}};
                    t.peak = t.energy = 0; t.samples = 0;
                }
                const auto bytes = QJsonDocument(QJsonObject{{"levels", levels}}).toJson(QJsonDocument::Compact) + '\n';
                (void)!write(STDOUT_FILENO, bytes.constData(), bytes.size());
            });
            meterTimer.start(50);
        }
        QTimer::singleShot(3000, [&] { if (!initialized) serverError(); });
    }
};

int finalize(const QStringList& args) {
    if (args.size() != 6) return 2;
    const QString directory = args[2], video = args[3], format = args[5];
    if (!privateDirectory(directory) || !regularOwnedFile(video)) return 2;
    if (format != "mp4" && format != "mov" && format != "webm" && format != "mkv") return 2;
    QFile metadata(directory + "/session.json");
    if (!metadata.open(QIODevice::ReadOnly) || metadata.size() > 8192) return 2;
    const auto meta = QJsonDocument::fromJson(metadata.readAll()).object();
    bool ok = false;
    auto videoStart = args[4].toLongLong(&ok);
    if (!ok || videoStart <= 0) {
        // GSR writes CLOCK_MONOTONIC microseconds followed by realtime microseconds.
        QFile ts(video + ".ts");
        if (ts.open(QIODevice::ReadOnly) && ts.size() < 1024) {
            const auto lines = ts.readAll().split('\n');
            if (lines.size() >= 2) videoStart = lines[1].simplified().split(' ').value(0).toLongLong(&ok);
        }
    }
    if (!ok || videoStart <= 0) { report("Video first-frame timestamp unavailable; original video and audio retained in " + directory); return 1; }
    const auto origin = static_cast<qint64>(meta.value("originUs").toDouble());
    const QString ffmpeg = program("ffmpeg"), ffprobe = program("ffprobe");
    if (ffmpeg.isEmpty() || ffprobe.isEmpty()) { report("FFmpeg/ffprobe unavailable; audio recovery files: " + directory); return 1; }
    QProcess probe;
    probe.start(ffprobe, {"-v", "error", "-select_streams", "v:0", "-show_entries", "stream=duration:format=duration", "-of", "json", video});
    if (!probe.waitForFinished(10000) || probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) { probe.kill(); probe.waitForFinished(); report("Cannot inspect video duration; recovery files: " + directory); return 1; }
    const auto doc = QJsonDocument::fromJson(probe.readAllStandardOutput()).object();
    const auto streams = doc.value("streams").toArray();
    double duration = streams.isEmpty() ? 0 : streams[0].toObject().value("duration").toString().toDouble();
    if (duration <= 0) duration = doc.value("format").toObject().value("duration").toString().toDouble();
    if (!std::isfinite(duration) || duration <= 0 || duration > 86401) { report("Invalid video duration; recovery files: " + directory); return 1; }
    QStringList command{"-hide_banner", "-loglevel", "error", "-nostdin", "-i", video};
    QStringList filters, labels;
    const auto delta = videoStart - origin;
    const auto preset = meta.value("mixPreset").toString();
    QString systemLabel, micLabel;
    int index = 1;
    for (const auto* file : {"system.f32", "microphone.f32"}) {
        const auto path = directory + '/' + file;
        if (!regularOwnedFile(path) || QFileInfo(path).size() < frameBytes) continue;
        command << "-f" << "f32le" << "-ar" << "48000" << "-ac" << "2" << "-i" << path;
        const auto label = QString("a%1").arg(index);
        QString f = QString("[%1:a]").arg(index);
        if (delta >= 0) f += QString("atrim=start_sample=%1,asetpts=PTS-STARTPTS,").arg(sampleAt(delta));
        else f += QString("adelay=%1S:all=1,").arg(sampleAt(-delta));
        f += QString("apad,atrim=duration=%1").arg(duration, 0, 'f', 6);
        const bool mic = QString(file) == "microphone.f32";
        if (!preset.isEmpty()) {
            if (preset != "manual") {
                if (mic) f += ",highpass=f=80,agate=threshold=0.003:ratio=2:attack=10:release=200,speechnorm=p=0.5:e=12:c=4:t=0.005:r=0.005:f=0.01:l=1";
                else f += ",acompressor=threshold=0.18:ratio=3:attack=20:release=300:makeup=1,volume=0.5";
            }
            const int gain = std::clamp(meta.value(mic ? "micGain" : "systemGain").toInt(), -61, 24);
            f += ",volume=" + QString::number(gain <= -61 ? 0 : std::pow(10., gain / 20.), 'g', 12);
        }
        f += '[' + label + ']';
        (mic ? micLabel : systemLabel) = '[' + label + ']';
        filters << f; labels << '[' + label + ']'; ++index;
    }
    if (labels.isEmpty()) {
        report("No audio captured; video saved without sound");
        QDir(directory).removeRecursively();
        QFile::remove(video + ".ts");
        return 0;
    }
    if (preset == "voice-priority" && !systemLabel.isEmpty() && !micLabel.isEmpty()) {
        filters << micLabel + "asplit=2[voice][key]";
        filters << systemLabel + "[key]sidechaincompress=threshold=0.03:ratio=6:attack=10:release=500:makeup=1[ducked]";
        labels = {"[ducked]", "[voice]"};
    }
    QString mix = labels.join("");
    if (labels.size() > 1) mix += QString("amix=inputs=%1:normalize=0:weights='%2',").arg(labels.size()).arg(preset.isEmpty() ? "0.5 0.5" : "1 1");
    else if (preset.isEmpty() && meta.value("mode").toString() == "mix") mix += "volume=0.5,";
    mix += "alimiter=limit=1:level=false:latency=true[out]";
    filters << mix;
    // Same directory as the destination: replacement is atomic, and a failed mux never touches video.
    const QString combined = directory + "/combined." + format;
    command << "-filter_complex" << filters.join(';') << "-map" << "0:v:0" << "-map" << "[out]" << "-c:v" << "copy"
            << "-c:a" << ((format == "mp4" || format == "mov") ? "aac" : "libopus") << "-b:a" << "192k"
            << "-t" << QString::number(duration, 'f', 6);
    if (format == "mp4" || format == "mov") command << "-movflags" << "+faststart";
    command << combined;
    QProcess mux;
    mux.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    mux.start(ffmpeg, command);
    if (!mux.waitForFinished(-1) || mux.exitStatus() != QProcess::NormalExit || mux.exitCode() != 0 || QFileInfo(combined).size() <= 0) {
        report("Audio merge failed; original video preserved, audio recovery files: " + directory); return 1;
    }
    QFile::setPermissions(combined, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (::rename(QFile::encodeName(combined).constData(), QFile::encodeName(video).constData()) != 0) {
        report("Cannot replace video; merged file retained: " + combined); return 1;
    }
    QDir(directory).removeRecursively();
    QFile::remove(video + ".ts");
    return 0;
}
}

int runHelper(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    umask(0077);
    if (args.value(1) == "--sound-finalize") return finalize(args);
    Recorder recorder;
    recorder.listOnly = args.value(1) == "--sound-list";
    recorder.meterOnly = args.value(1) == "--sound-meter";
    if (recorder.meterOnly) {
        if (args.size() != 5 && args.size() != 6 && args.size() != 7) return 2;
        if (args.size() >= 6) { if (args[5] != "-1" && args[5] != "0" && args[5] != "1") return 2; recorder.echoPolicy = args[5].toInt(); recorder.echoEnabled = recorder.echoPolicy != 0; }
        if (args.size() == 7) { if (args[6] != "cpu" && args[6] != "npu") return 2; recorder.echoBackend = args[6]; }
        recorder.mode = args[2]; recorder.output = args[3]; recorder.input = args[4];
        if (recorder.mode != "system" && recorder.mode != "microphone" && recorder.mode != "mix") return 2;
    } else if (!recorder.listOnly) {
        if ((args.size() != 6 && args.size() != 9 && args.size() != 10 && args.size() != 11) || args[1] != "--sound-capture" || !privateDirectory(args[5])) return 2;
        recorder.mode = args[2]; recorder.output = args[3]; recorder.input = args[4]; recorder.directory = args[5];
        if (recorder.mode != "system" && recorder.mode != "microphone" && recorder.mode != "mix") return 2;
        QFile metadata(recorder.directory + "/session.json");
        if (!metadata.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 2;
        QJsonObject meta{{"originUs", static_cast<double>(recorder.origin)}, {"mode", recorder.mode}};
        if (args.size() >= 10) { if (args[9] != "-1" && args[9] != "0" && args[9] != "1") return 2; recorder.echoPolicy = args[9].toInt(); recorder.echoEnabled = recorder.echoPolicy != 0; }
        if (args.size() == 11) { if (args[10] != "cpu" && args[10] != "npu") return 2; recorder.echoBackend = args[10]; }
        meta["echoCancellationRequested"] = recorder.echoEnabled;
        meta["echoPolicy"] = recorder.echoPolicy;
        meta["echoBackend"] = recorder.echoBackend;
        if (args.size() >= 9) {
            bool systemOK = false, micOK = false;
            const int systemGain = args[7].toInt(&systemOK), micGain = args[8].toInt(&micOK);
            if (!systemOK || !micOK || systemGain < -61 || systemGain > 24 || micGain < -61 || micGain > 24 ||
                (args[6] != "manual" && args[6] != "auto-balance" && args[6] != "voice-priority")) return 2;
            meta["mixPreset"] = args[6]; meta["systemGain"] = systemGain; meta["micGain"] = micGain;
        }
        const auto bytes = QJsonDocument(meta).toJson();
        if (metadata.write(bytes) != bytes.size()) return 2;
        metadata.close();
    }
    if (recorder.listOnly || recorder.output.startsWith("window:")) recorder.windows = windowSources();
    recorder.applicationSource = recorder.output.startsWith("window:") || recorder.output.startsWith("pid:");
    if (recorder.applicationSource) {
        if (recorder.output.startsWith("pid:")) recorder.applicationPid = recorder.output.mid(4).toInt();
        else for (const auto& value : recorder.windows) {
            const auto window = value.toObject();
            if (window["name"].toString() == recorder.output) { recorder.applicationPid = window["pid"].toInt(); break; }
        }
        recorder.applicationStart = processIdentity(recorder.applicationPid).start;
    }
    QSocketNotifier stop(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&stop, &QSocketNotifier::activated, [&] {
        char bytes[64];
        if (read(STDIN_FILENO, bytes, sizeof(bytes)) <= 0) {
            stop.setEnabled(false);
            // Drain the fixed processing delay plus the capture transport.
            // Final mux duration trims the look-ahead beyond the last frame.
            QTimer::singleShot(recorder.echoActive ? 180 : 100, &app, &QCoreApplication::quit);
        }
    });
    if (recorder.listOnly) stop.setEnabled(false);
    recorder.start();
    return app.exec();
}
}
