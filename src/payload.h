#pragma once

#include "core.h"

namespace twinfile {

struct PayloadExtent { quint64 start = 0; quint64 blocks = 0; };
struct PayloadOperation {
    quint64 type = 0;
    quint64 offset = 0;
    quint64 length = 0;
    QList<PayloadExtent> extents;
    QByteArray hash;
};
struct PayloadPartition {
    QString name;
    quint64 size = 0;
    QByteArray hash;
    QList<PayloadOperation> operations;
};
struct PayloadManifest {
    quint64 blockSize = 4096;
    QList<PayloadPartition> partitions;
};
struct PayloadOptions {
    QString userAgent = QStringLiteral("TwinFileNative/" TWINFILE_VERSION);
    int idleTimeoutMs = 15000;
    int requestTimeoutMs = 60000;
};

Result parsePayloadManifest(const QByteArray &bytes, PayloadManifest *manifest, const Cancellation &cancel = {});
Result inspectPayload(const QString &source, const Cancellation &cancel = {}, const PayloadOptions &options = {}, const Progress &progress = {});
Result extractPayload(const QString &source, const QStringList &partitions, const QString &destination,
                      int workers = 4, const Cancellation &cancel = {}, const Progress &progress = {}, const PayloadOptions &options = {});

}
