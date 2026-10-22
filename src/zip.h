#pragma once

#include "core.h"
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QIODevice>
#include <memory>

namespace twinfile {

struct ZipInfo {
    QString filePath;
    bool isDir = false;
    bool isFile = false;
    bool isSymLink = false;
    qint64 size = 0;
    QDateTime lastModified;
    int compression = 8;
    unsigned attributes = 0100644U << 16;
};

class ZipReader {
public:
    enum Status { NoError, FileError };
    explicit ZipReader(const QString &path);
    ~ZipReader();
    QList<ZipInfo> fileInfoList() const { return files_; }
    Status status() const { return error_.isEmpty() ? NoError : FileError; }
    QString errorString() const { return error_; }
    QByteArray fileData(const QString &name, qint64 limit = 128 * 1024 * 1024);
    bool streamFile(const QString &name, QIODevice *output, const Cancellation &cancel = {});
private:
    QFile file_;
    void *handle_ = nullptr;
    QList<ZipInfo> files_;
    QString error_;
    QHash<QString, quint64> positions_;
};

class ZipWriter {
public:
    enum Status { NoError, FileError };
    explicit ZipWriter(QIODevice *output);
    explicit ZipWriter(const QString &path);
    ~ZipWriter();
    void addDirectory(const QString &name, const ZipInfo *original = nullptr);
    void addSymLink(const QString &name, const QString &target);
    void addFile(const QString &name, const QByteArray &bytes);
    void addFile(const QString &name, QIODevice *input, const Cancellation &cancel = {}, const ZipInfo *original = nullptr);
    void close();
    Status status() const { return failed_ ? FileError : NoError; }
private:
    void open(QIODevice *output);
    void add(const QString &name, QIODevice *input, unsigned attributes, const Cancellation &cancel, const ZipInfo *original = nullptr);
    std::unique_ptr<QFile> file_;
    void *handle_ = nullptr;
    QIODevice *output_ = nullptr;
    bool failed_ = false;
};

}
