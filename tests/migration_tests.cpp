#include "core.h"
#include "editor.h"
#include "hfx.h"
#include "operations.h"
#include "payload.h"
#include "zip.h"
#include "android.h"
#include "adbsync.h"
#include "inspection.h"
#include "media.h"
#include "shell.h"
#include "appearance.h"
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScrollBar>
#include <QProgressDialog>
#include <QProgressBar>
#include <QToolButton>
#include <QDockWidget>
#include <QLabel>
#include "task_progress.h"
#include <QPushButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QScopeGuard>
#include <QSet>
#include <QTimer>
#include <QTableView>
#include "window.h"
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTest>
#include <QtEndian>
#include <bzlib.h>
#include <lzma.h>
#include <zstd.h>
#include <chrono>
#include <future>
#include <thread>

using namespace twinfile;

template<class Function>
class QtTask {
    using Value = std::invoke_result_t<Function>;
public:
    explicit QtTask(Function function) {
        std::packaged_task<Value()> task(std::move(function));
        result_ = task.get_future();
        thread_.reset(QThread::create([task = std::move(task)]() mutable { task(); }));
        thread_->start();
    }
    ~QtTask() { thread_->wait(); }
    template<class Rep, class Period> auto wait_for(const std::chrono::duration<Rep, Period> &duration) { return result_.wait_for(duration); }
    Value get() { const auto wait = qScopeGuard([&] { thread_->wait(); }); return result_.get(); }
private:
    std::future<Value> result_;
    std::unique_ptr<QThread> thread_;
};

// Match the application's Qt-owned workers, including deterministic thread teardown.
template<class Function> static auto qtAsync(Function function) { return QtTask<Function>(std::move(function)); }

namespace twinfile {
class PayloadTestAccess {
public:
    static void open(Window &window) { window.payload(); }
    static bool busy(const Window &window) { return window.busy_; }
    static void cancel(Window &window) { if (window.cancellation_) window.cancellation_->store(true); }
    static QJsonObject settings(const Window &window) { return window.settings_; }
    static void job(Window &window, std::function<Result(const Cancellation &, const Progress &)> work) {
        window.runJob(QStringLiteral("解析 Payload 分区"), std::move(work));
    }
};
}

static bool writeFile(const QString &path, const QByteArray &bytes) { QFile output(path); return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size(); }
static QByteArray readFile(const QString &path) { QFile input(path); if (!input.open(QIODevice::ReadOnly)) return {}; return input.readAll(); }
static QByteArray varint(quint64 value) { QByteArray bytes; do { const auto part = quint8(value & 127); value >>= 7; bytes.append(char(part | (value ? 128 : 0))); } while (value); return bytes; }
static QByteArray integer(int field, quint64 value) { return varint(quint64(field) << 3) + varint(value); }
static QByteArray message(int field, const QByteArray &bytes) { return varint((quint64(field) << 3) | 2) + varint(quint64(bytes.size())) + bytes; }
template<class Integer> static QByteArray network(Integer value) { QByteArray bytes(sizeof(Integer), '\0'); qToBigEndian<Integer>(value, bytes.data()); return bytes; }
static QByteArray utf(const QString &text) { return network<quint16>(quint16(text.toUtf8().size())) + text.toUtf8(); }
static QByteArray little(quint32 value) { QByteArray bytes(4, '\0'); qToLittleEndian<quint32>(value, bytes.data()); return bytes; }

class FakeAdb : public QObject {
public:
    QTcpServer server;
    QHash<QString, QByteArray> files;
    QStringList directories;
    bool badReceive = false;
    bool version2 = true;
    FakeAdb() {
        server.listen(QHostAddress::LocalHost);
        qputenv("TWINFILE_ADB_TEST_PORT", QByteArray::number(server.serverPort()));
        connect(&server, &QTcpServer::newConnection, this, [this] {
            auto *client = server.nextPendingConnection();
            struct State { QByteArray bytes, body; int mode = 0; QString file; };
            auto state = std::make_shared<State>();
            connect(client, &QTcpSocket::readyRead, client, [this, state, client] {
                state->bytes += client->readAll();
                for (;;) {
                    if (state->mode < 2) {
                        if (state->bytes.size() < 4) return;
                        bool ok = false; const int length = state->bytes.left(4).toInt(&ok, 16);
                        if (!ok || length < 0 || length > 4096) { client->abort(); return; }
                        if (state->bytes.size() < length + 4) return;
                        const QByteArray service = state->bytes.mid(4, length);
                        state->bytes.remove(0, length + 4); ++state->mode; client->write("OKAY");
                        if (service.endsWith(":features")) { const QByteArray features = version2 ? QByteArray("stat_v2,ls_v2") : QByteArray(); client->write(QByteArray::number(features.size(), 16).rightJustified(4, '0') + features); }
                        continue;
                    }
                    if (state->bytes.size() < 8) return;
                    const QByteArray id = state->bytes.left(4); const quint32 length = qFromLittleEndian<quint32>(state->bytes.constData() + 4);
                    const bool done = id == "DONE";
                    if (length > 1024 * 1024 && !done) { client->abort(); return; }
                    if (!done && state->bytes.size() < 8 + length) return;
                    const QByteArray payload = done ? QByteArray() : state->bytes.mid(8, length);
                    state->bytes.remove(0, 8 + (done ? 0 : length));
                    const QString path = QString::fromUtf8(payload);
                    if (state->mode == 3) {
                        if (id == "DATA") state->body += payload;
                        else if (id == "DONE") { files.insert(state->file, state->body); state->body.clear(); state->mode = 2; client->write(QByteArray("OKAY") + little(0)); }
                        else { client->abort(); return; }
                    } else if (id == "STAT" || id == "LST2") {
                        const quint32 mode = directories.contains(path) ? 0040755U : files.contains(path) ? 0100644U : 0;
                        if (id == "STAT") client->write(QByteArray("STAT") + little(mode) + little(quint32(files.value(path).size())) + little(1234));
                        else client->write(QByteArray("LST2") + little(mode ? 0 : 2) + QByteArray(16, '\0') + little(mode) + QByteArray(12, '\0') + little(quint32(files.value(path).size())) + little(0) + QByteArray(8, '\0') + little(1234) + little(0) + QByteArray(8, '\0'));
                    } else if (id == "LIST" || id == "LIS2") {
                        for (auto item = files.cbegin(); item != files.cend(); ++item) {
                            if (QFileInfo(item.key()).path() != path) continue;
                            const QByteArray name = QFileInfo(item.key()).fileName().toUtf8();
                            if (id == "LIST") client->write(QByteArray("DENT") + little(0100644U) + little(quint32(item.value().size())) + little(1234) + little(quint32(name.size())) + name);
                            else client->write(QByteArray("DNT2") + little(0) + QByteArray(16, '\0') + little(0100644U) + QByteArray(12, '\0') + little(quint32(item.value().size())) + little(0) + QByteArray(8, '\0') + little(1234) + little(0) + QByteArray(8, '\0') + little(quint32(name.size())) + name);
                        }
                        client->write(QByteArray("DONE") + QByteArray(id == "LIS2" ? 72 : 16, '\0'));
                    } else if (id == "RECV") {
                        if (badReceive) { client->write(QByteArray("DATA") + little(1000000)); continue; }
                        const QByteArray bytes = files.value(path);
                        for (qsizetype index = 0; index < bytes.size(); index += 65536) { const QByteArray frame = bytes.mid(index, 65536); client->write(QByteArray("DATA") + little(quint32(frame.size())) + frame); }
                        client->write(QByteArray("DONE") + little(0));
                    } else if (id == "SEND") { state->file = path.section(QChar(','), 0, -2); state->mode = 3; }
                    else { client->abort(); return; }
                }
            });
            connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        });
    }
    ~FakeAdb() override { qunsetenv("TWINFILE_ADB_TEST_PORT"); }
};
static QByteArray encodePayload(const QByteArray &data, int type = 0, bool badHash = false, int partitionCount = 1) {
    QByteArray compressed = data;
    if (type == 1) { unsigned size = unsigned(data.size() * 2 + 1024); compressed.resize(size); if (BZ2_bzBuffToBuffCompress(compressed.data(), &size, const_cast<char *>(data.constData()), unsigned(data.size()), 1, 0, 0) != BZ_OK) return {}; compressed.resize(size); }
    if (type == 8) { size_t size = 0; compressed.resize(qsizetype(lzma_stream_buffer_bound(size_t(data.size())))); if (lzma_easy_buffer_encode(1, LZMA_CHECK_CRC64, nullptr, reinterpret_cast<const uint8_t *>(data.constData()), size_t(data.size()), reinterpret_cast<uint8_t *>(compressed.data()), &size, size_t(compressed.size())) != LZMA_OK) return {}; compressed.resize(qsizetype(size)); }
    if (type == 14) { compressed.resize(qsizetype(ZSTD_compressBound(size_t(data.size())))); const size_t size = ZSTD_compress(compressed.data(), size_t(compressed.size()), data.constData(), size_t(data.size()), 1); if (ZSTD_isError(size)) return {}; compressed.resize(qsizetype(size)); }
    if (type == 6 || type == 7) compressed.clear();
    const QByteArray expected = type == 6 || type == 7 ? QByteArray(data.size(), '\0') : data;
    const QByteArray extent = integer(1, 0) + integer(2, quint64(data.size() / 4096));
    const QByteArray operation = integer(1, type) + integer(2, 0) + integer(3, quint64(compressed.size())) + message(6, extent) + (compressed.isEmpty() ? QByteArray() : message(8, badHash ? QByteArray(32, 'x') : QCryptographicHash::hash(compressed, QCryptographicHash::Sha256)));
    const QByteArray info = integer(1, quint64(data.size())) + message(2, QCryptographicHash::hash(expected, QCryptographicHash::Sha256));
    QByteArray manifest = integer(3, 4096);
    for (int index = 0; index < partitionCount; ++index) {
        const QByteArray name = index == 0 ? QByteArray("system") : QByteArray("partition_") + QByteArray::number(index);
        manifest += message(13, message(1, name) + message(7, info) + message(8, operation));
    }
    return QByteArray("CrAU") + network<quint64>(2) + network<quint64>(quint64(manifest.size())) + network<quint32>(0) + manifest + compressed;
}

class PayloadHttpFixture : public QObject {
public:
    enum Mode { Range, Silent, Trickle, IgnoredRange };
    QTcpServer server;
    QByteArray bytes;
    Mode mode = Range;
    qint64 transferred = 0;
    int requests = 0, active = 0, peak = 0;
    QList<QPair<qint64, qint64>> ranges;
    qint64 delayedFrom = -1;
    qint64 silentFrom = -1;
    qint64 delayedMinimumSize = 0;
    int releaseAt = 0;
    QString dataFailure;
    bool failureUsed = false;
    bool stableValidator = true;
    QSet<qint64> stalledRanges;
    QList<QPair<QPointer<QTcpSocket>, QByteArray>> pending;
    explicit PayloadHttpFixture(QByteArray data = {}, Mode behavior = Range) : bytes(std::move(data)), mode(behavior) {
        server.setMaxPendingConnections(1024); server.setListenBacklogSize(1024);
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *client = server.nextPendingConnection(); auto request = std::make_shared<QByteArray>();
                connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
                connect(client, &QTcpSocket::readyRead, client, [this, client, request] {
                    *request += client->readAll(); if (!request->contains("\r\n\r\n") || client->property("handled").toBool()) return;
                    client->setProperty("handled", true); ++requests;
                    if (mode == Silent) return;
                    if (mode == IgnoredRange) { client->write("HTTP/1.1 200 OK\r\nContent-Length: 999999999\r\n\r\n"); return; }
                    if (mode == Trickle) {
                        client->write("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-3/999\r\nContent-Length: 4\r\n\r\n");
                        auto *timer = new QTimer(client); timer->setInterval(60);
                        connect(timer, &QTimer::timeout, client, [client] { if (client->state() == QAbstractSocket::ConnectedState) client->write("C"); }); timer->start(); return;
                    }
                    const auto range = QRegularExpression(QStringLiteral("Range: bytes=(\\d+)-(\\d+)"), QRegularExpression::CaseInsensitiveOption).match(QString::fromLatin1(*request));
                    const qint64 start = range.captured(1).toLongLong(), end = range.captured(2).toLongLong();
                    ranges.append({start, end});
                    if (silentFrom >= 0 && start >= silentFrom) return;
                    if (!range.hasMatch() || start < 0 || end < start || end >= bytes.size()) { client->abort(); return; }
                    const QByteArray body = bytes.mid(start, end - start + 1);
                    QByteArray response = QByteArray("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes ") + QByteArray::number(start) + '-' + QByteArray::number(end) + '/' + QByteArray::number(bytes.size())
                        + (stableValidator ? QByteArray("\r\nETag: \"payload-fixture\"") : QByteArray())
                        + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                    transferred += body.size();
                    if (delayedFrom >= 0 && start >= delayedFrom && body.size() >= delayedMinimumSize) {
                        ++active; peak = qMax(peak, active);
                        connect(client, &QTcpSocket::disconnected, client, [this] { --active; });
                        if (dataFailure == QStringLiteral("version")) response.replace("\"payload-fixture\"", "\"changed\"");
                        if (dataFailure == QStringLiteral("range")) response.replace("Content-Range: bytes ", "Content-Range: bytes 999");
                        if (dataFailure == QStringLiteral("ignored")) response.replace("206 Partial Content", "200 OK");
                        if (dataFailure == QStringLiteral("retry") && !failureUsed) { response.chop(100); failureUsed = true; }
                        if (dataFailure == QStringLiteral("429") || dataFailure == QStringLiteral("503") || (dataFailure == QStringLiteral("503-once") && !failureUsed)) {
                            failureUsed = true;
                            response = "HTTP/1.1 " + (dataFailure == QStringLiteral("429") ? QByteArray("429 Too Many Requests") : QByteArray("503 Service Unavailable"))
                                + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                        }
                        if (dataFailure == QStringLiteral("many-stalls") && start < delayedFrom + (bytes.size() - delayedFrom) / 2 && !stalledRanges.contains(end)) {
                            stalledRanges.insert(end); client->write(response.left(response.size() - body.size() / 2)); return;
                        }
                        if (dataFailure == QStringLiteral("stall") && !failureUsed && start == delayedFrom) {
                            failureUsed = true; client->write(response.left(response.size() - body.size() / 2)); return;
                        }
                        if (releaseAt > 0) {
                            pending.append({client, response});
                            if (pending.size() >= releaseAt) {
                                for (const auto &item : pending) if (item.first) { item.first->write(item.second); item.first->disconnectFromHost(); }
                                pending.clear(); releaseAt = 0;
                            }
                        } else QTimer::singleShot(100, client, [client, response] { client->write(response); client->disconnectFromHost(); });
                    } else { client->write(response); client->disconnectFromHost(); }
                });
            }
        });
    }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1/payload").arg(server.serverPort()); }
};

class MigrationTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qputenv("TWINFILE_TEST_MODE", "1"); }
    void archiveAddRenameDeleteAndPreserve() {
        QTemporaryDir root; const QString archive = root.filePath(QStringLiteral("包.zip"));
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("keep.txt"), QByteArray("keep")); writer.addFile(QStringLiteral("folder/old.txt"), QByteArray("old")); writer.addFile(QStringLiteral("remove.txt"), QByteArray("remove")); writer.close(); }
        const QString replacement = root.filePath(QStringLiteral("new.txt")); QVERIFY(writeFile(replacement, "new"));
        ArchiveChanges changes; changes.remove = {QStringLiteral("remove.txt")}; changes.rename.insert(QStringLiteral("folder"), QStringLiteral("新目录")); changes.files.insert(QStringLiteral("added.txt"), replacement);
        const Result edited = editArchive(archive, changes); QVERIFY2(edited.ok, qPrintable(edited.error)); ZipReader reader(archive);
        QCOMPARE(reader.fileData(QStringLiteral("keep.txt")), QByteArray("keep")); QCOMPARE(reader.fileData(QStringLiteral("新目录/old.txt")), QByteArray("old")); QCOMPARE(reader.fileData(QStringLiteral("added.txt")), QByteArray("new")); QVERIFY(reader.fileData(QStringLiteral("remove.txt")).isEmpty());
    }
    void archiveEditRejectsConflictsWithoutMutation() {
        QTemporaryDir root; const QString archive = root.filePath(QStringLiteral("source.zip")); { ZipWriter writer(archive); writer.addFile(QStringLiteral("a"), QByteArray("a")); writer.addFile(QStringLiteral("b"), QByteArray("b")); writer.close(); }
        const QByteArray before = readFile(archive); ArchiveChanges conflict; conflict.rename.insert(QStringLiteral("a"), QStringLiteral("b")); QVERIFY(!editArchive(archive, conflict).ok); QCOMPARE(readFile(archive), before);
        ArchiveChanges escape; escape.rename.insert(QStringLiteral("a"), QStringLiteral("../out")); QVERIFY(!editArchive(archive, escape).ok); QCOMPARE(readFile(archive), before);
        auto cancel = std::make_shared<std::atomic_bool>(true); QVERIFY(!editArchive(archive, {}, cancel).ok); QCOMPARE(readFile(archive), before);
    }
    void archiveWriteBackDetectsOutsideModification() {
        QTemporaryDir root; const QString archive = root.filePath(QStringLiteral("source.zip")); { ZipWriter writer(archive); writer.addFile(QStringLiteral("file.txt"), QByteArray("original")); writer.close(); }
        Entry entry; entry.source = Source::Archive; entry.path = archive; entry.inner = entry.name = QStringLiteral("file.txt");
        const QString edited = root.filePath(QStringLiteral("edited.txt")); QVERIFY(writeFile(edited, "changed")); const QByteArray hash = QCryptographicHash::hash("original", QCryptographicHash::Sha256);
        QVERIFY(writeBackEntry(entry, edited, hash).ok); QVERIFY(!writeBackEntry(entry, edited, hash).ok); ZipReader reader(archive); QCOMPARE(reader.fileData(entry.inner), QByteArray("changed"));
    }
    void nestedArchiveCommitReachesOuterContainer() {
        QTemporaryDir root; auto temporary = std::make_shared<QTemporaryDir>(); const QString child = temporary->filePath(QStringLiteral("inner.zip")), outer = root.filePath(QStringLiteral("outer.zip"));
        { ZipWriter writer(child); writer.addFile(QStringLiteral("file.txt"), QByteArray("before")); writer.close(); }
        { ZipWriter writer(outer); writer.addFile(QStringLiteral("inner.zip"), readFile(child)); writer.close(); }
        auto mount = std::make_shared<MountedArchive>(); mount->local = child; mount->temporary = temporary; mount->hash = fileDigest(child); mount->original.source = Source::Archive; mount->original.path = outer; mount->original.inner = mount->original.name = QStringLiteral("inner.zip");
        const QString replacement = root.filePath(QStringLiteral("replace.txt")); QVERIFY(writeFile(replacement, "after")); ArchiveChanges change; change.files.insert(QStringLiteral("file.txt"), replacement); QVERIFY(editArchive(child, change).ok);
        const Result committed = commitArchiveMounts({{child, mount}}); QVERIFY2(committed.ok, qPrintable(committed.error)); ZipReader reader(outer); const QString extracted = root.filePath(QStringLiteral("extracted.zip")); QVERIFY(writeFile(extracted, reader.fileData(QStringLiteral("inner.zip")))); ZipReader inner(extracted); QCOMPARE(inner.fileData(QStringLiteral("file.txt")), QByteArray("after"));
    }
    void archiveCreateOperations() {
        QTemporaryDir root; const QString archive = root.filePath(QStringLiteral("source.zip")); { ZipWriter writer(archive); writer.close(); }
        Location location{Source::Archive, archive, {}, {}}; QVERIFY(createEntry(location, QStringLiteral("empty"), true).ok); QVERIFY(!createEntry(location, QStringLiteral("empty"), true).ok); location.inner = QStringLiteral("empty"); QVERIFY(createEntry(location, QStringLiteral("file.txt"), false).ok);
        ZipReader reader(archive); QCOMPARE(reader.fileData(QStringLiteral("empty/file.txt")), QByteArray()); QCOMPARE(reader.fileInfoList().size(), 2);
    }
    void archiveRewritePreservesDirectoryTimestamp() {
        QTemporaryDir root; const QString path = root.filePath(QStringLiteral("source.zip")); ZipInfo original; original.lastModified = QDateTime(QDate(2020, 3, 4), QTime(12, 30, 0)); original.attributes = (0040755U << 16) | 0x10;
        { ZipWriter writer(path); writer.addDirectory(QStringLiteral("folder"), &original); writer.close(); }
        ArchiveChanges changes; changes.rename.insert(QStringLiteral("folder"), QStringLiteral("renamed")); const auto result = editArchive(path, changes); QVERIFY2(result.ok, qPrintable(result.error)); ZipReader reader(path); QCOMPARE(reader.fileInfoList().size(), 1); QCOMPARE(reader.fileInfoList().first().lastModified, original.lastModified);
    }
    void textEncodingRoundTrip_data() {
        QTest::addColumn<QString>("encoding"); for (const QString &name : {QStringLiteral("UTF-8"), QStringLiteral("UTF-16LE"), QStringLiteral("UTF-16BE"), QStringLiteral("GB18030"), QStringLiteral("GBK"), QStringLiteral("Big5")}) QTest::newRow(qPrintable(name)) << name;
    }
    void textEncodingRoundTrip() {
        QFETCH(QString, encoding); TextDocument input; input.text = QStringLiteral("中文測試\nHello 123"); input.encoding = encoding; input.bom = encoding.startsWith(QStringLiteral("UTF")); QByteArray bytes; QVERIFY(encodeText(input, &bytes).ok); TextDocument decoded; const Result result = decodeText(bytes, encoding, &decoded); QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(decoded.text, input.text); QCOMPARE(decoded.encoding, encoding);
    }
    void textEncodingRefusesLossySave() { TextDocument text; text.text = QString::fromUcs4(U"😀"); text.encoding = QStringLiteral("GBK"); QByteArray bytes; QVERIFY(!encodeText(text, &bytes).ok); }
    void textPreservesWindowsAndMixedLineEndings() {
        TextDocument document; QVERIFY(decodeText("one\r\ntwo\r\n", {}, &document).ok); QCOMPARE(document.newline, QStringLiteral("\r\n"));
        const auto changed = editedText(document, QStringLiteral("one\ntwo\nthree\n")); QByteArray bytes; QVERIFY(encodeText(changed, &bytes).ok); QCOMPARE(bytes, QByteArray("one\r\ntwo\r\nthree\r\n"));
        QVERIFY(decodeText("one\r\ntwo\nthree\r", {}, &document).ok); const auto unchanged = editedText(document, QStringLiteral("one\ntwo\nthree\n")); QVERIFY(encodeText(unchanged, &bytes).ok); QCOMPARE(bytes, QByteArray("one\r\ntwo\nthree\r"));
    }
    void textReplacementBoundsBeforeAllocation() {
        QCOMPARE(replaceTextBounded(QStringLiteral("a1 b2"), QStringLiteral("([ab])([12])"), QStringLiteral("\\2\\1"), true, true).output, QStringLiteral("1a 2b"));
        QVERIFY(!replaceTextBounded(QStringLiteral("aaaa"), QStringLiteral("a"), QString(1000, QChar('x')), false, true, 100).ok);
        QCOMPARE(replaceTextBounded(QStringLiteral("Aa"), QStringLiteral("a"), QStringLiteral("\\1"), false, false).output, QStringLiteral("\\1\\1"));
    }
    void legacySettingsImportPreservesOldSchemaWithoutWritingFiles() {
        QTemporaryDir root; const QJsonObject node{{QStringLiteral("kind"), QStringLiteral("dir")}, {QStringLiteral("path"), root.path()}, {QStringLiteral("inner"), QString()}};
        const QJsonObject legacy{{QStringLiteral("theme"), QStringLiteral("cream")}, {QStringLiteral("accent"), QStringLiteral("teal")}, {QStringLiteral("desc"), true}, {QStringLiteral("player_order"), QStringLiteral("shuffle")}, {QStringLiteral("workspace"), QJsonObject{{QStringLiteral("left"), QJsonObject{{QStringLiteral("tabs"), QJsonArray{QJsonArray{node}}}, {QStringLiteral("current"), 0}}}, {QStringLiteral("active"), QStringLiteral("right")}}}};
        const auto result = migrateLegacySettings(legacy); QCOMPARE(result.value(QStringLiteral("theme")).toString(), QStringLiteral("cream")); QVERIFY(result.value(QStringLiteral("descending")).toBool()); QCOMPARE(result.value(QStringLiteral("player_order")).toInt(), 2); const auto workspace = result.value(QStringLiteral("workspace")).toObject(); const auto first = workspace.value(QStringLiteral("left")).toObject().value(QStringLiteral("tabs")).toArray().first().toObject(); QCOMPARE(Location::fromJson(first).path, root.path()); QCOMPARE(legacy.value(QStringLiteral("desc")).toBool(), true);
    }
    void batchPlanQuotesSpecialCharactersWithoutExecutingScript() {
        QTemporaryDir root; const QString path = root.filePath(QStringLiteral("测试 & %TEMP% !.bat")); QVERIFY(writeFile(path, "@echo off\r\necho fixture\r\n"));
        const BatchPlan plan = batchPlan(path); QVERIFY(!plan.program.isEmpty()); QVERIFY(plan.command.contains(QStringLiteral("/v:off"))); QVERIFY(!plan.command.contains(QStringLiteral("TEMP"))); QCOMPARE(plan.environment.value(QStringLiteral("TWINFILE_BATCH")), QDir::toNativeSeparators(path)); QCOMPARE(plan.directory, root.path()); QVERIFY(plan.environment.value(QStringLiteral("PATH")).startsWith(QDir::toNativeSeparators(QFileInfo(toolPath(QStringLiteral("adb/adb.exe"))).absolutePath()))); QVERIFY(batchPlan(root.filePath(QStringLiteral("missing.bat"))).program.isEmpty());
    }
    void storedZipAlignmentAndRewrite() {
        QTemporaryDir root; const QString path = root.filePath(QStringLiteral("aligned.zip")); ZipInfo info; info.compression = 0;
        { ZipWriter writer(path); QBuffer input; input.setData(QByteArray(123, 'a')); input.open(QIODevice::ReadOnly); writer.addFile(QStringLiteral("assets/data"), &input, {}, &info); input.seek(0); writer.addFile(QStringLiteral("lib/arm64-v8a/libtest.so"), &input, {}, &info); writer.close(); QVERIFY(writer.status() == ZipWriter::NoError); }
        QVERIFY(editArchive(path, {}).ok); const QByteArray bytes = readFile(path); qsizetype offset = 0; int checked = 0;
        while ((offset = bytes.indexOf(QByteArray("PK\3\4", 4), offset)) >= 0) { const auto nameSize = qFromLittleEndian<quint16>(bytes.constData() + offset + 26), extraSize = qFromLittleEndian<quint16>(bytes.constData() + offset + 28); const QByteArray name = bytes.mid(offset + 30, nameSize); const qsizetype data = offset + 30 + nameSize + extraSize; QCOMPARE(data % (name.endsWith(".so") ? 16384 : 4), 0); ++checked; ++offset; }
        QCOMPARE(checked, 2); ZipReader reader(path); for (const auto &file : reader.fileInfoList()) QCOMPARE(file.compression, 0);
    }
    void binaryManifestRewritePreservesComponents() {
        ZipReader archive(toolPath(QStringLiteral("hfx/HybridFileXfer-phone.apk"))); const QByteArray original = archive.fileData(QStringLiteral("AndroidManifest.xml")); QString error; const auto before = androidXml(original, &error); QVERIFY2(!before.isNull(), qPrintable(error));
        const QMap<QString, QString> fields{{QStringLiteral("package"), QStringLiteral("org.twinfile.test")}, {QStringLiteral("versionCode"), QStringLiteral("123")}, {QStringLiteral("versionName"), QStringLiteral("测试😀")}, {QStringLiteral("label"), QStringLiteral("测试标签")}};
        const QByteArray patched = patchAndroidManifest(original, fields, &error); QVERIFY2(!patched.isEmpty(), qPrintable(error)); const auto after = androidXml(patched, &error); QVERIFY(!after.isNull()); QCOMPARE(after.documentElement().attribute(QStringLiteral("package")), QStringLiteral("org.twinfile.test"));
        const auto activities = after.elementsByTagName(QStringLiteral("activity")); QVERIFY(!activities.isEmpty());
        QVERIFY(patchAndroidManifest(original, {{QStringLiteral("package"), QStringLiteral("invalid/package")}}, &error).isEmpty()); QVERIFY(patchAndroidManifest(original, {{QStringLiteral("versionCode"), QStringLiteral("4294967296")}}, &error).isEmpty()); QVERIFY(patchAndroidManifest(original.left(100), fields, &error).isEmpty());
    }
    void drawableInlineGradientAndAlpha() {
        QTemporaryDir root; const QString path = root.filePath(QStringLiteral("drawable.zip")); { ZipWriter writer(path); writer.close(); }
        ZipReader reader(path);
        const QString xml = QStringLiteral("<vector xmlns:android=\"http://schemas.android.com/apk/res/android\" xmlns:aapt=\"http://schemas.android.com/aapt\" android:viewportWidth=\"24\" android:viewportHeight=\"24\"><path android:pathData=\"M0,0H24V24H0Z\"><aapt:attr name=\"android:fillColor\"><gradient android:startX=\"0\" android:startY=\"0\" android:endX=\"24\" android:endY=\"0\" android:startColor=\"#ff0000\" android:endColor=\"#0000ff\"/></aapt:attr></path></vector>");
        const QImage image = androidDrawable(reader, {}, xml, 128); QVERIFY(!image.isNull()); QVERIFY(image.pixelColor(10, 64).red() > image.pixelColor(110, 64).red()); QVERIFY(image.pixelColor(110, 64).blue() > image.pixelColor(10, 64).blue());
        const QImage inlineLayer = androidDrawable(reader, {}, QStringLiteral("<adaptive-icon><background android:drawable=\"#ffffff\" xmlns:android=\"http://schemas.android.com/apk/res/android\"/><foreground>") + xml + QStringLiteral("</foreground></adaptive-icon>")); QVERIFY(!inlineLayer.isNull());
    }
    void textManifestQualifiesRelativeClassesAndRejectsInvalidPackage() {
        const QByteArray xml("<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"org.old.app\"><application android:name=\"App\"><activity android:name=\".Main\"/><activity-alias android:name=\".Alias\" android:targetActivity=\".Main\"/></application></manifest>");
        QString error; const QByteArray result = patchAndroidManifest(xml, {{QStringLiteral("package"), QStringLiteral("org.new.app")}}, &error); QVERIFY2(!result.isEmpty(), qPrintable(error)); QVERIFY(result.contains("org.old.app.Main")); QVERIFY(result.contains("org.old.app.App")); QVERIFY(patchAndroidManifest(xml, {{QStringLiteral("package"), QStringLiteral("bad")}}, &error).isEmpty());
    }
    void projectRepairIsDeterministicAndKeepsOriginal() {
        const QString xml = QStringLiteral("<view a=\"1\" a=\"2\"><坏类 b=\"3\"/></view>"); const QString repaired = repairResourceXml(xml, false); QVERIFY(repaired.contains(QStringLiteral("MTClass"))); QVERIFY(!repaired.contains(QStringLiteral("a=\"2\""))); QCOMPARE(repairResourceXml(repaired, false), repaired);
        QTemporaryDir root; const QString project = root.filePath(QStringLiteral("original")), output = root.filePath(QStringLiteral("working")); QVERIFY(QDir().mkpath(project + QStringLiteral("/res/layout"))); QVERIFY(writeFile(project + QStringLiteral("/apktool.yml"), "versionInfo:\n  versionCode: 1\n  versionName: old\n"));
        const QByteArray manifest("<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"org.old.app\"><application android:name=\"App\" android:label=\"old\"/></manifest>"); QVERIFY(writeFile(project + QStringLiteral("/AndroidManifest.xml"), manifest)); QVERIFY(writeFile(project + QStringLiteral("/res/layout/main.xml"), xml.toUtf8()));
        const Result result = prepareApkProject(project, output, {{QStringLiteral("package"), QStringLiteral("org.new.app")}, {QStringLiteral("versionName"), QStringLiteral("new's")}}); QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(readFile(project + QStringLiteral("/AndroidManifest.xml")), manifest); QVERIFY(readFile(output + QStringLiteral("/AndroidManifest.xml")).contains("org.old.app.App")); QVERIFY(readFile(output + QStringLiteral("/apktool.yml")).contains("new''s"));
    }
    void signingAndQuickEditUseIsolatedKeyAndPreserveInput() {
        QTemporaryDir root; const QByteArray previous = qgetenv("TWINFILE_SETTINGS"); qputenv("TWINFILE_SETTINGS", root.filePath(QStringLiteral("settings.json")).toUtf8());
        const QString source = toolPath(QStringLiteral("hfx/HybridFileXfer-phone.apk")), output = root.filePath(QStringLiteral("edited.apk")); const QByteArray hash = fileDigest(source);
        const Result result = quickEditApk(source, output, {{QStringLiteral("label"), QStringLiteral("TwinFile Test")}});
        if (previous.isEmpty()) qunsetenv("TWINFILE_SETTINGS"); else qputenv("TWINFILE_SETTINGS", previous);
        QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(fileDigest(source), hash); QVERIFY(QFileInfo::exists(root.filePath(QStringLiteral("debug.keystore")))); QVERIFY(QFileInfo::exists(output)); QVERIFY(!quickEditApk(source, output, {}).ok);
    }
    void projectIconPreservesAdaptiveBackgroundInClone() {
        QTemporaryDir root; const QString project = root.filePath(QStringLiteral("source")); QVERIFY(QDir().mkpath(project + QStringLiteral("/res/mipmap-anydpi-v26")));
        QVERIFY(writeFile(project + QStringLiteral("/apktool.yml"), "versionInfo:\n  versionCode: 1\n"));
        QVERIFY(writeFile(project + QStringLiteral("/AndroidManifest.xml"), "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"org.test.app\"><application android:icon=\"@mipmap/launcher\"/></manifest>"));
        const QByteArray adaptive("<adaptive-icon xmlns:android=\"http://schemas.android.com/apk/res/android\"><background android:drawable=\"#ff123456\"/><foreground android:drawable=\"@drawable/old\"/></adaptive-icon>");
        QVERIFY(writeFile(project + QStringLiteral("/res/mipmap-anydpi-v26/launcher.xml"), adaptive)); const QString icon = root.filePath(QStringLiteral("new.png")); QImage image(100, 60, QImage::Format_ARGB32); image.fill(Qt::red); QVERIFY(image.save(icon));
        const QString output = root.filePath(QStringLiteral("working")); const Result result = prepareApkProject(project, output, {}, icon); QVERIFY2(result.ok, qPrintable(result.error));
        QString error; const auto manifest = androidXml(readFile(output + QStringLiteral("/AndroidManifest.xml")), &error); const QString reference = manifest.documentElement().firstChildElement(QStringLiteral("application")).attribute(QStringLiteral("android:icon")); QVERIFY(reference.startsWith(QStringLiteral("@mipmap/twinfile_icon_")));
        const QString name = reference.mid(8); const auto document = androidXml(readFile(output + QStringLiteral("/res/mipmap-anydpi-v26/") + name + QStringLiteral(".xml")), &error); QVERIFY2(!document.isNull(), qPrintable(error)); QCOMPARE(document.documentElement().firstChildElement(QStringLiteral("background")).attribute(QStringLiteral("android:drawable")), QStringLiteral("#ff123456")); QVERIFY(document.toString().contains(QStringLiteral("_foreground"))); QCOMPARE(readFile(project + QStringLiteral("/res/mipmap-anydpi-v26/launcher.xml")), adaptive);
    }
    void checksumManifestRoundTripAndMismatch() {
        QTemporaryDir root; const QString file = root.filePath(QStringLiteral("中文.txt")), manifest = root.filePath(QStringLiteral("files.sha256")); QVERIFY(writeFile(file, "fixture"));
        const Result exported = checksumManifest({file}, root.path(), QCryptographicHash::Sha256); QVERIFY(exported.ok); QVERIFY(writeFile(manifest, exported.output.toUtf8())); const Result passed = verifyChecksumManifest(manifest, root.path()); QVERIFY(passed.ok); QVERIFY(passed.output.contains(QStringLiteral("通过 1 / 1"))); QVERIFY(writeFile(file, "modified")); const Result mismatch = verifyChecksumManifest(manifest, root.path()); QVERIFY(mismatch.ok); QVERIFY(mismatch.output.contains(QStringLiteral("不一致")));
        QVERIFY(writeFile(manifest, QByteArray(64, 'a') + " *../escape\n")); QVERIFY(!verifyChecksumManifest(manifest, root.path()).ok);
    }
    void directoryCompareIncludesEmptyDirectoriesTypesAndContents() {
        QTemporaryDir left, right; QVERIFY(QDir().mkpath(left.filePath(QStringLiteral("empty")))); QVERIFY(QDir().mkpath(left.filePath(QStringLiteral("kind")))); QVERIFY(writeFile(right.filePath(QStringLiteral("kind")), "")); QVERIFY(writeFile(left.filePath(QStringLiteral("same")), "a")); QVERIFY(writeFile(right.filePath(QStringLiteral("same")), "b"));
        const Result result = compareDirectories(left.path(), right.path(), 2); QVERIFY(result.ok); QVERIFY(result.output.contains(QStringLiteral("仅左侧：empty"))); QVERIFY(result.output.contains(QStringLiteral("类型不同：kind"))); QVERIFY(result.output.contains(QStringLiteral("内容不同：same"))); QVERIFY(!compareDirectories(left.path(), right.path(), 2, std::make_shared<std::atomic_bool>(true)).ok);
    }
    void lyricsAndPlaybackModes() {
        const auto lines = parseLyrics(QStringLiteral("[offset:100]\n[00:01.20][00:03:500]<0,200>你<200,300>好\n[00:99.00]invalid")); QCOMPARE(lines.size(), 2); QCOMPARE(lines[0].time, 1300); QCOMPARE(lines[0].text, QStringLiteral("你好")); QCOMPARE(lines[0].words.size(), 2); QCOMPARE(lines[0].words[1].start, 1500); QCOMPARE(lines[1].time, 3600);
        QCOMPARE(mediaIndex(2, 3, 0), 0); QCOMPARE(mediaIndex(0, 3, 0, false), 2); QCOMPARE(mediaIndex(1, 3, 1), 1); QCOMPARE(mediaIndex(0, 0, 0), -1); for (int index = 0; index < 30; ++index) QVERIFY(mediaIndex(1, 3, 2) != 1); QVERIFY(isMedia(QStringLiteral("a.MKA"))); QVERIFY(isMedia(QStringLiteral("a.3g2"))); QVERIFY(!isMedia(QStringLiteral("a.bin")));
    }
    void embeddedFlacAndId3LyricsAreBounded() {
        QTemporaryDir root; const QString flac = root.filePath(QStringLiteral("test.flac")); const QByteArray comment("LYRICS=[00:01.00]hello"); const QByteArray vorbis = little(0) + little(1) + little(quint32(comment.size())) + comment;
        QByteArray header(4, '\0'); header[0] = char(0x84); header[1] = char((vorbis.size() >> 16) & 255); header[2] = char((vorbis.size() >> 8) & 255); header[3] = char(vorbis.size() & 255); QVERIFY(writeFile(flac, QByteArray("fLaC") + header + vorbis)); QCOMPARE(readMediaTags(flac).lyrics, QStringLiteral("[00:01.00]hello"));
        const QString mp3 = root.filePath(QStringLiteral("test.mp3")); const QByteArray body = QByteArray(1, char(3)) + "eng" + QByteArray(1, '\0') + "[00:02.00]world"; const QByteArray frame = QByteArray("USLT") + network<quint32>(quint32(body.size())) + QByteArray(2, '\0') + body; QByteArray id3("ID3\3\0\0", 6); id3 += QByteArray(3, '\0') + char(frame.size()); QVERIFY(writeFile(mp3, id3 + frame)); QCOMPARE(readMediaTags(mp3).lyrics, QStringLiteral("[00:02.00]world"));
        QVERIFY(writeFile(mp3, QByteArray("ID3\4\0\0\x7f\x7f\x7f\x7f", 10))); QVERIFY(readMediaTags(mp3).lyrics.isEmpty());
    }
    void remoteMediaPlaylistKeepsSelectedEntryAndBoundsSize() {
        QList<Entry> entries;
        for (int index = 0; index < 5010; ++index) { Entry entry; entry.name = QStringLiteral("track.mp3"); entry.path = QStringLiteral("/sdcard/%1/track.mp3").arg(index); entry.source = Source::Phone; entry.serial = QStringLiteral("fixture"); entries.append(entry); }
        const auto playlist = mediaPlaylist(entries, entries.last()); QCOMPARE(playlist.size(), 5000); QCOMPARE(playlist.last().key(), entries.last().key()); QSet<QString> keys; for (const auto &entry : playlist) keys.insert(entry.key()); QCOMPARE(keys.size(), playlist.size());
    }
    void phoneDirectoryCanBeCopiedIntoArchive() {
        FakeAdb fake; fake.directories.append(QStringLiteral("/sdcard/folder")); fake.files.insert(QStringLiteral("/sdcard/folder/中文.txt"), "folder-content"); QTemporaryDir root; const QString archive = root.filePath(QStringLiteral("target.zip")); { ZipWriter writer(archive); writer.close(); }
        Entry entry; entry.source = Source::Phone; entry.serial = QStringLiteral("fixture"); entry.path = QStringLiteral("/sdcard/folder"); entry.name = QStringLiteral("folder"); entry.directory = true;
        auto task = qtAsync([&] { return transferEntries({entry}, {Source::Archive, archive, {}, {}}, false); }); QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 10000); const Result result = task.get(); QVERIFY2(result.ok, qPrintable(result.error)); ZipReader reader(archive); QCOMPARE(reader.fileData(QStringLiteral("folder/中文.txt")), QByteArray("folder-content"));
    }
    void adbNativeSyncListsAndCopiesUnicodeFile_data() { QTest::addColumn<bool>("version2"); QTest::newRow("v1") << false; QTest::newRow("v2") << true; }
    void adbNativeSyncListsAndCopiesUnicodeFile() {
        QFETCH(bool, version2); FakeAdb fake; fake.version2 = version2; fake.files.insert(QStringLiteral("/sdcard/中文.txt"), QByteArray(200000, 'q')); fake.directories.append(QStringLiteral("/sdcard")); QTemporaryDir root;
        auto listing = qtAsync([&] { return adbSyncList(QStringLiteral("fixture"), QStringLiteral("/sdcard"), true); }); QTRY_VERIFY_WITH_TIMEOUT(listing.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000); const Listing listed = listing.get(); QVERIFY2(listed.error.isEmpty(), qPrintable(listed.error)); QCOMPARE(listed.entries.size(), 1); QCOMPARE(listed.entries[0].name, QStringLiteral("中文.txt"));
        qint64 previous = 0, reportedTotal = 0; int reports = 0; bool ordered = true;
        const Progress progress = [&](qint64 done, qint64 total, const QString &) { ordered &= done >= previous; previous = done; reportedTotal = total; ++reports; };
        const QString local = root.filePath(QStringLiteral("local.txt")); auto pull = qtAsync([&] { return adbSyncPull(QStringLiteral("fixture"), QStringLiteral("/sdcard/中文.txt"), local, {}, progress); }); QTRY_VERIFY_WITH_TIMEOUT(pull.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000); const Result pulled = pull.get(); QVERIFY2(pulled.ok, qPrintable(pulled.error)); QCOMPARE(readFile(local), QByteArray(200000, 'q'));
        QVERIFY(ordered); QCOMPARE(previous, 200000); QCOMPARE(reportedTotal, version2 ? 200000 : 0); QVERIFY(reports >= 4);
        previous = 0; reports = 0;
        auto push = qtAsync([&] { return adbSyncPush(QStringLiteral("fixture"), local, QStringLiteral("/sdcard/new.txt"), {}, progress); }); QTRY_VERIFY_WITH_TIMEOUT(push.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000); const Result pushed = push.get(); QVERIFY2(pushed.ok, qPrintable(pushed.error)); QCOMPARE(fake.files.value(QStringLiteral("/sdcard/new.txt")), QByteArray(200000, 'q'));
        QVERIFY(ordered); QCOMPARE(previous, 200000); QCOMPARE(reportedTotal, 200000); QVERIFY(reports >= 4);
        Entry entry; entry.source = Source::Phone; entry.path = QStringLiteral("/sdcard/中文.txt"); entry.serial = QStringLiteral("fixture"); auto hashing = qtAsync([&] { return checksumEntries({entry}); }); QTRY_VERIFY_WITH_TIMEOUT(hashing.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000); const Result hashed = hashing.get(); QVERIFY2(hashed.ok, qPrintable(hashed.error)); QVERIFY(hashed.output.contains(QString::fromLatin1(QCryptographicHash::hash(QByteArray(200000, 'q'), QCryptographicHash::Sha256).toHex())));
    }
    void realApktoolProjectRebuildAndSign() {
        QTemporaryDir root; const QString project = root.filePath(QStringLiteral("decoded"));
        const Result decoded = runProcess(toolPath(QStringLiteral("runtime/bin/java.exe")), {QStringLiteral("-Xmx2g"), QStringLiteral("-jar"), toolPath(QStringLiteral("apktool.jar")), QStringLiteral("d"), QStringLiteral("-s"), QStringLiteral("-p"), root.filePath(QStringLiteral("framework")), toolPath(QStringLiteral("hfx/HybridFileXfer-phone.apk")), QStringLiteral("-o"), project}, 120000);
        QVERIFY2(decoded.ok, qPrintable(decoded.error)); const QByteArray before = fileDigest(project + QStringLiteral("/AndroidManifest.xml"));
        const QByteArray previous = qgetenv("TWINFILE_SETTINGS"); qputenv("TWINFILE_SETTINGS", root.filePath(QStringLiteral("settings.json")).toUtf8());
        const QString output = root.filePath(QStringLiteral("built.apk")); const Result built = buildApk(project, output, true, {}, {{QStringLiteral("versionName"), QStringLiteral("native-regression")}});
        if (previous.isEmpty()) qunsetenv("TWINFILE_SETTINGS"); else qputenv("TWINFILE_SETTINGS", previous);
        QVERIFY2(built.ok, qPrintable(built.error)); QCOMPARE(fileDigest(project + QStringLiteral("/AndroidManifest.xml")), before); QVERIFY(QFileInfo::exists(output));
    }
    void adbNativeSyncRejectsMalformedFrameAndCancellation() {
        FakeAdb fake; fake.files.insert(QStringLiteral("/sdcard/file"), "data"); fake.badReceive = true; QTemporaryDir root;
        auto task = qtAsync([&] { return adbSyncPull(QStringLiteral("fixture"), QStringLiteral("/sdcard/file"), root.filePath(QStringLiteral("file"))); }); QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000); QVERIFY(!task.get().ok);
        const Result cancelled = adbSyncPull(QStringLiteral("fixture"), QStringLiteral("/sdcard/file"), root.filePath(QStringLiteral("cancelled")), std::make_shared<std::atomic_bool>(true)); QVERIFY(!cancelled.ok); QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("cancelled"))));
    }
    void textDiffAlignsInsertedLines() { const QStringList diff = textDifference(QStringLiteral("a\nb\nc"), QStringLiteral("a\ninserted\nb\nc")); QVERIFY(diff.contains(QStringLiteral("+ inserted"))); QVERIFY(!diff.contains(QStringLiteral("- b"))); QVERIFY(!diff.contains(QStringLiteral("- c"))); }
    void unifiedDifferencePreservesLineNumbersAndIgnoreModes() {
        const QString left = QStringLiteral("first\n\n second  \nlast"), right = QStringLiteral("first\nsecond\ninserted\nlast");
        const auto result = alignedDifference(left, right, 3); QVERIFY(result.error.isEmpty()); QList<int> leftLines, rightLines; int additions = 0, removals = 0;
        for (const auto &row : result.rows) { if (row.left) leftLines.append(row.left); if (row.right) rightLines.append(row.right); additions += row.kind > 0; removals += row.kind < 0; }
        QCOMPARE(leftLines, QList<int>({1, 2, 3, 4})); QCOMPARE(rightLines, QList<int>({1, 2, 3, 4})); QCOMPARE(additions, 1); QCOMPARE(removals, 0);
        const auto trimmed = alignedDifference(QStringLiteral("  text\n"), QStringLiteral("text\n"), 1); for (const auto &row : trimmed.rows) QCOMPARE(row.kind, 0);
    }
    void textDifferenceMarksChangedCharactersWithoutSplittingEmoji() {
        const auto result = alignedDifference(QStringLiteral("prefix old suffix"), QStringLiteral("prefix new suffix"), 0); QCOMPARE(result.rows.size(), 2); QCOMPARE(result.rows[0].changedStart, 7); QCOMPARE(result.rows[0].changedLength, 3); QCOMPARE(result.rows[1].changedLength, 3);
        const auto emoji = alignedDifference(QString::fromUcs4(U"a😀z"), QString::fromUcs4(U"a😁z"), 0); QCOMPARE(emoji.rows[0].changedStart, 1); QCOMPARE(emoji.rows[0].changedLength, 2);
    }
    void archiveHashMatchesLocalHashWithoutExtraction() {
        QTemporaryDir root; const QString archive = root.filePath(QStringLiteral("hash.zip")); { ZipWriter writer(archive); writer.addFile(QStringLiteral("file"), QByteArray("abc")); writer.close(); }
        Entry entry; entry.source = Source::Archive; entry.path = archive; entry.inner = entry.name = QStringLiteral("file"); const Result result = checksumEntries({entry}); QVERIFY(result.ok); QVERIFY(result.output.contains(QString::fromLatin1(QCryptographicHash::hash("abc", QCryptographicHash::Sha256).toHex())));
    }
    void pastedChecksumSupportsPrefixesAndRejectsAmbiguity() {
        QTemporaryDir root; const QString path = root.filePath(QStringLiteral("test.txt")); QVERIFY(writeFile(path, "abc")); Entry entry; entry.path = path;
        const QString expected = QString::fromLatin1(QCryptographicHash::hash("abc", QCryptographicHash::Sha256).toHex());
        QCOMPARE(normalizedChecksum(QStringLiteral("SHA256 (test.txt) = ") + expected.toUpper()), expected.toLatin1());
        QVERIFY(normalizedChecksum(expected + QChar('a')).isEmpty()); QVERIFY(normalizedChecksum(expected + QChar('\n') + QString(32, QChar('0'))).isEmpty());
        QVERIFY(checksumEntries({entry}, {}, expected).output.contains(QStringLiteral("校验通过")));
        QVERIFY(checksumEntries({entry}, {}, QString(64, QChar('0'))).output.contains(QStringLiteral("校验失败")));
        QVERIFY(!checksumEntries({entry, entry}, {}, expected).ok);
    }
    void archiveSearchPreservesSourceAndHiddenFilter() {
        QTemporaryDir root; const QString path = root.filePath(QStringLiteral("search.zip")); { ZipWriter writer(path); writer.addFile(QStringLiteral("folder/hello.txt"), QByteArray("a")); writer.addFile(QStringLiteral("folder/.hidden/hello.txt"), QByteArray("b")); writer.close(); }
        const Location location{Source::Archive, path, QStringLiteral("folder"), {}}; const auto visible = searchLocation(location, QStringLiteral("hello"), false, {}); QVERIFY(visible.error.isEmpty()); QCOMPARE(visible.entries.size(), 1); QCOMPARE(visible.entries.first().inner, QStringLiteral("folder/hello.txt")); QCOMPARE(searchLocation(location, QStringLiteral("hello"), true, {}).entries.size(), 2); QVERIFY(!searchLocation(location, QStringLiteral("hello"), true, std::make_shared<std::atomic_bool>(true)).error.isEmpty());
    }
    void payloadCompression_data() { QTest::addColumn<int>("type"); for (int type : {0, 1, 6, 7, 8, 14}) QTest::newRow(qPrintable(QString::number(type))) << type; }
    void payloadCompression() {
        QFETCH(int, type); QTemporaryDir root; const QByteArray data(4096 * 30, 'a'); const QString source = root.filePath(QStringLiteral("payload.bin")); QVERIFY(writeFile(source, encodePayload(data, type))); const Result info = inspectPayload(source); QVERIFY2(info.ok, qPrintable(info.error)); QCOMPARE(info.paths, QStringList{QStringLiteral("system")});
        const Result extracted = extractPayload(source, info.paths, root.path()); QVERIFY2(extracted.ok, qPrintable(extracted.error)); QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), type == 6 || type == 7 ? QByteArray(data.size(), '\0') : data); QVERIFY(!extractPayload(source, info.paths, root.path()).ok);
    }
    void payloadRejectsDeltaAndCorruption() {
        QTemporaryDir root; const QString source = root.filePath(QStringLiteral("payload.bin")); QVERIFY(writeFile(source, encodePayload(QByteArray(4096, 'a'), 2))); QVERIFY(!inspectPayload(source).ok);
        QVERIFY(writeFile(source, encodePayload(QByteArray(4096, 'a'), 0, true))); QVERIFY(!extractPayload(source, {QStringLiteral("system")}, root.path()).ok); QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("system.img"))));
        QVERIFY(!extractPayload(source, {QStringLiteral("unknown")}, root.path()).ok);
    }
    void payloadInsideDeflatedZip() {
        QTemporaryDir root; const QString archive = root.filePath(QStringLiteral("ota.zip")); const QByteArray data(4096, 'z'); { ZipWriter writer(archive); writer.addFile(QStringLiteral("payload.bin"), encodePayload(data)); writer.close(); }
        const Result result = extractPayload(archive, {QStringLiteral("system")}, root.path()); QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), data);
    }
    void payloadDeflatedInspectionReadsOnlyMetadata() {
        QTemporaryDir root; QByteArray data(8 * 1024 * 1024, '\0'); QRandomGenerator random(77);
        for (qsizetype index = 0; index < data.size(); index += 4) qToLittleEndian<quint32>(random.generate(), data.data() + index);
        const QString archive = root.filePath(QStringLiteral("ota.zip"));
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("payload.bin"), encodePayload(data)); writer.close(); }
        PayloadHttpFixture fixture(readFile(archive)); QVERIFY(fixture.server.isListening()); QVERIFY(fixture.bytes.size() > 8 * 1024 * 1024);
        QStringList stages; auto task = qtAsync([&] { return inspectPayload(fixture.url(), {}, {}, [&](qint64, qint64, const QString &stage) { stages.append(stage); }); });
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000);
        const Result result = task.get(); QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(result.paths, QStringList{QStringLiteral("system")});
        QVERIFY2(fixture.transferred < 256 * 1024, qPrintable(QString::number(fixture.transferred))); QVERIFY(!stages.isEmpty());
        const Result extracted = extractPayload(archive, result.paths, root.path()); QVERIFY2(extracted.ok, qPrintable(extracted.error)); QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), data);
    }
    void payloadDeflatedInspectionRejectsInvalidMetadataAndExtractionChecksCrc() {
        QTemporaryDir root; const QString archive = root.filePath(QStringLiteral("ota.zip"));
        QByteArray payload = encodePayload(QByteArray(4096, 'a')); qToBigEndian<quint64>(65ULL * 1024 * 1024, payload.data() + 12);
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("payload.bin"), payload); writer.close(); }
        QVERIFY(!inspectPayload(archive).ok);
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("payload.bin"), encodePayload(QByteArray(4096, 'a'))); writer.close(); }
        QByteArray corrupt = readFile(archive); const qsizetype directory = corrupt.indexOf(QByteArray("PK\x01\x02", 4)); QVERIFY(directory >= 0); corrupt[directory + 16] ^= 1; QVERIFY(writeFile(archive, corrupt));
        QVERIFY(inspectPayload(archive).ok); const Result result = extractPayload(archive, {QStringLiteral("system")}, root.path());
        QVERIFY(!result.ok); QVERIFY(result.error.contains(QStringLiteral("CRC"))); QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("system.img"))));
    }
    void payloadDeflatedLargeManifestAndSignature() {
        QTemporaryDir root; const QByteArray data(4096, 's'); const QByteArray original = encodePayload(data);
        const qsizetype originalSize = qsizetype(qFromBigEndian<quint64>(original.constData() + 12));
        QByteArray padding(256 * 1024, '\0'); QRandomGenerator random(123);
        for (qsizetype index = 0; index < padding.size(); index += 4) qToLittleEndian<quint32>(random.generate(), padding.data() + index);
        const QByteArray manifest = original.mid(24, originalSize) + message(100, padding);
        const QByteArray signature(128, 'x');
        const QByteArray payload = QByteArray("CrAU") + network<quint64>(2) + network<quint64>(quint64(manifest.size())) + network<quint32>(quint32(signature.size())) + manifest + signature + original.mid(24 + originalSize);
        const QString archive = root.filePath(QStringLiteral("ota.zip"));
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("payload.bin"), payload); writer.close(); }
        PayloadHttpFixture fixture(readFile(archive)); QVERIFY(fixture.bytes.size() > padding.size());
        auto task = qtAsync([&] { return inspectPayload(fixture.url()); });
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000);
        const Result info = task.get(); QVERIFY2(info.ok, qPrintable(info.error)); QCOMPARE(info.paths, QStringList{QStringLiteral("system")});
        const Result result = extractPayload(archive, info.paths, root.path()); QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), data);
    }
    void payloadNetworkDeadlines_data() {
        QTest::addColumn<int>("mode"); QTest::addColumn<bool>("idle");
        QTest::newRow("no-response") << int(PayloadHttpFixture::Silent) << false;
        QTest::newRow("idle-timeout") << int(PayloadHttpFixture::Silent) << true;
        QTest::newRow("slow-trickle") << int(PayloadHttpFixture::Trickle) << false;
        QTest::newRow("ignored-range-headers") << int(PayloadHttpFixture::IgnoredRange) << false;
    }
    void payloadNetworkDeadlines() {
        QFETCH(int, mode); QFETCH(bool, idle); PayloadHttpFixture fixture({}, PayloadHttpFixture::Mode(mode)); PayloadOptions options; options.idleTimeoutMs = idle ? 140 : 250; options.requestTimeoutMs = idle ? 2000 : 140;
        auto task = qtAsync([&] { return inspectPayload(fixture.url(), {}, options); });
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 3000);
        const Result result = task.get(); QVERIFY(!result.ok); QCOMPARE(fixture.requests, mode == PayloadHttpFixture::IgnoredRange ? 1 : 3);
        QVERIFY2(result.error.contains(mode == PayloadHttpFixture::IgnoredRange ? QStringLiteral("Range") : QStringLiteral("超时")), qPrintable(result.error));
    }
    void payloadCancelDuringNetworkAndManifest() {
        PayloadHttpFixture fixture({}, PayloadHttpFixture::Silent); auto cancel = std::make_shared<std::atomic_bool>(false);
        auto task = qtAsync([&] { return inspectPayload(fixture.url(), cancel); });
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests, 1, 2000); cancel->store(true);
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 1000);
        QVERIFY(task.get().error.contains(QStringLiteral("取消"))); QCOMPARE(fixture.requests, 1);
        const QByteArray payload = encodePayload(QByteArray(4096, 'a')); PayloadManifest manifest;
        QVERIFY(parsePayloadManifest(payload.mid(24, qsizetype(qFromBigEndian<quint64>(payload.constData() + 12))), &manifest, cancel).error.contains(QStringLiteral("取消")));
    }
    void payloadThreadCountControlsConcurrency_data() {
        QTest::addColumn<int>("threads"); QTest::addColumn<int>("connections"); QTest::addColumn<int>("peak");
        QTest::newRow("one-partition-worker") << 1 << 16 << 1;
        QTest::newRow("two-partition-workers") << 2 << 16 << 2;
        QTest::newRow("shared-one-connection") << 3 << 1 << 1;
        QTest::newRow("shared-two-connections") << 3 << 2 << 2;
    }
    void payloadThreadCountControlsConcurrency() {
        QFETCH(int, threads); QFETCH(int, connections); QFETCH(int, peak); const QByteArray data(4096, 't'); QByteArray manifest = integer(3, 4096);
        const QStringList names{QStringLiteral("system"), QStringLiteral("vendor"), QStringLiteral("boot")};
        for (int index = 0; index < names.size(); ++index) {
            const QByteArray operation = integer(1, 0) + integer(2, quint64(index * data.size())) + integer(3, quint64(data.size())) + message(6, integer(1, 0) + integer(2, 1));
            manifest += message(13, message(1, names[index].toUtf8()) + message(7, integer(1, quint64(data.size()))) + message(8, operation));
        }
        PayloadHttpFixture fixture(QByteArray("CrAU") + network<quint64>(2) + network<quint64>(quint64(manifest.size())) + network<quint32>(0) + manifest + data.repeated(3));
        fixture.delayedFrom = 24 + manifest.size(); QTemporaryDir root; qint64 previous = 0, reportedTotal = 0; bool monotonic = true;
        PayloadOptions options; options.downloadConnections = connections;
        auto task = qtAsync([&] {
            return extractPayload(fixture.url(), names, root.path(), threads, {}, [&](qint64 done, qint64 total, const QString &stage) {
                if (!stage.startsWith(QStringLiteral("提取分区"))) return;
                monotonic &= done >= previous; previous = done; reportedTotal = total;
            }, options);
        });
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000);
        const Result result = task.get(); QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(fixture.peak, peak); QVERIFY(monotonic); QCOMPARE(previous, qint64(data.size() * 3)); QCOMPARE(reportedTotal, previous);
        for (const QString &name : names) QCOMPARE(readFile(root.filePath(name + QStringLiteral(".img"))), data);
    }
    void payloadDownloadConnections_data() {
        QTest::addColumn<int>("connections"); QTest::addColumn<bool>("zip");
        QTest::newRow("one-connection") << 1 << false;
        QTest::newRow("16-connections") << 16 << false;
        QTest::newRow("512-connections") << 512 << false;
        QTest::newRow("compressed-ota-16-connections") << 16 << true;
    }
    void payloadDownloadConnections() {
        QFETCH(int, connections); QFETCH(bool, zip); QTemporaryDir root;
        QByteArray data((connections == 512 ? 128 : 4) * 1024 * 1024, 'p');
        if (zip) {
            QRandomGenerator random(512);
            for (qsizetype index = 0; index < data.size(); index += 4) qToLittleEndian<quint32>(random.generate(), data.data() + index);
        }
        QByteArray payload = encodePayload(data);
        const qint64 dataOffset = 24 + qint64(qFromBigEndian<quint64>(payload.constData() + 12));
        if (zip) {
            const QString archive = root.filePath(QStringLiteral("ota.zip"));
            { ZipWriter writer(archive); writer.addFile(QStringLiteral("payload.bin"), payload); writer.close(); }
            payload = readFile(archive);
        }
        PayloadHttpFixture fixture(std::move(payload)); fixture.delayedFrom = zip ? 30 + QByteArray("payload.bin").size() : dataOffset;
        if (zip) fixture.delayedMinimumSize = 256 * 1024;
        // Wait for every requested range before responding, so peak concurrency is deterministic.
        fixture.releaseAt = connections;
        PayloadOptions options; options.downloadConnections = connections;
        qint64 networkBytes = 0;
        auto task = qtAsync([&] {
            return extractPayload(fixture.url(), {QStringLiteral("system")}, root.path(), 4, {}, [&](qint64 done, qint64, const QString &stage) {
                if (stage.startsWith(QStringLiteral("下载 Payload 分区数据")) || stage.startsWith(QStringLiteral("下载并解压"))) networkBytes = qMax(networkBytes, done);
            }, options);
        });
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 60000);
        const Result result = task.get(); QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(fixture.peak, connections); QVERIFY(networkBytes >= data.size());
        QCOMPARE(QCryptographicHash::hash(readFile(root.filePath(QStringLiteral("system.img"))), QCryptographicHash::Sha256), QCryptographicHash::hash(data, QCryptographicHash::Sha256));
        QTRY_COMPARE(fixture.active, 0);
        QCOMPARE(QDir(root.path()).entryList({QStringLiteral(".twinfile-payload-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
    }
    void payloadParallelRangeFailures_data() {
        QTest::addColumn<QString>("failure");
        for (const QString &failure : {QStringLiteral("version"), QStringLiteral("range"), QStringLiteral("ignored"), QStringLiteral("cancel")})
            QTest::newRow(qPrintable(failure)) << failure;
    }
    void payloadParallelRangeFailures() {
        QFETCH(QString, failure); QTemporaryDir root; PayloadHttpFixture fixture(encodePayload(QByteArray(4 * 1024 * 1024, 'f')));
        fixture.delayedFrom = 24 + qint64(qFromBigEndian<quint64>(fixture.bytes.constData() + 12)); fixture.dataFailure = failure;
        if (failure == QStringLiteral("cancel")) fixture.releaseAt = 17;
        PayloadOptions options; options.downloadConnections = 16; auto cancel = std::make_shared<std::atomic_bool>(false);
        auto task = qtAsync([&] { return extractPayload(fixture.url(), {QStringLiteral("system")}, root.path(), 4, cancel, {}, options); });
        if (failure == QStringLiteral("cancel")) { QTRY_COMPARE_WITH_TIMEOUT(fixture.active, 16, 5000); cancel->store(true); }
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000);
        const Result result = task.get(); QVERIFY(!result.ok); QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("system.img")))); QTRY_COMPARE(fixture.active, 0);
        const QString expected = failure == QStringLiteral("version") ? QStringLiteral("ETag") : failure == QStringLiteral("cancel") ? QStringLiteral("取消") : QStringLiteral("Range");
        QVERIFY2(result.error.contains(expected), qPrintable(result.error));
    }
    void payloadRejectsInvalidDownloadConnections() {
        PayloadHttpFixture fixture(encodePayload(QByteArray(4096, 'x')));
        for (int connections : {0, 513}) { PayloadOptions options; options.downloadConnections = connections; QVERIFY(!inspectPayload(fixture.url(), {}, options).ok); }
        QCOMPARE(fixture.requests, 0);
    }
    void payloadConcurrentDeadlinesDoNotCrash() {
        for (int iteration = 0; iteration < 8; ++iteration) {
            QTemporaryDir root; PayloadHttpFixture fixture(encodePayload(QByteArray(32 * 1024 * 1024, 'd')));
            fixture.delayedFrom = 24 + qint64(qFromBigEndian<quint64>(fixture.bytes.constData() + 12));
            PayloadOptions options; options.downloadConnections = 512;
            options.requestTimeoutMs = 120; options.idleTimeoutMs = 5000;
            auto task = qtAsync([&] { return extractPayload(fixture.url(), {QStringLiteral("system")}, root.path(), 1, {}, {}, options); });
            QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 10000);
            const Result result = task.get();
            if (result.ok) QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), QByteArray(32 * 1024 * 1024, 'd'));
            else { QVERIFY2(result.error.contains(QStringLiteral("重试失败")), qPrintable(result.error)); QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("system.img")))); }
            QTRY_COMPARE(fixture.active, 0);
        }
    }
    void payloadParallelHttpErrors_data() {
        QTest::addColumn<QString>("failure");
        for (const QString &failure : {QStringLiteral("429"), QStringLiteral("503"), QStringLiteral("503-once")})
            QTest::newRow(qPrintable(failure)) << failure;
    }
    void payloadParallelHttpErrors() {
        QFETCH(QString, failure); QTemporaryDir root; const QByteArray data(4 * 1024 * 1024, 'e');
        PayloadHttpFixture fixture(encodePayload(data)); fixture.dataFailure = failure;
        fixture.delayedFrom = 24 + qint64(qFromBigEndian<quint64>(fixture.bytes.constData() + 12));
        PayloadOptions options; options.downloadConnections = 16;
        auto task = qtAsync([&] { return extractPayload(fixture.url(), {QStringLiteral("system")}, root.path(), 1, {}, {}, options); });
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 10000);
        const auto result = task.get(); QVERIFY(fixture.failureUsed);
        if (failure.endsWith(QStringLiteral("-once"))) {
            QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), data);
        } else {
            QVERIFY(!result.ok); QVERIFY2(result.error.contains(QStringLiteral("重试失败")), qPrintable(result.error));
            QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("system.img"))));
        }
        QTRY_COMPARE(fixture.active, 0);
    }
    void payloadManySilentRangesRecover() {
        QTemporaryDir root; const QByteArray data(128 * 1024 * 1024, 's');
        PayloadHttpFixture fixture(encodePayload(data)); fixture.dataFailure = QStringLiteral("many-stalls");
        fixture.delayedFrom = 24 + qint64(qFromBigEndian<quint64>(fixture.bytes.constData() + 12));
        PayloadOptions options; options.downloadConnections = 512; options.idleTimeoutMs = options.requestTimeoutMs = 20000;
        auto cancel = std::make_shared<std::atomic_bool>(false);
        QTimer::singleShot(18000, &fixture, [cancel] { cancel->store(true); });
        auto task = qtAsync([&] { return extractPayload(fixture.url(), {QStringLiteral("system")}, root.path(), 1, cancel, {}, options); });
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 17000);
        const auto result = task.get(); QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(fixture.stalledRanges.size() >= 256); QVERIFY(fixture.peak <= 512);
        QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), data); QTRY_COMPARE(fixture.active, 0);
    }
    void payloadParallelRangeRetriesTruncatedPart() {
        QTemporaryDir root; const QByteArray data(4 * 1024 * 1024, 'r'); PayloadHttpFixture fixture(encodePayload(data));
        fixture.delayedFrom = 24 + qint64(qFromBigEndian<quint64>(fixture.bytes.constData() + 12)); fixture.dataFailure = QStringLiteral("retry");
        PayloadOptions options; options.downloadConnections = 16;
        auto task = qtAsync([&] { return extractPayload(fixture.url(), {QStringLiteral("system")}, root.path(), 4, {}, {}, options); });
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000);
        const Result result = task.get(); QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), data);
        QVERIFY(fixture.failureUsed); QCOMPARE(fixture.requests, 20); QVERIFY(fixture.peak <= 16); QTRY_COMPARE(fixture.active, 0);
        const qint64 remaining = fixture.ranges.last().second - fixture.ranges.last().first + 1;
        QVERIFY(remaining >= 100 && remaining < 256 * 1024);
    }
    void payloadDynamicQueueRecoversSilentTail_data() {
        QTest::addColumn<bool>("validator");
        QTest::newRow("resume-validated-prefix") << true;
        QTest::newRow("restart-unvalidated-prefix") << false;
    }
    void payloadDynamicQueueRecoversSilentTail() {
        QFETCH(bool, validator); QTemporaryDir root; const QByteArray data(4 * 1024 * 1024, 'q');
        PayloadHttpFixture fixture(encodePayload(data)); fixture.stableValidator = validator;
        fixture.delayedFrom = 24 + qint64(qFromBigEndian<quint64>(fixture.bytes.constData() + 12));
        fixture.dataFailure = QStringLiteral("stall"); PayloadOptions options; options.downloadConnections = 4;
        options.idleTimeoutMs = options.requestTimeoutMs = 20000; auto cancel = std::make_shared<std::atomic_bool>(false);
        QTimer::singleShot(10000, &fixture, [cancel] { cancel->store(true); });
        auto task = qtAsync([&] { return extractPayload(fixture.url(), {QStringLiteral("system")}, root.path(), 1, cancel, {}, options); });
        QTRY_VERIFY_WITH_TIMEOUT(fixture.ranges.size() >= 10, 2500);
        QVERIFY(fixture.active <= 4); QVERIFY(fixture.failureUsed);
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 9000);
        const auto result = task.get(); QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), data);
        QVERIFY(fixture.peak <= 4); QTRY_COMPARE(fixture.active, 0);
        const qint64 start = fixture.delayedFrom; int matchingRanges = 0;
        for (qsizetype index = 3; index < fixture.ranges.size(); ++index) {
            if (fixture.ranges[index].second == start + 262144 - 1) {
                QCOMPARE(fixture.ranges[index].first, start + (matchingRanges && validator ? 131072 : 0)); ++matchingRanges;
            }
        }
        QCOMPARE(matchingRanges, 2);
    }
    void payloadDownloadsIgnoreApplicationProxy() {
        const auto previous = QNetworkProxy::applicationProxy();
        const auto restore = qScopeGuard([&] { QNetworkProxy::setApplicationProxy(previous); });
        QTcpServer proxy; QVERIFY(proxy.listen(QHostAddress::LocalHost)); int hits = 0;
        connect(&proxy, &QTcpServer::newConnection, &proxy, [&] {
            while (proxy.hasPendingConnections()) {
                auto *socket = proxy.nextPendingConnection(); auto request = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket, request] {
                    *request += socket->readAll(); if (!request->contains("\r\n\r\n")) return;
                    ++hits; request->clear();
                    socket->write("HTTP/1.1 502 Proxy blocked\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); socket->disconnectFromHost();
                });
            }
        });
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), proxy.serverPort()));
        PayloadHttpFixture fixture(encodePayload(QByteArray(4 * 1024 * 1024, 'p')));
        {
            QNetworkAccessManager control; control.setProxy(QNetworkProxy::applicationProxy());
            auto *reply = control.get(QNetworkRequest(QUrl(fixture.url())));
            QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 3000); QCOMPARE(hits, 1);
        }
        const int proxyRequests = hits;
        QTemporaryDir root; PayloadOptions options; options.downloadConnections = 16;
        auto task = qtAsync([&] { return extractPayload(fixture.url(), {QStringLiteral("system")}, root.path(), 4, {}, {}, options); });
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000);
        const auto result = task.get(); QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(hits, proxyRequests);
        QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), QByteArray(4 * 1024 * 1024, 'p'));
        QCOMPARE(QNetworkProxy::applicationProxy().type(), QNetworkProxy::HttpProxy);
    }
    void payloadAdjacentOperationsUseParallelReadAhead_data() {
        QTest::addColumn<bool>("corrupt"); QTest::addColumn<int>("connections"); QTest::addColumn<int>("blockSize");
        QTest::newRow("valid") << false << 16 << 4096;
        QTest::newRow("bad-operation-hash") << true << 16 << 4096;
        QTest::newRow("window-boundary") << false << 1 << 266240;
    }
    void payloadAdjacentOperationsUseParallelReadAhead() {
        QFETCH(bool, corrupt); QFETCH(int, connections); QFETCH(int, blockSize); QTemporaryDir root;
        const int operations = connections == 1 ? 4 : 1024; const QByteArray data(operations * blockSize, 'a');
        QByteArray partition = message(1, "system") + message(7, integer(1, data.size()));
        for (int index = 0; index < operations; ++index) {
            const QByteArray block = data.mid(index * blockSize, blockSize);
            partition += message(8, integer(1, 0) + integer(2, index * blockSize) + integer(3, blockSize)
                + message(6, integer(1, index * blockSize / 4096) + integer(2, blockSize / 4096))
                + message(8, corrupt && index == 500 ? QByteArray(32, 'x') : QCryptographicHash::hash(block, QCryptographicHash::Sha256)));
        }
        const QByteArray manifest = integer(3, 4096) + message(13, partition);
        PayloadHttpFixture fixture(QByteArray("CrAU") + network<quint64>(2) + network<quint64>(manifest.size()) + network<quint32>(0) + manifest + data);
        fixture.delayedFrom = 24 + manifest.size(); fixture.releaseAt = connections;
        PayloadOptions options; options.downloadConnections = connections;
        auto task = qtAsync([&] { return extractPayload(fixture.url(), {QStringLiteral("system")}, root.path(), 4, {}, {}, options); });
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 5000);
        const auto result = task.get(); QCOMPARE(result.ok, !corrupt); QCOMPARE(fixture.peak, connections); QCOMPARE(fixture.requests, connections == 1 ? 8 : 19);
        if (corrupt) { QVERIFY(result.error.contains(QStringLiteral("SHA256"))); QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("system.img")))); }
        else QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), data);
    }
    void payloadHttpStrictRange_data() { QTest::addColumn<bool>("valid"); QTest::newRow("range") << true; QTest::newRow("ignored-range") << false; }
    void payloadPrefetchOverlapsOutputAndReusesConnections_data() {
        QTest::addColumn<int>("layout");
        QTest::newRow("contiguous") << 0; QTest::newRow("reverse") << 1; QTest::newRow("gapped") << 2;
    }
    void payloadPrefetchOverlapsOutputAndReusesConnections() {
        QFETCH(int, layout); QTemporaryDir root; const int block = 256 * 1024, count = 8;
        QByteArray blobs, expected, partition = message(1, "system") + message(7, integer(1, block * count));
        for (int index = 0; index < count * (layout == 2 ? 2 : 1); ++index) blobs += QByteArray(block, char('a' + index));
        for (int index = 0; index < count; ++index) {
            const int blob = layout == 1 ? count - index - 1 : layout == 2 ? index * 2 : index;
            const QByteArray bytes = blobs.mid(blob * block, block); expected += bytes;
            partition += message(8, integer(1, 0) + integer(2, blob * block) + integer(3, block)
                + message(6, integer(1, index * block / 4096) + integer(2, block / 4096))
                + message(8, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)));
        }
        const QByteArray manifest = integer(3, 4096) + message(13, partition);
        const QByteArray payload = QByteArray("CrAU") + network<quint64>(2) + network<quint64>(manifest.size()) + network<quint32>(0) + manifest + blobs;
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); int sockets = 0, requests = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (server.hasPendingConnections()) {
                auto *client = server.nextPendingConnection(); ++sockets; auto request = std::make_shared<QByteArray>();
                connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
                connect(client, &QTcpSocket::readyRead, client, [&, client, request] {
                    *request += client->readAll(); if (!request->contains("\r\n\r\n")) return;
                    const auto match = QRegularExpression(QStringLiteral("Range: bytes=(\\d+)-(\\d+)"), QRegularExpression::CaseInsensitiveOption).match(QString::fromLatin1(*request));
                    request->clear(); ++requests; const qint64 first = match.captured(1).toLongLong(), last = match.captured(2).toLongLong();
                    const QByteArray body = payload.mid(first, last - first + 1);
                    client->write(QByteArray("HTTP/1.1 206 Partial Content\r\nETag: \"stable\"\r\nContent-Range: bytes ")
                        + QByteArray::number(first) + '-' + QByteArray::number(last) + '/' + QByteArray::number(payload.size())
                        + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: keep-alive\r\n\r\n" + body);
                });
            }
        });
        std::atomic_bool writing{false}, release{false}; PayloadOptions options; options.downloadConnections = 1;
        auto task = qtAsync([&] {
            return extractPayload(QStringLiteral("http://127.0.0.1:%1/payload").arg(server.serverPort()), {QStringLiteral("system")}, root.path(), 1, {},
                [&](qint64, qint64, const QString &stage) {
                    if (!stage.startsWith(QStringLiteral("提取分区")) || writing.exchange(true)) return;
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
                    while (!release.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }, options);
        });
        QTRY_VERIFY_WITH_TIMEOUT(writing.load(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(requests >= 5, 3000); release.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 10000);
        const auto result = task.get(); QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), expected); QCOMPARE(requests, 3 + count);
        QVERIFY2(sockets <= 3, qPrintable(QStringLiteral("Opened %1 sockets for %2 requests").arg(sockets).arg(requests)));
    }
    void payloadStopsPendingPrefetch_data() {
        QTest::addColumn<bool>("corrupt");
        QTest::newRow("cancel") << false; QTest::newRow("bad-hash") << true;
    }
    void payloadStopsPendingPrefetch() {
        QFETCH(bool, corrupt); QTemporaryDir root; const int block = 256 * 1024; const QByteArray data(block, 'p');
        QByteArray partition = message(1, "system") + message(7, integer(1, block * 2));
        for (int index = 0; index < 2; ++index) {
            partition += message(8, integer(1, 0) + integer(2, index * block) + integer(3, block)
                + message(6, integer(1, index * block / 4096) + integer(2, block / 4096))
                + message(8, corrupt && index == 0 ? QByteArray(32, 'x') : QCryptographicHash::hash(data, QCryptographicHash::Sha256)));
        }
        const QByteArray manifest = integer(3, 4096) + message(13, partition);
        PayloadHttpFixture fixture(QByteArray("CrAU") + network<quint64>(2) + network<quint64>(manifest.size()) + network<quint32>(0) + manifest + data.repeated(2));
        fixture.silentFrom = 24 + manifest.size() + block;
        auto cancel = std::make_shared<std::atomic_bool>(false); std::atomic_bool writing{false}, release{false};
        PayloadOptions options; options.downloadConnections = 1; options.requestTimeoutMs = 20000;
        auto task = qtAsync([&] {
            return extractPayload(fixture.url(), {QStringLiteral("system")}, root.path(), 1, cancel,
                [&](qint64, qint64, const QString &stage) {
                    if (!stage.startsWith(QStringLiteral("提取分区")) || writing.exchange(true)) return;
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
                    while (!release.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }, options);
        });
        QTRY_VERIFY_WITH_TIMEOUT(writing.load(), 3000); QTRY_COMPARE_WITH_TIMEOUT(fixture.requests, 5, 3000);
        if (!corrupt) cancel->store(true);
        release.store(true);
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 3000);
        const auto result = task.get(); QVERIFY(!result.ok);
        QVERIFY2(result.error.contains(corrupt ? QStringLiteral("SHA256") : QStringLiteral("取消")), qPrintable(result.error));
        QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("system.img"))));
        QCOMPARE(QDir(root.path()).entryList({QStringLiteral(".twinfile-payload-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
    }
    void payloadHttpStrictRange() {
        QFETCH(bool, valid); QTemporaryDir root; const QByteArray payload = encodePayload(QByteArray(4096, 'r')); QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            QTcpSocket *client = server.nextPendingConnection(); auto request = std::make_shared<QByteArray>();
            connect(client, &QTcpSocket::readyRead, client, [&, client, request] {
                *request += client->readAll(); if (!request->contains("\r\n\r\n")) return; const auto match = QRegularExpression(QStringLiteral("Range: bytes=(\\d+)-(\\d+)"), QRegularExpression::CaseInsensitiveOption).match(QString::fromLatin1(*request));
                const qint64 start = match.captured(1).toLongLong(), end = match.captured(2).toLongLong(); const QByteArray body = valid ? payload.mid(start, end - start + 1) : payload;
                QByteArray header = valid ? QByteArray("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes ") + QByteArray::number(start) + '-' + QByteArray::number(end) + '/' + QByteArray::number(payload.size()) + "\r\n" : QByteArray("HTTP/1.1 200 OK\r\n");
                header += "ETag: \"fixture\"\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n"; client->write(header + body); client->disconnectFromHost();
            }); connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        });
        const QString url = QStringLiteral("http://127.0.0.1:%1/payload.bin").arg(server.serverPort()); auto task = qtAsync([&] { return extractPayload(url, {QStringLiteral("system")}, root.path()); });
        QTRY_VERIFY_WITH_TIMEOUT(task.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 10000); const Result result = task.get(); QCOMPARE(result.ok, valid); if (valid) QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), QByteArray(4096, 'r')); else QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("system.img"))));
    }
    void payloadCancellationLeavesNoOutput() { QTemporaryDir root; const QString source = root.filePath(QStringLiteral("payload.bin")); QVERIFY(writeFile(source, encodePayload(QByteArray(4096, 'a')))); QVERIFY(!extractPayload(source, {QStringLiteral("system")}, root.path(), 4, std::make_shared<std::atomic_bool>(true)).ok); QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("system.img")))); }
    void payloadRejectsHeaderInjection() { QTemporaryDir root; const QString source = root.filePath(QStringLiteral("payload.bin")); QVERIFY(writeFile(source, encodePayload(QByteArray(4096, 'a')))); PayloadOptions options; options.userAgent = QStringLiteral("TwinFile\r\nInjected: x"); QVERIFY(!inspectPayload(source, {}, options).ok); }
    void payloadDialogsAndProgress_data() {
        QTest::addColumn<QString>("theme"); QTest::addColumn<QString>("mode");
        for (const QString &theme : themeIds())
            for (const QString &mode : {QStringLiteral("dark"), QStringLiteral("light")}) QTest::newRow(qPrintable(theme + QChar('-') + mode)) << theme << mode;
    }
    void payloadDialogsAndProgress() {
        QFETCH(QString, theme); QFETCH(QString, mode); QTemporaryDir root; const QByteArray previous = qgetenv("TWINFILE_SETTINGS");
        const auto restore = qScopeGuard([previous] { if (previous.isEmpty()) qunsetenv("TWINFILE_SETTINGS"); else qputenv("TWINFILE_SETTINGS", previous); });
        const QString settingsFile = root.filePath(QStringLiteral("settings.json")); qputenv("TWINFILE_SETTINGS", settingsFile.toUtf8());
        QVERIFY(writeSettings(settingsFile, {{QStringLiteral("payload_user_agent"), QStringLiteral("old-agent")}}));
        PayloadHttpFixture fixture(encodePayload(QByteArray(4096, 'u'), 0, false, 40)); Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.setTheme(theme); window.setColorMode(mode); window.show();
        const QString evidence = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT"); int step = 0; bool valid = true;
        const auto screenshot = [&](QWidget *widget, const QString &name) {
            const QPixmap image = widget->grab(); valid &= !image.isNull();
            if (!evidence.isEmpty()) valid &= image.save(QDir(evidence).filePath(QStringLiteral("native-payload-") + name + QChar('-') + theme + QChar('-') + mode + QStringLiteral(".png")));
        };
        QTimer driver; driver.setInterval(20);
        connect(&driver, &QTimer::timeout, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            if (step == 0) {
                auto *source = qobject_cast<QInputDialog *>(dialog); if (!source) return;
                auto *combo = source->findChild<QComboBox *>(); valid &= combo != nullptr;
                if (combo) combo->setCurrentIndex(1);
                ++step; source->accept();
            } else if (step == 1 && dialog->objectName() == QStringLiteral("remotePayloadDialog")) {
                auto *threads = dialog->findChild<QSpinBox *>(QStringLiteral("payloadThreads")); auto *url = dialog->findChild<QLineEdit *>(QStringLiteral("payloadUrl"));
                auto *output = dialog->findChild<QLineEdit *>(QStringLiteral("payloadOutputDirectory"));
                valid &= threads && url && output && dialog->findChild<QPushButton *>(QStringLiteral("payloadBrowse"));
                if (threads) { valid &= threads->value() == 16 && threads->minimum() == 1 && threads->maximum() == 512; threads->setValue(512); }
                if (output) output->setText(root.path());
                if (url) url->setText(fixture.url());
                dialog->resize(480, dialog->height()); screenshot(dialog, QStringLiteral("remote"));
                ++step; dialog->accept();
            } else if (step == 2 && dialog->objectName() == QStringLiteral("payloadPartitionsDialog")) {
                auto *threads = dialog->findChild<QSpinBox *>(QStringLiteral("payloadThreads")); valid &= threads && threads->value() == 512 && threads->maximum() == 512;
                valid &= window.findChildren<QProgressDialog *>().isEmpty();
                auto *output = dialog->findChild<QLineEdit *>(QStringLiteral("payloadOutputDirectory")); valid &= output && output->text() == root.path();
                valid &= dialog->findChild<QPushButton *>(QStringLiteral("payloadBrowse")) != nullptr;
                auto *choices = dialog->findChild<QListWidget *>(QStringLiteral("payloadPartitions"));
                auto *selectAll = dialog->findChild<QPushButton *>(QStringLiteral("payloadSelectAll"));
                auto *selectNone = dialog->findChild<QPushButton *>(QStringLiteral("payloadSelectNone"));
                valid &= choices && selectAll && selectNone;
                if (choices && selectAll && selectNone) {
                    valid &= choices->count() == 40 && !selectAll->autoDefault() && !selectNone->autoDefault();
                    choices->setCurrentRow(20); choices->scrollToBottom();
                    const int scroll = choices->verticalScrollBar()->value();
                    const auto allChecked = [&](Qt::CheckState state) {
                        for (int index = 0; index < choices->count(); ++index) if (choices->item(index)->checkState() != state) return false;
                        return true;
                    };
                    valid &= allChecked(Qt::Checked); choices->item(0)->setCheckState(Qt::Unchecked);
                    QTest::mouseClick(selectNone, Qt::LeftButton); valid &= allChecked(Qt::Unchecked);
                    QTest::mouseClick(selectNone, Qt::LeftButton); valid &= allChecked(Qt::Unchecked);
                    QTest::mouseClick(selectAll, Qt::LeftButton); valid &= allChecked(Qt::Checked);
                    QTest::mouseClick(selectAll, Qt::LeftButton); valid &= allChecked(Qt::Checked);
                    valid &= choices->currentRow() == 20 && choices->verticalScrollBar()->value() == scroll;
                    valid &= dialog->isVisible() && threads->value() == 512 && output->text() == root.path();
                    dialog->resize(480, dialog->height());
                    valid &= !selectAll->icon().isNull() && !selectNone->icon().isNull();
                    valid &= dialog->rect().contains(QRect(selectNone->mapTo(dialog, QPoint()), selectNone->size()));
                }
                screenshot(dialog, QStringLiteral("partitions")); ++step; dialog->reject();
            }
        });
        QTimer watchdog; watchdog.setInterval(7000); connect(&watchdog, &QTimer::timeout, &window, [&] { valid = false; if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        driver.start(); watchdog.start(); PayloadTestAccess::open(window);
        QTRY_COMPARE_WITH_TIMEOUT(step, 3, 5000); driver.stop(); watchdog.stop(); QVERIFY(valid);
        QCOMPARE(PayloadTestAccess::settings(window).value(QStringLiteral("payload_threads")).toInt(), 512);
        QVERIFY(!PayloadTestAccess::settings(window).contains(QStringLiteral("payload_user_agent")));
        auto sawCancel = std::make_shared<std::atomic_bool>(false);
        PayloadTestAccess::job(window, [sawCancel](const Cancellation &cancel, const Progress &progress) {
            progress(25 * 1024 * 1024, 100 * 1024 * 1024, QStringLiteral("提取分区 · system")); const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!cancel->load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            sawCancel->store(cancel->load()); return Result{};
        });
        auto *progress = window.findChild<QProgressBar *>(QStringLiteral("jobProgress")); QVERIFY(progress);
        QTRY_COMPARE_WITH_TIMEOUT(progress->value(), 250, 1000); QCOMPARE(progress->maximum(), 1000);
        auto *task = window.findChild<QWidget *>(QStringLiteral("taskProgress")); QVERIFY(task); QVERIFY(task->isVisible());
        QVERIFY(window.findChildren<QProgressDialog *>().isEmpty()); QVERIFY(!QApplication::activeModalWidget());
        QVERIFY(task->toolTip().contains(QStringLiteral("提取分区 · system")));
        QTest::qWait(550); QVERIFY(task->findChild<QLabel *>(QStringLiteral("taskSpeed"))->text().contains(QStringLiteral("/s")));
        screenshot(&window, QStringLiteral("progress"));
        window.resize(1100, 650); QTest::qWait(40);
        const auto taskArea = QRect(task->mapTo(&window, QPoint()), task->size());
        const auto sort = window.findChild<QPushButton *>(QStringLiteral("action_sort"));
        QVERIFY(sort); QVERIFY(taskArea.right() < sort->mapTo(&window, QPoint()).x());
        QVERIFY(window.rect().contains(taskArea));
        screenshot(&window, QStringLiteral("progress-narrow"));
        auto *cancel = task->findChild<QToolButton *>(QStringLiteral("taskCancel")); QVERIFY(cancel); QTest::mouseClick(cancel, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!PayloadTestAccess::busy(window), 1000); QVERIFY(sawCancel->load()); QVERIFY(valid);
        PayloadTestAccess::job(window, [](const Cancellation &, const Progress &) { return Result{false, QStringLiteral("测试网络错误"), {}, {}}; });
        QTRY_VERIFY_WITH_TIMEOUT(!PayloadTestAccess::busy(window), 1000);
        QVERIFY(task->toolTip().contains(QStringLiteral("测试网络错误"))); QVERIFY(!QApplication::activeModalWidget());
        window.close(); QCOMPARE(readSettings(settingsFile).value(QStringLiteral("payload_threads")).toInt(), 512);
    }
    void payloadOutputDirectoryAndInlineCompletion_data() {
        payloadDialogsAndProgress_data();
    }
    void payloadOutputDirectoryAndInlineCompletion() {
        QFETCH(QString, theme); QFETCH(QString, mode);
        QTemporaryDir root; const auto previous = qgetenv("TWINFILE_SETTINGS");
        const auto restore = qScopeGuard([&] { if (previous.isEmpty()) qunsetenv("TWINFILE_SETTINGS"); else qputenv("TWINFILE_SETTINGS", previous); });
        const QString settings = root.filePath(QStringLiteral("settings.json")); qputenv("TWINFILE_SETTINGS", settings.toUtf8());
        PayloadHttpFixture fixture(encodePayload(QByteArray(4096, 'd'))); Window window; window.setTheme(theme); window.setColorMode(mode); window.show();
        int step = 0; bool valid = true; QTimer driver; driver.setInterval(250);
        connect(&driver, &QTimer::timeout, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            auto *ok = buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
            if (!ok) { valid = false; dialog->reject(); return; }
            if (step == 0) {
                auto *source = qobject_cast<QInputDialog *>(dialog); if (!source) return;
                source->findChild<QComboBox *>()->setCurrentIndex(1); ++step; QTest::mouseClick(ok, Qt::LeftButton);
            } else if (step == 1 && dialog->objectName() == QStringLiteral("remotePayloadDialog")) {
                dialog->findChild<QLineEdit *>(QStringLiteral("payloadUrl"))->setText(fixture.url());
                dialog->findChild<QSpinBox *>(QStringLiteral("payloadThreads"))->setValue(512);
                auto *path = dialog->findChild<QLineEdit *>(QStringLiteral("payloadOutputDirectory"));
                valid &= path != nullptr; if (path) path->setText(root.path()); ++step; QTest::mouseClick(ok, Qt::LeftButton);
            } else if (step == 2 && dialog->objectName() == QStringLiteral("payloadPartitionsDialog")) {
                auto *path = dialog->findChild<QLineEdit *>(QStringLiteral("payloadOutputDirectory"));
                auto *choices = dialog->findChild<QListWidget *>(QStringLiteral("payloadPartitions"));
                auto *selectAll = dialog->findChild<QPushButton *>(QStringLiteral("payloadSelectAll"));
                auto *selectNone = dialog->findChild<QPushButton *>(QStringLiteral("payloadSelectNone"));
                valid &= path && path->text() == root.path() && choices && selectAll && selectNone;
                if (choices && selectAll && selectNone) {
                    QTest::mouseClick(selectNone, Qt::LeftButton); valid &= choices->item(0)->checkState() == Qt::Unchecked;
                    QTest::mouseClick(selectAll, Qt::LeftButton); valid &= choices->item(0)->checkState() == Qt::Checked;
                }
                ++step; QTest::mouseClick(ok, Qt::LeftButton);
            }
        });
        QTimer watchdog; watchdog.setInterval(20000); connect(&watchdog, &QTimer::timeout, &window, [&] { valid = false; if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        const auto cleanup = qScopeGuard([&] {
            driver.stop(); watchdog.stop(); PayloadTestAccess::cancel(window);
            (void)QTest::qWaitFor([&] { return !PayloadTestAccess::busy(window); }, 3000);
        });
        driver.start(); watchdog.start(); PayloadTestAccess::open(window);
        QTRY_COMPARE_WITH_TIMEOUT(step, 3, 15000); QTRY_VERIFY_WITH_TIMEOUT(!PayloadTestAccess::busy(window), 15000);
        driver.stop(); watchdog.stop(); QVERIFY(valid); QCOMPARE(readFile(root.filePath(QStringLiteral("system.img"))), QByteArray(4096, 'd'));
        QCOMPARE(readSettings(settings).value(QStringLiteral("payload_output_directory")).toString(), root.path());
        QVERIFY(!QApplication::activeModalWidget()); QVERIFY(window.findChildren<QProgressDialog *>().isEmpty());
        QVERIFY(window.findChild<QWidget *>(QStringLiteral("taskProgress"))->toolTip().contains(root.path())); window.close();
    }
    void transferByteProgressAndCancellation() {
        QTemporaryDir root; const QString source = root.filePath(QStringLiteral("source")), target = root.filePath(QStringLiteral("target"));
        QVERIFY(QDir().mkpath(source + QStringLiteral("/nested"))); QVERIFY(QDir().mkpath(target));
        const QByteArray data(3 * 1024 * 1024 + 17, 'x'); QVERIFY(writeFile(source + QStringLiteral("/first.bin"), data));
        QVERIFY(writeFile(source + QStringLiteral("/nested/second.bin"), "second"));
        Entry entry; entry.path = source; entry.name = QStringLiteral("source"); entry.directory = true;
        qint64 previous = 0, expected = 0; int calls = 0; bool monotonic = true;
        const Progress progress = [&](qint64 done, qint64 total, const QString &) { monotonic &= done >= previous && done <= total; previous = done; expected = total; ++calls; };
        const Result copied = transferEntries({entry}, {Source::Local, target, {}, {}}, false, {}, progress);
        QVERIFY2(copied.ok, qPrintable(copied.error)); QVERIFY(monotonic); QVERIFY(calls >= 5);
        QCOMPARE(previous, data.size() + 6); QCOMPARE(expected, previous);
        QCOMPARE(readFile(target + QStringLiteral("/source/first.bin")), data);
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("cancel"))));
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        const Result stopped = transferEntries({entry}, {Source::Local, root.filePath(QStringLiteral("cancel")), {}, {}}, false, cancellation,
            [&](qint64 done, qint64, const QString &) { if (done >= 1024 * 1024) cancellation->store(true); });
        QVERIFY(!stopped.ok); QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("cancel/source")))); QVERIFY(QFileInfo::exists(source));
        const QString archive = root.filePath(QStringLiteral("test.zip")); previous = 0; calls = 0; monotonic = true;
        QVERIFY(createArchive({source}, archive, {}, progress).ok); QVERIFY(monotonic); QVERIFY(calls > 1); QCOMPARE(previous, expected);
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("extract")))); previous = 0; calls = 0;
        QVERIFY(extractArchive(archive, root.filePath(QStringLiteral("extract")), {}, {}, progress).ok); QVERIFY(monotonic); QVERIFY(calls > 1); QCOMPARE(previous, expected);
    }
    void inlineTaskSpeedCancellationAndFailure() {
        TaskProgress task; task.resize(320, 44); bool cancelled = false;
        task.begin(QStringLiteral("复制文件"), [&] { cancelled = true; }); task.show();
        task.updateProgress(1024 * 1024, 4 * 1024 * 1024, QStringLiteral("source.bin"));
        QTRY_VERIFY_WITH_TIMEOUT(!task.toolTip().section(QChar('\n'), 1).startsWith(QStringLiteral("0 B/s")), 1500);
        QTRY_VERIFY_WITH_TIMEOUT(task.toolTip().section(QChar('\n'), 1).startsWith(QStringLiteral("0 B/s")), 1500);
        task.cancel(); QVERIFY(cancelled); QVERIFY(task.active());
        task.updateProgress(2 * 1024 * 1024, 4 * 1024 * 1024, QStringLiteral("late.bin"));
        QVERIFY(task.toolTip().contains(QStringLiteral("正在取消")));
        task.finish(QStringLiteral("操作已取消"), false); QVERIFY(!task.active());
        task.begin(QStringLiteral("解析 Payload"), [] {}); task.updateProgress(100, 400, QStringLiteral("读取元数据"));
        QCOMPARE(task.findChild<QProgressBar *>()->value(), 250);
        task.finish(QStringLiteral("连接超时\n详细原因"), false); QVERIFY(!task.active());
        QVERIFY(task.toolTip().contains(QStringLiteral("详细原因")));
        QVERIFY(!QApplication::activeModalWidget());
    }
    void payloadNetworkSpeedIgnoresExtractionCounters() {
        TaskProgress task; task.resize(440, 44); task.begin(QStringLiteral("Payload"), [] {}); task.show();
        task.updateNetworkProgress(1024 * 1024, 8 * 1024 * 1024);
        task.updateProgress(4096, 100 * 1024 * 1024, QStringLiteral("提取分区 · system"));
        QCOMPARE(task.findChild<QProgressBar *>()->value(), 125);
        QTRY_VERIFY_WITH_TIMEOUT(!task.toolTip().section(QChar('\n'), 1).startsWith(QStringLiteral("0 B/s")), 1500);
        task.updateProgress(100 * 1024 * 1024, 100 * 1024 * 1024, QStringLiteral("校验分区 · system"));
        task.updateNetworkProgress(2 * 1024 * 1024, 8 * 1024 * 1024);
        task.updateProgress(8192, 100 * 1024 * 1024, QStringLiteral("提取分区 · vendor"));
        QCOMPARE(task.findChild<QProgressBar *>()->value(), 250);
        QVERIFY(!task.toolTip().section(QChar('\n'), 1).startsWith(QStringLiteral("0 B/s")));
        QVERIFY(task.toolTip().contains(QStringLiteral("2.0 MB / 8.0 MB")));
        QTest::qWait(750);
        QVERIFY(!task.toolTip().section(QChar('\n'), 1).startsWith(QStringLiteral("0 B/s")));
        QTRY_VERIFY_WITH_TIMEOUT(task.toolTip().section(QChar('\n'), 1).startsWith(QStringLiteral("0 B/s")), 4000);
        task.finish(QStringLiteral("完成"), true);
        task.begin(QStringLiteral("复制文件"), [] {}); task.updateProgress(1, 4, QStringLiteral("copy"));
        QCOMPARE(task.findChild<QProgressBar *>()->value(), 250);
    }
    void hfxLoopbackHandshakeAndListing() {
        HfxSession session; HfxOptions options; options.port = 0; options.interfaces = {{QStringLiteral("loopback"), QHostAddress::LocalHost}}; auto cancel = std::make_shared<std::atomic_bool>(false); std::promise<quint16> listening; auto port = listening.get_future();
        auto task = qtAsync([&] { return session.connect(options, cancel, [&](quint16 value) { listening.set_value(value); }); });
        QVERIFY(port.wait_for(std::chrono::seconds(3)) == std::future_status::ready); QTcpSocket control; control.connectToHost(QHostAddress::LocalHost, port.get()); QVERIFY(control.waitForConnected(3000));
        auto read = [&](qsizetype count) { QByteArray bytes; while (bytes.size() < count) { if (!control.bytesAvailable() && !control.waitForReadyRead(3000)) break; bytes += control.read(count - bytes.size()); } return bytes; };
        control.write(QByteArray("HFXC") + network<qint32>(300)); control.waitForBytesWritten(3000); QCOMPARE(read(1), QByteArray(1, char(1))); QCOMPARE(read(4), network<qint32>(1)); const QByteArray length = read(2); QCOMPARE(length.size(), 2); read(qFromBigEndian<quint16>(length.constData())); QCOMPARE(read(1), QByteArray(1, char(4))); read(4); QCOMPARE(read(1), QByteArray(1, '\0'));
        control.write(QByteArray(1, char(1)) + utf(QStringLiteral("loopback"))); control.waitForBytesWritten(3000); QTcpSocket channel; channel.connectToHost(QHostAddress::LocalHost, control.peerPort()); QVERIFY(channel.waitForConnected(3000)); QCOMPARE(read(1), QByteArray(1, char(1))); QCOMPARE(read(4), network<qint32>(64)); control.write(QByteArray(1, char(1))); control.waitForBytesWritten(3000); QCOMPARE(read(1), QByteArray(1, char(1))); control.write(network<qint32>(0) + utf(QStringLiteral("/sdcard"))); control.waitForBytesWritten(3000);
        QVERIFY(task.wait_for(std::chrono::seconds(3)) == std::future_status::ready); const Result connected = task.get(); QVERIFY2(connected.ok, qPrintable(connected.error)); QVERIFY(session.connected()); QCOMPARE(session.home(), QStringLiteral("/sdcard"));
        auto listing = qtAsync([&] { return session.list(QStringLiteral("/sdcard"), cancel); }); QCOMPARE(read(2), network<qint16>(1)); const QByteArray pathLength = read(2); read(qFromBigEndian<quint16>(pathLength.constData())); control.write(network<qint32>(1) + utf(QStringLiteral("file.txt")) + utf(QStringLiteral("/sdcard/file.txt")) + network<qint64>(0) + network<qint64>(3) + QByteArray(1, '\0')); control.waitForBytesWritten(3000);
        QVERIFY(listing.wait_for(std::chrono::seconds(3)) == std::future_status::ready); const Listing result = listing.get(); QVERIFY(result.error.isEmpty()); QCOMPARE(result.entries.size(), 1); QCOMPARE(result.entries.first().name, QStringLiteral("file.txt"));
        QVERIFY(!session.remove(QStringLiteral("/sdcard/..")).ok); QVERIFY(!session.remove(QStringLiteral("/storage/emulated/0/")).ok); QVERIFY(session.connected());
        auto readText = [&] { const QByteArray count = read(2); return count.size() == 2 ? QString::fromUtf8(read(qFromBigEndian<quint16>(count.constData()))) : QString(); };
        auto readChannel = [&](qsizetype count) { QByteArray bytes; while (bytes.size() < count) { if (!channel.bytesAvailable() && !channel.waitForReadyRead(3000)) break; bytes += channel.read(count - bytes.size()); } return bytes; };
        QTemporaryDir files; const QString source = files.filePath(QStringLiteral("push.bin")); const QByteArray contents(300000, 's'); QVERIFY(writeFile(source, contents));
        auto push = qtAsync([&] { return session.push({source}, QStringLiteral("/sdcard"), cancel); });
        QCOMPARE(read(2), network<qint16>(1)); QCOMPARE(readText(), QStringLiteral("/sdcard")); control.write(network<qint32>(0)); control.waitForBytesWritten(3000);
        QCOMPARE(read(2), network<qint16>(10)); QCOMPARE(readChannel(2), network<qint16>(0)); QCOMPARE(readChannel(4), network<qint32>(0)); const QByteArray nameLength = readChannel(2); QCOMPARE(nameLength.size(), 2); QCOMPARE(QString::fromUtf8(readChannel(qFromBigEndian<quint16>(nameLength.constData()))), QStringLiteral("/sdcard/push.bin")); readChannel(8); QCOMPARE(readChannel(8), network<qint64>(contents.size())); QCOMPARE(readChannel(4), network<qint32>(0)); QCOMPARE(readChannel(4), network<qint32>(contents.size())); QCOMPARE(readChannel(contents.size()), contents); QCOMPARE(readChannel(2), network<qint16>(3)); control.write(QByteArray(1, char(1))); control.waitForBytesWritten(3000); QCOMPARE(read(1), QByteArray(1, char(1)));
        QVERIFY(push.wait_for(std::chrono::seconds(3)) == std::future_status::ready); const Result pushed = push.get(); QVERIFY2(pushed.ok, qPrintable(pushed.error));
        auto pull = qtAsync([&] { return session.pull({QStringLiteral("/sdcard/pulled.bin")}, QStringLiteral("/sdcard"), files.path(), cancel); });
        QCOMPARE(read(2), network<qint16>(11)); QCOMPARE(read(4), network<qint32>(1)); QCOMPARE(readText(), QStringLiteral("/sdcard/pulled.bin")); const QString destination = readText() + QStringLiteral("pulled.bin"); QCOMPARE(read(4), network<qint32>(1)); QCOMPARE(readText(), QStringLiteral("/sdcard/"));
        const QByteArray pulledContents(1024 * 1024 + 123, 'r');
        for (int block : {1, 0}) { const QByteArray data = pulledContents.mid(block * 1024 * 1024, 1024 * 1024); channel.write(network<qint16>(0) + network<qint32>(0) + utf(destination) + network<qint64>(1200) + network<qint64>(pulledContents.size()) + network<qint32>(block) + network<qint32>(data.size()) + data); while (channel.bytesToWrite()) QVERIFY(channel.waitForBytesWritten(3000)); }
        channel.write(network<qint16>(3)); channel.waitForBytesWritten(3000); QCOMPARE(read(1), QByteArray(1, char(1))); control.write(QByteArray(1, char(1))); control.waitForBytesWritten(3000);
        QVERIFY(pull.wait_for(std::chrono::seconds(3)) == std::future_status::ready); const Result pulled = pull.get(); QVERIFY2(pulled.ok, qPrintable(pulled.error)); QCOMPARE(readFile(files.filePath(QStringLiteral("pulled.bin"))), pulledContents);
        session.close(); QVERIFY(!session.connected());
    }
    void comparisonDialogsAndThemesRenderWithoutUserSettings() {
        QTemporaryDir root; const QByteArray previous = qgetenv("TWINFILE_SETTINGS"); qputenv("TWINFILE_SETTINGS", root.filePath(QStringLiteral("settings.json")).toUtf8());
        const QString evidence = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        {
            Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.show();
            for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("mt"), QStringLiteral("cream"), QStringLiteral("bataUI01")}) { window.setTheme(theme); QApplication::processEvents(); const QPixmap screenshot = window.grab(); QVERIFY(!screenshot.isNull()); if (!evidence.isEmpty()) QVERIFY(screenshot.save(QDir(evidence).filePath(QStringLiteral("native-theme-") + theme + QStringLiteral(".png")))); }
            window.setTheme(QStringLiteral("dark"));
        }
        if (previous.isEmpty()) qunsetenv("TWINFILE_SETTINGS"); else qputenv("TWINFILE_SETTINGS", previous);
        DirectoryComparison comparison; comparison.rows.append({QStringLiteral("sub"), {}, {}, {}, true}); comparison.rows.append({QStringLiteral("sub/file.txt"), QStringLiteral("内容不同"), {}, {}, false}); comparison.summary = QStringLiteral("测试目录比较");
        DirectoryComparisonDialog directory(comparison, {}, nullptr); directory.setAttribute(Qt::WA_DeleteOnClose, false); directory.setAttribute(Qt::WA_DontShowOnScreen); directory.show(); QApplication::processEvents(); auto *listing = directory.findChild<QTableView *>(); QVERIFY(listing); QCOMPARE(listing->model()->rowCount(), 1); listing->doubleClicked(listing->model()->index(0, 0)); QCOMPARE(listing->model()->rowCount(), 1); QCOMPARE(listing->model()->data(listing->model()->index(0, 0)).toString(), QStringLiteral("sub/file.txt"));
        if (!evidence.isEmpty()) QVERIFY(directory.grab().save(QDir(evidence).filePath(QStringLiteral("native-directory-compare.png"))));
        DifferenceDialog difference(QStringLiteral("prefix old suffix\nunchanged"), QStringLiteral("prefix new suffix\nunchanged"), nullptr); difference.setAttribute(Qt::WA_DeleteOnClose, false); difference.setAttribute(Qt::WA_DontShowOnScreen); difference.show(); auto *table = difference.findChild<QTableView *>(); QVERIFY(table); QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 3, 3000); QVERIFY(!difference.grab().isNull()); if (!evidence.isEmpty()) QVERIFY(difference.grab().save(QDir(evidence).filePath(QStringLiteral("native-text-compare.png"))));
    }
};

int main(int argc, char **argv) { QApplication application(argc, argv); application.setStyle(QStringLiteral("Fusion")); MigrationTests tests; return QTest::qExec(&tests, argc, argv); }
#include "migration_tests.moc"
