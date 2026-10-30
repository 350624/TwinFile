#include "payload.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHttp1Configuration>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStorageInfo>
#include <QSet>
#include <QTemporaryFile>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QtEndian>
#include <bzlib.h>
#include <lzma.h>
#include <zstd.h>
#include <zlib.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <mutex>
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
constexpr int maxRangeAttempts = 8;

QString createStagingDirectory(const QString &destination) {
    const QDir parent(QFileInfo(destination).absoluteFilePath());
    const QString path = parent.filePath(QStringLiteral(".twinfile-payload-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
#ifdef Q_OS_WIN
    // Inherit the target ACL instead of rewriting permissions on external/shared volumes.
    QString native = QDir::toNativeSeparators(path);
    if (!native.startsWith(QStringLiteral("\\\\?\\"))) {
        if (native.startsWith(QStringLiteral("\\\\"))) native = QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
        else native.prepend(QStringLiteral("\\\\?\\"));
    }
    if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(native.utf16()), nullptr)) {
        const DWORD code = GetLastError(); LPWSTR message = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
        const QString reason = message ? QString::fromWCharArray(message).trimmed() : QString::number(code);
        if (message) LocalFree(message);
        throw PayloadError(QStringLiteral("无法创建分区暂存目录：%1（%2，系统错误 %3）").arg(QDir::toNativeSeparators(path), reason).arg(code));
    }
#else
    ensure(QDir().mkdir(path), QStringLiteral("无法创建分区暂存目录：") + path);
#endif
    return path;
}
quint16 little16(const QByteArray &bytes, qsizetype offset) { ensure(offset >= 0 && offset + 2 <= bytes.size(), QStringLiteral("ZIP 字段越界")); return qFromLittleEndian<quint16>(bytes.constData() + offset); }
quint32 little32(const QByteArray &bytes, qsizetype offset) { ensure(offset >= 0 && offset + 4 <= bytes.size(), QStringLiteral("ZIP 字段越界")); return qFromLittleEndian<quint32>(bytes.constData() + offset); }
quint64 little64(const QByteArray &bytes, qsizetype offset) { ensure(offset >= 0 && offset + 8 <= bytes.size(), QStringLiteral("ZIP64 字段越界")); return qFromLittleEndian<quint64>(bytes.constData() + offset); }

class Proto {
public:
    explicit Proto(QByteArray bytes, Cancellation cancel = {}) : bytes_(std::move(bytes)), cancel_(std::move(cancel)) {}
    bool next() {
        checkCancel(cancel_);
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
    Cancellation cancel_;
    qsizetype position_ = 0;
};

class RangeSource {
public:
    RangeSource() = default;
    // Copies start a thread-local connection pool; Qt network objects never cross workers.
    RangeSource(const RangeSource &other)
        : source(other.source), userAgent(other.userAgent), etag(other.etag), modified(other.modified),
          total(other.total), base(other.base), length(other.length), temporary(other.temporary),
          options(other.options), progress(other.progress), stage(other.stage), stageOffset(other.stageOffset),
          stageTotal(other.stageTotal), connections(other.connections), inputSize(other.inputSize), interrupt(other.interrupt) {}
    RangeSource &operator=(const RangeSource &other) { RangeSource copy(other); return *this = std::move(copy); }
    RangeSource(RangeSource &&) = default;
    RangeSource &operator=(RangeSource &&) = default;
    QString source;
    QByteArray userAgent;
    QByteArray etag, modified;
    quint64 total = 0, base = 0, length = 0;
    std::shared_ptr<QTemporaryFile> temporary;
    PayloadOptions options;
    Progress progress;
    QString stage = QStringLiteral("读取 Payload 文件信息");
    quint64 stageOffset = 0, stageTotal = 0;
    std::shared_ptr<std::atomic_int> connections = std::make_shared<std::atomic_int>(0);
    quint64 inputSize = 128 * 1024 * 1024;
    Cancellation interrupt;
    std::vector<std::unique_ptr<QNetworkAccessManager>> managers;

    bool remote() const {
        return source.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) || source.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive);
    }

    void notify(quint64 done, quint64 count, const QString &detail = {}) const {
        if (progress) progress(qint64(stageOffset + done), qint64(stageTotal ? stageTotal : count), stage + detail);
    }

    QByteArray raw(quint64 offset, quint64 count, const Cancellation &cancel) {
        checkCancel(interrupt);
        checkCancel(cancel); ensure(count <= 128 * 1024 * 1024 && offset <= quint64(std::numeric_limits<qint64>::max()) && count <= quint64(std::numeric_limits<qint64>::max()) - offset, QStringLiteral("读取范围过大或越界"));
        if (!remote()) {
            QFile file(source); ensure(file.open(QIODevice::ReadOnly), file.errorString());
            if (total) ensure(total == quint64(file.size()), QStringLiteral("源文件大小在提取期间发生变化"));
            total = quint64(file.size()); ensure(fits(offset, count, total) && file.seek(qint64(offset)), QStringLiteral("读取超出源文件"));
            const QByteArray bytes = file.read(qint64(count)); ensure(quint64(bytes.size()) == count, QStringLiteral("源文件读取不完整")); notify(count, count); return bytes;
        }
        QByteArray bytes(qsizetype(count), Qt::Uninitialized); char *const buffer = bytes.data();
        transfer({{offset, count, 0}}, cancel,
            [buffer](quint64 position, const QByteArray &block) { std::copy(block.cbegin(), block.cend(), buffer + position); });
        return bytes;
    }

    struct Span { quint64 offset, size, destination; };
    void transfer(const std::vector<Span> &spans, const Cancellation &cancel,
                  const std::function<void(quint64, const QByteArray &)> &write,
                  const std::function<void(quint64, quint64)> &committed = {}) {
        quint64 count = 0;
        for (const auto &span : spans) {
            ensure(span.size > 0 && span.size <= 128 * 1024 * 1024
                && fits(span.offset, span.size, quint64(std::numeric_limits<qint64>::max())), QStringLiteral("HTTP Range 长度无效"));
            ensure(count <= quint64(std::numeric_limits<qint64>::max()) - span.size, QStringLiteral("HTTP Range 总长度溢出"));
            count += span.size;
        }
        ensure(count > 0, QStringLiteral("HTTP Range 长度为空"));
        constexpr quint64 minimumRangeSize = 256 * 1024;
        const auto spanSlots = [&](const Span &span) {
            return int(qMin<quint64>(options.downloadConnections, qMax<quint64>(1, span.size / minimumRangeSize)));
        };
        struct Part {
            quint64 start = 0, destination = 0, size = 0, received = 0, requestStart = 0;
            QNetworkAccessManager *manager = nullptr;
            QNetworkReply *reply = nullptr;
            QElapsedTimer elapsed, lastData;
            std::deque<std::pair<qint64, quint64>> rateSamples;
            int attempts = 0, managerSlot = -1;
            bool complete = false, held = false, timedOut = false, validated = false;
        };
        QEventLoop loop; QTimer poll; poll.setInterval(50);
        std::vector<std::unique_ptr<QNetworkAccessManager>> recoveryManagers;
        std::vector<Part> parts;
        int planned = 0;
        for (const auto &span : spans) {
            planned = qMin(options.downloadConnections, planned + spanSlots(span));
            if (planned == options.downloadConnections) break;
        }
        size_t nextSpan = 0; quint64 nextOffset = 0, completedBytes = 0;
        const auto refill = [&](Part &part) {
            if (nextSpan == spans.size()) return false;
            const auto &span = spans[nextSpan];
            const int rangeSlots = spanSlots(span);
            const quint64 chunkSize = rangeSlots == 1 ? span.size : qMax<quint64>(minimumRangeSize,
                (span.size + quint64(rangeSlots) * 4 - 1) / (quint64(rangeSlots) * 4));
            part.start = span.offset + nextOffset; part.destination = span.destination + nextOffset;
            part.size = qMin(chunkSize, span.size - nextOffset); nextOffset += part.size;
            if (nextOffset == span.size) { ++nextSpan; nextOffset = 0; }
            part.complete = false; part.received = 0; part.attempts = 0;
            return true;
        };
        double completedRateSum = 0;
        int completedRequests = 0;
        constexpr int connectionsPerManager = 255;
        const int managerCount = (planned + connectionsPerManager - 1) / connectionsPerManager;
        std::vector<int> managerLoads(size_t(managerCount), 0);
        parts.resize(size_t(planned));
        QString error;
        // Configure HTTP/1 pools explicitly instead of multiplying Qt network threads for the default six slots.
        const auto cleanup = qScopeGuard([&] {
            for (auto &part : parts) {
                if (part.reply) { QObject::disconnect(part.reply, nullptr, &loop, nullptr); part.reply->abort(); }
                if (part.held) connections->fetch_sub(1);
            }
        });
        for (int index = 0; index < planned; ++index) {
            auto &part = parts[size_t(index)];
            if (!refill(part)) part.complete = true;
        }
        const auto receive = [&](Part &part) {
            if (!part.reply || !part.validated || !part.reply->isOpen()) return;
            const QByteArray block = part.reply->read(qint64(part.size - part.received + 1));
            if (quint64(block.size()) > part.size - part.received) { error = QStringLiteral("服务器忽略 Range 或响应超出范围"); return; }
            try { write(part.destination + part.received, block); }
            catch (const std::exception &exception) { error = QString::fromUtf8(exception.what()); return; }
            part.received += quint64(block.size());
            if (!block.isEmpty()) part.lastData.restart();
        };
        const auto pump = [&] {
            if (!error.isEmpty() || (cancel && cancel->load()) || (interrupt && interrupt->load())) { loop.quit(); return; }
            bool complete = true; quint64 received = completedBytes;
            for (auto &part : parts) {
                received += part.received; complete &= part.complete;
                if (part.complete) continue;
                if (part.reply) {
                    const qint64 now = part.elapsed.elapsed();
                    if (part.rateSamples.empty() || now - part.rateSamples.back().first >= 500)
                        part.rateSamples.emplace_back(now, part.received);
                    while (part.rateSamples.size() > 1 && part.rateSamples[1].first <= now - 3000)
                        part.rateSamples.pop_front();
                    const double peerRate = completedRequests ? completedRateSum / completedRequests : 0;
                    // A full rolling window includes fresh bursts and does not confuse a gap between packets with a slow tail.
                    const double recentRate = double(part.received - part.rateSamples.front().second) * 1000
                        / qMax<qint64>(1, now - part.rateSamples.front().first);
                    const bool trickling = part.elapsed.elapsed() >= 3000 && recentRate >= 0
                        && recentRate * 8 < peerRate && double(part.size - part.received) > recentRate * 2;
                    const bool slowTrickle = trickling && recentRate > 0 && recentRate < 16 * 1024;
                    const bool retryStarting = part.attempts > 1 && part.received == part.requestStart - part.start;
                    // Recover only after all queued ranges have been assigned. Slow tails can
                    // occupy most slots, so completed-slot counts cannot detect this boundary.
                    if (nextSpan == spans.size() && part.elapsed.elapsed() >= (retryStarting ? 10000 : 3000)
                        && (part.lastData.elapsed() >= 3000 || slowTrickle)) {
                        if (qEnvironmentVariableIsSet("TWINFILE_PAYLOAD_TRACE"))
                            qInfo("Payload tail start=%llu attempt=%d received=%llu/%llu elapsed=%lld idle=%lld rate=%.0f peers=%.0f",
                                part.requestStart, part.attempts, part.received, part.size, part.elapsed.elapsed(),
                                part.lastData.elapsed(), recentRate, peerRate);
                        part.reply->abort();
                    }
                    else if (part.elapsed.elapsed() >= options.requestTimeoutMs) { part.timedOut = true; part.reply->abort(); }
                    continue;
                }
                int current = connections->load();
                while (current < options.downloadConnections && !connections->compare_exchange_weak(current, current + 1)) {}
                if (current >= options.downloadConnections) continue;
                part.held = true; part.validated = false; part.timedOut = false; ++part.attempts; part.elapsed.start(); part.lastData.start();
                part.rateSamples.clear(); part.rateSamples.emplace_back(0, part.received);
                part.requestStart = part.start + part.received;
                part.managerSlot = int(std::min_element(managerLoads.cbegin(), managerLoads.cend()) - managerLoads.cbegin());
                ++managerLoads[size_t(part.managerSlot)];
                if (part.attempts > 1) {
                    // Failed transports must not poison subsequent attempts. Keep each retry
                    // generation in a separate bounded pool, including ordinary timeouts.
                    const size_t recoverySlot = size_t((part.attempts - 2) * managerCount + part.managerSlot);
                    while (recoverySlot >= recoveryManagers.size()) {
                        recoveryManagers.emplace_back(std::make_unique<QNetworkAccessManager>());
                        recoveryManagers.back()->setProxy(QNetworkProxy::NoProxy);
                    }
                    part.manager = recoveryManagers[recoverySlot].get();
                } else {
                    while (size_t(part.managerSlot) >= managers.size()) {
                        managers.emplace_back(std::make_unique<QNetworkAccessManager>());
                        managers.back()->setProxy(QNetworkProxy::NoProxy);
                    }
                    part.manager = managers[size_t(part.managerSlot)].get();
                }
                QNetworkRequest request{QUrl(source)};
                QHttp1Configuration http1;
                http1.setNumberOfConnectionsPerHost(connectionsPerManager);
                request.setHttp1Configuration(http1);
                request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
                request.setMaximumRedirectsAllowed(5);
                request.setRawHeader("Accept-Encoding", "identity");
                request.setRawHeader("Range", "bytes=" + QByteArray::number(part.requestStart) + '-' + QByteArray::number(part.start + part.size - 1));
                request.setRawHeader("User-Agent", userAgent);
                if (!etag.isEmpty()) request.setRawHeader("If-Match", etag);
                else if (!modified.isEmpty()) request.setRawHeader("If-Unmodified-Since", modified);
                request.setTransferTimeout(options.idleTimeoutMs);
                auto *const reply = part.manager->get(request);
                part.reply = reply; reply->setReadBufferSize(64 * 1024);
                QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, [&, item = &part, reply] {
                    // Qt can still deliver queued response metadata after abort() emitted finished().
                    if (item->reply != reply) return;
                    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    if (status >= 300 && status < 400) return;
                    if (status == 429 || status >= 500) { reply->abort(); return; }
                    if (status != 206) error = QStringLiteral("HTTP 服务必须支持严格的 206 Range（状态 %1）").arg(status);
                    else {
                        const auto match = QRegularExpression(QStringLiteral("^bytes (\\d+)-(\\d+)/(\\d+)$")).match(QString::fromLatin1(reply->rawHeader("Content-Range")));
                        const quint64 first = item->requestStart, last = item->start + item->size - 1, size = match.captured(3).toULongLong();
                        const QByteArray encoding = reply->rawHeader("Content-Encoding").toLower();
                        if (!match.hasMatch() || match.captured(1).toULongLong() != first || match.captured(2).toULongLong() != last || size <= last || (total && total != size))
                            error = QStringLiteral("服务器返回了错误的 Content-Range 或文件长度变化");
                        else if (!encoding.isEmpty() && encoding != "identity") error = QStringLiteral("Range 响应使用了内容压缩");
                        else if ((!etag.isEmpty() && reply->rawHeader("ETag") != etag) || (!modified.isEmpty() && reply->rawHeader("Last-Modified") != modified))
                            error = QStringLiteral("远程文件 ETag 或修改时间发生变化");
                        else {
                            total = size;
                            const QByteArray tag = reply->rawHeader("ETag");
                            if (!tag.startsWith("W/")) etag = tag;
                            modified = reply->rawHeader("Last-Modified");
                            item->validated = true;
                        }
                    }
                    if (!error.isEmpty()) reply->abort();
                });
                QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&, item = &part, reply] {
                    if (item->reply != reply) return;
                    receive(*item);
                    if (!error.isEmpty()) reply->abort();
                });
                QObject::connect(reply, &QNetworkReply::finished, &loop, [&, item = &part, reply] {
                    if (item->reply != reply) return;
                    QObject::disconnect(reply, nullptr, &loop, nullptr);
                    receive(*item);
                    item->reply = nullptr;
                    item->held = false; connections->fetch_sub(1);
                    --managerLoads[size_t(item->managerSlot)];
                    item->complete = error.isEmpty() && reply->error() == QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 206 && item->received == item->size;
                    if (item->complete) {
                        completedRateSum += double(item->start + item->size - item->requestStart) * 1000 / qMax<qint64>(1, item->elapsed.elapsed());
                        ++completedRequests;
                        try { if (committed) committed(item->destination, item->size); }
                        catch (const std::exception &exception) { error = QString::fromUtf8(exception.what()); }
                        completedBytes += item->size; item->received = 0;
                        // One queue crosses every input window; completed slots never wait for a batch barrier.
                        if (error.isEmpty()) refill(*item);
                    }
                    // A retry can append only to data protected by a stable validator.
                    if (!item->complete && ((etag.isEmpty() && modified.isEmpty()) || item->received == item->size)) item->received = 0;
                    if (!item->complete && error.isEmpty() && item->attempts >= maxRangeAttempts) {
                        const bool timeout = item->timedOut || reply->error() == QNetworkReply::TimeoutError
                            || (reply->error() == QNetworkReply::OperationCanceledError && item->lastData.elapsed() >= options.idleTimeoutMs);
                        const QString cause = timeout
                            ? QStringLiteral("网络请求超时") : reply->errorString();
                        error = QStringLiteral("Range 请求重试失败（%1 次，%2-%3，已接收 %4/%5 字节）：%6")
                            .arg(item->attempts).arg(item->start).arg(item->start + item->size - 1)
                            .arg(item->received).arg(item->size).arg(cause);
                    }
                    reply->deleteLater();
                });
            }
            notify(received, count, QStringLiteral(" · %1/%2 连接").arg(connections->load()).arg(options.downloadConnections));
            if (complete) loop.quit();
        };
        QObject::connect(&poll, &QTimer::timeout, &loop, pump);
        notify(0, count); pump(); poll.start(); loop.exec();
        checkCancel(interrupt); checkCancel(cancel); ensure(error.isEmpty(), error); notify(count, count);
    }
    QByteArray read(quint64 offset, quint64 count, const Cancellation &cancel) {
        ensure(fits(offset, count, length), QStringLiteral("Payload 读取越界")); return count ? raw(base + offset, count, cancel) : QByteArray();
    }
};

RangeSource locate(QString source, const Cancellation &cancel, const PayloadOptions &options, bool metadataOnly, const Progress &progress) {
    RangeSource reader; reader.source = std::move(source); reader.userAgent = options.userAgent.toUtf8(); reader.options = options; reader.progress = progress;
    ensure(options.idleTimeoutMs > 0 && options.requestTimeoutMs > 0, QStringLiteral("网络超时必须大于零"));
    ensure(options.downloadConnections >= 1 && options.downloadConnections <= 512, QStringLiteral("下载线程数量须为 1–512"));
    reader.inputSize = options.downloadConnections == 1 ? 256 * 1024 : qMin<quint64>(128 * 1024 * 1024, quint64(options.downloadConnections) * 1024 * 1024);
    ensure(!reader.userAgent.isEmpty() && reader.userAgent.size() <= 512 && std::all_of(reader.userAgent.cbegin(), reader.userAgent.cend(), [](char value) { return quint8(value) >= 32 && quint8(value) <= 126; }), QStringLiteral("User-Agent 只能包含 1–512 个可打印 ASCII 字符"));
    const QByteArray magic = reader.raw(0, 4, cancel); reader.length = reader.total;
    if (magic == "CrAU") return reader;
    reader.stage = QStringLiteral("读取 OTA ZIP 目录");
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
        checkCancel(cancel);
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
    quint64 written = 0, required = metadataOnly ? 24 : uncompressed;
    quint32 crc = crc32(0, nullptr, 0); int status = Z_OK; QByteArray output(1024 * 1024, '\0');
    reader.stage = metadataOnly ? QStringLiteral("读取压缩 Payload 元数据") : QStringLiteral("下载并解压 Payload 缓存");
    reader.stageTotal = compressed;
    const quint64 inputSize = metadataOnly ? 64 * 1024 : reader.remote() ? reader.inputSize : 1024 * 1024;
    try {
        for (quint64 offset = 0; offset < compressed && (!metadataOnly || written < required); offset += inputSize) {
            reader.stageOffset = offset;
            QByteArray input = reader.raw(start + offset, qMin<quint64>(inputSize, compressed - offset), cancel);
            stream.next_in = reinterpret_cast<Bytef *>(input.data()); stream.avail_in = uInt(input.size());
            do {
                checkCancel(cancel); stream.next_out = reinterpret_cast<Bytef *>(output.data());
                const uInt capacity = uInt(metadataOnly ? qMin<quint64>(output.size(), required - written) : quint64(output.size()));
                stream.avail_out = capacity; status = inflate(&stream, Z_NO_FLUSH);
                const quint64 produced = capacity - stream.avail_out;
                if (status == Z_BUF_ERROR && !stream.avail_in && !produced) break;
                ensure(status == Z_OK || status == Z_STREAM_END, QStringLiteral("payload.bin ZIP 解压失败"));
                ensure(fits(written, produced, uncompressed), QStringLiteral("ZIP 解压超出声明大小"));
                ensure(temporary->write(output.constData(), qint64(produced)) == qint64(produced), temporary->errorString()); crc = crc32(crc, reinterpret_cast<const Bytef *>(output.constData()), uInt(produced)); written += produced;
                if (metadataOnly && written == 24 && required == 24) {
                    ensure(temporary->seek(0), temporary->errorString()); const QByteArray header = temporary->read(24);
                    ensure(header.startsWith("CrAU") && qFromBigEndian<quint64>(header.constData() + 4) == 2, QStringLiteral("仅支持 CrAU v2 Payload"));
                    const quint64 manifestSize = qFromBigEndian<quint64>(header.constData() + 12), signatureSize = qFromBigEndian<quint32>(header.constData() + 20);
                    ensure(manifestSize > 0 && manifestSize <= 64 * 1024 * 1024 && fits(24, manifestSize + signatureSize, uncompressed), QStringLiteral("Payload 元数据长度无效"));
                    required = 24 + manifestSize; ensure(temporary->seek(qint64(written)), temporary->errorString());
                }
                // Listing partitions only needs the header and manifest. Full extraction still verifies the complete ZIP CRC.
                if (metadataOnly && written == required) break;
                if (status == Z_STREAM_END) { ensure(!stream.avail_in && offset + quint64(input.size()) == compressed, QStringLiteral("ZIP 压缩数据存在尾随内容")); break; }
            } while (stream.avail_in || !stream.avail_out);
        }
        ensure(metadataOnly ? written == required : status == Z_STREAM_END && written == uncompressed && crc == expectedCrc, QStringLiteral("ZIP 解压尺寸或 CRC 校验失败"));
        ensure(temporary->flush(), temporary->errorString()); inflateEnd(&stream);
    } catch (...) { inflateEnd(&stream); throw; }
    reader.source = temporary->fileName(); temporary->close(); reader.temporary = temporary; reader.etag.clear(); reader.modified.clear(); reader.base = 0; reader.total = written; reader.length = uncompressed; reader.stageOffset = reader.stageTotal = 0; return reader;
}

struct OpenPayload { RangeSource source; PayloadManifest manifest; quint64 dataOffset = 0; };
OpenPayload openPayload(const QString &source, const Cancellation &cancel, const PayloadOptions &options, bool metadataOnly, const Progress &progress) {
    OpenPayload payload; payload.source = locate(source, cancel, options, metadataOnly, progress); payload.source.stage = QStringLiteral("读取 Payload 元数据"); const QByteArray header = payload.source.read(0, 24, cancel);
    ensure(header.startsWith("CrAU") && qFromBigEndian<quint64>(header.constData() + 4) == 2, QStringLiteral("仅支持 CrAU v2 Payload"));
    const quint64 manifestSize = qFromBigEndian<quint64>(header.constData() + 12), signatureSize = qFromBigEndian<quint32>(header.constData() + 20);
    ensure(manifestSize > 0 && manifestSize <= 64 * 1024 * 1024 && fits(24, manifestSize + signatureSize, payload.source.length), QStringLiteral("Payload 元数据长度无效"));
    QByteArray bytes;
    payload.source.stageTotal = manifestSize;
    for (quint64 offset = 0; offset < manifestSize; offset += 4 * 1024 * 1024) { payload.source.stageOffset = offset; bytes += payload.source.read(24 + offset, qMin<quint64>(4 * 1024 * 1024, manifestSize - offset), cancel); }
    if (progress) progress(0, 0, QStringLiteral("解析 Payload 分区表"));
    const Result parsed = parsePayloadManifest(bytes, &payload.manifest, cancel); ensure(parsed.ok, parsed.error); payload.dataOffset = 24 + manifestSize + signatureSize;
    payload.source.progress = {}; payload.source.stageOffset = payload.source.stageTotal = 0; return payload;
}

void extractPartition(RangeSource source, const PayloadPartition &partition, quint64 blockSize, quint64 dataOffset,
                      const QString &path, const Cancellation &cancel, const Progress &progress, const std::function<void(qint64)> &downloaded) {
    QFile output(path); ensure(output.open(QIODevice::ReadWrite | QIODevice::NewOnly) && output.resize(qint64(partition.size)), output.errorString());
    quint64 done = 0;
    source.interrupt = std::make_shared<std::atomic_bool>(false);
    std::vector<RangeSource::Span> spans;
    std::vector<quint64> operationCache(size_t(partition.operations.size()), 0);
    quint64 cacheSize = 0;
    if (source.remote()) for (qsizetype index = 0; index < partition.operations.size(); ++index) {
        const auto &operation = partition.operations[index];
        checkCancel(cancel);
        ensure(fits(dataOffset, operation.offset, source.length) && fits(dataOffset + operation.offset, operation.length, source.length), QStringLiteral("分区操作读取越界"));
        ensure(fits(cacheSize, operation.length, quint64(std::numeric_limits<qint64>::max())), QStringLiteral("下载缓存大小溢出"));
        operationCache[size_t(index)] = cacheSize;
        for (quint64 offset = 0; offset < operation.length;) {
            if (!spans.empty() && spans.back().offset + spans.back().size == source.base + dataOffset + operation.offset + offset
                && spans.back().size < source.inputSize) {
                const quint64 size = qMin(source.inputSize - spans.back().size, operation.length - offset);
                spans.back().size += size; cacheSize += size; offset += size;
            } else {
                const quint64 size = qMin(source.inputSize, operation.length - offset);
                spans.push_back({source.base + dataOffset + operation.offset + offset, size, cacheSize});
                cacheSize += size; offset += size;
            }
        }
    }
    QTemporaryFile cache(path + QStringLiteral(".ranges-XXXXXX"));
    if (!spans.empty()) ensure(cache.open() && cache.resize(qint64(cacheSize)), QStringLiteral("无法创建下载缓存：") + cache.errorString());
    QFile cacheInput(cache.fileName());
    if (!spans.empty()) ensure(cacheInput.open(QIODevice::ReadOnly | QIODevice::Unbuffered), cacheInput.errorString());
    std::mutex cacheMutex; std::condition_variable cacheChanged;
    std::map<quint64, quint64> available;
    std::exception_ptr downloadError;
    bool downloadFinished = false;
    std::unique_ptr<QThread> producer;
    const auto stopProducer = qScopeGuard([&] {
        source.interrupt->store(true);
        if (producer) producer->wait();
    });
    if (!spans.empty()) {
        const QString cachePath = cache.fileName();
        producer.reset(QThread::create([&, cachePath] {
            try {
                QFile writer(cachePath); ensure(writer.open(QIODevice::ReadWrite | QIODevice::Unbuffered), writer.errorString());
                RangeSource reader(source); qint64 reported = 0;
                reader.progress = [&](qint64 received, qint64, const QString &) {
                    const qint64 current = qMax(reported, received);
                    if (downloaded && current > reported) downloaded(current - reported);
                    reported = current;
                };
                reader.transfer(spans, cancel, [&](quint64 position, const QByteArray &block) {
                    ensure(writer.seek(qint64(position)) && writer.write(block) == block.size(), QStringLiteral("下载缓存写入失败：") + writer.errorString());
                }, [&](quint64 position, quint64 size) {
                    // Publish only validated, completed ranges. Adjacent ranges collapse into one interval.
                    std::lock_guard<std::mutex> lock(cacheMutex);
                    quint64 start = position, end = position + size;
                    auto next = available.lower_bound(start);
                    if (next != available.begin()) {
                        const auto previous = std::prev(next);
                        if (previous->second == start) { start = previous->first; available.erase(previous); }
                    }
                    if (next != available.end() && next->first == end) { end = next->second; available.erase(next); }
                    available.emplace(start, end); cacheChanged.notify_all();
                });
            } catch (...) { std::lock_guard<std::mutex> lock(cacheMutex); downloadError = std::current_exception(); }
            { std::lock_guard<std::mutex> lock(cacheMutex); downloadFinished = true; }
            cacheChanged.notify_all();
        }));
        producer->start();
    }
    const auto readInput = [&](qsizetype operationIndex, quint64 offset, quint64 count) {
        if (!source.remote()) return source.read(dataOffset + offset, count, cancel);
        const quint64 position = operationCache[size_t(operationIndex)] + offset - partition.operations[operationIndex].offset;
        std::unique_lock<std::mutex> lock(cacheMutex);
        for (;;) {
            checkCancel(cancel);
            if (downloadError) std::rethrow_exception(downloadError);
            auto next = available.upper_bound(position);
            if (next != available.begin()) {
                const auto range = std::prev(next);
                if (range->first <= position && range->second > position) {
                    count = qMin(count, range->second - position); break;
                }
            }
            ensure(!downloadFinished, QStringLiteral("下载缓存缺少分区数据"));
            cacheChanged.wait_for(lock, std::chrono::milliseconds(50));
        }
        lock.unlock();
        ensure(cacheInput.seek(qint64(position)), cacheInput.errorString());
        const QByteArray bytes = cacheInput.read(qint64(count));
        ensure(quint64(bytes.size()) == count, QStringLiteral("下载缓存读取不完整"));
        return bytes;
    };
    for (qsizetype operationIndex = 0; operationIndex < partition.operations.size(); ++operationIndex) {
        const PayloadOperation &operation = partition.operations[operationIndex];
        checkCancel(cancel); ensure(fits(dataOffset, operation.offset, source.length) && fits(dataOffset + operation.offset, operation.length, source.length), QStringLiteral("分区操作读取越界"));
        quint64 expected = 0;
        for (const auto &extent : operation.extents) expected += extent.blocks * blockSize;
        quint64 produced = 0, extentPosition = 0; qsizetype extentIndex = 0;
        auto write = [&](const char *bytes, quint64 size) {
            ensure(fits(produced, size, expected), QStringLiteral("解压输出超过目标 extent"));
            while (size) {
                checkCancel(cancel); ensure(extentIndex < operation.extents.size(), QStringLiteral("目标 extent 不足"));
                const auto &extent = operation.extents[extentIndex]; const quint64 count = std::min({size, extent.blocks * blockSize - extentPosition, quint64(1024 * 1024)});
                ensure(count > 0 && output.seek(qint64(extent.start * blockSize + extentPosition)) && output.write(bytes, qint64(count)) == qint64(count), QStringLiteral("分区写入失败"));
                bytes += count; size -= count; produced += count; extentPosition += count;
                if (progress) progress(qint64(done + produced), qint64(partition.size), QStringLiteral("提取分区 · ") + partition.name);
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
                const quint64 inputSize = source.remote() ? source.inputSize : 1024 * 1024;
                for (quint64 offset = 0; offset < operation.length;) {
                    const QByteArray input = readInput(operationIndex, operation.offset + offset, qMin(inputSize, operation.length - offset)); digest.addData(input);
                    ensure(!input.isEmpty(), QStringLiteral("Payload input is empty")); offset += quint64(input.size());
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
                        if (ended) { ensure(consumed == input.size() && offset == operation.length, QStringLiteral("压缩流有尾随数据")); break; }
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
    if (producer) {
        producer->wait();
        if (downloadError) std::rethrow_exception(downloadError);
    }
    ensure(output.flush(), output.errorString());
    if (!partition.hash.isEmpty()) {
        ensure(output.seek(0), output.errorString()); QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!output.atEnd()) { checkCancel(cancel); const QByteArray block = output.read(1024 * 1024); ensure(output.error() == QFile::NoError, output.errorString()); hash.addData(block); if (progress) progress(qint64(done), qint64(partition.size), QStringLiteral("校验分区 · ") + partition.name); }
        ensure(hash.result() == partition.hash, QStringLiteral("分区 SHA256 校验失败：") + partition.name);
    }
}

}

Result parsePayloadManifest(const QByteArray &bytes, PayloadManifest *manifest, const Cancellation &cancel) {
    try {
        ensure(manifest && bytes.size() <= 64 * 1024 * 1024, QStringLiteral("无效的 Payload 元数据")); PayloadManifest parsed; QSet<QString> names; quint64 operationCount = 0;
        checkCancel(cancel); Proto root(bytes, cancel);
        while (root.next()) {
            if (root.field == 3 && root.wire == 0) parsed.blockSize = root.value;
            if (root.field != 13 || root.wire != 2) continue;
            ensure(parsed.partitions.size() < 1024, QStringLiteral("分区数量超过限制")); PayloadPartition partition; Proto fields(root.data, cancel);
            while (fields.next()) {
                if (fields.field == 1 && fields.wire == 2) partition.name = QString::fromUtf8(fields.data);
                if (fields.field == 7 && fields.wire == 2) {
                    Proto info(fields.data, cancel); while (info.next()) { if (info.field == 1 && info.wire == 0) partition.size = info.value; if (info.field == 2 && info.wire == 2) partition.hash = info.data; }
                }
                if (fields.field != 8 || fields.wire != 2) continue;
                ensure(++operationCount <= 2000000, QStringLiteral("Payload 操作数量超过限制")); PayloadOperation operation; Proto operationFields(fields.data, cancel);
                while (operationFields.next()) {
                    if (operationFields.wire == 0) {
                        if (operationFields.field == 1) operation.type = operationFields.value;
                        if (operationFields.field == 2) operation.offset = operationFields.value;
                        if (operationFields.field == 3) operation.length = operationFields.value;
                    }
                    if (operationFields.field == 8 && operationFields.wire == 2) operation.hash = operationFields.data;
                    if (operationFields.field == 6 && operationFields.wire == 2) {
                        ensure(operation.extents.size() < 200000, QStringLiteral("extent 数量超过限制")); PayloadExtent extent; Proto extentFields(operationFields.data, cancel);
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
                checkCancel(cancel);
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

Result inspectPayload(const QString &source, const Cancellation &cancel, const PayloadOptions &options, const Progress &progress) {
    try {
        const auto payload = openPayload(source, cancel, options, true, progress); Result result;
        for (const auto &partition : payload.manifest.partitions) { result.paths.append(partition.name); result.output += QStringLiteral("%1\t%2\t%3 个操作\n").arg(partition.name, humanSize(qint64(partition.size))).arg(partition.operations.size()); }
        return result;
    } catch (const std::exception &exception) { return {false, QString::fromUtf8(exception.what()), {}, {}}; }
}

Result extractPayload(const QString &source, const QStringList &partitions, const QString &destination, int workers, const Cancellation &cancel, const Progress &progress, const PayloadOptions &options) {
    try {
        checkCancel(cancel); ensure(QFileInfo(destination).isDir() && !partitions.isEmpty(), QStringLiteral("请选择输出目录和分区"));
        auto payload = openPayload(source, cancel, options, false, progress); QList<PayloadPartition> selected; QSet<QString> selectedNames; quint64 total = 0;
        for (const auto &partition : payload.manifest.partitions) if (partitions.contains(partition.name)) {
            ensure(!QFileInfo::exists(QDir(destination).filePath(partition.name + QStringLiteral(".img"))), QStringLiteral("输出已存在：") + partition.name);
            total += partition.size; selected.append(partition); selectedNames.insert(partition.name);
        }
        ensure(selectedNames.size() == QSet<QString>(partitions.cbegin(), partitions.cend()).size(), QStringLiteral("未知分区名称"));
        quint64 cacheBytes = 0;
        if (payload.source.remote()) for (const auto &partition : selected)
            for (const auto &operation : partition.operations) {
                ensure(fits(cacheBytes, operation.length, quint64(std::numeric_limits<qint64>::max()) - total), QStringLiteral("下载缓存大小溢出"));
                cacheBytes += operation.length;
            }
        const QStorageInfo storage(destination);
        ensure(!storage.isValid() || storage.bytesAvailable() < 0 || quint64(storage.bytesAvailable()) >= total + cacheBytes,
            QStringLiteral("磁盘空间不足（需要容纳分区镜像及远程下载缓存）"));
        const QDir staging(createStagingDirectory(destination));
        const auto removeStaging = qScopeGuard([&] { QDir(staging.absolutePath()).removeRecursively(); });
        std::atomic<qsizetype> next{0}; const int concurrency = qBound(1, qMin(workers, int(selected.size())), 8);
        std::vector<std::future<void>> tasks; std::vector<std::unique_ptr<QThread>> threads;
        payload.source.inputSize = qMax<quint64>(256 * 1024, payload.source.inputSize / quint64(concurrency));
        qint64 networkTotal = 0, networkDone = 0;
        for (const auto &partition : selected) for (const auto &operation : partition.operations) networkTotal += qint64(operation.length);
        std::mutex progressMutex; std::vector<qint64> completed(size_t(selected.size()), 0); qint64 done = 0;
        const auto waitWorkers = qScopeGuard([&] { for (auto &thread : threads) thread->wait(); });
        for (int worker = 0; worker < concurrency; ++worker) {
            std::packaged_task<void()> task([&] {
                for (;;) { const qsizetype index = next.fetch_add(1); if (index >= selected.size()) break;
                    extractPartition(payload.source, selected[index], payload.manifest.blockSize, payload.dataOffset, staging.filePath(selected[index].name + QStringLiteral(".img")), cancel,
                        [&](qint64 current, qint64, const QString &stage) {
                            if (!progress) return;
                            std::lock_guard<std::mutex> lock(progressMutex); done += current - completed[size_t(index)]; completed[size_t(index)] = current; progress(done, qint64(total), stage);
                        }, [&](qint64 received) {
                            if (!progress) return;
                            std::lock_guard<std::mutex> lock(progressMutex); networkDone += received;
                            progress(networkDone, networkTotal, QStringLiteral("下载 Payload 分区数据 · %1/%2 连接").arg(payload.source.connections->load()).arg(options.downloadConnections));
                        });
                }
            });
            tasks.emplace_back(task.get_future());
            threads.emplace_back(QThread::create([task = std::move(task)]() mutable { task(); }));
            threads.back()->start();
        }
        std::exception_ptr error; for (auto &task : tasks) { try { task.get(); } catch (...) { if (!error) error = std::current_exception(); } } if (error) std::rethrow_exception(error);
        for (auto &thread : threads) thread->wait();
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
