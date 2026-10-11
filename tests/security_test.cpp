#include "ui/clipboard_utils.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdlib>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

mode_t modeFor(const QString& path) {
    struct stat st {};
    const auto  native = QFile::encodeName(path);
    if (stat(native.constData(), &st) != 0) {
        std::cerr << "security test failed: stat failed for " << path.toStdString() << '\n';
        std::exit(1);
    }
    return st.st_mode & 0777;
}

void require(bool condition, const char* message) {
    if (condition)
        return;

    std::cerr << "security test failed: " << message << '\n';
    std::exit(1);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto originalMask = umask(0000);

    const QString path = hyprcapture::ui::runtimeFile(QStringLiteral("security"), QStringLiteral(".png"));
    require(!path.isEmpty(), "runtime path is not empty");
    require(path.contains(QStringLiteral("hyprcapture-%1").arg(QString::number(static_cast<qulonglong>(geteuid())))), "runtime path is per-user");

    const QFileInfo info(path);
    require(modeFor(info.absolutePath()) == 0700, "runtime directory is 0700");

    QImage image(QSize(1, 1), QImage::Format_ARGB32);
    image.fill(Qt::red);
    require(hyprcapture::ui::savePrivatePng(image, path), "private png save succeeds");
    require(modeFor(path) == 0600, "private png is 0600");
    require(hyprcapture::ui::isPrivateRuntimeFile(path, 1024 * 1024), "private png is accepted as private runtime file");

    QFile::remove(path);
    char publicDirectory[] = "/tmp/hyprcapture-public-output-XXXXXX";
    require(mkdtemp(publicDirectory) != nullptr, "public output directory");
    require(chmod(publicDirectory, 0755) == 0, "traversable output directory");
    const QString publicPath = QString::fromLocal8Bit(publicDirectory) + "/thumbnail.png";
    require(hyprcapture::ui::savePrivatePng(image, publicPath), "PNG save in public directory");
    require(modeFor(publicPath) == 0600, "PNG private even with umask zero");
    QFile::remove(publicPath);
    require(rmdir(publicDirectory) == 0, "remove public output directory");

    hyprcapture::ui::ClipboardSnapshotData snapshot;
    snapshot.valid = true;
    snapshot.empty = false;
    snapshot.text = QStringLiteral("previous clipboard");
    snapshot.image = image;
    const QString snapshotPath = hyprcapture::ui::saveClipboardSnapshotData(snapshot);
    require(!snapshotPath.isEmpty(), "clipboard snapshot save succeeds");

    QFile snapshotFile(snapshotPath);
    require(snapshotFile.open(QIODevice::ReadOnly), "clipboard snapshot json opens");
    const auto snapshotDoc = QJsonDocument::fromJson(snapshotFile.readAll());
    const QString imagePath = snapshotDoc.object().value(QStringLiteral("image")).toString();
    require(!imagePath.isEmpty() && QFileInfo::exists(imagePath), "clipboard snapshot image exists");
    hyprcapture::ui::discardClipboardSnapshot(snapshotPath);
    require(!QFileInfo::exists(snapshotPath), "clipboard snapshot json is discarded");
    require(!QFileInfo::exists(imagePath), "clipboard snapshot image is discarded");

    hyprcapture::ui::ClipboardSnapshotData oversized;
    oversized.valid = true;
    oversized.empty = false;
    oversized.text = QString(5 * 1024 * 1024, QLatin1Char('x'));
    require(hyprcapture::ui::saveClipboardSnapshotData(oversized).isEmpty(), "oversized clipboard text is rejected");
    oversized.text = QString(17 * 1024 * 1024, QLatin1Char('x'));
    require(hyprcapture::ui::saveClipboardSnapshotData(oversized).isEmpty(), "oversized clipboard snapshot is rejected");

    hyprcapture::ui::ClipboardSnapshotData tooManyUrls;
    tooManyUrls.valid = true;
    tooManyUrls.empty = false;
    for (int i = 0; i < 129; ++i)
        tooManyUrls.urls.push_back(QUrl(QStringLiteral("file:///tmp/%1").arg(i)));
    require(hyprcapture::ui::saveClipboardSnapshotData(tooManyUrls).isEmpty(), "oversized clipboard url list is rejected");

    QDir(QFileInfo(info.absolutePath()).absolutePath()).rmdir(QFileInfo(info.absolutePath()).fileName());

    umask(originalMask);
    std::cout << "hyprcapture security tests passed\n";
    return 0;
}
