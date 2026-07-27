#include "ui/clipboard_utils.hpp"
#include "shared/trusted_path.hpp"

#include <QBuffer>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QPixmap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QUrl>

#include <atomic>
#include <cerrno>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>

namespace hyprcapture::ui {
namespace {

constexpr qint64 kMaxClipboardSnapshotBytes = 16 * 1024 * 1024;
constexpr qint64 kMaxClipboardImageBytes = 128 * 1024 * 1024;
constexpr qint64 kMaxClipboardTextBytes = 4 * 1024 * 1024;
constexpr qint64 kMaxClipboardUrlBytes = 1024 * 1024;
constexpr int    kMaxClipboardUrls = 128;

QString trustedExecutablePath(const QString& path) {
    const auto trusted = security::trustedExecutablePath(QFile::encodeName(path).toStdString());
    return trusted ? QFile::decodeName(trusted->c_str()) : QString{};
}

QString trustedSystemProgramImpl(const QString& name) {
    QStringList directories;
#ifdef HYPRCAPTURE_TRUSTED_BIN_DIRS
    directories.append(QString::fromUtf8(HYPRCAPTURE_TRUSTED_BIN_DIRS).split(QLatin1Char(':'), Qt::SkipEmptyParts));
#endif
    if (const QString home = QDir::homePath(); !home.isEmpty())
        directories.append(QDir(home).filePath(QStringLiteral(".nix-profile/bin")));
    if (const QString user = qEnvironmentVariable("USER"); !user.isEmpty())
        directories.append(QDir(QStringLiteral("/etc/profiles/per-user")).filePath(user + QStringLiteral("/bin")));
    directories.append({QStringLiteral("/run/current-system/sw/bin"), QStringLiteral("/nix/var/nix/profiles/default/bin")});
    directories.append({QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/bin"), QStringLiteral("/bin")});
    directories.removeDuplicates();

    for (const QString& dir : directories) {
        const QString trusted = trustedExecutablePath(QDir(dir).filePath(name));
        if (!trusted.isEmpty())
            return trusted;
    }
    return {};
}

QString wlCopyProgram() {
    return trustedSystemProgramImpl(QStringLiteral("wl-copy"));
}

bool ensurePrivateDirectory(const QString& path) {
    const QByteArray native = QFile::encodeName(path);
    if (native.isEmpty())
        return false;

    if (mkdir(native.constData(), 0700) != 0 && errno != EEXIST)
        return false;

    struct stat st {};
    if (lstat(native.constData(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
        return false;

    if ((st.st_mode & 0777) != 0700 && chmod(native.constData(), 0700) != 0)
        return false;

    return true;
}

QString privateRuntimeRoot() {
    const QString rootName = QStringLiteral("hyprcapture-%1").arg(QString::number(static_cast<qulonglong>(geteuid())));
    QStringList   bases{QStringLiteral("/dev/shm"), QDir::tempPath()};
    bases.removeDuplicates();
    for (const QString& base : bases) {
        const QFileInfo baseInfo(base);
        if (!baseInfo.exists() || !baseInfo.isDir())
            continue;
        const QString path = QDir(base).filePath(rootName);
        if (ensurePrivateDirectory(path))
            return path;
    }
    return {};
}

bool pathIsInPrivateRuntimeRoot(const QString& path) {
    const QString root = privateRuntimeRoot();
    if (root.isEmpty())
        return false;

    const QString rootCanonical = QFileInfo(root).canonicalFilePath();
    if (rootCanonical.isEmpty() || path.isEmpty())
        return false;

    const QFileInfo pathInfo(path);
    if (pathInfo.exists()) {
        const QString pathCanonical = pathInfo.canonicalFilePath();
        if (pathCanonical.isEmpty())
            return false;
        return pathCanonical == rootCanonical || pathCanonical.startsWith(rootCanonical + QLatin1Char('/'));
    }

    const QString parentCanonical = QFileInfo(pathInfo.absolutePath()).canonicalFilePath();
    const QString filename = pathInfo.fileName();
    if (parentCanonical.isEmpty() || filename.isEmpty() || filename == QLatin1String(".") || filename == QLatin1String(".."))
        return false;

    return parentCanonical == rootCanonical || parentCanonical.startsWith(rootCanonical + QLatin1Char('/'));
}

bool privateRuntimeFileExists(const QString& path, qint64 maxSize) {
    if (path.isEmpty() || !pathIsInPrivateRuntimeRoot(path))
        return false;
    const QFileInfo info(path);
    return info.exists() && info.isFile() && info.isReadable() && info.size() >= 0 && info.size() <= maxSize;
}

bool utf8WithinLimit(const QString& value, qint64 maxBytes) {
    return value.toUtf8().size() <= maxBytes;
}

bool imageWithinLimit(const QImage& image, qint64 maxBytes) {
    return !image.isNull() && image.sizeInBytes() >= 0 && image.sizeInBytes() <= maxBytes;
}

bool savedFileWithinLimit(const QString& path, qint64 maxBytes) {
    const QFileInfo info(path);
    return info.exists() && info.isFile() && info.size() >= 0 && info.size() <= maxBytes;
}

bool urlsWithinLimit(const QList<QUrl>& urls) {
    if (urls.size() > kMaxClipboardUrls)
        return false;

    qint64 totalBytes = 0;
    for (const auto& url : urls) {
        const qint64 urlBytes = url.toString().toUtf8().size();
        if (urlBytes < 0 || totalBytes + urlBytes > kMaxClipboardUrlBytes)
            return false;
        totalBytes += urlBytes;
    }
    return true;
}

bool allowEnvironmentName(const QString& name) {
    static const QSet<QString> allowed{
        QStringLiteral("HOME"),
        QStringLiteral("USER"),
        QStringLiteral("LOGNAME"),
        QStringLiteral("LANG"),
        QStringLiteral("XDG_RUNTIME_DIR"),
        QStringLiteral("XDG_CURRENT_DESKTOP"),
        QStringLiteral("XDG_SESSION_TYPE"),
        QStringLiteral("XDG_DATA_HOME"),
        QStringLiteral("XDG_CONFIG_HOME"),
        QStringLiteral("XDG_DATA_DIRS"),
        QStringLiteral("DESKTOP_SESSION"),
        QStringLiteral("WAYLAND_DISPLAY"),
        QStringLiteral("DISPLAY"),
        QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
        QStringLiteral("QT_QPA_PLATFORM"),
        QStringLiteral("QT_SCALE_FACTOR"),
        QStringLiteral("QT_AUTO_SCREEN_SCALE_FACTOR"),
        QStringLiteral("QT_ENABLE_HIGHDPI_SCALING"),
        QStringLiteral("HYPRCAPTURE_TIMING"),
        QStringLiteral("HYPRLAND_INSTANCE_SIGNATURE"),
    };
    return allowed.contains(name) || name.startsWith(QStringLiteral("LC_"));
}

bool copyBytesWithWlCopy(const QByteArray& bytes, const QString& mimeType) {
    if (bytes.isEmpty())
        return false;

    const QString program = wlCopyProgram();
    if (program.isEmpty())
        return false;

    QProcess process;
    process.setProgram(program);
    process.setArguments({QStringLiteral("--type"), mimeType});
    process.setProcessEnvironment(trustedProcessEnvironment());
    process.start();
    if (!process.waitForStarted(1000))
        return false;

    qsizetype written = 0;
    while (written < bytes.size()) {
        const qint64 chunk = process.write(bytes.constData() + written, bytes.size() - written);
        if (chunk <= 0) {
            process.kill();
            process.waitForFinished(500);
            return false;
        }
        written += chunk;
        if (!process.waitForBytesWritten(1000)) {
            process.kill();
            process.waitForFinished(500);
            return false;
        }
    }
    process.closeWriteChannel();

    if (!process.waitForFinished(2500)) {
        process.kill();
        process.waitForFinished(500);
        return false;
    }

    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool copyFileWithWlCopyDetached(const QString& path, const QString& mimeType, bool removeAfterCopy) {
    if (path.isEmpty() || !QFileInfo::exists(path) || !savedFileWithinLimit(path, kMaxClipboardImageBytes))
        return false;
    if (removeAfterCopy && !pathIsInPrivateRuntimeRoot(path))
        return false;

    const QString program = wlCopyProgram();
    if (program.isEmpty())
        return false;

    QProcess process;
    process.setProgram(program);
    process.setArguments({QStringLiteral("--type"), mimeType});
    process.setProcessEnvironment(trustedProcessEnvironment());
    process.setStandardInputFile(path);
    const bool started = process.startDetached();
    if (started && removeAfterCopy)
        QFile::remove(path);
    return started;
}

bool clearWithWlCopy() {
    const QString program = wlCopyProgram();
    if (program.isEmpty())
        return false;

    QProcess process;
    process.setProgram(program);
    process.setArguments({QStringLiteral("--clear")});
    process.setProcessEnvironment(trustedProcessEnvironment());
    process.start();
    if (!process.waitForStarted(1000))
        return false;
    if (!process.waitForFinished(2500)) {
        process.kill();
        process.waitForFinished(500);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QByteArray pngBytes(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    QImageWriter writer(&buffer, "PNG");
    writer.setQuality(75);
    if (!writer.write(image))
        return {};
    return bytes;
}

bool writePrivateFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.remove();
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        file.remove();
        return false;
    }
    return true;
}

bool savePng(const QImage& image, const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return false;
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.remove();
        return false;
    }

    QImageWriter writer(&file, "PNG");
    writer.setQuality(75);
    if (!writer.write(image)) {
        file.remove();
        return false;
    }
    return true;
}

bool copyImageToQtClipboard(const QImage& image) {
    auto* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return false;
    clipboard->setImage(image);
    return true;
}

void removeSnapshotFiles(const QJsonObject& obj, const QString& path) {
    if (obj.contains("image")) {
        const QString imagePath = obj.value("image").toString();
        if (pathIsInPrivateRuntimeRoot(imagePath))
            QFile::remove(imagePath);
    }
    if (pathIsInPrivateRuntimeRoot(path))
        QFile::remove(path);
}

bool snapshotStringValue(const QJsonObject& obj, const QString& key, QString& out) {
    if (!obj.contains(key))
        return true;
    const auto value = obj.value(key);
    if (!value.isString())
        return false;
    out = value.toString();
    return utf8WithinLimit(out, kMaxClipboardTextBytes);
}

bool snapshotUrlsValue(const QJsonObject& obj, QList<QUrl>& out) {
    if (!obj.contains("urls"))
        return true;
    const auto value = obj.value("urls");
    if (!value.isArray())
        return false;

    QList<QUrl> urls;
    for (const auto urlValue : value.toArray()) {
        if (!urlValue.isString())
            return false;
        urls.push_back(QUrl(urlValue.toString()));
        if (!urlsWithinLimit(urls))
            return false;
    }

    out = std::move(urls);
    return true;
}

bool snapshotColorValue(const QJsonObject& obj, QColor& out) {
    if (!obj.contains("color"))
        return true;
    const auto value = obj.value("color");
    if (!value.isString())
        return false;
    const QColor color(value.toString());
    if (!color.isValid())
        return false;
    out = color;
    return true;
}

bool snapshotImageValue(const QJsonObject& obj, QString& out) {
    if (!obj.contains("image"))
        return true;
    const auto value = obj.value("image");
    if (!value.isString())
        return false;
    out = value.toString();
    return privateRuntimeFileExists(out, kMaxClipboardImageBytes);
}

} // namespace

QString runtimeFile(const QString& prefix, const QString& suffix) {
    static std::atomic_uint64_t counter{0};
    const QString root = privateRuntimeRoot();
    if (root.isEmpty())
        return {};
    return QDir(root).filePath(QStringLiteral("%1-%2-%3-%4%5")
                                   .arg(prefix)
                                   .arg(QCoreApplication::applicationPid())
                                   .arg(QDateTime::currentMSecsSinceEpoch())
                                   .arg(counter.fetch_add(1, std::memory_order_relaxed))
                                   .arg(suffix));
}

QString trustedSystemProgram(const QString& name) {
    return trustedSystemProgramImpl(name);
}

QProcessEnvironment trustedProcessEnvironment() {
    const auto source = QProcessEnvironment::systemEnvironment();
    QProcessEnvironment env;
    QStringList directories;
#ifdef HYPRCAPTURE_TRUSTED_BIN_DIRS
    directories.append(QString::fromUtf8(HYPRCAPTURE_TRUSTED_BIN_DIRS).split(QLatin1Char(':'), Qt::SkipEmptyParts));
#endif
    if (const QString home = QDir::homePath(); !home.isEmpty())
        directories.append(QDir(home).filePath(QStringLiteral(".nix-profile/bin")));
    if (const QString user = qEnvironmentVariable("USER"); !user.isEmpty())
        directories.append(QDir(QStringLiteral("/etc/profiles/per-user")).filePath(user + QStringLiteral("/bin")));
    directories.append({QStringLiteral("/run/current-system/sw/bin"), QStringLiteral("/nix/var/nix/profiles/default/bin")});
    directories.append({QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/bin"), QStringLiteral("/bin")});
    directories.removeDuplicates();
    env.insert(QStringLiteral("PATH"), directories.join(QLatin1Char(':')));
    for (const QString& name : source.keys()) {
        if (allowEnvironmentName(name))
            env.insert(name, source.value(name));
    }
    return env;
}

bool isPrivateRuntimePath(const QString& path) {
    return pathIsInPrivateRuntimeRoot(path);
}

bool isPrivateRuntimeFile(const QString& path, qint64 maxSize) {
    return privateRuntimeFileExists(path, maxSize);
}

bool savePrivatePng(const QImage& image, const QString& path) {
    if (image.isNull() || path.isEmpty())
        return false;
    return savePng(image, path);
}

ClipboardSnapshotData captureClipboardSnapshotData() {
    ClipboardSnapshotData snapshot;
    const auto* clipboard = QGuiApplication::clipboard();
    const auto* mime = clipboard ? clipboard->mimeData() : nullptr;
    if (!mime)
        return snapshot;

    bool sawContent = false;
    bool capturedContent = false;
    if (mime->hasText()) {
        sawContent = true;
        const QString text = mime->text();
        if (utf8WithinLimit(text, kMaxClipboardTextBytes)) {
            snapshot.text = text;
            capturedContent = true;
        }
    }
    if (mime->hasHtml()) {
        sawContent = true;
        const QString html = mime->html();
        if (utf8WithinLimit(html, kMaxClipboardTextBytes)) {
            snapshot.html = html;
            capturedContent = true;
        }
    }
    if (mime->hasUrls()) {
        sawContent = true;
        QList<QUrl> urls;
        for (const auto& url : mime->urls()) {
            urls.append(url);
            if (!urlsWithinLimit(urls)) {
                urls.clear();
                break;
            }
        }
        if (!urls.isEmpty()) {
            snapshot.urls = std::move(urls);
            capturedContent = true;
        }
    }
    if (mime->hasColor()) {
        sawContent = true;
        const QColor color = qvariant_cast<QColor>(mime->colorData());
        if (color.isValid()) {
            snapshot.color = color;
            capturedContent = true;
        }
    }
    if (mime->hasImage()) {
        sawContent = true;
        QImage image = qvariant_cast<QImage>(mime->imageData());
        if (image.isNull()) {
            const QPixmap pixmap = qvariant_cast<QPixmap>(mime->imageData());
            image = pixmap.toImage();
        }
        if (imageWithinLimit(image, kMaxClipboardImageBytes)) {
            snapshot.image = image;
            capturedContent = true;
        }
    }

    if (!capturedContent && sawContent)
        return {};

    snapshot.valid = true;
    snapshot.empty = !capturedContent;
    return snapshot;
}

bool saveClipboardSnapshotDataToPath(const ClipboardSnapshotData& snapshot, const QString& path) {
    if (!snapshot.valid)
        return false;

    QJsonObject root;
    root.insert("empty", snapshot.empty);
    bool wroteContent = false;
    if (!snapshot.text.isEmpty()) {
        if (!utf8WithinLimit(snapshot.text, kMaxClipboardTextBytes))
            return false;
        root.insert("text", snapshot.text);
        root.insert("empty", false);
        wroteContent = true;
    }
    if (!snapshot.html.isEmpty()) {
        if (!utf8WithinLimit(snapshot.html, kMaxClipboardTextBytes))
            return false;
        root.insert("html", snapshot.html);
        root.insert("empty", false);
        wroteContent = true;
    }
    if (!snapshot.urls.isEmpty()) {
        if (!urlsWithinLimit(snapshot.urls))
            return false;
        QJsonArray urls;
        for (const auto& url : snapshot.urls)
            urls.append(url.toString());
        root.insert("urls", urls);
        root.insert("empty", false);
        wroteContent = true;
    }
    if (snapshot.color.isValid()) {
        root.insert("color", snapshot.color.name(QColor::HexArgb));
        root.insert("empty", false);
        wroteContent = true;
    }
    QString imagePath;
    if (!snapshot.image.isNull()) {
        if (!imageWithinLimit(snapshot.image, kMaxClipboardImageBytes))
            return false;
        imagePath = runtimeFile("clipboard-image", ".png");
        if (savePng(snapshot.image, imagePath) && savedFileWithinLimit(imagePath, kMaxClipboardImageBytes)) {
            root.insert("image", imagePath);
            root.insert("empty", false);
            wroteContent = true;
        } else if (!imagePath.isEmpty())
            QFile::remove(imagePath);
    }
    if (!snapshot.empty && !wroteContent) {
        if (!imagePath.isEmpty())
            QFile::remove(imagePath);
        return false;
    }

    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (json.size() > kMaxClipboardSnapshotBytes || !writePrivateFile(path, json)) {
        if (!imagePath.isEmpty())
            QFile::remove(imagePath);
        return false;
    }
    return true;
}

QString saveClipboardSnapshotData(const ClipboardSnapshotData& snapshot) {
    const QString path = runtimeFile("clipboard", ".json");
    return saveClipboardSnapshotDataToPath(snapshot, path) ? path : QString{};
}

QString saveClipboardSnapshot() {
    return saveClipboardSnapshotData(captureClipboardSnapshotData());
}

bool copyImageToClipboard(const QImage& image) {
    if (!imageWithinLimit(image, kMaxClipboardImageBytes))
        return false;

    const QByteArray bytes = pngBytes(image);
    if (!bytes.isEmpty() && bytes.size() <= kMaxClipboardImageBytes && copyBytesWithWlCopy(bytes, QStringLiteral("image/png")))
        return true;

    return copyImageToQtClipboard(image);
}

bool copyImageToClipboardDetached(const QImage& image) {
    if (image.isNull())
        return false;

    const QString path = runtimeFile("clipboard-copy", ".png");
    if (savePng(image, path)) {
        if (savedFileWithinLimit(path, kMaxClipboardImageBytes) && copyFileWithWlCopyDetached(path, QStringLiteral("image/png"), true))
            return true;
        QFile::remove(path);
    }

    return false;
}

bool copyImageFileToClipboardDetached(const QString& path) {
    return copyFileWithWlCopyDetached(path, QStringLiteral("image/png"), false);
}

bool copyFileUrlToClipboard(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || !info.exists() || !info.isFile())
        return false;

    const QUrl url = QUrl::fromLocalFile(canonical);
    const QByteArray uriList = (url.toString() + QStringLiteral("\r\n")).toUtf8();
    if (!uriList.isEmpty() && uriList.size() <= kMaxClipboardUrlBytes && copyBytesWithWlCopy(uriList, QStringLiteral("text/uri-list")))
        return true;

    auto* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return false;

    auto* mime = new QMimeData;
    mime->setUrls({url});
    clipboard->setMimeData(mime);
    return true;
}

bool copyPixmapToClipboard(const QPixmap& pixmap) {
    if (pixmap.isNull())
        return false;
    return copyImageToClipboard(pixmap.toImage());
}

void discardClipboardSnapshot(const QString& path) {
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!privateRuntimeFileExists(path, kMaxClipboardSnapshotBytes) || !file.open(QIODevice::ReadOnly)) {
        if (pathIsInPrivateRuntimeRoot(path))
            QFile::remove(path);
        return;
    }

    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isObject())
        removeSnapshotFiles(doc.object(), path);
    else if (pathIsInPrivateRuntimeRoot(path))
        QFile::remove(path);
}

void restoreClipboardSnapshot(const QString& path) {
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!privateRuntimeFileExists(path, kMaxClipboardSnapshotBytes) || !file.open(QIODevice::ReadOnly))
        return;

    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return;

    const auto obj = doc.object();
    if (obj.contains("empty") && !obj.value("empty").isBool()) {
        removeSnapshotFiles(obj, path);
        return;
    }

    QString     text;
    QString     html;
    QList<QUrl> urls;
    QColor      color;
    QString     imagePath;
    if (!snapshotStringValue(obj, QStringLiteral("text"), text) || !snapshotStringValue(obj, QStringLiteral("html"), html) ||
        !snapshotUrlsValue(obj, urls) || !snapshotColorValue(obj, color) || !snapshotImageValue(obj, imagePath)) {
        removeSnapshotFiles(obj, path);
        return;
    }

    if (obj.value("empty").toBool(false)) {
        if (!clearWithWlCopy()) {
            auto* clipboard = QGuiApplication::clipboard();
            if (clipboard)
                clipboard->clear();
        }
        removeSnapshotFiles(obj, path);
        return;
    }

    if (!imagePath.isEmpty()) {
        QFile imageFile(imagePath);
        if (imageFile.open(QIODevice::ReadOnly) && imageFile.size() <= kMaxClipboardImageBytes &&
            copyBytesWithWlCopy(imageFile.readAll(), QStringLiteral("image/png"))) {
            removeSnapshotFiles(obj, path);
            return;
        }
    }

    if (!text.isEmpty() && copyBytesWithWlCopy(text.toUtf8(), QStringLiteral("text/plain"))) {
        removeSnapshotFiles(obj, path);
        return;
    }

    if (!html.isEmpty() && copyBytesWithWlCopy(html.toUtf8(), QStringLiteral("text/html"))) {
        removeSnapshotFiles(obj, path);
        return;
    }

    if (!urls.isEmpty()) {
        QString uriList;
        for (const auto& url : urls)
            uriList += url.toString() + QStringLiteral("\r\n");
        const QByteArray bytes = uriList.toUtf8();
        if (bytes.size() <= kMaxClipboardUrlBytes && copyBytesWithWlCopy(bytes, QStringLiteral("text/uri-list"))) {
            removeSnapshotFiles(obj, path);
            return;
        }
    }

    auto* clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return;

    auto* mime = new QMimeData;
    if (!text.isEmpty())
        mime->setText(text);
    if (!html.isEmpty())
        mime->setHtml(html);
    if (!urls.isEmpty())
        mime->setUrls(urls);
    if (color.isValid())
        mime->setColorData(color);
    clipboard->setMimeData(mime);
    removeSnapshotFiles(obj, path);
}

} // namespace hyprcapture::ui
