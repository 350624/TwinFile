#include "thumbnails.h"
#include "android.h"
#include "shell.h"

#include <QCache>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QMutex>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>

namespace twinfile {
namespace {

QMutex cacheMutex;
QCache<QString, QImage> images(32 * 1024);

QString cacheDirectory() {
    return qEnvironmentVariable("TWINFILE_THUMBNAIL_CACHE", QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)).filePath(QStringLiteral("TwinFileNative/thumbnails-v2")));
}

void remember(const QString &key, const QImage &image) {
    QMutexLocker lock(&cacheMutex); images.insert(key, new QImage(image), qMax<qsizetype>(1, image.sizeInBytes() / 1024));
}

void pruneCache(const QString &directory) {
    static QMutex mutex; static int writes = 0;
    if (!mutex.tryLock()) return;
    const auto unlock = qScopeGuard([&] { mutex.unlock(); });
    if (++writes % 64) return;
    const auto files = QDir(directory).entryInfoList({QStringLiteral("*.png")}, QDir::Files | QDir::NoSymLinks, QDir::Time);
    qint64 bytes = 0; int retained = 0;
    for (const auto &file : files) {
        if (file.completeBaseName().size() != 64) continue;
        bytes += file.size();
        if (++retained > 1024 || bytes > 64 * 1024 * 1024) QFile::remove(file.absoluteFilePath());
    }
}

}

QThreadPool *thumbnailPool() {
    static QThreadPool pool;
    static const bool initialized = [] { pool.setMaxThreadCount(4); pool.setExpiryTimeout(15000); return true; }();
    Q_UNUSED(initialized)
    return &pool;
}

bool supportsThumbnail(const Entry &entry) {
    if (entry.source != Source::Local || entry.directory) return false;
    return isApk(entry.name) || isImage(entry.name) || QStringList{QStringLiteral("exe"), QStringLiteral("dll"), QStringLiteral("lnk"), QStringLiteral("msi"), QStringLiteral("ico")}.contains(QFileInfo(entry.name).suffix().toLower());
}

QString thumbnailCacheKey(const Entry &entry) {
    const QString identity = QDir::fromNativeSeparators(entry.path).toCaseFolded() + QChar('\n') + QString::number(entry.size) + QChar('\n') + QString::number(entry.modified.toMSecsSinceEpoch());
    return QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QImage cachedThumbnail(const Entry &entry) {
    const QString key = thumbnailCacheKey(entry); QMutexLocker lock(&cacheMutex);
    const QImage *image = images.object(key); return image ? *image : QImage();
}

QImage loadThumbnail(const Entry &entry, const Cancellation &cancel) {
    if (!supportsThumbnail(entry) || (cancel && cancel->load())) return {};
    QImage image = cachedThumbnail(entry); if (!image.isNull()) return image;
    const QString key = thumbnailCacheKey(entry), directory = cacheDirectory(), path = QDir(directory).filePath(key + QStringLiteral(".png"));
    const QFileInfo cache(path);
    if (cache.isFile() && cache.size() <= 256 * 1024) {
        QImageReader reader(path); const QSize size = reader.size();
        if (size.isValid() && size.width() <= 128 && size.height() <= 128) image = reader.read();
        if (!image.isNull()) { remember(key, image); return image; }
    }
    if (cancel && cancel->load()) return {};
    if (isApk(entry.name)) image = apkIcon(entry.path, cancel);
    else if (isImage(entry.name)) {
        if (entry.size > 64 * 1024 * 1024) return {};
        QImageReader reader(entry.path); reader.setAutoTransform(true); const QSize size = reader.size();
        if (size.isValid() && qint64(size.width()) * size.height() <= 64000000) { reader.setScaledSize(size.scaled(128, 128, Qt::KeepAspectRatio)); image = reader.read(); }
        if (image.isNull() && QFileInfo(entry.name).suffix().compare(QStringLiteral("webp"), Qt::CaseInsensitive) == 0 && entry.size <= 4 * 1024 * 1024) {
            QFile file(entry.path); if (file.open(QIODevice::ReadOnly)) image = androidBitmap(file.read(4 * 1024 * 1024));
        }
    } else image = shellThumbnail(entry.path, cancel);
    if (image.isNull() || (cancel && cancel->load())) return {};
    image = image.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QFileInfo current(entry.path);
    if (!current.isFile() || current.size() != entry.size || current.lastModified() != entry.modified) return {};
    remember(key, image);
    if (QDir().mkpath(directory)) {
        QSaveFile file(path); if (file.open(QIODevice::WriteOnly) && image.save(&file, "PNG")) file.commit();
        pruneCache(directory);
    }
    return image;
}

}
