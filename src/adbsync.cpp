#include "adbsync.h"
#include "operations.h"
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QSet>
#include <QStorageInfo>
#include <QTcpSocket>
#include <QtEndian>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace twinfile {
namespace {
constexpr qint64 fileLimit = 64LL * 1024 * 1024 * 1024;
constexpr qint64 totalLimit = 256LL * 1024 * 1024 * 1024;
class SyncError : public std::runtime_error {
public:
    explicit SyncError(const QString &message) : std::runtime_error(message.toUtf8().constData()) {}
};
void require(bool value, const QString &message) { if (!value) throw SyncError(message); }
void check(const Cancellation &cancel) { require(!cancel || !cancel->load(), QStringLiteral("ADB 同步已取消")); }
QByteArray word(quint32 value) { QByteArray bytes(4, '\0'); qToLittleEndian<quint32>(value, bytes.data()); return bytes; }
quint32 value(const QByteArray &bytes, qsizetype position = 0) { require(position >= 0 && position + 4 <= bytes.size(), QStringLiteral("ADB 同步响应不完整")); return qFromLittleEndian<quint32>(bytes.constData() + position); }
class Sync {
public:
    Sync(const QString &serial, const Cancellation &cancel) : cancel_(cancel) {
        require(!serial.isEmpty() && serial.toUtf8().size() < 1024 && !serial.contains(QChar(0)), QStringLiteral("无效设备序列号"));
        check(cancel_);
        const bool testing = qEnvironmentVariableIsSet("TWINFILE_TEST_MODE") && qEnvironmentVariableIsSet("TWINFILE_ADB_TEST_PORT");
        if (!testing) { const Result started = runProcess(toolPath(QStringLiteral("adb/adb.exe")), {QStringLiteral("start-server")}, 15000, cancel_); require(started.ok, started.error); }
        const int port = testing ? qEnvironmentVariableIntValue("TWINFILE_ADB_TEST_PORT") : 5037;
        require(port > 0 && port <= 65535, QStringLiteral("无效 ADB 端口"));
        auto reconnect = [&] { socket_.abort(); socket_.connectToHost(QHostAddress::LocalHost, quint16(port)); require(socket_.waitForConnected(5000), QStringLiteral("无法连接本机 ADB 服务：") + socket_.errorString()); };
        reconnect();
        try {
            service(QByteArray("host-serial:") + serial.toUtf8() + ":features");
            bool ok = false; const quint32 count = read(4).toUInt(&ok, 16); require(ok && count <= 65536, QStringLiteral("ADB 特性响应无效"));
            const auto features = read(count).split(','); stat2_ = features.contains("stat_v2"); list2_ = features.contains("ls_v2");
        } catch (const SyncError &) { check(cancel_); }
        reconnect();
        service(QByteArray("host:transport:") + serial.toUtf8());
        service("sync:");
    }
    ~Sync() { socket_.abort(); }
    QByteArray read(qsizetype count) {
        require(count >= 0 && count <= 1024 * 1024, QStringLiteral("ADB 同步读取尺寸超限"));
        QByteArray bytes;
        QElapsedTimer timer; timer.start();
        while (bytes.size() < count) {
            check(cancel_);
            if (!socket_.bytesAvailable()) {
                require(socket_.state() == QAbstractSocket::ConnectedState && timer.elapsed() < 30000, QStringLiteral("ADB 同步连接断开或超时"));
                socket_.waitForReadyRead(100);
                continue;
            }
            bytes += socket_.read(count - bytes.size());
            timer.restart();
        }
        return bytes;
    }
    void send(const QByteArray &bytes) {
        check(cancel_);
        require(bytes.size() <= 1024 * 1024 && socket_.write(bytes) == bytes.size(), QStringLiteral("ADB 同步写入失败"));
        QElapsedTimer timer; timer.start();
        while (socket_.bytesToWrite()) {
            check(cancel_);
            require(socket_.state() == QAbstractSocket::ConnectedState && timer.elapsed() < 30000, QStringLiteral("ADB 同步写入超时"));
            if (socket_.waitForBytesWritten(100)) timer.restart();
        }
    }
    void request(const QByteArray &id, const QString &path) {
        const QByteArray bytes = path.toUtf8();
        require(path.startsWith(QChar('/')) && !path.contains(QChar(0)) && bytes.size() <= 1024, QStringLiteral("手机路径无效或超过 ADB v1 的 1024 字节上限"));
        send(id + word(quint32(bytes.size())) + bytes);
    }
    struct Item { QString path, name; quint32 mode = 0; quint64 size = 0; qint64 modified = 0; bool exact = false; };
    Item stat(const QString &path) {
        request(stat2_ ? "LST2" : "STAT", path);
        const QByteArray header = read(8);
        if (header.left(4) == "FAIL") fail(value(header, 4));
        if (stat2_) {
            require(header.left(4) == "LST2" && value(header, 4) == 0, QStringLiteral("手机 STAT v2 失败"));
            const QByteArray rest = read(64);
            return {path, path.section(QChar('/'), -1), value(rest, 16), qFromLittleEndian<quint64>(rest.constData() + 32), qFromLittleEndian<qint64>(rest.constData() + 48), true};
        }
        require(header.left(4) == "STAT", QStringLiteral("手机不支持 ADB 同步 STAT"));
        const QByteArray rest = read(8);
        const quint32 mode = value(header, 4);
        require(mode != 0, QStringLiteral("手机文件不存在或不可访问"));
        return {path, path.section(QChar('/'), -1), mode, value(rest), value(rest, 4)};
    }
    QList<Item> list(const QString &path) {
        request(list2_ ? "LIS2" : "LIST", path);
        QList<Item> items;
        QSet<QString> seen;
        for (;;) {
            const QByteArray header = read(8);
            if (header.left(4) == "FAIL") fail(value(header, 4));
            require(header.left(4) == (list2_ ? "DNT2" : "DENT") || header.left(4) == "DONE", QStringLiteral("手机 ADB 目录帧无效"));
            const QByteArray rest = read(list2_ ? 68 : 12);
            if (header.left(4) == "DONE") break;
            if (list2_) require(value(header, 4) == 0, QStringLiteral("手机目录项读取失败"));
            const quint32 length = value(rest, list2_ ? 64 : 8);
            require(length > 0 && length <= 1024 && items.size() < 200000, QStringLiteral("手机目录数量或名称超限"));
            const QByteArray bytes = read(length);
            const QString name = QString::fromUtf8(bytes);
            require(name.toUtf8() == bytes && !name.contains(QChar(0)) && !name.contains(QChar('/')), QStringLiteral("手机文件名编码无效"));
            if (name == QStringLiteral(".") || name == QStringLiteral("..")) continue;
            require(!seen.contains(name), QStringLiteral("手机返回了重复文件名"));
            seen.insert(name);
            items.append({QDir::cleanPath(path + QChar('/') + name), name, list2_ ? value(rest, 16) : value(header, 4), list2_ ? qFromLittleEndian<quint64>(rest.constData() + 32) : value(rest), list2_ ? qFromLittleEndian<qint64>(rest.constData() + 48) : value(rest, 4), list2_});
        }
        return items;
    }
    void pull(const Item &item, const QString &local, qint64 &total) {
        QFile output(local);
        require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly), output.errorString());
        receive(item, &output, QFileInfo(local).absolutePath(), total);
        require(output.flush(), output.errorString());
        output.setFileTime(QDateTime::fromSecsSinceEpoch(item.modified), QFileDevice::FileModificationTime);
    }
    void receive(const Item &item, QIODevice *output, const QString &storagePath, qint64 &total) {
        require((item.mode & 0170000U) == 0100000U && item.size <= fileLimit && output && output->isWritable(), QStringLiteral("拒绝读取手机链接、特殊文件、超限文件或无效输出"));
        request("RECV", item.path);
        qint64 size = 0;
        for (;;) {
            const QByteArray header = read(8), id = header.left(4);
            const quint32 count = value(header, 4);
            if (id == "FAIL") fail(count);
            if (id == "DONE") break;
            require(id == "DATA" && count > 0 && count <= 65536, QStringLiteral("ADB 文件帧无效"));
            require(size <= fileLimit - count && total <= totalLimit - count, QStringLiteral("ADB 文件或总量超限"));
            if (!storagePath.isEmpty()) { const QStorageInfo storage(storagePath); require(storage.bytesAvailable() < 0 || storage.bytesAvailable() > count, QStringLiteral("本地磁盘空间不足")); }
            const QByteArray bytes = read(count);
            require(output->write(bytes) == count, output->errorString());
            size += count; total += count;
        }
        require(item.exact ? quint64(size) == item.size : quint32(quint64(size) & 0xffffffffU) == item.size, QStringLiteral("手机文件在传输时变化或写入失败"));
    }
    void push(const QString &local, const QString &remote) {
        QFile input(local); const QFileInfo before(local);
        require(before.isFile() && !before.isSymLink() && !before.isJunction() && before.size() <= fileLimit && input.open(QIODevice::ReadOnly), QStringLiteral("本地文件无效、为链接或超限"));
        request("SEND", remote + QStringLiteral(",33206"));
        qint64 done = 0;
        while (!input.atEnd()) {
            check(cancel_);
            const QByteArray bytes = input.read(65536);
            require(!bytes.isEmpty() && input.error() == QFile::NoError, QStringLiteral("本地文件读取失败"));
            send(QByteArray("DATA") + word(quint32(bytes.size())) + bytes);
            done += bytes.size();
        }
        require(done == before.size() && QFileInfo(local).lastModified() == before.lastModified(), QStringLiteral("来源文件在传输时变化"));
        send(QByteArray("DONE") + word(quint32(qBound<qint64>(0, before.lastModified().toSecsSinceEpoch(), 0xffffffffLL))));
        const QByteArray header = read(8);
        if (header.left(4) == "FAIL") fail(value(header, 4));
        require(header == QByteArray("OKAY") + word(0), QStringLiteral("手机未确认文件写入"));
    }
private:
    void fail(quint32 count) { require(count <= 65536, QStringLiteral("ADB 错误消息超限")); throw SyncError(QStringLiteral("ADB：") + QString::fromUtf8(read(count))); }
    void service(const QByteArray &name) {
        send(QByteArray::number(name.size(), 16).rightJustified(4, '0') + name);
        const QByteArray status = read(4);
        if (status == "FAIL") { bool ok = false; const quint32 count = read(4).toUInt(&ok, 16); require(ok, QStringLiteral("ADB 服务错误长度无效")); fail(count); }
        require(status == "OKAY", QStringLiteral("ADB 服务握手失败"));
    }
    Cancellation cancel_;
    QTcpSocket socket_;
    bool stat2_ = false, list2_ = false;
};
}
Listing adbSyncList(const QString &serial, const QString &path, bool hidden, const Cancellation &cancel) {
    try {
        Sync sync(serial, cancel);
        Listing result;
        for (const auto &item : sync.list(path)) {
            if (!hidden && item.name.startsWith(QChar('.'))) continue;
            Entry entry; entry.name = item.name; entry.path = item.path; entry.serial = serial; entry.source = Source::Phone;
            entry.directory = (item.mode & 0170000U) == 0040000U; entry.size = qint64(qMin<quint64>(item.size, std::numeric_limits<qint64>::max())); entry.modified = QDateTime::fromSecsSinceEpoch(item.modified);
            if ((item.mode & 0170000U) == 0120000U) entry.detail = QStringLiteral("符号链接（不跟随传输）");
            result.entries.append(entry);
        }
        std::stable_sort(result.entries.begin(), result.entries.end(), [](const auto &first, const auto &second) { return first.directory != second.directory ? first.directory : first.name.toCaseFolded() < second.name.toCaseFolded(); });
        return result;
    } catch (const std::exception &error) { return {{}, QString::fromUtf8(error.what())}; }
}
Result adbSyncPush(const QString &serial, const QString &local, const QString &remote, const Cancellation &cancel) {
    try {
        Sync sync(serial, cancel);
        const QFileInfo root(local);
        require(root.exists() && !root.isSymLink() && !root.isJunction(), QStringLiteral("来源不存在或为链接"));
        QStringList directories, files;
        qint64 total = 0;
        auto add = [&](const QFileInfo &info) {
            require(!info.isSymLink() && !info.isJunction() && (info.isDir() || info.isFile()), QStringLiteral("不传输链接或特殊文件"));
            require(directories.size() + files.size() < 200000 && info.size() <= fileLimit, QStringLiteral("ADB 来源数量或文件尺寸超限"));
            if (info.isDir()) directories.append(info.filePath());
            else { require(total <= totalLimit - info.size(), QStringLiteral("ADB 来源总量超限")); total += info.size(); files.append(info.filePath()); }
        };
        add(root);
        if (root.isDir()) { QDirIterator iterator(local, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories); while (iterator.hasNext()) { check(cancel); iterator.next(); add(iterator.fileInfo()); } }
        auto target = [&](const QString &path) { return normalizedPath(path) == normalizedPath(local) ? remote : remote + QChar('/') + QDir(local).relativeFilePath(path); };
        for (qsizetype index = 0; index < directories.size(); index += 64) {
            QStringList quoted;
            for (qsizetype offset = index; offset < qMin(index + 64, directories.size()); ++offset) quoted.append(shellQuote(target(directories[offset])));
            const Result created = phoneCommand(serial, QStringLiteral("mkdir -p -- ") + quoted.join(QChar(' ')), cancel);
            require(created.ok, created.error);
        }
        for (const auto &path : files) sync.push(path, target(path));
        return {true, {}, {}, {remote}};
    } catch (const std::exception &error) { return {false, QString::fromUtf8(error.what()), {}, {}}; }
}
Result adbSyncPull(const QString &serial, const QString &remote, const QString &local, const Cancellation &cancel) {
    try {
        require(!QFileInfo::exists(local), QStringLiteral("本地暂存目标已经存在"));
        Sync sync(serial, cancel);
        const auto root = sync.stat(remote);
        QList<Sync::Item> files;
        QStringList directories;
        QSet<QString> names;
        std::function<void(const Sync::Item &, const QString &, int)> collect = [&](const auto &item, const QString &relative, int depth) {
            check(cancel);
            require(depth <= 128 && files.size() + directories.size() < 200000, QStringLiteral("手机目录层级或数量超限"));
            if (!relative.isEmpty()) require(!safeArchiveName(relative).isEmpty() && !names.contains(relative.toCaseFolded()), QStringLiteral("手机文件名不兼容 Windows 或大小写冲突"));
            names.insert(relative.toCaseFolded());
            const quint32 type = item.mode & 0170000U;
            require(type == 0040000U || type == 0100000U, QStringLiteral("不传输手机链接或特殊文件"));
            if (type == 0040000U) {
                directories.append(relative);
                for (const auto &child : sync.list(item.path)) collect(child, relative.isEmpty() ? child.name : relative + QChar('/') + child.name, depth + 1);
            } else { auto file = item; file.name = relative; files.append(file); }
        };
        collect(root, {}, 0);
        for (const auto &directory : directories) require(QDir().mkpath(directory.isEmpty() ? local : QDir(local).filePath(directory)), QStringLiteral("不能创建本地暂存目录"));
        qint64 total = 0;
        for (const auto &file : files) sync.pull(file, file.name.isEmpty() ? local : QDir(local).filePath(file.name), total);
        return {true, {}, {}, {local}};
    } catch (const std::exception &error) { return {false, QString::fromUtf8(error.what()), {}, {}}; }
}
Result adbSyncRead(const QString &serial, const QString &remote, QIODevice *output, const Cancellation &cancel) {
    try { Sync sync(serial, cancel); qint64 total = 0; sync.receive(sync.stat(remote), output, {}, total); return {}; }
    catch (const std::exception &error) { return {false, QString::fromUtf8(error.what()), {}, {}}; }
}
}
