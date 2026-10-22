#pragma once

#include "core.h"
#include <QMap>
#include <QTemporaryDir>
#include <mutex>

namespace twinfile {

struct ArchiveChanges {
    QStringList remove;
    QMap<QString, QString> rename;
    QMap<QString, QString> files;
    QStringList directories;
    QMap<QString, QByteArray> expected;
};
struct MountedArchive {
    Entry original;
    QString local;
    QByteArray hash;
    std::shared_ptr<QTemporaryDir> temporary;
    std::mutex mutex;
};
using ArchiveMounts = QMap<QString, std::shared_ptr<MountedArchive>>;

Result editArchive(const QString &path, const ArchiveChanges &changes, const Cancellation &cancel = {});
Result phoneCommand(const QString &serial, const QString &command, const Cancellation &cancel = {});
Result createEntry(const Location &location, const QString &name, bool directory, const Cancellation &cancel = {});
Result removeEntries(const QList<Entry> &entries, const Cancellation &cancel = {});
Result renameEntry(const Entry &entry, const QString &name, const Cancellation &cancel = {});
Result materializeEntry(const Entry &entry, const QString &destination, const Cancellation &cancel = {});
Result writeBackEntry(const Entry &entry, const QString &local, const QByteArray &expectedHash, const Cancellation &cancel = {});
Result transferEntries(const QList<Entry> &entries, const Location &destination, bool move, const Cancellation &cancel = {});
QByteArray fileDigest(const QString &path, const Cancellation &cancel = {});
Result commitArchiveMounts(const ArchiveMounts &mounts, const Cancellation &cancel = {});

}
