#include "audio/helper.hpp"
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
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <memory>
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
    uint32_t sourceIndex = PA_INVALID_INDEX;
    std::int64_t next = -1, lastTrace = 0;
    bool failed = false;
};
struct Recorder {
    pa_mainloop* loop = nullptr;
    pa_context* context = nullptr;
    std::vector<Device> sinks, sources;
    QString defaultSink, defaultSource, directory, mode, output, input;
    std::array<Track, 2> tracks;
    std::int64_t origin = monotonicUs();
    bool listOnly = false, initialized = false, serverFailed = false;
    int pending = 3;
    QTimer pump;

    ~Recorder() {
        pump.stop();
        if (context) pa_context_set_state_callback(context, nullptr, nullptr);
        for (auto& t : tracks) {
            if (t.stream) { pa_stream_set_state_callback(t.stream, nullptr, nullptr); pa_stream_disconnect(t.stream); pa_stream_unref(t.stream); }
            if (t.fd >= 0) close(t.fd);
        }
        if (context) { pa_context_disconnect(context); pa_context_unref(context); }
        if (loop) pa_mainloop_free(loop);
    }
    void fail(Track& t, const QString& reason) {
        if (t.failed) return;
        t.failed = true;
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
            for (const auto& d : sources) if (d.monitor.isEmpty()) inputs.append(QJsonObject{{"name", d.name}, {"description", d.description}});
            auto bytes = QJsonDocument(QJsonObject{{"outputs", outputs}, {"inputs", inputs}}).toJson(QJsonDocument::Compact) + '\n';
            (void)!write(STDOUT_FILENO, bytes.constData(), bytes.size());
            QCoreApplication::quit();
            return;
        }
        if (mode == "system" || mode == "mix") {
            auto name = output == "default" ? defaultSink : output;
            QString source;
            for (const auto& d : sinks) if (d.name == name) source = d.monitor;
            openTrack(tracks[0], "System", source);
        }
        if (mode == "microphone" || mode == "mix") {
            auto name = input == "default" ? defaultSource : input;
            bool found = false;
            for (const auto& d : sources) if (d.name == name && d.monitor.isEmpty()) found = true;
            openTrack(tracks[1], "Microphone", found ? name : QString{});
        }
    }
    void openTrack(Track& t, const QString& role, const QString& source) {
        t.owner = this; t.role = role; t.device = source;
        if (source.isEmpty()) { fail(t, "selected device unavailable"); return; }
        for (const auto& d : sources) if (d.name == source) t.sourceIndex = d.index;
        const auto path = QFile::encodeName(directory + (role == "System" ? "/system.f32" : "/microphone.f32"));
        t.fd = open(path.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (t.fd < 0) { fail(t, "cannot create audio recovery file"); return; }
        pa_sample_spec spec{PA_SAMPLE_FLOAT32LE, 48000, 2};
        pa_channel_map map; pa_channel_map_init_stereo(&map);
        t.stream = pa_stream_new(context, role.toUtf8().constData(), &spec, &map);
        if (!t.stream) { fail(t, "cannot create capture stream"); return; }
        pa_stream_set_state_callback(t.stream, [](pa_stream* s, void* data) {
            auto& t = *static_cast<Track*>(data);
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
                pa_usec_t latency = 0; int negative = 0;
                const bool timed = pa_stream_get_latency(s, &latency, &negative) == 0;
                const auto timestamp = monotonicUs() + (timed ? (negative ? static_cast<std::int64_t>(latency) : -static_cast<std::int64_t>(latency)) : 0);
                auto position = alignedSample(sampleAt(timestamp - t.owner->origin), t.next);
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
                    if ((event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SOURCE &&
                        (event & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE)
                        for (auto& t : r.tracks) if (!t.role.isEmpty() && t.sourceIndex == index) r.fail(t, "device removed");
                }, &r);
                release(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SOURCE, nullptr, nullptr));
            }
        }, this);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) serverError();
        QObject::connect(&pump, &QTimer::timeout, [&] {
            for (int i = 0; i < 64; ++i) {
                int result = 0;
                const int dispatched = pa_mainloop_iterate(loop, 0, &result);
                if (dispatched < 0) { serverError(); break; }
                if (dispatched == 0) break;
            }
        });
        pump.start(2);
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
    int index = 1;
    for (const auto* file : {"system.f32", "microphone.f32"}) {
        const auto path = directory + '/' + file;
        if (!regularOwnedFile(path) || QFileInfo(path).size() < frameBytes) continue;
        command << "-f" << "f32le" << "-ar" << "48000" << "-ac" << "2" << "-i" << path;
        const auto label = QString("a%1").arg(index);
        QString f = QString("[%1:a]").arg(index);
        if (delta >= 0) f += QString("atrim=start_sample=%1,asetpts=PTS-STARTPTS,").arg(sampleAt(delta));
        else f += QString("adelay=%1S:all=1,").arg(sampleAt(-delta));
        f += QString("apad,atrim=duration=%1[%2]").arg(duration, 0, 'f', 6).arg(label);
        filters << f; labels << '[' + label + ']'; ++index;
    }
    if (labels.isEmpty()) {
        report("No audio captured; video saved without sound");
        QDir(directory).removeRecursively();
        QFile::remove(video + ".ts");
        return 0;
    }
    QString mix = labels.join("");
    if (labels.size() > 1) mix += QString("amix=inputs=%1:normalize=0:weights='0.5 0.5',").arg(labels.size());
    else if (meta.value("mode").toString() == "mix") mix += "volume=0.5,";
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
    if (!recorder.listOnly) {
        if (args.size() != 6 || args[1] != "--sound-capture" || !privateDirectory(args[5])) return 2;
        recorder.mode = args[2]; recorder.output = args[3]; recorder.input = args[4]; recorder.directory = args[5];
        if (recorder.mode != "system" && recorder.mode != "microphone" && recorder.mode != "mix") return 2;
        QFile metadata(recorder.directory + "/session.json");
        if (!metadata.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 2;
        const auto bytes = QJsonDocument(QJsonObject{{"originUs", static_cast<double>(recorder.origin)}, {"mode", recorder.mode}}).toJson();
        if (metadata.write(bytes) != bytes.size()) return 2;
        metadata.close();
    }
    QSocketNotifier stop(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&stop, &QSocketNotifier::activated, [&] {
        char bytes[64];
        if (read(STDIN_FILENO, bytes, sizeof(bytes)) <= 0) {
            stop.setEnabled(false);
            QTimer::singleShot(100, &app, &QCoreApplication::quit);
        }
    });
    if (recorder.listOnly) stop.setEnabled(false);
    recorder.start();
    return app.exec();
}
}
