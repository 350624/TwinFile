#include "download.h"
#include "appearance.h"
#include "window.h"
#include "task_progress.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkReply>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <functional>

using namespace twinfile;

namespace twinfile {
class DownloadTestAccess {
public:
    static void fastWatchdog(DirectDownload &download, int idle = 450, int slow = 1000) {
        download.stallTimeoutMs_ = idle;
        download.handshakeTimeoutMs_ = idle;
        download.slowWindowMs_ = slow;
        download.watchdog_.setInterval(25);
    }
    static void sample(DirectDownload &download) { download.notify(); }
};
}

static QByteArray contents(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
static bool putFile(const QString &path, const QByteArray &bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
static QByteArray payload(qint64 start, qint64 length) { QByteArray bytes(length, Qt::Uninitialized); for (qint64 index = 0; index < length; ++index) bytes[index] = char((start + index) * 31 % 251); return bytes; }
static QStringList partials(const QTemporaryDir &directory) { return QDir(directory.path()).entryList({".twinfile-download-*.part"}, QDir::Files | QDir::Hidden); }

class HttpFixture : public QTcpServer {
public:
    struct Request { QByteArray range, ifRange, encoding; QString path; };
    QList<Request> requests;
    int connected = 0, maximumConnected = 0;
    std::function<void(QTcpSocket *, const Request &)> respond;
    explicit HttpFixture(QObject *parent = nullptr) : QTcpServer(parent) {
        setMaxPendingConnections(1024); setListenBacklogSize(1024);
        listen(QHostAddress::LocalHost, 0);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection(); ++connected; maximumConnected = qMax(maximumConnected, connected);
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] { --connected; socket->deleteLater(); });
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    *buffer += socket->readAll(); if (socket->property("handled").toBool() || !buffer->contains("\r\n\r\n")) return;
                    socket->setProperty("handled", true); Request request;
                    const auto lines = buffer->split('\n'); request.path = QString::fromUtf8(lines.first().split(' ').value(1));
                    for (const auto &line : lines) { const auto key = line.left(line.indexOf(':')).toLower(); const auto value = line.mid(line.indexOf(':') + 1).trimmed(); if (key == "range") request.range = value; else if (key == "if-range") request.ifRange = value; else if (key == "accept-encoding") request.encoding = value; }
                    requests.append(request); if (respond) respond(socket, request);
                });
            }
        });
    }
    QUrl url(const QString &path = "/file.bin") const { return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path)); }
    static void reply(QTcpSocket *socket, int status, const QByteArray &body, QByteArray headers = {}, qint64 declared = -1) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nConnection: close\r\nContent-Length: " + QByteArray::number(declared < 0 ? body.size() : declared) + "\r\n" + headers + "\r\n" + body); socket->disconnectFromHost();
    }
    static QPair<qint64, qint64> bounds(const Request &request) { const auto match = QRegularExpression("^bytes=([0-9]+)-([0-9]+)$").match(QString::fromLatin1(request.range)); return {match.captured(1).toLongLong(), match.captured(2).toLongLong()}; }
    static QByteArray rangeHeader(qint64 start, qint64 end, qint64 size) { return "Content-Range: bytes " + QByteArray::number(start) + '-' + QByteArray::number(end) + '/' + QByteArray::number(size) + "\r\n"; }
    static void rangeReply(QTcpSocket *socket, const Request &request, qint64 size, const QByteArray &headers = "ETag: \"fixture-v1\"\r\n") { const auto range = bounds(request); reply(socket, 206, payload(range.first, range.second - range.first + 1), rangeHeader(range.first, range.second, size) + headers); }
};

class DownloadTests : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid()); qputenv("TWINFILE_TEST_MODE", "1"); qputenv("TWINFILE_SETTINGS", settings_.filePath("settings.json").toUtf8()); qputenv("TWINFILE_THUMBNAIL_CACHE", settings_.filePath("thumbnails").toUtf8()); qApp->setStyle("Fusion");
    }
    void rangePlanning() {
        for (const qint64 size : {1LL, 262144LL, 1000001LL, 134217729LL, 1099511627776LL}) for (const int count : {1, 2, 16, 512}) {
            const auto planned = DirectDownload::ranges(size, count); QVERIFY(!planned.isEmpty()); QVERIFY(planned.size() <= count); QCOMPARE(planned.first().first, 0); QCOMPARE(planned.last().second, size - 1); qint64 next = 0;
            for (const auto &range : planned) { QCOMPARE(range.first, next); QVERIFY(range.second >= range.first); next = range.second + 1; } QCOMPARE(next, size);
        }
        QCOMPARE(DirectDownload::ranges(134217728, 512).size(), 512);
        for (const qint64 size : {-1LL, 0LL, 1099511627777LL}) QVERIFY(DirectDownload::ranges(size, 16).isEmpty());
        QVERIFY(DirectDownload::ranges(1048576, 0).isEmpty()); QVERIFY(DirectDownload::ranges(1048576, 513).isEmpty());
    }
    void inputValidation() {
        QTemporaryDir root; DirectDownload download; QString error; const auto path = root.filePath("result.bin");
        for (const auto &url : {"file:///C:/test", "ftp://example.com/a", "https://user:password@example.com/a", "https://example.com/a#fragment", "http:///missing"}) { QVERIFY(!download.start(QUrl(url), path, 16, &error)); QVERIFY(!error.isEmpty()); }
        QVERIFY(!download.start(QUrl("http://127.0.0.1"), path, 513, &error)); QVERIFY(!download.start(QUrl("http://127.0.0.1"), path, 0, &error));
        for (const auto &name : {"CON", "nul.bin", "COM1.txt", "bad:name", "trailing.", "trailing ", "bad?file"}) QVERIFY(!download.start(QUrl("http://127.0.0.1"), root.filePath(name), 16, &error));
        QVERIFY(putFile(path, "original")); QVERIFY(!download.start(QUrl("http://127.0.0.1"), path, 16, &error)); QCOMPARE(contents(path), QByteArray("original")); QVERIFY(partials(root).isEmpty());
    }
    void segmentedConcurrent_data() { QTest::addColumn<int>("count"); QTest::newRow("16-connections") << 16; QTest::newRow("512-connections") << 512; }
    void segmentedConcurrent() {
        QFETCH(int, count); QTemporaryDir root; HttpFixture server; const qint64 size = qint64(count) * 262144 + 37; int segments = 0; QList<QPointer<QTcpSocket>> waiting;
        auto release = [&] { const auto sockets = waiting; waiting.clear(); for (const auto &socket : sockets) if (socket) { const auto start = socket->property("start").toLongLong(), end = socket->property("end").toLongLong(); HttpFixture::reply(socket, 206, payload(start, end - start + 1), HttpFixture::rangeHeader(start, end, size) + "ETag: \"fixture-v1\"\r\n"); } };
        server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &request) {
            QCOMPARE(request.encoding, QByteArray("identity")); if (request.range == "bytes=0-0") { HttpFixture::rangeReply(socket, request, size); return; }
            QCOMPARE(request.ifRange, QByteArray("\"fixture-v1\"")); ++segments; const auto range = HttpFixture::bounds(request); socket->setProperty("start", range.first); socket->setProperty("end", range.second); waiting.append(socket); if (segments == count) release();
        };
        DirectDownload download; QSignalSpy done(&download, &DirectDownload::completed), failed(&download, &DirectDownload::failed), progress(&download, &DirectDownload::progress); QString error; const auto path = root.filePath("完整文件.bin");
        QVERIFY(download.start(server.url(), path, count, &error)); QTRY_VERIFY_WITH_TIMEOUT(done.size() + failed.size() > 0, 45000); QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString())); QCOMPARE(done.size(), 1); QCOMPARE(segments, count); QVERIFY(server.maximumConnected >= count);
        const auto bytes = contents(path); QCOMPARE(bytes.size(), size); QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), QCryptographicHash::hash(payload(0, size), QCryptographicHash::Sha256)); QVERIFY(partials(root).isEmpty()); QVERIFY(!progress.isEmpty()); QCOMPARE(progress.last()[3].toInt(), 0);
    }
    void singleFallback_data() { QTest::addColumn<QString>("mode"); for (const auto &mode : {"ignored-range", "no-validator", "weak-validator", "one-connection", "small-file", "chunked", "chunked-after-probe", "empty", "range-rejected", "last-modified"}) QTest::newRow(mode) << QString(mode); }
    void singleFallback() {
        QFETCH(QString, mode); QTemporaryDir root; HttpFixture server; const qint64 size = mode == "empty" ? 0 : mode == "small-file" ? 103 : 1048613;
        server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &request) {
            QByteArray headers = mode == "weak-validator" ? "ETag: W/\"fixture-v1\"\r\n" : mode == "last-modified" ? "Last-Modified: Sat, 24 Oct 2026 00:00:00 GMT\r\n" : QByteArray();
            if (mode == "one-connection" || mode == "small-file" || mode == "chunked-after-probe") headers = "ETag: \"fixture-v1\"\r\n";
            if (request.range == "bytes=0-0" && mode == "range-rejected") { HttpFixture::reply(socket, 416, {}); return; }
            if (!request.range.isEmpty() && mode != "ignored-range" && mode != "chunked" && mode != "empty" && mode != "range-rejected") { HttpFixture::rangeReply(socket, request, size, headers); return; }
            if (mode.startsWith("chunked")) { const auto body = payload(0, size); socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n" + headers + "\r\n" + QByteArray::number(body.size(), 16) + "\r\n" + body + "\r\n0\r\n\r\n"); socket->disconnectFromHost(); }
            else HttpFixture::reply(socket, 200, payload(0, size), headers);
        };
        DirectDownload download; QSignalSpy done(&download, &DirectDownload::completed), failed(&download, &DirectDownload::failed); QString error; const auto path = root.filePath("single.bin");
        QVERIFY(download.start(server.url(), path, mode == "one-connection" || mode == "chunked-after-probe" ? 1 : 16, &error)); QTRY_VERIFY_WITH_TIMEOUT(done.size() + failed.size() > 0, 10000); QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString())); QCOMPARE(contents(path), payload(0, size)); QVERIFY(partials(root).isEmpty());
        if (mode == "no-validator" || mode == "weak-validator") { QCOMPARE(server.requests.size(), 2); QVERIFY(server.requests.last().range.isEmpty()); }
    }
    void unsafeResponses_data() { QTest::addColumn<QString>("mode"); for (const auto &mode : {"malformed-probe", "oversized-probe", "changed-etag", "wrong-range", "ignored-segment", "encoded", "short-segment", "overlong-segment", "not-found", "single-truncated", "single-version", "single-size"}) QTest::newRow(mode) << QString(mode); }
    void unsafeResponses() {
        QFETCH(QString, mode); QTemporaryDir root; HttpFixture server; const qint64 size = 1048576;
        server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &request) {
            if (mode == "not-found") { HttpFixture::reply(socket, 404, "missing"); return; }
            if (mode == "single-truncated") { HttpFixture::reply(socket, 200, "short", {}, size); return; }
            if (request.range == "bytes=0-0") {
                if (mode == "malformed-probe") HttpFixture::reply(socket, 206, "a", "Content-Range: bytes 0-9/1048576\r\n");
                else if (mode == "oversized-probe") HttpFixture::reply(socket, 206, "ab", HttpFixture::rangeHeader(0, 0, size));
                else HttpFixture::rangeReply(socket, request, size); return;
            }
            if (mode.startsWith("single-")) { HttpFixture::reply(socket, 200, payload(0, size), mode == "single-version" ? "ETag: \"changed\"\r\n" : "ETag: \"fixture-v1\"\r\n", mode == "single-size" ? size + 1 : size); return; }
            auto range = HttpFixture::bounds(request); QByteArray header = HttpFixture::rangeHeader(range.first + (mode == "wrong-range" ? 1 : 0), range.second, size); header += mode == "changed-etag" ? "ETag: \"changed\"\r\n" : "ETag: \"fixture-v1\"\r\n"; if (mode == "encoded") header += "Content-Encoding: gzip\r\n";
            const auto length = range.second - range.first + 1; HttpFixture::reply(socket, mode == "ignored-segment" ? 200 : 206, payload(range.first, length + (mode == "short-segment" ? -1 : mode == "overlong-segment" ? 1 : 0)), header);
        };
        DirectDownload download; QSignalSpy done(&download, &DirectDownload::completed), failed(&download, &DirectDownload::failed); QString error; const auto path = root.filePath("bad.bin");
        QVERIFY(download.start(server.url(), path, mode.startsWith("single-") ? 1 : 4, &error)); QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000); QVERIFY(done.isEmpty()); QVERIFY(!download.busy()); QVERIFY(!QFile::exists(path)); QVERIFY(partials(root).isEmpty());
    }
    void retries_data() { QTest::addColumn<QString>("mode"); for (const auto &mode : {"503", "429", "interrupted", "exhausted"}) QTest::newRow(mode) << QString(mode); }
    void retries() {
        QFETCH(QString, mode); QTemporaryDir root; HttpFixture server; const qint64 size = 1048576; QMap<qint64, int> attempts; bool resumed = false;
        server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &request) {
            if (request.range == "bytes=0-0") { HttpFixture::rangeReply(socket, request, size); return; } const auto range = HttpFixture::bounds(request); const int attempt = ++attempts[range.second];
            if (attempt == 1 || mode == "exhausted") { if (mode == "interrupted") HttpFixture::reply(socket, 206, payload(range.first, 32768), HttpFixture::rangeHeader(range.first, range.second, size) + "ETag: \"fixture-v1\"\r\n", range.second - range.first + 1); else HttpFixture::reply(socket, mode == "429" ? 429 : 503, "busy"); return; }
            if (mode == "interrupted" && range.first % 262144 == 32768) resumed = true; HttpFixture::rangeReply(socket, request, size);
        };
        DirectDownload download; QSignalSpy done(&download, &DirectDownload::completed), failed(&download, &DirectDownload::failed); QString error; const auto path = root.filePath("retry.bin");
        QVERIFY(download.start(server.url(), path, 4, &error)); QTRY_VERIFY_WITH_TIMEOUT(done.size() + failed.size() > 0, 12000);
        if (mode == "exhausted") {
            QCOMPARE(failed.size(), 1); QVERIFY(!QFile::exists(path)); bool exhausted = false;
            for (const auto count : attempts) { QVERIFY(count >= 1 && count <= 3); exhausted = exhausted || count == 3; }
            QVERIFY(exhausted);
        } else { QCOMPARE(done.size(), 1); QCOMPARE(contents(path), payload(0, size)); if (mode == "interrupted") QVERIFY(resumed); }
        QVERIFY(partials(root).isEmpty());
    }
    void stalledTailRecovers_data() {
        QTest::addColumn<bool>("trickle");
        QTest::newRow("97-percent-no-data") << false;
        QTest::newRow("97-percent-trickling") << true;
    }
    void stalledTailRecovers() {
        QFETCH(bool, trickle);
        QTemporaryDir root; HttpFixture server; const qint64 size = 32 * 1048576;
        const qint64 tail = 31 * 1048576, prefix = 131072;
        bool held = false, recovering = false; int recovered = 0;
        server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &request) {
            if (request.range == "bytes=0-0") { HttpFixture::rangeReply(socket, request, size); return; }
            QCOMPARE(request.ifRange, QByteArray("\"fixture-v1\""));
            const auto range = HttpFixture::bounds(request);
            if (range.first == tail && !held) {
                held = true;
                socket->write("HTTP/1.1 206 Test\r\nContent-Length: " + QByteArray::number(range.second - range.first + 1) + "\r\n" + HttpFixture::rangeHeader(range.first, range.second, size) + "ETag: \"fixture-v1\"\r\n\r\n" + payload(range.first, prefix));
                if (trickle) {
                    auto *timer = new QTimer(socket); timer->setInterval(50);
                    connect(timer, &QTimer::timeout, socket, [socket, offset = range.first + prefix]() mutable { socket->write(payload(offset, 1)); ++offset; });
                    timer->start();
                }
                return;
            }
            if (range.first >= tail + prefix) { recovering = true; ++recovered; }
            HttpFixture::rangeReply(socket, request, size);
        };
        DirectDownload download; DownloadTestAccess::fastWatchdog(download, trickle ? 500 : 450, 650);
        QSignalSpy done(&download, &DirectDownload::completed), failed(&download, &DirectDownload::failed), progress(&download, &DirectDownload::progress), status(&download, &DirectDownload::status);
        QString error; const auto path = root.filePath("tail.bin");
        QVERIFY(download.start(server.url(), path, 32, &error));
        QTRY_VERIFY_WITH_TIMEOUT(done.size() + failed.size() > 0, 12000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first()[0].toString()));
        QCOMPARE(done.size(), 1); QVERIFY(recovering); QCOMPARE(recovered, 3);
        QCOMPARE(QCryptographicHash::hash(contents(path), QCryptographicHash::Sha256), QCryptographicHash::hash(payload(0, size), QCryptographicHash::Sha256));
        QVERIFY(partials(root).isEmpty());
        bool sawTail = false, sawRecovery = false;
        for (const auto &sample : progress) { if (sample[0].toLongLong() * 100 / size >= 97 && sample[0].toLongLong() < size) { sawTail = true; QVERIFY(sample[3].toInt() < 32); } }
        for (const auto &sample : status) if (sample[0].toString().contains(QStringLiteral("恢复"))) sawRecovery = true;
        QVERIFY(sawTail); QVERIFY(sawRecovery); QTRY_COMPARE(server.connected, 0);
    }
    void stalledRetriesRemainBoundedAndCancellable_data() {
        QTest::addColumn<bool>("cancel"); QTest::newRow("retry-exhaustion") << false; QTest::newRow("cancel-reconnect") << true;
    }
    void stalledRetriesRemainBoundedAndCancellable() {
        QFETCH(bool, cancel); QTemporaryDir root; HttpFixture server; int segments = 0;
        server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &request) {
            if (request.range == "bytes=0-0") { HttpFixture::rangeReply(socket, request, 1048576); return; }
            ++segments;
            const auto range = HttpFixture::bounds(request);
            socket->write("HTTP/1.1 206 Test\r\nContent-Length: " + QByteArray::number(range.second - range.first + 1) + "\r\n" + HttpFixture::rangeHeader(range.first, range.second, 1048576) + "ETag: \"fixture-v1\"\r\n\r\n" + payload(range.first, 1));
        };
        DirectDownload download; DownloadTestAccess::fastWatchdog(download, 200, 600);
        QSignalSpy failed(&download, &DirectDownload::failed), done(&download, &DirectDownload::completed), status(&download, &DirectDownload::status);
        QString error; const auto path = root.filePath("never.bin");
        QVERIFY(download.start(server.url(), path, 4, &error));
        if (cancel) { QTRY_VERIFY(status.size() > 2); download.cancel(); QTest::qWait(1200); QCOMPARE(segments, 4); }
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
        QVERIFY(done.isEmpty()); QVERIFY(!download.busy()); QVERIFY(!QFile::exists(path)); QVERIFY(partials(root).isEmpty());
        QVERIFY(segments <= 12); QTRY_COMPARE(server.connected, 0);
        auto *watchdog = download.findChild<QTimer *>("downloadWatchdog"); QVERIFY(watchdog); QVERIFY(!watchdog->isActive());
    }
    void inactivitySpeedAndTimeoutCancellation() {
        QTemporaryDir root; HttpFixture server;
        server.respond = [](QTcpSocket *socket, const HttpFixture::Request &request) {
            if (request.range == "bytes=0-0") { HttpFixture::rangeReply(socket, request, 1048576); return; }
            const auto range = HttpFixture::bounds(request);
            socket->write("HTTP/1.1 206 Test\r\nContent-Length: " + QByteArray::number(range.second - range.first + 1) + "\r\n" + HttpFixture::rangeHeader(range.first, range.second, 1048576) + "ETag: \"fixture-v1\"\r\n\r\n" + payload(range.first, 1024));
        };
        DirectDownload download; QSignalSpy progress(&download, &DirectDownload::progress), failed(&download, &DirectDownload::failed); QString error;
        QVERIFY(download.start(server.url(), root.filePath("idle.bin"), 4, &error)); QTRY_COMPARE(server.requests.size(), 5);
        QTest::qWait(150); DownloadTestAccess::sample(download); QTest::qWait(300); DownloadTestAccess::sample(download);
        QCOMPARE(progress.last()[2].toDouble(), 0.0); QCOMPARE(progress.last()[3].toInt(), 4);
        const auto replies = download.findChildren<QNetworkReply *>();
        auto iterator = std::find_if(replies.begin(), replies.end(), [](QNetworkReply *reply) { return reply->isRunning(); });
        QVERIFY(iterator != replies.end()); (*iterator)->abort();
        QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() > 5, 3500); QVERIFY(download.busy()); QVERIFY(failed.isEmpty());
        download.cancel(); QTRY_COMPARE(server.connected, 0); QVERIFY(partials(root).isEmpty());
    }
    void recoveryStillRejectsChangedFile() {
        QTemporaryDir root; HttpFixture server; QSet<qint64> seen;
        server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &request) {
            if (request.range == "bytes=0-0") { HttpFixture::rangeReply(socket, request, 1048576); return; }
            const auto range = HttpFixture::bounds(request);
            const auto end = range.second;
            if (seen.contains(end)) { HttpFixture::rangeReply(socket, request, 1048576, "ETag: \"changed\"\r\n"); return; }
            seen.insert(end);
            socket->write("HTTP/1.1 206 Test\r\nContent-Length: " + QByteArray::number(end - range.first + 1) + "\r\n" + HttpFixture::rangeHeader(range.first, end, 1048576) + "ETag: \"fixture-v1\"\r\n\r\n" + payload(range.first, 1));
        };
        DirectDownload download; DownloadTestAccess::fastWatchdog(download, 200, 600);
        QSignalSpy failed(&download, &DirectDownload::failed), done(&download, &DirectDownload::completed); QString error;
        const auto path = root.filePath("changed.bin"); QVERIFY(download.start(server.url(), path, 4, &error));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 6000); QVERIFY(done.isEmpty()); QVERIFY(!QFile::exists(path)); QVERIFY(partials(root).isEmpty());
        QVERIFY(failed.first()[0].toString().contains(QStringLiteral("版本"))); QTRY_COMPARE(server.connected, 0);
    }
    void tailChildrenInheritRetryBudget() {
        QTemporaryDir root; HttpFixture server; const qint64 size = 32 * 1048576, tail = 31 * 1048576; int tailRequests = 0;
        server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &request) {
            if (request.range == "bytes=0-0") { HttpFixture::rangeReply(socket, request, size); return; }
            const auto range = HttpFixture::bounds(request);
            if (range.first < tail) { HttpFixture::rangeReply(socket, request, size); return; }
            ++tailRequests;
            socket->write("HTTP/1.1 206 Test\r\nContent-Length: " + QByteArray::number(range.second - range.first + 1) + "\r\n" + HttpFixture::rangeHeader(range.first, range.second, size) + "ETag: \"fixture-v1\"\r\n\r\n" + payload(range.first, 1));
        };
        DirectDownload download; DownloadTestAccess::fastWatchdog(download, 250, 600);
        QSignalSpy failed(&download, &DirectDownload::failed), done(&download, &DirectDownload::completed), progress(&download, &DirectDownload::progress); QString error;
        const auto path = root.filePath("tail-exhausted.bin"); QVERIFY(download.start(server.url(), path, 32, &error));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000); QVERIFY(done.isEmpty());
        QVERIFY(tailRequests > 1); QVERIFY(tailRequests <= 7);
        for (const auto &sample : progress) QVERIFY(sample[3].toInt() <= 32);
        QVERIFY(!QFile::exists(path)); QVERIFY(partials(root).isEmpty()); QTRY_COMPARE(server.connected, 0);
    }
    void unresponsiveSegmentRestartsBeforeFirstByte() {
        QTemporaryDir root; HttpFixture server; QSet<qint64> waiting; int retries = 0;
        server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &request) {
            if (request.range == "bytes=0-0") { HttpFixture::rangeReply(socket, request, 1048576); return; }
            const auto range = HttpFixture::bounds(request);
            if (!waiting.contains(range.first)) { waiting.insert(range.first); return; }
            ++retries; HttpFixture::rangeReply(socket, request, 1048576);
        };
        DirectDownload download; DownloadTestAccess::fastWatchdog(download, 200, 600);
        QSignalSpy done(&download, &DirectDownload::completed), failed(&download, &DirectDownload::failed); QString error;
        const auto path = root.filePath("handshake.bin"); QVERIFY(download.start(server.url(), path, 4, &error));
        QTRY_VERIFY_WITH_TIMEOUT(done.size() + failed.size() > 0, 6000); QVERIFY(failed.isEmpty()); QCOMPARE(done.size(), 1); QCOMPARE(retries, 4);
        QCOMPARE(contents(path), payload(0, 1048576)); QVERIFY(partials(root).isEmpty()); QTRY_COMPARE(server.connected, 0);
    }
    void recoveryStatusSurvivesProgressUpdates() {
        QTemporaryDir root; DownloadDialog dialog(root.path());
        auto *engine = dialog.findChild<DirectDownload *>(); QVERIFY(engine);
        auto *status = dialog.findChild<QLabel *>("downloadStatus"); QVERIFY(status);
        engine->status(QStringLiteral("正在恢复未完成分段"));
        engine->progress(970, 1000, 0.0, 1);
        QCOMPARE(status->text(), QStringLiteral("正在恢复未完成分段"));
        QVERIFY(dialog.findChild<QLabel *>("downloadDetail")->text().contains(QStringLiteral("1 并发")));
    }
    void cancelRestartAndPreserveCompletedFile() {
        QTemporaryDir root; HttpFixture server; bool hold = true; server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &) { if (!hold) HttpFixture::reply(socket, 200, "complete"); };
        DirectDownload download; QSignalSpy done(&download, &DirectDownload::completed), failed(&download, &DirectDownload::failed); QString error; const auto first = root.filePath("first.bin"), second = root.filePath("second.bin");
        QVERIFY(download.start(server.url(), first, 512, &error)); QVERIFY(!download.start(server.url(), second, 16, &error)); QTRY_COMPARE(server.requests.size(), 1); QVERIFY(!partials(root).isEmpty()); download.cancel(); QCOMPARE(failed.size(), 1); QTRY_COMPARE(server.connected, 0); QVERIFY(partials(root).isEmpty()); QVERIFY(!QFile::exists(first));
        hold = false; QVERIFY(download.start(server.url(), first, 16, &error)); QTRY_COMPARE(done.size(), 1); QCOMPARE(contents(first), QByteArray("complete"));
        QVERIFY(download.start(server.url(), second, 16, &error)); QTRY_COMPARE(done.size(), 2); QCOMPARE(contents(first), QByteArray("complete")); QCOMPARE(contents(second), QByteArray("complete")); QVERIFY(partials(root).isEmpty()); download.cancel(); QCOMPARE(failed.size(), 1);
    }
    void cancelPendingRetryAndTimeout() {
        QTemporaryDir root; HttpFixture server; server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &request) { if (request.range == "bytes=0-0") HttpFixture::rangeReply(socket, request, 1048576); else HttpFixture::reply(socket, 503, {}); };
        DirectDownload download; QSignalSpy failed(&download, &DirectDownload::failed); QString error; QVERIFY(download.start(server.url(), root.filePath("retry.bin"), 4, &error)); QTRY_COMPARE(server.requests.size(), 5); QTest::qWait(100); download.cancel(); QTest::qWait(1200); QCOMPARE(server.requests.size(), 5); QCOMPARE(failed.size(), 1); QVERIFY(partials(root).isEmpty());
        server.respond = {}; QVERIFY(download.start(server.url(), root.filePath("timeout.bin"), 16, &error)); QTRY_COMPARE(server.requests.size(), 6); auto *deadline = download.findChild<QTimer *>("downloadDeadline"); QVERIFY(deadline); deadline->start(20); QTRY_COMPARE(failed.size(), 2); QVERIFY(partials(root).isEmpty()); QTRY_COMPARE(server.connected, 0);
    }
    void cancelSegmentsAndDestruction() {
        QTemporaryDir root; HttpFixture server; server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &request) { if (request.range == "bytes=0-0") HttpFixture::rangeReply(socket, request, 4194304); }; QString error;
        { DirectDownload download; QVERIFY(download.start(server.url(), root.filePath("cancel.bin"), 16, &error)); QTRY_COMPARE(server.requests.size(), 17); download.cancel(); QTRY_COMPARE(server.connected, 0); QVERIFY(partials(root).isEmpty()); }
        { DirectDownload download; QVERIFY(download.start(server.url(), root.filePath("destruct.bin"), 16, &error)); QTRY_COMPARE(server.requests.size(), 34); } QTRY_COMPARE(server.connected, 0); QVERIFY(partials(root).isEmpty()); QVERIFY(!QFile::exists(root.filePath("destruct.bin")));
    }
    void targetRaceAndRedirect() {
        QTemporaryDir root; HttpFixture server; const auto path = root.filePath("race.bin"); server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &request) { if (request.path == "/redirect") HttpFixture::reply(socket, 302, {}, "Location: /file.bin\r\n"); else { QVERIFY(putFile(path, "external")); HttpFixture::reply(socket, 200, "download"); } };
        DirectDownload download; QSignalSpy failed(&download, &DirectDownload::failed), done(&download, &DirectDownload::completed); QString error; QVERIFY(download.start(server.url("/redirect"), path, 16, &error)); QTRY_COMPARE(failed.size(), 1); QCOMPARE(contents(path), QByteArray("external")); QVERIFY(partials(root).isEmpty());
        server.respond = [&](QTcpSocket *socket, const HttpFixture::Request &request) { if (request.path == "/redirect") HttpFixture::reply(socket, 302, {}, "Location: /file.bin\r\n"); else HttpFixture::reply(socket, 200, "redirected"); };
        const auto second = root.filePath("redirect.bin"); QVERIFY(download.start(server.url("/redirect"), second, 16, &error)); QTRY_COMPARE(done.size(), 1); QCOMPARE(contents(second), QByteArray("redirected"));
    }
    void dialogRendersAndOperates_data() { QTest::addColumn<QString>("theme"); QTest::addColumn<bool>("dark"); for (const auto &theme : themeIds()) for (const bool dark : {false, true}) { const auto name = (theme + (dark ? "-dark" : "-light")).toUtf8(); QTest::newRow(name.constData()) << theme << dark; } }
    void dialogRendersAndOperates() {
        QFETCH(QString, theme); QFETCH(bool, dark); const auto previousPalette = qApp->palette(); const auto previousSheet = qApp->styleSheet(); const auto restore = qScopeGuard([&] { qApp->setPalette(previousPalette); qApp->setStyleSheet(previousSheet); }); const auto palette = appearancePalette(theme, dark, {}); qApp->setPalette(palette); qApp->setStyleSheet(appearanceStyleSheet(theme, dark, palette));
        QTemporaryDir root; HttpFixture server; DownloadDialog dialog(root.path()); dialog.show(); QTest::qWait(40); auto *url = dialog.findChild<QLineEdit *>("downloadUrl"), *path = dialog.findChild<QLineEdit *>("downloadPath"); auto *count = dialog.findChild<QSpinBox *>("downloadConnections"); auto *start = dialog.findChild<QPushButton *>("downloadStart"), *stop = dialog.findChild<QPushButton *>("downloadCancel"); auto *detail = dialog.findChild<QLabel *>("downloadDetail"); QVERIFY(url && path && count && start && stop && detail); QCOMPARE(count->maximum(), 512); count->setValue(512); QCOMPARE(count->value(), 512); QVERIFY(start->isEnabled()); QVERIFY(!stop->isEnabled());
        const auto screenshot = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT"); if (!screenshot.isEmpty()) QVERIFY(dialog.grab().save(QDir(screenshot).filePath("native-download-" + theme + (dark ? "-dark.png" : "-light.png"))));
        start->click(); QVERIFY(!detail->text().isEmpty()); QVERIFY(start->isEnabled()); url->setText(server.url().toString()); path->setText(root.filePath("ui.bin")); start->click(); QVERIFY(stop->isEnabled()); QVERIFY(!url->isEnabled()); QTRY_COMPARE(server.requests.size(), 1);
        QTimer::singleShot(30, &dialog, [] { if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) box->button(QMessageBox::No)->click(); }); QVERIFY(!dialog.close()); QVERIFY(dialog.isVisible()); stop->click(); QVERIFY(start->isEnabled()); QVERIFY(!stop->isEnabled()); QVERIFY(partials(root).isEmpty());
        server.respond = [](QTcpSocket *socket, const HttpFixture::Request &) { HttpFixture::reply(socket, 200, "ui complete"); }; start->click(); QTRY_VERIFY(start->isEnabled()); QCOMPARE(contents(root.filePath("ui.bin")), QByteArray("ui complete")); QCOMPARE(dialog.findChild<QProgressBar *>("downloadProgress")->value(), 1000);
        QTest::keyClick(&dialog, Qt::Key_Escape); QVERIFY(!dialog.isVisible());
    }
    void closeConfirmsCancellation() {
        QTemporaryDir root; HttpFixture server; DownloadDialog dialog(root.path()); dialog.show(); dialog.findChild<QLineEdit *>("downloadUrl")->setText(server.url().toString()); dialog.findChild<QLineEdit *>("downloadPath")->setText(root.filePath("closing.bin")); dialog.findChild<QPushButton *>("downloadStart")->click(); QTRY_COMPARE(server.requests.size(), 1);
        QTimer::singleShot(30, &dialog, [] { if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) box->button(QMessageBox::Yes)->click(); }); QVERIFY(dialog.close()); QTRY_COMPARE(server.connected, 0); QVERIFY(partials(root).isEmpty()); QVERIFY(!QFile::exists(root.filePath("closing.bin")));
    }
    void windowMenuOpensSingleDialog() {
        Window window; window.show(); bool found = false; QTimer::singleShot(30, &window, [&] { auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); if (!menu) return; for (auto *action : menu->actions()) if (action->text().startsWith(QStringLiteral("直链下载"))) { found = true; action->trigger(); menu->close(); return; } menu->close(); });
        auto *more = window.findChild<QPushButton *>("action_more"); QVERIFY(more); more->click(); QVERIFY(found); QTRY_VERIFY(window.findChild<DownloadDialog *>("directDownloadDialog")); auto *dialog = window.findChild<DownloadDialog *>("directDownloadDialog"); window.showDownload(); QCOMPARE(window.findChildren<DownloadDialog *>().size(), 1); QVERIFY(dialog->isVisible()); dialog->close(); QTRY_VERIFY(window.findChild<DownloadDialog *>("directDownloadDialog") == nullptr); window.close();
    }
    void windowDownloadUsesInlineProgressAndRemembersDestination() {
        QTemporaryDir root; const auto oldSettings = qgetenv("TWINFILE_SETTINGS");
        qputenv("TWINFILE_SETTINGS", root.filePath("settings.json").toUtf8());
        const auto restore = qScopeGuard([&] { if (oldSettings.isEmpty()) qunsetenv("TWINFILE_SETTINGS"); else qputenv("TWINFILE_SETTINGS", oldSettings); });
        HttpFixture server; Window window; window.show(); window.showDownload();
        auto *dialog = window.findChild<DownloadDialog *>("directDownloadDialog"); QVERIFY(dialog);
        auto *path = dialog->findChild<QLineEdit *>("downloadPath"); QVERIFY(path);
        QVERIFY(dialog->findChild<QPushButton *>("downloadBrowse"));
        dialog->findChild<QLineEdit *>("downloadUrl")->setText(server.url("/chosen.bin").toString());
        QVERIFY(path->text().endsWith(QStringLiteral("chosen.bin")));
        const QString destination = root.filePath("saved.bin"); path->setText(destination);
        dialog->findChild<QPushButton *>("downloadStart")->click();
        QTRY_COMPARE(server.requests.size(), 1); QVERIFY(!dialog->isVisible());
        auto *task = window.findChild<QWidget *>("taskProgress"); QVERIFY(task); QVERIFY(task->isVisible());
        QVERIFY(!QApplication::activeModalWidget());
        auto *engine = dialog->findChild<DirectDownload *>(); QVERIFY(engine);
        engine->progress(1024, 4096, 2048, 1);
        QCOMPARE(task->findChild<QProgressBar *>("jobProgress")->value(), 250);
        QVERIFY(task->toolTip().contains(QStringLiteral("2.0 KB/s")));
        task->findChild<QToolButton *>("taskCancel")->click(); QTRY_COMPARE(server.connected, 0);
        QVERIFY(!dialog->isVisible()); QVERIFY(partials(root).isEmpty());
        QCOMPARE(readSettings(root.filePath("settings.json")).value(QStringLiteral("download_directory")).toString(), root.path());
        dialog->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window.showDownload(); dialog = window.findChild<DownloadDialog *>("directDownloadDialog"); QVERIFY(dialog);
        dialog->findChild<QLineEdit *>("downloadUrl")->setText(server.url("/second.bin").toString());
        QCOMPARE(dialog->findChild<QLineEdit *>("downloadPath")->text(), root.filePath("second.bin"));
        server.respond = [](QTcpSocket *socket, const HttpFixture::Request &) { HttpFixture::reply(socket, 200, "inline complete"); };
        dialog->findChild<QPushButton *>("downloadStart")->click();
        QTRY_COMPARE(contents(root.filePath("second.bin")), QByteArray("inline complete"));
        QVERIFY(!dialog->isVisible()); QVERIFY(!QApplication::activeModalWidget());
        QCOMPARE(task->findChild<QProgressBar *>("jobProgress")->value(), 1000);
        window.close();
    }
};
QTEST_MAIN(DownloadTests)
#include "download_tests.moc"
