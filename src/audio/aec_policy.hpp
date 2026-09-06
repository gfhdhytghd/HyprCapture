#pragma once
#include <QJsonObject>
#include <QString>

namespace hyprcapture::audio::aec {
constexpr int policyVersion = 1;
constexpr int processingDelayUs = 80000; // measured Stream48 (40 ms) + SPA queue (40 ms)
struct ModelFile { const char* name; const char* sha256; };
extern const ModelFile modelFiles[4];
QString dataDirectory();
QString modelDirectory();
QString runtimeLibrary();
QString nativeComponent(const QString& name);
QString workerPath();
QString fingerprint(const QString& backend);
QString cachePath(const QString& backend);
bool modelsValid(int size, QString* error = nullptr);
QJsonObject readCache(const QString& backend);
bool writeCache(const QString& backend, QJsonObject value);
QJsonObject selection(int policy, const QString& backend);
QString description(const QJsonObject& decision, int policy);
} // namespace hyprcapture::audio::aec
