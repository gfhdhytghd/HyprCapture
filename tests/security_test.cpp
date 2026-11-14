#include "ui/clipboard_utils.hpp"
#include "shared/trusted_path.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <cstdlib>
#include <pwd.h>
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

void testXdgNixProfile() {
    // Keep all ancestors trusted; /tmp is rejected by the executable policy.
    const auto mask = umask(0077);
    const auto trustedHome = [](const QString& path) {
        return hyprcapture::security::parentChainTrusted(QFile::encodeName(path + QStringLiteral("/fixture")).toStdString());
    };
    QString fixtureHome = qEnvironmentVariable("HYPRCAPTURE_TEST_HOME", QDir::homePath());
    // Build runners may place HOME below /tmp; fall back to the account home.
    if (!qEnvironmentVariableIsSet("HYPRCAPTURE_TEST_HOME") && !trustedHome(fixtureHome)) {
        if (const auto* account = getpwuid(geteuid()))
            fixtureHome = QFile::decodeName(account->pw_dir);
    }
    require(trustedHome(fixtureHome), "XDG profile test needs a home with trusted ancestors (set HYPRCAPTURE_TEST_HOME)");
    QTemporaryDir tree(fixtureHome + QStringLiteral("/.hyprcapture-xdg-test-XXXXXX"));
    require(tree.isValid(), "XDG profile temporary tree is created");
    const auto oldHome = qgetenv("HOME");
    const auto oldState = qgetenv("XDG_STATE_HOME");
    const QString home = tree.filePath(QStringLiteral("home"));
    const QString state = tree.filePath(QStringLiteral("state"));
    const QString bin = state + QStringLiteral("/nix/profile/bin");
    const QString fallback = home + QStringLiteral("/.local/state/nix/profile/bin");
    require(QDir().mkpath(bin) && QDir().mkpath(fallback), "XDG profile directories are created");
    const QString name = QStringLiteral("hyprcapture-xdg-profile-test-tool");
    QFile tool(bin + QLatin1Char('/') + name);
    require(tool.open(QIODevice::WriteOnly), "XDG profile executable is created");
    tool.write("#!/bin/sh\nexit 0\n");
    tool.close();
    require(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner), "XDG executable is private");
    qputenv("HOME", home.toUtf8());
    qputenv("XDG_STATE_HOME", state.toUtf8());
    require(hyprcapture::security::trustedExecutablePath(QFile::encodeName(tool.fileName()).toStdString()).has_value(),
            "fixture executable passes trust checks");
    require(hyprcapture::ui::trustedSystemProgram(name) == tool.fileName(), "absolute XDG profile executable is resolved");
    const auto paths = hyprcapture::ui::trustedProcessEnvironment().value(QStringLiteral("PATH")).split(QLatin1Char(':'));
    require(paths.indexOf(bin) == paths.indexOf(home + QStringLiteral("/.nix-profile/bin")) + 1, "XDG profile follows legacy profile");
    require(hyprcapture::ui::trustedProcessEnvironment().value(QStringLiteral("XDG_STATE_HOME")) == state, "XDG state home reaches child processes");
    require(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::WriteOther),
            "XDG executable can be made untrusted");
    require(hyprcapture::ui::trustedSystemProgram(name).isEmpty(), "world-writable XDG executable is rejected");
    require(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner), "XDG executable permissions restored");
    qputenv("XDG_STATE_HOME", QDir::current().relativeFilePath(state).toUtf8());
    require(hyprcapture::ui::trustedSystemProgram(name).isEmpty(), "relative XDG state home is ignored");
    require(tool.rename(fallback + QLatin1Char('/') + name), "executable moves to default profile");
    require(hyprcapture::ui::trustedSystemProgram(name) == tool.fileName(), "relative XDG state home uses default profile");
    qputenv("XDG_STATE_HOME", "");
    require(hyprcapture::ui::trustedSystemProgram(name) == tool.fileName(), "empty XDG state home uses default profile");
    qunsetenv("XDG_STATE_HOME");
    require(hyprcapture::ui::trustedSystemProgram(name) == tool.fileName(), "unset XDG state home uses default profile");
    oldHome.isNull() ? qunsetenv("HOME") : qputenv("HOME", oldHome);
    oldState.isNull() ? qunsetenv("XDG_STATE_HOME") : qputenv("XDG_STATE_HOME", oldState);
    umask(mask);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testXdgNixProfile();
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
