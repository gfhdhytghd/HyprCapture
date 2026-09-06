#include "audio/aec_policy.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <sched.h>
#include <thread>

namespace hyprcapture::audio::aec {
const ModelFile modelFiles[4] = {
    {"dtln_aec_256_1.tflite", "4a3a588b69fd79d837bc068b579a26faa92cac39dddbb00001d2dc1c3d869d60"},
    {"dtln_aec_256_2.tflite", "fa2590243aad1bf893c5be45b20709e8c50feec65e3604d1d52bae6eeddc23d3"},
    {"dtln_aec_512_1.tflite", "569f7c3cfac96b1e093229c3ca10b5d892f5b1906105b644e457f8245b4f7383"},
    {"dtln_aec_512_2.tflite", "fb423d867ab25d5f4716bd369c7126b6c84926175c019b832e71bf21de0e9907"},
};
namespace {
QByteArray read(const QString& path, qint64 maximum = 1024*1024) {
    QFile f(path); return f.open(QIODevice::ReadOnly) ? f.read(maximum) : QByteArray{};
}
QByteArray fileHash(const QString& path) {
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha256); if (!h.addData(&f)) return {}; return h.result().toHex();
}
QString firstExisting(const QStringList& paths) {
    for (const auto& p : paths) if (QFileInfo(p).isFile()) return QFileInfo(p).absoluteFilePath();
    return paths.value(0);
}
}
QString dataDirectory() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/hyprcapture/aec"; }
QString modelDirectory() {
    if (qEnvironmentVariableIsSet("HYPRCAPTURE_AEC_MODEL_DIR")) return qEnvironmentVariable("HYPRCAPTURE_AEC_MODEL_DIR");
#ifdef HYPRCAPTURE_AEC_MODEL_DIR
    if (QDir(HYPRCAPTURE_AEC_MODEL_DIR).exists()) return HYPRCAPTURE_AEC_MODEL_DIR;
#endif
    const auto packaged = QCoreApplication::applicationDirPath() + "/../share/hyprcapture/aec/models";
    if (QDir(packaged).exists()) return QDir(packaged).absolutePath();
    return dataDirectory() + "/models";
}
QString nativeComponent(const QString& name) {
    const auto app = QCoreApplication::applicationDirPath();
    return firstExisting({app + "/" + name, app + "/../lib/hyprcapture/" + name,
                          app + "/../lib64/hyprcapture/" + name, dataDirectory() + "/runtime/" + name});
}
QString runtimeLibrary() {
    if (qEnvironmentVariableIsSet("HYPRCAPTURE_TFLITE_LIBRARY")) return qEnvironmentVariable("HYPRCAPTURE_TFLITE_LIBRARY");
#ifdef HYPRCAPTURE_TFLITE_LIBRARY
    if (QFileInfo::exists(HYPRCAPTURE_TFLITE_LIBRARY)) return HYPRCAPTURE_TFLITE_LIBRARY;
#endif
    return nativeComponent("libtensorflowlite_c.so");
}
QString workerPath() { return QCoreApplication::applicationDirPath() + "/hyprcapture-aec"; }
QString fingerprint(const QString& backend) {
    QByteArray stable = "dtln-policy-2-stream48-state-split-1\n";
    // Local OS installation identifier, hashed only. Never persist the raw ID.
    stable += read("/etc/machine-id", 256).trimmed() + '\n' + QSysInfo::currentCpuArchitecture().toUtf8() + '\n';
    const auto cpu = read("/proc/cpuinfo");
    for (const auto& line : cpu.split('\n')) {
        const auto key = line.split(':').value(0).trimmed();
        if (key == "model name" || key == "flags" || key == "Features" || key == "CPU implementer" || key == "CPU part") stable += line + '\n';
        if (line.isEmpty() && stable.contains("model name")) break;
    }
    cpu_set_t mask; CPU_ZERO(&mask);
    stable += QByteArray::number(sched_getaffinity(0, sizeof(mask), &mask) == 0 ? CPU_COUNT(&mask) : std::thread::hardware_concurrency());
    for (const auto& line : read("/proc/self/cgroup", 4096).split('\n')) {
        if (!line.startsWith("0::")) continue;
        auto group = QDir::cleanPath("/sys/fs/cgroup" + QString::fromUtf8(line.mid(3)));
        while (group.startsWith("/sys/fs/cgroup")) {
            stable += read(group + "/cpu.max", 256) + read(group + "/cpuset.cpus.effective", 256);
            if (group == "/sys/fs/cgroup") break;
            group = QFileInfo(group).absolutePath();
        }
    }
    stable += backend.toUtf8() + fileHash(runtimeLibrary());
    stable += fileHash(workerPath()) + fileHash(nativeComponent("libspa-aec-dtln.so"));
    for (const auto& directory : {QString("/usr/lib"), QString("/usr/lib/x86_64-linux-gnu"), QString("/usr/lib/aarch64-linux-gnu")})
        for (const auto& library : QDir(directory).entryList({"libswresample.so.*", "libavutil.so.*", "libfftw3.so.*"}, QDir::Files | QDir::NoSymLinks))
            stable += fileHash(directory + "/" + library);
    if (backend == "npu") {
        stable += fileHash(nativeComponent("libhyprcapture-aec-openvino.so"));
        stable += read("/sys/module/intel_vpu/version", 256);
        for (const auto& d : QDir("/sys/class/accel").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            stable += read("/sys/class/accel/" + d + "/device/vendor", 256) + read("/sys/class/accel/" + d + "/device/device", 256);
            stable += QFileInfo("/sys/class/accel/" + d + "/device/driver/module").canonicalFilePath().toUtf8();
        }
        for (const auto& d : {QString("/usr/lib"), QString("/usr/lib/x86_64-linux-gnu")})
            for (const auto& f : QDir(d).entryList({"libze_intel_vpu.so*", "libopenvino_intel_npu_plugin.so*", "libnpu_compiler.so*"}, QDir::Files))
                stable += fileHash(d + "/" + f);
    }
    for (const auto& f : modelFiles) stable += f.sha256;
    return QString::fromLatin1(QCryptographicHash::hash(stable, QCryptographicHash::Sha256).toHex());
}
QString cachePath(const QString& backend) {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/hyprcapture/aec/" + (backend == "npu" ? "npu" : "cpu") + ".json";
}
bool modelsValid(int size, QString* error) {
    if (size != 256 && size != 512) return false;
    for (const auto& f : modelFiles) {
        if (!QString::fromLatin1(f.name).contains(QString::number(size))) continue;
        if (fileHash(modelDirectory() + "/" + f.name) != f.sha256) {
            if (error) *error = "Missing or damaged model: " + QString::fromLatin1(f.name);
            return false;
        }
    }
    return true;
}
QJsonObject readCache(const QString& backend) {
    auto result = QJsonDocument::fromJson(read(cachePath(backend))).object();
    if (result["version"].toInt() != policyVersion || result["fingerprint"].toString() != fingerprint(backend)) return {};
    return result;
}
bool writeCache(const QString& backend, QJsonObject value) {
    value["version"] = policyVersion; value["fingerprint"] = fingerprint(backend);
    const auto path = cachePath(backend); if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile file(path); if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return file.write(QJsonDocument(value).toJson()) >= 0 && file.commit();
}
QJsonObject selection(int policy, const QString& backend) {
    QJsonObject result{{"aec", "off"}, {"backend", backend}, {"model", 0}, {"reason", "disabled"}};
    if (policy == 0) return result;
    const auto cache = readCache(backend);
    if (cache.isEmpty() || cache["status"] == "pending") { result["aec"] = "pending"; result["reason"] = cache["reason"].toString("test required"); return result; }
    const auto entries = cache["models"].toObject();
    for (int model : {512, 256}) {
        auto entry = entries[QString::number(model)].toObject();
        if (!entry["correct"].toBool() || (!entry["fast"].toBool() && !(policy == 1 && model == 256 && backend == "cpu"))) continue;
        QString error;
        if (!modelsValid(model, &error)) { result["aec"] = "pending"; result["reason"] = error; return result; }
        result["aec"] = "ready"; result["model"] = model; result["reason"] = ""; return result;
    }
    result["reason"] = cache["reason"].toString("insufficient performance"); return result;
}
QString description(const QJsonObject& d, int policy) {
    if (policy == 0) return "AEC off";
    if (d["model"].toInt() > 0) return QString("%1 · %2 %3").arg(policy < 0 ? "Auto" : "On", d["backend"].toString().toUpper()).arg(d["model"].toInt());
    if (d["aec"] == "pending") return "AEC · test pending";
    return "AEC off · " + d["reason"].toString();
}
}
