#include <winsock2.h>
#include <ws2tcpip.h>
#include "hfx.h"
#include "operations.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <future>
#include <limits>
#include <stdexcept>
#include <vector>

namespace twinfile {
namespace {
constexpr qint64 blockSize = 1024 * 1024;
class HfxError : public std::runtime_error {
public:
    explicit HfxError(const QString &message) : std::runtime_error(message.toUtf8().constData()) {}
};
void require(bool condition, const QString &message) { if (!condition) throw HfxError(message); }
void cancelled(const Cancellation &cancel) { require(!cancel || !cancel->load(), QStringLiteral("HFX 已取消，当前连接将关闭")); }
void releaseSocket(SOCKET &socket) { if (socket != INVALID_SOCKET) { shutdown(socket, SD_BOTH); closesocket(socket); socket = INVALID_SOCKET; } }
void tune(SOCKET socket) {
    const int enabled = 1, size = 4 * 1024 * 1024; setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&enabled), sizeof(enabled));
    setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&size), sizeof(size)); setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&size), sizeof(size));
    u_long nonblocking = 1; require(ioctlsocket(socket, FIONBIO, &nonblocking) == 0, QStringLiteral("不能设置 HFX socket"));
}
void ready(SOCKET socket, bool write, const Cancellation &cancel, int timeout = 60000, const Cancellation &secondary = {}) {
    QElapsedTimer elapsed; elapsed.start();
    for (;;) {
        cancelled(cancel); cancelled(secondary); require(elapsed.elapsed() < timeout, QStringLiteral("HFX 通道等待超时")); fd_set sockets; FD_ZERO(&sockets); FD_SET(socket, &sockets); timeval interval{0, 100000};
        const int status = select(0, write ? nullptr : &sockets, write ? &sockets : nullptr, nullptr, &interval); require(status != SOCKET_ERROR, QStringLiteral("HFX 通道已断开")); if (status > 0) return;
    }
}
class Wire {
public:
    SOCKET socket = INVALID_SOCKET;
    Cancellation cancel;
    Cancellation secondary;
    QByteArray read(qint64 size) {
        require(size >= 0 && size <= blockSize + 65536, QStringLiteral("HFX 数据块超限")); QByteArray bytes(size, '\0'); qint64 offset = 0;
        while (offset < size) { ready(socket, false, cancel, 60000, secondary); const int count = recv(socket, bytes.data() + offset, int(size - offset), 0); if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK) continue; require(count > 0, QStringLiteral("HFX 对端断开连接")); offset += count; }
        return bytes;
    }
    void send(const QByteArray &bytes) {
        qsizetype offset = 0;
        while (offset < bytes.size()) { ready(socket, true, cancel, 60000, secondary); const int count = ::send(socket, bytes.constData() + offset, int(qMin<qsizetype>(bytes.size() - offset, blockSize)), 0); if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK) continue; require(count > 0, QStringLiteral("HFX 通道写入失败")); offset += count; }
    }
    template<class Integer> Integer number() { const QByteArray bytes = read(sizeof(Integer)); return qFromBigEndian<Integer>(bytes.constData()); }
    template<class Integer> void number(Integer value) { QByteArray bytes(sizeof(Integer), '\0'); qToBigEndian<Integer>(value, bytes.data()); send(bytes); }
    bool boolean() { const quint8 value = number<quint8>(); require(value <= 1, QStringLiteral("HFX 布尔值无效")); return value != 0; }
    void boolean(bool value) { number<quint8>(value ? 1 : 0); }
    QString text() { const quint16 length = number<quint16>(); const QByteArray bytes = read(length); const QString value = QString::fromUtf8(bytes); require(value.toUtf8() == bytes && !value.contains(QChar(0)), QStringLiteral("HFX UTF-8 路径无效")); return value; }
    void text(const QString &value) { const QByteArray bytes = value.toUtf8(); require(bytes.size() <= 65535 && !value.contains(QChar(0)), QStringLiteral("HFX 路径超长或无效")); number<quint16>(quint16(bytes.size())); send(bytes); }
};
Result failure(const std::exception &error) { return {false, QString::fromUtf8(error.what()), {}, {}}; }
QString joinRemote(const QString &directory, const QString &relative) { return directory.endsWith(QChar('/')) ? directory + relative : directory + QChar('/') + relative; }
// File and locale APIs also need Qt-owned worker teardown on Windows.
class ChannelTasks {
public:
    ~ChannelTasks() { for (auto &thread : threads_) thread->wait(); }
    template<class Function> void start(Function function) {
        std::packaged_task<void()> task(std::move(function));
        results_.emplace_back(task.get_future());
        threads_.emplace_back(QThread::create([task = std::move(task)]() mutable { task(); }));
        threads_.back()->start();
    }
    void wait() {
        std::exception_ptr error;
        for (auto &result : results_) try { result.get(); } catch (...) { if (!error) error = std::current_exception(); }
        for (auto &thread : threads_) thread->wait();
        if (error) std::rethrow_exception(error);
    }
private:
    std::vector<std::future<void>> results_;
    std::vector<std::unique_ptr<QThread>> threads_;
};
}

struct HfxSession::State {
    SOCKET control = INVALID_SOCKET;
    std::vector<SOCKET> channels;
    HfxOptions options;
    QString home;
    QStringList names;
    int filesystem = 0;
    std::atomic_bool online{false};
    quint16 reversePort = 0;
    void close() {
        online.store(false);
        releaseSocket(control);
        for (auto &channel : channels) releaseSocket(channel);
        channels.clear();
        if (reversePort && !options.serial.isEmpty()) runProcess(toolPath(QStringLiteral("adb/adb.exe")), {QStringLiteral("-s"), options.serial, QStringLiteral("reverse"), QStringLiteral("--remove"), QStringLiteral("tcp:%1").arg(reversePort)}, 10000);
        reversePort = 0;
    }
};

HfxSession::HfxSession() : state_(std::make_unique<State>()) {
    static std::once_flag initialized; std::call_once(initialized, [] { WSADATA data{}; require(WSAStartup(MAKEWORD(2, 2), &data) == 0, QStringLiteral("Winsock 初始化失败")); });
}
HfxSession::~HfxSession() { close(); }
bool HfxSession::connected() const { return state_->online.load(); }
QString HfxSession::home() const { return state_->home; }
void HfxSession::close() { std::lock_guard guard(operation_); state_->close(); }

Result HfxSession::connect(const HfxOptions &options, const Cancellation &cancel, std::function<void(quint16)> listening) {
    std::lock_guard guard(operation_); state_->close(); state_->options = options; state_->names.clear(); SOCKET server = INVALID_SOCKET;
    try {
        cancelled(cancel); require(!options.interfaces.isEmpty() && options.interfaces.size() <= 8, QStringLiteral("请选择 1–8 条 IPv4 通道"));
        bool loopback = true; for (const auto &interface : options.interfaces) { require(interface.second.protocol() == QAbstractSocket::IPv4Protocol, QStringLiteral("HFX v300 仅支持 IPv4 通道")); loopback &= interface.second.isLoopback(); }
        server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); require(server != INVALID_SOCKET, QStringLiteral("不能创建 HFX 监听"));
        const int exclusive = 1; setsockopt(server, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(options.port); address.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
        require(bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 && listen(server, 9) == 0, QStringLiteral("HFX 端口被占用或无法监听"));
        int addressSize = sizeof(address); getsockname(server, reinterpret_cast<sockaddr *>(&address), &addressSize); const quint16 port = ntohs(address.sin_port); tune(server);
        if (listening) listening(port);
        if (!options.serial.isEmpty()) {
            const Result reversed = runProcess(toolPath(QStringLiteral("adb/adb.exe")), {QStringLiteral("-s"), options.serial, QStringLiteral("reverse"), QStringLiteral("tcp:%1").arg(port), QStringLiteral("tcp:%1").arg(port)}, 15000, cancel); require(reversed.ok, reversed.error);
            state_->reversePort = port;
            phoneCommand(options.serial, QStringLiteral("am force-stop top.weixiansen574.hybridfilexfer"), cancel);
            const Result launched = phoneCommand(options.serial, QStringLiteral("am start -n top.weixiansen574.hybridfilexfer/.ClientActivity --ei io_mode 0 --es controller_ip 127.0.0.1 --es home_dir /storage/emulated/0"), cancel); require(launched.ok, launched.error);
        }
        auto acceptChannel = [&](int timeout) { ready(server, false, cancel, timeout); const SOCKET channel = accept(server, nullptr, nullptr); require(channel != INVALID_SOCKET, QStringLiteral("接受 HFX 连接失败")); tune(channel); return channel; };
        state_->control = acceptChannel(90000); Wire control{state_->control, cancel}; require(control.read(4) == "HFXC", QStringLiteral("不是 HFX 控制连接"));
        if (control.number<qint32>() != 300) { control.boolean(false); control.number<qint32>(300); throw HfxError(QStringLiteral("HFX 协议版本不匹配，需要 v300 手机端")); }
        control.boolean(true); control.number<qint32>(qint32(options.interfaces.size()));
        for (const auto &interface : options.interfaces) { control.text(interface.first); control.number<quint8>(4); control.number<quint32>(interface.second.toIPv4Address()); control.number<quint8>(0); }
        for (const auto &interface : options.interfaces) {
            const bool ok = control.boolean(); const QString name = control.text(); require(ok, QStringLiteral("手机无法连接通道：") + interface.first + QStringLiteral("；请检查网络与防火墙，程序不会自动改动防火墙"));
            state_->channels.push_back(acceptChannel(15000)); state_->names.append(name); control.boolean(true);
        }
        control.number<qint32>(64); require(control.boolean(), QStringLiteral("手机缓冲区分配失败")); control.boolean(true); state_->filesystem = control.number<qint32>(); state_->home = control.text();
        require(state_->filesystem == 0 && state_->home.startsWith(QChar('/')), QStringLiteral("当前只接受 Android/UNIX 手机文件系统"));
        state_->online.store(true); releaseSocket(server); return {true, {}, QStringLiteral("已连接：") + state_->names.join(QStringLiteral(" + ")), {state_->home}};
    } catch (const std::exception &error) { releaseSocket(server); state_->close(); return failure(error); }
}

Listing HfxSession::list(const QString &path, const Cancellation &cancel) {
    std::lock_guard guard(operation_);
    try {
        require(connected(), QStringLiteral("HFX 尚未连接")); Wire control{state_->control, cancel}; control.number<qint16>(1); control.text(path); const qint32 count = control.number<qint32>(); require(count >= 0 && count <= 200000, QStringLiteral("HFX 目录读取失败或条目过多")); Listing listing;
        for (int index = 0; index < count; ++index) { Entry entry; entry.name = control.text(); entry.path = control.text(); entry.modified = QDateTime::fromMSecsSinceEpoch(control.number<qint64>()); entry.size = control.number<qint64>(); entry.directory = control.boolean(); entry.source = Source::Phone; entry.serial = state_->options.serial; listing.entries.append(entry); }
        return listing;
    } catch (const std::exception &error) { state_->close(); return {{}, QString::fromUtf8(error.what())}; }
}
Result HfxSession::mkdir(const QString &parent, const QString &name, const Cancellation &cancel) {
    std::lock_guard guard(operation_);
    try { require(connected() && validateName(name).isEmpty(), QStringLiteral("HFX 未连接或名称无效")); Wire control{state_->control, cancel}; control.number<qint16>(3); control.text(parent); control.text(name); require(control.boolean(), QStringLiteral("手机新建文件夹失败")); return {}; } catch (const std::exception &error) { state_->close(); return failure(error); }
}
Result HfxSession::remove(const QString &path, const Cancellation &cancel) {
    std::lock_guard guard(operation_);
    const QString clean = QDir::cleanPath(path);
    const QStringList protectedPaths{QStringLiteral("/"), QStringLiteral("/sdcard"), QStringLiteral("/storage"), QStringLiteral("/storage/emulated"), QStringLiteral("/storage/emulated/0"), QDir::cleanPath(state_->home)};
    if (!path.startsWith(QChar('/')) || path.contains(QChar(0)) || protectedPaths.contains(clean) || clean.count(QChar('/')) < 2) return {false, QStringLiteral("禁止删除手机根目录或共享存储根目录"), {}, {}};
    try { require(connected(), QStringLiteral("HFX 未连接")); Wire control{state_->control, cancel}; control.number<qint16>(2); control.text(clean); require(control.boolean(), QStringLiteral("手机删除失败")); return {}; } catch (const std::exception &error) { state_->close(); return failure(error); }
}

Result HfxSession::push(const QStringList &paths, const QString &destination, const Cancellation &cancel, const Progress &progress) {
    std::lock_guard guard(operation_);
    try {
        require(connected() && destination.startsWith(QChar('/')) && !paths.isEmpty(), QStringLiteral("HFX 未连接或目标无效"));
        struct File { QString local, remote; qint64 size, modified; bool directory; }; QList<File> files; QSet<QString> remoteNames; qint64 total = 0;
        auto append = [&](const QString &path, const QString &remote) { const QFileInfo info(path); require(info.exists() && !info.isSymLink() && !info.isJunction() && files.size() < 200000, QStringLiteral("来源不存在、包含链接或条目过多")); require(!remoteNames.contains(remote), QStringLiteral("远程目标名称重复")); remoteNames.insert(remote); files.append({path, remote, info.isDir() ? 0 : info.size(), info.lastModified().toMSecsSinceEpoch(), info.isDir()}); total += info.isDir() ? 0 : info.size(); };
        for (const QString &root : paths) {
            cancelled(cancel); const QString remoteRoot = joinRemote(destination, QFileInfo(root).fileName());
            Wire control{state_->control, cancel}; control.number<qint16>(1); control.text(destination); const int count = control.number<qint32>(); require(count >= 0 && count <= 200000, QStringLiteral("不能确认 HFX 目标是否存在"));
            bool exists = false; for (int index = 0; index < count; ++index) { const QString name = control.text(); control.text(); control.number<qint64>(); control.number<qint64>(); control.boolean(); exists |= name == QFileInfo(root).fileName(); } require(!exists, QStringLiteral("HFX 目标已存在，不覆盖：") + remoteRoot);
            append(root, remoteRoot);
            if (QFileInfo(root).isDir()) { QDirIterator iterator(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories); while (iterator.hasNext()) { iterator.next(); append(iterator.filePath(), joinRemote(remoteRoot, QDir(root).relativeFilePath(iterator.filePath()))); } }
        }
        Wire control{state_->control, cancel}; control.number<qint16>(10); std::mutex schedule, reports; qsizetype fileIndex = 0; qint32 block = 0; std::atomic<qint64> done{0}; auto aborted = std::make_shared<std::atomic_bool>(false);
        ChannelTasks tasks;
        for (const SOCKET socket : state_->channels) tasks.start([&, socket] {
            try {
                Wire channel{socket, cancel, aborted};
                for (;;) {
                    cancelled(cancel); require(!aborted->load(), QStringLiteral("其它 HFX 通道已失败")); qsizetype current; qint32 currentBlock;
                    { std::lock_guard next(schedule); if (fileIndex >= files.size()) break; current = fileIndex; currentBlock = block; const File &file = files[current]; if (file.directory || (qint64(++block) * blockSize >= file.size)) { ++fileIndex; block = 0; } }
                    const File &file = files[current]; channel.number<qint16>(file.directory ? 1 : 0); channel.number<qint32>(qint32(current)); channel.text(file.remote); channel.number<qint64>(file.modified);
                    if (file.directory) continue;
                    const qint64 count = qMin(blockSize, file.size - qint64(currentBlock) * blockSize); QFile input(file.local); require(input.open(QIODevice::ReadOnly) && input.size() == file.size && input.seek(qint64(currentBlock) * blockSize), QStringLiteral("HFX 来源变化或不能读取")); const QByteArray bytes = input.read(count); require(bytes.size() == count, QStringLiteral("HFX 来源读取不完整"));
                    channel.number<qint64>(file.size); channel.number<qint32>(currentBlock); channel.number<qint32>(qint32(count)); channel.send(bytes); const qint64 completed = done.fetch_add(count) + count;
                    if (progress) { std::lock_guard report(reports); progress(completed, total, QFileInfo(file.local).fileName()); }
                }
                channel.number<qint16>(3);
            } catch (...) { aborted->store(true); throw; }
        });
        tasks.wait();
        if (!control.boolean()) throw HfxError(QStringLiteral("手机写入失败：") + control.text()); control.boolean(true); return {};
    } catch (const std::exception &error) { state_->close(); return failure(error); }
}

Result HfxSession::pull(const QStringList &paths, const QString &source, const QString &destination, const Cancellation &cancel, const Progress &progress) {
    std::lock_guard guard(operation_);
    try {
        require(connected() && !paths.isEmpty() && QFileInfo(destination).isDir(), QStringLiteral("HFX 未连接或目标不是文件夹"));
        QSet<QString> roots; for (const QString &path : paths) { require(path.startsWith(source.endsWith(QChar('/')) ? source : source + QChar('/')), QStringLiteral("选择的手机文件不在来源目录")); const QString relative = path.mid(source.size()).remove(0, source.endsWith(QChar('/')) ? 0 : 1); require(!relative.contains(QChar('/')) && validateName(relative).isEmpty(), QStringLiteral("请选择同一目录的文件")); require(!QFileInfo::exists(QDir(destination).filePath(relative)), QStringLiteral("本地目标已存在：") + relative); roots.insert(relative); }
        QTemporaryDir staging(QDir(destination).filePath(QStringLiteral(".twinfile-hfx-XXXXXX"))); require(staging.isValid(), QStringLiteral("不能创建 HFX 暂存目录"));
        Wire control{state_->control, cancel}; control.number<qint16>(11); control.number<qint32>(qint32(paths.size())); for (const QString &path : paths) control.text(path);
        control.text(QDir::toNativeSeparators(staging.path()) + QChar('\\')); control.number<qint32>(1); control.text(source.endsWith(QChar('/')) ? source : source + QChar('/'));
        struct FileState { QString path; qint64 total = 0, modified = 0; QSet<qint32> blocks; bool directory = false; };
        QHash<qint32, FileState> files; QHash<QString, qint32> names; std::mutex write; qint64 done = 0, total = 0; auto aborted = std::make_shared<std::atomic_bool>(false); ChannelTasks tasks;
        for (const SOCKET socket : state_->channels) tasks.start([&, socket] {
            try {
                Wire channel{socket, cancel, aborted};
                for (;;) {
                    cancelled(cancel); require(!aborted->load(), QStringLiteral("其它 HFX 通道失败")); const qint16 type = channel.number<qint16>(); if (type == 3) break; require(type == 0 || type == 1, QStringLiteral("手机传输中断或发送了无效帧"));
                    const qint32 index = channel.number<qint32>(); const QString raw = QDir::fromNativeSeparators(channel.text()); const qint64 modified = channel.number<qint64>();
                    const QString absolute = QDir::cleanPath(QFileInfo(raw).isAbsolute() ? raw : QDir(staging.path()).filePath(raw)); const QString relative = QDir(staging.path()).relativeFilePath(absolute);
                    require(index >= 0 && !safeArchiveName(relative).isEmpty() && normalizedPath(absolute).startsWith(normalizedPath(staging.path()) + QChar('/')) && roots.contains(relative.section(QChar('/'), 0, 0)), QStringLiteral("手机返回了越界或不安全路径"));
                    qint64 size = 0; qint32 block = -1, length = 0; QByteArray bytes;
                    if (type == 0) { size = channel.number<qint64>(); block = channel.number<qint32>(); length = channel.number<qint32>(); require(size >= 0 && size <= 256LL * 1024 * 1024 * 1024 && block >= 0 && qint64(block) * blockSize <= size && length >= 0 && length == qMin(blockSize, size - qint64(block) * blockSize) && (length > 0 || (size == 0 && block == 0)), QStringLiteral("手机文件块尺寸无效")); bytes = channel.read(length); }
                    std::lock_guard guard(write); require(files.size() < 200000 || files.contains(index), QStringLiteral("HFX 文件数超限"));
                    if (!files.contains(index)) {
                        require(!names.contains(normalizedPath(absolute)), QStringLiteral("手机发送了重复目标路径")); names.insert(normalizedPath(absolute), index);
                        FileState state; state.path = absolute; state.total = size; state.modified = modified; state.directory = type == 1; files.insert(index, state); total += size;
                        const QStorageInfo storage(staging.path()); require(storage.bytesAvailable() < 0 || size <= storage.bytesAvailable(), QStringLiteral("磁盘空间不足"));
                        require(QDir().mkpath(type == 1 ? absolute : QFileInfo(absolute).absolutePath()), QStringLiteral("不能创建 HFX 目录"));
                        if (type == 0) { QFile output(absolute); require(output.open(QIODevice::ReadWrite | QIODevice::NewOnly) && output.resize(size), QStringLiteral("不能创建 HFX 文件")); }
                    }
                    auto &state = files[index]; require(state.path == absolute && state.total == size && state.directory == (type == 1) && state.modified == modified, QStringLiteral("手机文件编号、大小或类型不一致"));
                    require(!state.blocks.contains(block), QStringLiteral("手机发送了重复文件块")); state.blocks.insert(block);
                    if (type == 0) { QFile output(absolute); require(output.open(QIODevice::ReadWrite) && output.seek(qint64(block) * blockSize) && output.write(bytes) == length && output.flush(), QStringLiteral("HFX 写入失败")); }
                    done += length; if (progress) progress(done, total, relative);
                }
            } catch (...) { aborted->store(true); throw; }
        });
        tasks.wait();
        for (const auto &file : files) { if (file.directory) continue; require(file.blocks.size() == qMax<qint64>(1, (file.total + blockSize - 1) / blockSize), QStringLiteral("手机文件块不完整")); QFile output(file.path); if (output.open(QIODevice::ReadWrite)) output.setFileTime(QDateTime::fromMSecsSinceEpoch(file.modified), QFileDevice::FileModificationTime); }
        for (const QString &root : roots) require(QFileInfo::exists(staging.filePath(root)), QStringLiteral("手机未发送所选文件：") + root);
        control.boolean(true); if (!control.boolean()) throw HfxError(QStringLiteral("手机读取失败：") + control.text()); cancelled(cancel);
        QStringList local; for (const QString &root : roots) local.append(staging.filePath(root)); return transferFiles(local, destination, true, cancel);
    } catch (const std::exception &error) { state_->close(); return failure(error); }
}

}
