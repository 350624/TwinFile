#include "payload.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QtEndian>
#include <bzlib.h>
#include <lzma.h>
#include <zstd.h>
#include <zlib.h>
#include <algorithm>
#include <future>
#include <limits>
#include <stdexcept>
#include <vector>

namespace twinfile {
namespace {

class PayloadError : public std::runtime_error {
public:
    explicit PayloadError(const QString &message) : std::runtime_error(message.toUtf8().constData()) {}
};

void ensure(bool value, const QString &message) { if (!value) throw PayloadError(message); }
void checkCancel(const Cancellation &cancel) { ensure(!cancel || !cancel->load(), QStringLiteral("Payload 操作已取消")); }
bool fits(quint64 offset, quint64 size, quint64 total) { return offset <= total && size <= total - offset; }
quint16 little16(const QByteArray &bytes, qsizetype offset) { ensure(offset >= 0 && offset + 2 <= bytes.size(), QStringLiteral("ZIP 字段越界")); return qFromLittleEndian<quint16>(bytes.constData() + offset); }
quint32 little32(const QByteArray &bytes, qsizetype offset) { ensure(offset >= 0 && offset + 4 <= bytes.size(), QStringLiteral("ZIP 字段越界")); return qFromLittleEndian<quint32>(bytes.constData() + offset); }
quint64 little64(const QByteArray &bytes, qsizetype offset) { ensure(offset >= 0 && offset + 8 <= bytes.size(), QStringLiteral("ZIP64 字段越界")); return qFromLittleEndian<quint64>(bytes.constData() + offset); }

class Proto {
public:
    explicit Proto(QByteArray bytes) : bytes_(std::move(bytes)) {}
    bool next() {
        if (position_ == bytes_.size()) return false;
        const quint64 tag = integer(); field = tag >> 3; wire = tag & 7;
        ensure(field > 0, QStringLiteral("protobuf 字段编号无效")); value = 0; data.clear();
        if (wire == 0) value = integer();
        else if (wire == 2) {
            const quint64 length = integer(); ensure(length <= quint64(bytes_.size() - position_), QStringLiteral("protobuf 数据越界"));
            data = bytes_.mid(position_, qsizetype(length)); position_ += qsizetype(length);
        } else if (wire == 1 || wire == 5) {
            const int length = wire == 1 ? 8 : 4; ensure(position_ + length <= bytes_.size(), QStringLiteral("protobuf 固定字段越界")); position_ += length;
        } else throw PayloadError(QStringLiteral("不支持的 protobuf wire type"));
        return true;
    }
    quint64 field = 0, wire = 0, value = 0;
    QByteArray data;
private:
    quint64 integer() {
        quint64 result = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            ensure(position_ < bytes_.size(), QStringLiteral("protobuf 整数被截断"));
            const auto byte = quint8(bytes_[position_++]);
            ensure(shift != 63 || byte <= 1, QStringLiteral("protobuf 整数溢出")); result |= quint64(byte & 127) << shift;
            if (!(byte & 128)) return result;
        }
        throw PayloadError(QStringLiteral("protobuf 整数过长"));
    }
    QByteArray bytes_;
    qsizetype position_ = 0;
};

class RangeSource {
public:
    QString source;
    QByteArray userAgent;
    QByteArray etag, modified;
    quint64 total = 0, base = 0, length = 0;
    std::shared_ptr<QTemporaryFile> temporary;

    QByteArray raw(quint64 offset, quint64 count, const Cancellation &cancel) {
        checkCancel(cancel); ensure(count <= 8 * 1024 * 1024 && offset <= quint64(std::numeric_limits<qint64>::max()) && count <= quint64(std::numeric_limits<qint64>::max()) - offset, QStringLiteral("读取范围过大或越界"));
        if (!source.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) && !source.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
            QFile file(source); ensure(file.open(QIODevice::ReadOnly), file.errorString());
            if (total) ensure(total == quint64(file.size()), QStringLiteral("源文件大小在提取期间发生变化"));
            total = quint64(file.size()); ensure(fits(offset, count, total) && file.seek(qint64(offset)), QStringLiteral("读取超出源文件"));
            const QByteArray bytes = file.read(qint64(count)); ensure(quint64(bytes.size()) == count, QStringLiteral("源文件读取不完整")); return bytes;
        }
        ensure(count > 0, QStringLiteral("HTTP Range 长度为空"));
        QString lastError;
        for (int attempt = 0; attempt < 3; ++attempt) {
            checkCancel(cancel);
            QNetworkAccessManager manager; QNetworkRequest request{QUrl(source)};
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            request.setRawHeader("Accept-Encoding", "identity"); request.setRawHeader("Range", "bytes=" + QByteArray::number(offset) + '-' + QByteArray::number(offset + count - 1));
            request.setRawHeader("User-Agent", userAgent);
            if (!etag.isEmpty()) request.setRawHeader("If-Match", etag);
            else if (!modified.isEmpty()) request.setRawHeader("If-Unmodified-Since", modified);
            request.setTransferTimeout(60000);
            QNetworkReply *reply = manager.get(request); QEventLoop loop; QTimer poll; poll.setInterval(50); QByteArray bytes;
            bool oversized = false;
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (cancel && cancel->load()) reply->abort(); });
            QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
                bytes += reply->read(qint64(count + 1 - quint64(bytes.size())));
                if (quint64(bytes.size()) > count) { oversized = true; reply->abort(); }
            });
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit); poll.start(); loop.exec();
            bytes += reply->read(qint64(count + 1 - quint64(bytes.size()))); checkCancel(cancel);
            ensure(!oversized && quint64(bytes.size()) <= count, QStringLiteral("服务器忽略 Range 或响应超出范围"));
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            ensure(status == 206 || status >= 500 || status == 429 || status == 0, QStringLiteral("HTTP 服务必须支持严格的 206 Range（状态 %1）").arg(status));
            if (reply->error() != QNetworkReply::NoError || status != 206 || quint64(bytes.size()) != count) { lastError = reply->errorString(); continue; }
            const QRegularExpression range(QStringLiteral("^bytes (\\d+)-(\\d+)/(\\d+)$"));
            const auto match = range.match(QString::fromLatin1(reply->rawHeader("Content-Range")));
            ensure(match.hasMatch() && match.captured(1).toULongLong() == offset && match.captured(2).toULongLong() == offset + count - 1, QStringLiteral("服务器返回了错误的 Content-Range"));
            const quint64 size = match.captured(3).toULongLong(); ensure(size > offset + count - 1 && (!total || total == size), QStringLiteral("远程文件长度变化或不合法"));
            const QByteArray encoding = reply->rawHeader("Content-Encoding").toLower(); ensure(encoding.isEmpty() || encoding == "identity", QStringLiteral("Range 响应使用了内容压缩"));
            const QByteArray currentTag = reply->rawHeader("ETag"), currentModified = reply->rawHeader("Last-Modified");
            ensure(etag.isEmpty() || currentTag == etag, QStringLiteral("远程文件 ETag 发生变化"));
            ensure(modified.isEmpty() || currentModified == modified, QStringLiteral("远程文件修改时间变化"));
            total = size; if (!currentTag.startsWith("W/")) etag = currentTag; modified = currentModified; return bytes;
        }
        throw PayloadError(QStringLiteral("Range 请求重试失败：") + lastError);
    }
    QByteArray read(quint64 offset, quint64 count, const Cancellation &cancel) {
        ensure(fits(offset, count, length), QStringLiteral("Payload 读取越界")); return count ? raw(base + offset, count, cancel) : QByteArray();
    }
};

RangeSource locate(QString source, const Cancellation &cancel, const PayloadOptions &options) {
    RangeSource reader; reader.source = std::move(source); reader.userAgent = options.userAgent.toUtf8();
    ensure(!reader.userAgent.isEmpty() && reader.userAgent.size() <= 512 && std::all_of(reader.userAgent.cbegin(), reader.userAgent.cend(), [](char value) { return quint8(value) >= 32 && quint8(value) <= 126; }), QStringLiteral("User-Agent 只能包含 1–512 个可打印 ASCII 字符"));
    const QByteArray magic = reader.raw(0, 4, cancel); reader.length = reader.total;
    if (magic == "CrAU") return reader;
    ensure(magic.startsWith("PK"), QStringLiteral("不是 Payload 或 ZIP 文件"));
    const quint64 tailSize = qMin<quint64>(reader.total, 65557); const quint64 tailOffset = reader.total - tailSize;
    const QByteArray tail = reader.raw(tailOffset, tailSize, cancel);
    qsizetype end = -1;
    for (qsizetype cursor = tail.size() - 22; cursor >= 0; --cursor) {
        if (tail.mid(cursor, 4) == QByteArray("PK\x05\x06", 4) && cursor + 22 + little16(tail, cursor + 20) == tail.size()) { end = cursor; break; }
    }
    ensure(end >= 0, QStringLiteral("找不到 ZIP 结束目录"));
    ensure(little16(tail, end + 4) == 0 && little16(tail, end + 6) == 0, QStringLiteral("不支持分卷 ZIP"));
    quint64 count = little16(tail, end + 10), directorySize = little32(tail, end + 12), directoryOffset = little32(tail, end + 16);
    if (count == 65535 || directorySize == 0xffffffff || directoryOffset == 0xffffffff) {
        ensure(end >= 20 && tail.mid(end - 20, 4) == QByteArray("PK\x06\x07", 4), QStringLiteral("缺少 ZIP64 locator"));
        ensure(little32(tail, end - 16) == 0 && little32(tail, end - 4) == 1, QStringLiteral("不支持 ZIP64 分卷"));
        const QByteArray record = reader.raw(little64(tail, end - 12), 56, cancel);
        ensure(record.startsWith(QByteArray("PK\x06\x06", 4)) && little32(record, 16) == 0 && little32(record, 20) == 0, QStringLiteral("ZIP64 目录无效"));
        count = little64(record, 32); directorySize = little64(record, 40); directoryOffset = little64(record, 48);
    }
    ensure(count <= 200000 && directorySize <= 64 * 1024 * 1024 && fits(directoryOffset, directorySize, reader.total), QStringLiteral("ZIP 目录超过安全限制"));
    QByteArray directory;
    for (quint64 offset = 0; offset < directorySize; offset += 4 * 1024 * 1024) directory += reader.raw(directoryOffset + offset, qMin<quint64>(4 * 1024 * 1024, directorySize - offset), cancel);
    qsizetype cursor = 0; bool found = false; quint64 compressed = 0, uncompressed = 0, localOffset = 0; quint16 method = 0, flags = 0; quint32 expectedCrc = 0;
    for (quint64 index = 0; index < count; ++index) {
        ensure(cursor + 46 <= directory.size() && directory.mid(cursor, 4) == QByteArray("PK\x01\x02", 4), QStringLiteral("ZIP 中央目录损坏"));
        const int nameLength = little16(directory, cursor + 28), extraLength = little16(directory, cursor + 30), commentLength = little16(directory, cursor + 32);
        ensure(cursor + 46 + nameLength + extraLength + commentLength <= directory.size(), QStringLiteral("ZIP 条目越界"));
        const QByteArray name = directory.mid(cursor + 46, nameLength);
        if (name == "payload.bin") {
            ensure(!found, QStringLiteral("ZIP 包含重复的 payload.bin")); found = true;
            flags = little16(directory, cursor + 8); method = little16(directory, cursor + 10); expectedCrc = little32(directory, cursor + 16);
            compressed = little32(directory, cursor + 20); uncompressed = little32(directory, cursor + 24); localOffset = little32(directory, cursor + 42);
            const QByteArray extra = directory.mid(cursor + 46 + nameLength, extraLength); bool zip64 = false;
            for (qsizetype extraCursor = 0; extraCursor + 4 <= extra.size();) {
                const quint16 type = little16(extra, extraCursor), size = little16(extra, extraCursor + 2); extraCursor += 4;
                ensure(extraCursor + size <= extra.size(), QStringLiteral("ZIP extra 数据越界"));
                if (type == 1) { zip64 = true; const qsizetype limit = extraCursor + size;
                    for (quint64 *value : {&uncompressed, &compressed, &localOffset}) if (*value == 0xffffffff) { ensure(extraCursor + 8 <= limit, QStringLiteral("ZIP64 extra 缺失")); *value = little64(extra, extraCursor); extraCursor += 8; }
                    extraCursor = limit;
                } else extraCursor += size;
            }
            ensure(zip64 || (compressed != 0xffffffff && uncompressed != 0xffffffff && localOffset != 0xffffffff), QStringLiteral("缺少 ZIP64 尺寸"));
        }
        cursor += 46 + nameLength + extraLength + commentLength;
    }
    ensure(found && !(flags & 1) && (method == 0 || method == 8), QStringLiteral("找不到未加密且支持的 payload.bin"));
    const QByteArray local = reader.raw(localOffset, 30, cancel);
    ensure(local.startsWith(QByteArray("PK\x03\x04", 4)) && little16(local, 8) == method, QStringLiteral("ZIP 本地文件头不匹配"));
    const quint64 start = localOffset + 30 + little16(local, 26) + little16(local, 28);
    ensure(fits(start, compressed, reader.total) && uncompressed <= 256ULL * 1024 * 1024 * 1024, QStringLiteral("ZIP payload 超出安全限制"));
    if (method == 0) { ensure(compressed == uncompressed, QStringLiteral("STORED ZIP 尺寸不匹配")); reader.base = start; reader.length = uncompressed; return reader; }
    auto temporary = std::make_shared<QTemporaryFile>(); ensure(temporary->open(), QStringLiteral("不能创建 Payload 解压缓存"));
    z_stream stream{}; ensure(inflateInit2(&stream, -MAX_WBITS) == Z_OK, QStringLiteral("不能初始化 ZIP 解压"));
    quint64 written = 0; quint32 crc = crc32(0, nullptr, 0); int status = Z_OK; QByteArray output(1024 * 1024, '\0');
    try {
        for (quint64 offset = 0; offset < compressed; offset += 1024 * 1024) {
            QByteArray input = reader.raw(start + offset, qMin<quint64>(1024 * 1024, compressed - offset), cancel);
            stream.next_in = reinterpret_cast<Bytef *>(input.data()); stream.avail_in = uInt(input.size());
            do {
                checkCancel(cancel); stream.next_out = reinterpret_cast<Bytef *>(output.data()); stream.avail_out = uInt(output.size()); status = inflate(&stream, Z_NO_FLUSH);
                ensure(status == Z_OK || status == Z_STREAM_END, QStringLiteral("payload.bin ZIP 解压失败")); const quint64 produced = output.size() - stream.avail_out;
                ensure(fits(written, produced, uncompressed), QStringLiteral("ZIP 解压超出声明大小"));
                ensure(temporary->write(output.constData(), qint64(produced)) == qint64(produced), temporary->errorString()); crc = crc32(crc, reinterpret_cast<const Bytef *>(output.constData()), uInt(produced)); written += produced;
                if (status == Z_STREAM_END) { ensure(!stream.avail_in && offset + quint64(input.size()) == compressed, QStringLiteral("ZIP 压缩数据存在尾随内容")); break; }
            } while (stream.avail_in || !stream.avail_out);
        }
        ensure(status == Z_STREAM_END && written == uncompressed && crc == expectedCrc, QStringLiteral("ZIP 解压尺寸或 CRC 校验失败"));
        ensure(temporary->flush(), temporary->errorString()); inflateEnd(&stream);
    } catch (...) { inflateEnd(&stream); throw; }
    reader.source = temporary->fileName(); temporary->close(); reader.temporary = temporary; reader.etag.clear(); reader.modified.clear(); reader.base = 0; reader.total = reader.length = uncompressed; return reader;
}

struct OpenPayload { RangeSource source; PayloadManifest manifest; quint64 dataOffset = 0; };
OpenPayload openPayload(const QString &source, const Cancellation &cancel, const PayloadOptions &options) {
    OpenPayload payload; payload.source = locate(source, cancel, options); const QByteArray header = payload.source.read(0, 24, cancel);
    ensure(header.startsWith("CrAU") && qFromBigEndian<quint64>(header.constData() + 4) == 2, QStringLiteral("仅支持 CrAU v2 Payload"));
    const quint64 manifestSize = qFromBigEndian<quint64>(header.constData() + 12), signatureSize = qFromBigEndian<quint32>(header.constData() + 20);
    ensure(manifestSize > 0 && manifestSize <= 64 * 1024 * 1024 && fits(24, manifestSize + signatureSize, payload.source.length), QStringLiteral("Payload 元数据长度无效"));
    QByteArray bytes;
    for (quint64 offset = 0; offset < manifestSize; offset += 4 * 1024 * 1024) bytes += payload.source.read(24 + offset, qMin<quint64>(4 * 1024 * 1024, manifestSize - offset), cancel);
    const Result parsed = parsePayloadManifest(bytes, &payload.manifest); ensure(parsed.ok, parsed.error); payload.dataOffset = 24 + manifestSize + signatureSize; return payload;
}

void extractPartition(RangeSource source, const PayloadPartition &partition, quint64 blockSize, quint64 dataOffset,
                      const QString &path, const Cancellation &cancel, const Progress &progress) {
    QFile output(path); ensure(output.open(QIODevice::ReadWrite | QIODevice::NewOnly) && output.resize(qint64(partition.size)), output.errorString());
    quint64 done = 0;
    for (const PayloadOperation &operation : partition.operations) {
        checkCancel(cancel); ensure(fits(dataOffset, operation.offset, source.length) && fits(dataOffset + operation.offset, operation.length, source.length), QStringLiteral("分区操作读取越界"));
        quint64 expected = 0;
        for (const auto &extent : operation.extents) expected += extent.blocks * blockSize;
        quint64 produced = 0, extentPosition = 0; qsizetype extentIndex = 0;
        auto write = [&](const char *bytes, quint64 size) {
            ensure(fits(produced, size, expected), QStringLiteral("解压输出超过目标 extent"));
            while (size) {
                checkCancel(cancel); ensure(extentIndex < operation.extents.size(), QStringLiteral("目标 extent 不足"));
                const auto &extent = operation.extents[extentIndex]; const quint64 count = qMin(size, extent.blocks * blockSize - extentPosition);
                ensure(count > 0 && output.seek(qint64(extent.start * blockSize + extentPosition)) && output.write(bytes, qint64(count)) == qint64(count), QStringLiteral("分区写入失败"));
                bytes += count; size -= count; produced += count; extentPosition += count;
                if (extentPosition == extent.blocks * blockSize) { extentPosition = 0; ++extentIndex; }
            }
        };
        if (operation.type == 6 || operation.type == 7) {
            const QByteArray zero(1024 * 1024, '\0'); while (produced < expected) write(zero.constData(), qMin<quint64>(zero.size(), expected - produced));
        } else {
            bz_stream bz{}; lzma_stream xz = LZMA_STREAM_INIT; ZSTD_DStream *zstd = nullptr;
            bool bzReady = false, xzReady = false, ended = false;
            QByteArray decoded(1024 * 1024, '\0'); QCryptographicHash digest(QCryptographicHash::Sha256);
            try {
                if (operation.type == 1) { ensure(BZ2_bzDecompressInit(&bz, 0, 0) == BZ_OK, QStringLiteral("BZ2 初始化失败")); bzReady = true; }
                if (operation.type == 8) { ensure(lzma_stream_decoder(&xz, 256 * 1024 * 1024, 0) == LZMA_OK, QStringLiteral("XZ 初始化失败")); xzReady = true; }
                if (operation.type == 14) { zstd = ZSTD_createDStream(); ensure(zstd && !ZSTD_isError(ZSTD_initDStream(zstd)) && !ZSTD_isError(ZSTD_DCtx_setParameter(zstd, ZSTD_d_windowLogMax, 28)), QStringLiteral("ZSTD 初始化失败")); }
                for (quint64 offset = 0; offset < operation.length; offset += 1024 * 1024) {
                    const QByteArray input = source.read(dataOffset + operation.offset + offset, qMin<quint64>(1024 * 1024, operation.length - offset), cancel); digest.addData(input);
                    ensure(!ended, QStringLiteral("压缩流有尾随数据"));
                    if (operation.type == 0) { write(input.constData(), quint64(input.size())); continue; }
                    qsizetype consumed = 0;
                    do {
                        checkCancel(cancel); quint64 count = 0; const qsizetype previous = consumed;
                        if (bzReady) {
                            bz.next_in = const_cast<char *>(input.constData() + consumed); bz.avail_in = unsigned(input.size() - consumed); bz.next_out = decoded.data(); bz.avail_out = unsigned(decoded.size());
                            const int status = BZ2_bzDecompress(&bz); ensure(status == BZ_OK || status == BZ_STREAM_END, QStringLiteral("BZ2 数据损坏")); consumed = input.size() - bz.avail_in; count = decoded.size() - bz.avail_out; ended = status == BZ_STREAM_END;
                        } else if (xzReady) {
                            xz.next_in = reinterpret_cast<const uint8_t *>(input.constData() + consumed); xz.avail_in = size_t(input.size() - consumed); xz.next_out = reinterpret_cast<uint8_t *>(decoded.data()); xz.avail_out = size_t(decoded.size());
                            const lzma_ret status = lzma_code(&xz, LZMA_RUN); ensure(status == LZMA_OK || status == LZMA_STREAM_END, QStringLiteral("XZ 数据损坏或内存限制超出")); consumed = input.size() - qsizetype(xz.avail_in); count = decoded.size() - xz.avail_out; ended = status == LZMA_STREAM_END;
                        } else {
                            ZSTD_inBuffer in{input.constData(), size_t(input.size()), size_t(consumed)}; ZSTD_outBuffer out{decoded.data(), size_t(decoded.size()), 0};
                            const size_t status = ZSTD_decompressStream(zstd, &out, &in); ensure(!ZSTD_isError(status), QStringLiteral("ZSTD 数据损坏")); consumed = qsizetype(in.pos); count = out.pos; ended = status == 0;
                        }
                        write(decoded.constData(), count); ensure(ended || consumed > previous || count > 0, QStringLiteral("压缩流无法前进"));
                        if (ended) { ensure(consumed == input.size() && offset + quint64(input.size()) == operation.length, QStringLiteral("压缩流有尾随数据")); break; }
                        if (consumed == input.size() && count < quint64(decoded.size())) break;
                    } while (true);
                }
                ensure(operation.type == 0 || ended, QStringLiteral("压缩流被截断"));
                ensure(operation.hash.isEmpty() || operation.hash == digest.result(), QStringLiteral("操作数据 SHA256 校验失败：") + partition.name);
            } catch (...) { if (bzReady) BZ2_bzDecompressEnd(&bz); if (xzReady) lzma_end(&xz); if (zstd) ZSTD_freeDStream(zstd); throw; }
            if (bzReady) BZ2_bzDecompressEnd(&bz); if (xzReady) lzma_end(&xz); if (zstd) ZSTD_freeDStream(zstd);
        }
        ensure(produced == expected, QStringLiteral("操作解压大小不足")); done += produced; if (progress) progress(qint64(done), qint64(partition.size), partition.name);
    }
    ensure(output.flush(), output.errorString());
    if (!partition.hash.isEmpty()) {
        ensure(output.seek(0), output.errorString()); QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!output.atEnd()) { checkCancel(cancel); const QByteArray block = output.read(1024 * 1024); ensure(output.error() == QFile::NoError, output.errorString()); hash.addData(block); }
        ensure(hash.result() == partition.hash, QStringLiteral("分区 SHA256 校验失败：") + partition.name);
    }
}

}

Result parsePayloadManifest(const QByteArray &bytes, PayloadManifest *manifest) {
    try {
        ensure(manifest && bytes.size() <= 64 * 1024 * 1024, QStringLiteral("无效的 Payload 元数据")); PayloadManifest parsed; QSet<QString> names; quint64 operationCount = 0;
        Proto root(bytes);
        while (root.next()) {
            if (root.field == 3 && root.wire == 0) parsed.blockSize = root.value;
            if (root.field != 13 || root.wire != 2) continue;
            ensure(parsed.partitions.size() < 1024, QStringLiteral("分区数量超过限制")); PayloadPartition partition; Proto fields(root.data);
            while (fields.next()) {
                if (fields.field == 1 && fields.wire == 2) partition.name = QString::fromUtf8(fields.data);
                if (fields.field == 7 && fields.wire == 2) {
                    Proto info(fields.data); while (info.next()) { if (info.field == 1 && info.wire == 0) partition.size = info.value; if (info.field == 2 && info.wire == 2) partition.hash = info.data; }
                }
                if (fields.field != 8 || fields.wire != 2) continue;
                ensure(++operationCount <= 2000000, QStringLiteral("Payload 操作数量超过限制")); PayloadOperation operation; Proto operationFields(fields.data);
                while (operationFields.next()) {
                    if (operationFields.wire == 0) {
                        if (operationFields.field == 1) operation.type = operationFields.value;
                        if (operationFields.field == 2) operation.offset = operationFields.value;
                        if (operationFields.field == 3) operation.length = operationFields.value;
                    }
                    if (operationFields.field == 8 && operationFields.wire == 2) operation.hash = operationFields.data;
                    if (operationFields.field == 6 && operationFields.wire == 2) {
                        ensure(operation.extents.size() < 200000, QStringLiteral("extent 数量超过限制")); PayloadExtent extent; Proto extentFields(operationFields.data);
                        while (extentFields.next()) if (extentFields.wire == 0) { if (extentFields.field == 1) extent.start = extentFields.value; if (extentFields.field == 2) extent.blocks = extentFields.value; }
                        operation.extents.append(extent);
                    }
                }
                ensure(operation.hash.isEmpty() || operation.hash.size() == 32, QStringLiteral("操作 SHA256 长度无效")); partition.operations.append(operation);
            }
            ensure(!partition.name.isEmpty() && validateName(partition.name).isEmpty() && !names.contains(partition.name.toCaseFolded()), QStringLiteral("分区名称非法或重复"));
            ensure(partition.size > 0 && partition.size <= 256ULL * 1024 * 1024 * 1024 && (partition.hash.isEmpty() || partition.hash.size() == 32), QStringLiteral("分区大小或 SHA256 无效"));
            names.insert(partition.name.toCaseFolded()); parsed.partitions.append(partition);
        }
        ensure(parsed.blockSize > 0 && parsed.blockSize <= 1024 * 1024 && (parsed.blockSize & (parsed.blockSize - 1)) == 0 && !parsed.partitions.isEmpty(), QStringLiteral("block_size 或分区列表无效"));
        for (const auto &partition : parsed.partitions) {
            QList<QPair<quint64, quint64>> intervals;
            for (const auto &operation : partition.operations) {
                ensure(operation.type == 0 || operation.type == 1 || operation.type == 6 || operation.type == 7 || operation.type == 8 || operation.type == 14, QStringLiteral("此 Payload 含增量操作 %1，需要旧版本分区，不是可独立提取的完整 OTA").arg(operation.type));
                ensure(!operation.extents.isEmpty(), QStringLiteral("操作缺少目标 extent"));
                quint64 total = 0;
                for (const auto &extent : operation.extents) {
                    ensure(extent.blocks > 0 && extent.start <= partition.size / parsed.blockSize && extent.blocks <= partition.size / parsed.blockSize - extent.start, QStringLiteral("extent 超出分区"));
                    ensure(total <= partition.size - extent.blocks * parsed.blockSize, QStringLiteral("extent 总长度溢出")); total += extent.blocks * parsed.blockSize;
                    intervals.append({extent.start, extent.start + extent.blocks});
                }
                ensure((operation.type != 0 || operation.length == total) && ((operation.type != 6 && operation.type != 7) || operation.length == 0), QStringLiteral("操作数据长度无效"));
            }
            std::sort(intervals.begin(), intervals.end()); for (qsizetype index = 1; index < intervals.size(); ++index) ensure(intervals[index - 1].second <= intervals[index].first, QStringLiteral("分区存在重复写入的 extent"));
        }
        *manifest = std::move(parsed); return {};
    } catch (const std::exception &exception) { return {false, QString::fromUtf8(exception.what()), {}, {}}; }
}

Result inspectPayload(const QString &source, const Cancellation &cancel, const PayloadOptions &options) {
    try {
        const auto payload = openPayload(source, cancel, options); Result result;
        for (const auto &partition : payload.manifest.partitions) { result.paths.append(partition.name); result.output += QStringLiteral("%1\t%2\t%3 个操作\n").arg(partition.name, humanSize(qint64(partition.size))).arg(partition.operations.size()); }
        return result;
    } catch (const std::exception &exception) { return {false, QString::fromUtf8(exception.what()), {}, {}}; }
}

Result extractPayload(const QString &source, const QStringList &partitions, const QString &destination, int workers, const Cancellation &cancel, const Progress &progress, const PayloadOptions &options) {
    try {
        checkCancel(cancel); ensure(QFileInfo(destination).isDir() && !partitions.isEmpty(), QStringLiteral("请选择输出目录和分区"));
        auto payload = openPayload(source, cancel, options); QList<PayloadPartition> selected; QSet<QString> selectedNames; quint64 total = 0;
        for (const auto &partition : payload.manifest.partitions) if (partitions.contains(partition.name)) {
            ensure(!QFileInfo::exists(QDir(destination).filePath(partition.name + QStringLiteral(".img"))), QStringLiteral("输出已存在：") + partition.name);
            total += partition.size; selected.append(partition); selectedNames.insert(partition.name);
        }
        ensure(selectedNames.size() == QSet<QString>(partitions.cbegin(), partitions.cend()).size(), QStringLiteral("未知分区名称"));
        const QStorageInfo storage(destination); ensure(!storage.isValid() || storage.bytesAvailable() < 0 || quint64(storage.bytesAvailable()) >= total, QStringLiteral("磁盘空间不足"));
        QTemporaryDir staging(QDir(destination).filePath(QStringLiteral(".twinfile-payload-XXXXXX"))); ensure(staging.isValid(), QStringLiteral("无法创建分区暂存目录"));
        std::atomic<qsizetype> next{0}; const int concurrency = qBound(1, qMin(workers, int(selected.size())), 8); std::vector<std::future<void>> tasks;
        for (int worker = 0; worker < concurrency; ++worker) tasks.emplace_back(std::async(std::launch::async, [&] {
            for (;;) { const qsizetype index = next.fetch_add(1); if (index >= selected.size()) break;
                extractPartition(payload.source, selected[index], payload.manifest.blockSize, payload.dataOffset, staging.filePath(selected[index].name + QStringLiteral(".img")), cancel, progress);
            }
        }));
        std::exception_ptr error; for (auto &task : tasks) { try { task.get(); } catch (...) { if (!error) error = std::current_exception(); } } if (error) std::rethrow_exception(error);
        checkCancel(cancel); QStringList published;
        for (const auto &partition : selected) {
            const QString name = partition.name + QStringLiteral(".img"), target = QDir(destination).filePath(name);
            if (!QFile::rename(staging.filePath(name), target)) { for (const QString &created : published) QFile::remove(created); throw PayloadError(QStringLiteral("分区发布失败，未覆盖现有文件：") + target); }
            published.append(target);
        }
        return {true, {}, QStringLiteral("已提取并校验 %1 个分区").arg(published.size()), published};
    } catch (const std::exception &exception) { return {false, QString::fromUtf8(exception.what()), {}, {}}; }
}

}
