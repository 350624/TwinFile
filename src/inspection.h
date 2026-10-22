#pragma once
#include "core.h"
#include <QCryptographicHash>
#include <QDialog>
#include <QTemporaryDir>

namespace twinfile {
QByteArray normalizedChecksum(const QString &text);
Result checksumEntries(const QList<Entry> &entries, const Cancellation &cancel = {}, const QString &expected = {});
Result checksumManifest(const QStringList &paths, const QString &base, QCryptographicHash::Algorithm algorithm, const Cancellation &cancel = {});
Result verifyChecksumManifest(const QString &manifest, const QString &base, const Cancellation &cancel = {});
struct DirectoryRow { QString name, status, left, right; bool directory = false; };
struct DirectoryComparison { QList<DirectoryRow> rows; QString summary, output, error; };
DirectoryComparison inspectDirectories(const QString &left, const QString &right, int mode, const Cancellation &cancel = {});
Result compareDirectories(const QString &left, const QString &right, int mode, const Cancellation &cancel = {});
class DirectoryComparisonDialog : public QDialog {
public:
    DirectoryComparisonDialog(DirectoryComparison comparison, std::shared_ptr<QTemporaryDir> temporary, QWidget *parent);
};
}
