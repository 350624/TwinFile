#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>

namespace twinfile {

enum class Source { Local, Drive, Archive, Device, Phone };

struct Entry {
    QString name;
    QString path;
    QString inner;
    QString serial;
    QString state;
    QString detail;
    bool directory = false;
    qint64 size = 0;
    QDateTime modified;
    Source source = Source::Local;
    QString key() const;
};

struct Location {
    Source source = Source::Drive;
    QString path;
    QString inner;
    QString label;
    QString display() const;
    QJsonObject json() const;
    static Location fromJson(const QJsonObject &object);
    bool operator==(const Location &) const = default;
};

struct Listing {
    QList<Entry> entries;
    QString error;
};

struct Result {
    bool ok = true;
    QString error;
    QString output;
    QStringList paths;
};

using Cancellation = std::shared_ptr<std::atomic_bool>;
using Progress = std::function<void(qint64, qint64, const QString &)>;

bool isApk(const QString &name);
bool isArchive(const QString &name);
bool isImage(const QString &name);
bool isText(const QString &name);
QString humanSize(qint64 bytes);
QString shellQuote(const QString &value);
QString toolPath(const QString &relative);
QString safeArchiveName(const QString &name);
QString normalizedPath(const QString &path);
QString validateName(const QString &name);
Listing listLocal(const QString &path, bool hidden, const QString &sort, bool descending);
Listing listArchive(const QString &path, const QString &inner);
Listing listPhone(const QString &serial, const QString &path, bool hidden);
Listing searchLocal(const QString &path, const QString &pattern, bool hidden, const Cancellation &cancel);
Listing searchLocation(const Location &location, const QString &pattern, bool hidden, const Cancellation &cancel);
Result runProcess(const QString &program, const QStringList &arguments, int timeoutMs = 30000,
                  const Cancellation &cancel = {});
Result transferFiles(const QStringList &sources, const QString &destination, bool move,
                     const Cancellation &cancel = {}, const Progress &progress = {});
Result batchRename(const QStringList &paths, const QStringList &names);
Result recycleFiles(const QStringList &paths);
Result extractArchive(const QString &path, const QString &destination, const QString &prefix = {},
                      const Cancellation &cancel = {}, const Progress &progress = {});
Result createArchive(const QStringList &paths, const QString &destination, const Cancellation &cancel = {}, const Progress &progress = {});
Result checksum(const QStringList &paths, const Cancellation &cancel = {});
Result phoneTransfer(const QList<Entry> &entries, const Location &destination, bool move,
                     const Cancellation &cancel = {}, const Progress &progress = {});
QByteArray readArchiveFile(const QString &path, const QString &inner, qint64 limit, QString *error);
QJsonObject readSettings(const QString &path);
QJsonObject migrateLegacySettings(const QJsonObject &legacy);
bool writeSettings(const QString &path, const QJsonObject &settings, QString *error = nullptr);
QString settingsPath();

}

Q_DECLARE_METATYPE(twinfile::Entry)
