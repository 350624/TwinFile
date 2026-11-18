#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "hfx.h"
#include "operations.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QScopeGuard>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <stdexcept>
#include <utility>
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
// Control replies and idle lanes share the transfer's activity, not its total duration.
void ready(SOCKET socket, bool write, const Cancellation &cancel, int timeout = 60000, const Cancellation &secondary = {}, std::atomic<qint64> *activity = nullptr, int startTimeout = 0) {
    qint64 seen = activity ? activity->load(std::memory_order_relaxed) : 0; QElapsedTimer elapsed; elapsed.start();
    for (;;) {
        cancelled(cancel); cancelled(secondary);
        if (activity) { const qint64 latest = activity->load(std::memory_order_relaxed); if (latest != seen) { seen = latest; elapsed.restart(); } }
        const int limit = startTimeout > 0 && (!activity || seen == 0) ? startTimeout : timeout;
        require(elapsed.elapsed() < limit, QStringLiteral("HFX 通道等待超时")); fd_set sockets; FD_ZERO(&sockets); FD_SET(socket, &sockets); timeval interval{0, 100000};
        const int status = select(0, write ? nullptr : &sockets, write ? &sockets : nullptr, nullptr, &interval); require(status != SOCKET_ERROR, QStringLiteral("HFX 通道已断开")); if (status > 0) return;
    }
}
class Wire {
public:
    Wire(SOCKET socket, const Cancellation &cancel, const Cancellation &secondary = {}, int timeout = 60000,
         int firstReadTimeout = 0, std::atomic<qint64> *activity = nullptr)
        : socket(socket), cancel(cancel), secondary(secondary), timeout(timeout), firstReadTimeout(firstReadTimeout), activity(activity) {}

    SOCKET socket;
    Cancellation cancel;
    Cancellation secondary;
    int timeout = 60000;
    int firstReadTimeout = 0;
    std::atomic<qint64> *activity = nullptr;
    void read(char *destination, qsizetype size) {
        while (size > 0) {
            cancelled(cancel); cancelled(secondary);
            if (begin_ != end_) {
                const qsizetype count = qMin(size, end_ - begin_);
                std::memcpy(destination, buffer_.data() + begin_, size_t(count));
                begin_ += count; destination += count; size -= count;
                continue;
            }
            // Read adjacent metadata fields together; large payloads bypass the read-ahead buffer.
            if (size <= 1024) {
                begin_ = 0; end_ = receive(buffer_.data(), int(buffer_.size()));
            } else {
                const int count = receive(destination, int(size));
                destination += count; size -= count;
            }
        }
    }
    QByteArray read(qint64 size) {
        require(size >= 0 && size <= blockSize + 65536, QStringLiteral("HFX 数据块超限"));
        QByteArray bytes(size, Qt::Uninitialized); read(bytes.data(), size);
        return bytes;
    }
    void send(QByteArrayView bytes) {
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            cancelled(cancel); cancelled(secondary);
            const int count = ::send(socket, bytes.data() + offset, int(bytes.size() - offset), 0);
            if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK) { ready(socket, true, cancel, timeout, secondary, activity); continue; }
            require(count > 0, QStringLiteral("HFX 通道写入失败"));
            offset += count; if (activity) activity->fetch_add(1, std::memory_order_relaxed);
        }
    }
    template<class Integer> Integer number() { char bytes[sizeof(Integer)]; read(bytes, sizeof(bytes)); return qFromBigEndian<Integer>(bytes); }
    template<class Integer> void number(Integer value) { char bytes[sizeof(Integer)]; qToBigEndian<Integer>(value, bytes); send(QByteArrayView(bytes, sizeof(bytes))); }
    bool boolean() { const quint8 value = number<quint8>(); require(value <= 1, QStringLiteral("HFX 布尔值无效")); return value != 0; }
    void boolean(bool value) { number<quint8>(value ? 1 : 0); }
    QString text() { const quint16 length = number<quint16>(); const QByteArray bytes = read(length); const QString value = QString::fromUtf8(bytes); require(value.toUtf8() == bytes && !value.contains(QChar(0)), QStringLiteral("HFX UTF-8 路径无效")); return value; }
    void text(const QString &value) { QByteArray bytes; appendText(bytes, value); send(bytes); }
    template<class Integer> static void appendNumber(QByteArray &bytes, Integer value) {
        const qsizetype end = bytes.size(); bytes.resizeForOverwrite(end + sizeof(Integer)); qToBigEndian<Integer>(value, bytes.data() + end);
    }
    static void appendText(QByteArray &buffer, const QString &value) {
        const QByteArray bytes = value.toUtf8();
        require(bytes.size() <= 65535 && !value.contains(QChar(0)), QStringLiteral("HFX 路径超长或无效"));
        appendNumber(buffer, quint16(bytes.size())); buffer.append(bytes);
    }
private:
    int receive(char *destination, int size) {
        for (;;) {
            const int count = recv(socket, destination, size, 0);
            if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
                ready(socket, false, cancel, timeout, secondary, activity, firstReadTimeout);
                continue;
            }
            require(count > 0, QStringLiteral("HFX 对端断开连接"));
            firstReadTimeout = 0;
            if (activity) activity->fetch_add(1, std::memory_order_relaxed);
            return count;
        }
    }
    std::array<char, 16 * 1024> buffer_;
    qsizetype begin_ = 0, end_ = 0;
};
Result failure(const std::exception &error) { return {false, QString::fromUtf8(error.what()), {}, {}}; }
QString joinRemote(const QString &directory, const QString &relative) { return directory.endsWith(QChar('/')) ? directory + relative : directory + QChar('/') + relative; }

Result publishPull(const QTemporaryDir &staging, const QDir &destination, const QSet<QString> &roots, const Cancellation &cancel) {
    QStringList sources, targets;
    for (const auto &root : roots) {
        sources.append(staging.filePath(root)); targets.append(destination.filePath(root));
        require(QFileInfo::exists(sources.last()), QStringLiteral("手机未发送所选文件：") + root);
        require(!QFileInfo::exists(targets.last()), QStringLiteral("本地目标已存在：") + root);
    }
    // The private staging directory is directly under the destination. Commit by rename only.
    const auto rename = [](const QString &from, const QString &to) {
        const auto nativePath = [](const QString &path) {
            const QString native = QDir::toNativeSeparators(path);
            if (native.startsWith(QStringLiteral("\\\\?\\"))) return native;
            if (native.startsWith(QStringLiteral("\\\\"))) return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
            return QStringLiteral("\\\\?\\") + native;
        };
        const QString source = nativePath(from), target = nativePath(to);
        return MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()), reinterpret_cast<LPCWSTR>(target.utf16()), 0) != 0;
    };
    qsizetype finished = 0;
    for (; finished < sources.size() && (!cancel || !cancel->load()); ++finished)
        if (!rename(sources[finished], targets[finished])) break;
    if (finished == sources.size()) return {true, {}, {}, targets};
    QStringList notRestored;
    while (finished > 0) {
        --finished;
        if (!rename(targets[finished], sources[finished])) notRestored.append(targets[finished]);
    }
    QString error = QStringLiteral("HFX 回传提交取消或失败");
    if (!notRestored.isEmpty()) error += QStringLiteral("；未能移回暂存目录的项目：\n") + notRestored.join(QChar('\n'));
    return {false, error, {}, {}};
}

// File and locale APIs also need Qt-owned worker teardown on Windows.
class ChannelTasks {
public:
    ~ChannelTasks() { if (aborted_) aborted_->store(true); for (auto &thread : threads_) thread->wait(); }
    template<class Function> void start(Function function, const Cancellation &aborted) {
        aborted_ = aborted;
        std::packaged_task<void()> task([this, function = std::move(function), aborted] {
            try { function(); }
            catch (...) {
                { std::lock_guard guard(errors_); if (!error_) error_ = std::current_exception(); }
                aborted->store(true);
                throw;
            }
        });
        results_.emplace_back(task.get_future());
        threads_.emplace_back(QThread::create([task = std::move(task)]() mutable { task(); }));
        threads_.back()->start();
    }
    void wait() {
        for (auto &result : results_) try { result.get(); } catch (...) {}
        for (auto &thread : threads_) thread->wait();
        if (error_) std::rethrow_exception(error_);
    }
private:
    std::vector<std::future<void>> results_;
    std::vector<std::unique_ptr<QThread>> threads_;
    std::mutex errors_;
    std::exception_ptr error_;
    Cancellation aborted_;
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

bool hfxStorageAccessDenied(const QString &appOpsOutput) {
    const auto options = QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption;
    static const QRegularExpression uidMode(QStringLiteral("^[\\t ]*Uid mode:[\\t ]*MANAGE_EXTERNAL_STORAGE:[\\t ]*([a-z_]+)\\b"), options);
    static const QRegularExpression packageMode(QStringLiteral("^[\\t ]*MANAGE_EXTERNAL_STORAGE:[\\t ]*([a-z_]+)\\b"), options);
    static const QRegularExpression noOperations(QStringLiteral("^[\\t ]*No operations\\.[\\t ]*\\r?$"), options);
    const auto denied = [](const QString &mode) {
        return mode == QStringLiteral("ignore") || mode == QStringLiteral("deny") || mode == QStringLiteral("errored");
    };
    const auto uid = uidMode.match(appOpsOutput);
    const QString uidValue = uid.captured(1).toLower();
    // A UID override wins over package state, including old package rejection history.
    if (uid.hasMatch() && uidValue != QStringLiteral("default")) return denied(uidValue);
    const auto package = packageMode.match(appOpsOutput);
    if (package.hasMatch()) {
        const QString mode = package.captured(1).toLower();
        return denied(mode) || mode == QStringLiteral("default");
    }
    return uidValue == QStringLiteral("default") || noOperations.match(appOpsOutput).hasMatch();
}

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
        require(options.idleTimeoutMs > 0 && options.startTimeoutMs >= options.idleTimeoutMs, QStringLiteral("HFX 超时设置无效"));
        if (!options.serial.isEmpty()) {
            const Result permission = phoneCommand(options.serial,
                QStringLiteral("if [ \"$(getprop ro.build.version.sdk)\" -ge 30 ]; then cmd appops get top.weixiansen574.hybridfilexfer MANAGE_EXTERNAL_STORAGE; fi"), cancel);
            require(permission.ok, permission.error);
            if (hfxStorageAccessDenied(permission.output)) {
                const Result settings = phoneCommand(options.serial,
                    QStringLiteral("am start -a android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION -d package:top.weixiansen574.hybridfilexfer"), cancel);
                throw HfxError(QStringLiteral("HFX 手机端未获“所有文件访问权限”，无法读取或写入共享存储。")
                    + (settings.ok ? QStringLiteral("请在手机打开的授权页开启“允许管理所有文件”，然后重新连接。")
                                   : QStringLiteral("请到手机设置中为 HFX 开启“允许管理所有文件”，然后重新连接。")));
            }
        }
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
        QSet<QString> names;
        for (int index = 0; index < count; ++index) {
            Entry entry; entry.name = control.text(); entry.path = control.text(); entry.modified = QDateTime::fromMSecsSinceEpoch(control.number<qint64>()); entry.size = control.number<qint64>(); entry.directory = control.boolean();
            require(!entry.name.isEmpty() && entry.name != QStringLiteral(".") && entry.name != QStringLiteral("..") && !entry.name.contains(QChar('/')) && !entry.name.contains(QChar('\\')) && entry.path == QDir::cleanPath(joinRemote(path, entry.name)) && !names.contains(entry.name) && entry.size >= 0, QStringLiteral("HFX 返回了无效、越界或重复目录条目"));
            names.insert(entry.name); entry.source = Source::Phone; entry.serial = state_->options.serial; listing.entries.append(entry);
        }
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
        struct File { QString local, remote, name; qint64 size, modified; bool directory; }; QList<File> files; QSet<QString> remoteNames; qint64 total = 0;
        auto append = [&](const QString &path, const QString &remote) {
            cancelled(cancel);
            const QFileInfo info(path); require(info.exists() && !info.isSymLink() && !info.isJunction() && files.size() < 200000, QStringLiteral("来源不存在、包含链接或条目过多"));
            require(!remoteNames.contains(remote), QStringLiteral("远程目标名称重复")); remoteNames.insert(remote);
            const qint64 size = info.isDir() ? 0 : info.size();
            files.append({path, remote, info.fileName(), size, info.lastModified().toMSecsSinceEpoch(), info.isDir()}); total += size;
        };
        QSet<QString> existing;
        {
            Wire control{state_->control, cancel}; control.number<qint16>(1); control.text(destination);
            const int count = control.number<qint32>(); require(count >= 0 && count <= 200000, QStringLiteral("不能确认 HFX 目标是否存在"));
            for (int index = 0; index < count; ++index) {
                existing.insert(control.text()); control.text(); control.number<qint64>(); control.number<qint64>(); control.boolean();
            }
        }
        for (const QString &root : paths) {
            const QString name = QFileInfo(root).fileName(), remoteRoot = joinRemote(destination, name);
            require(!existing.contains(name), QStringLiteral("HFX 目标已存在，不覆盖：") + remoteRoot);
            append(root, remoteRoot);
            if (files.last().directory) {
                const QDir rootDir(root);
                QDirIterator iterator(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
                while (iterator.hasNext()) { iterator.next(); append(iterator.filePath(), joinRemote(remoteRoot, rootDir.relativeFilePath(iterator.filePath()))); }
            }
        }
        auto activity = std::make_shared<std::atomic<qint64>>(0);
        auto stopControl = std::make_shared<std::atomic_bool>(false);
        Wire control{state_->control, cancel, stopControl, state_->options.idleTimeoutMs, 0, activity.get()}; control.number<qint16>(10); std::mutex schedule, reports; qsizetype fileIndex = 0; qint32 block = 0; qint64 done = 0; auto aborted = std::make_shared<std::atomic_bool>(false);
        const auto &sourceFiles = std::as_const(files);
        auto phoneResult = std::async(std::launch::async, [&] {
            try {
                if (control.boolean()) return QString();
                const QString error = control.text();
                aborted->store(true);
                return error.isEmpty() ? QStringLiteral("手机未提供错误详情") : error;
            } catch (...) { aborted->store(true); throw; }
        });
        const auto stopReader = qScopeGuard([&] { stopControl->store(true); });
        ChannelTasks tasks;
        for (const SOCKET socket : state_->channels) tasks.start([&, socket] {
            Wire channel{socket, cancel, aborted, state_->options.idleTimeoutMs}; channel.activity = activity.get();
            QFile input;
            QByteArray frame;
            frame.reserve(blockSize + 1024);
            qsizetype previous = -1, prefixSize = 0;
            qint64 sourceSize = 0;
            const auto closeInput = [&] {
                if (!input.isOpen()) return;
                require(input.size() == sourceSize, QStringLiteral("HFX 来源大小发生变化"));
                input.close();
            };
            for (;;) {
                cancelled(cancel); require(!aborted->load(), QStringLiteral("其它 HFX 通道已失败")); qsizetype current; qint32 currentBlock;
                { std::lock_guard next(schedule); if (fileIndex >= sourceFiles.size()) break; current = fileIndex; currentBlock = block; const File &file = sourceFiles[current]; if (file.directory || (qint64(++block) * blockSize >= file.size)) { ++fileIndex; block = 0; } }
                const File &file = sourceFiles[current];
                if (previous != current) {
                    previous = current;
                    closeInput();
                    frame.resize(0);
                    Wire::appendNumber<qint16>(frame, file.directory ? 1 : 0);
                    Wire::appendNumber(frame, qint32(current));
                    Wire::appendText(frame, file.remote);
                    Wire::appendNumber(frame, file.modified);
                    if (!file.directory) {
                        input.setFileName(file.local); sourceSize = file.size;
                        require(input.open(QIODevice::ReadOnly | QIODevice::Unbuffered) && input.size() == sourceSize, QStringLiteral("HFX 来源变化或不能读取"));
                        Wire::appendNumber(frame, file.size);
                    }
                    prefixSize = frame.size();
                }
                if (file.directory) { channel.send(frame); continue; }
                const qint64 offset = qint64(currentBlock) * blockSize, count = qMin(blockSize, file.size - offset);
                const qsizetype headerSize = prefixSize + 2 * sizeof(qint32);
                frame.resizeForOverwrite(headerSize + count);
                qToBigEndian(currentBlock, frame.data() + prefixSize);
                qToBigEndian(qint32(count), frame.data() + prefixSize + sizeof(qint32));
                require(input.pos() == offset || input.seek(offset), QStringLiteral("HFX 来源不能定位"));
                require(count == 0 || input.read(frame.data() + headerSize, count) == count, QStringLiteral("HFX 来源读取不完整"));
                channel.send(frame);
                if (progress) { std::lock_guard report(reports); done += count; progress(done, total, file.name); }
            }
            closeInput();
            channel.number<qint16>(3);
        }, aborted);
        std::exception_ptr transferError;
        try { tasks.wait(); } catch (...) { transferError = std::current_exception(); }
        if (transferError) {
            QString phoneError;
            if (phoneResult.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready) {
                phoneError = phoneResult.get();
            } else {
                // Stop the control reader before closing sockets or destroying its captured state.
                stopControl->store(true);
                try { phoneError = phoneResult.get(); } catch (...) {}
            }
            if (!phoneError.isEmpty()) throw HfxError(QStringLiteral("手机写入失败：") + phoneError);
            std::rethrow_exception(transferError);
        }
        // All lanes are done: give the phone a fresh idle window for finalizing and reporting the result.
        activity->fetch_add(1);
        const QString error = phoneResult.get();
        if (!error.isEmpty()) throw HfxError(QStringLiteral("手机写入失败：") + error);
        control.boolean(true); return {};
    } catch (const std::exception &error) { state_->close(); return failure(error); }
}

Result HfxSession::pull(const QStringList &paths, const QString &source, const QString &destination, const Cancellation &cancel, const Progress &progress) {
    std::lock_guard guard(operation_);
    try {
        require(connected() && !paths.isEmpty() && QFileInfo(destination).isDir(), QStringLiteral("HFX 未连接或目标不是文件夹"));
        QSet<QString> roots; for (const QString &path : paths) { require(path.startsWith(source.endsWith(QChar('/')) ? source : source + QChar('/')), QStringLiteral("选择的手机文件不在来源目录")); const QString relative = path.mid(source.size()).remove(0, source.endsWith(QChar('/')) ? 0 : 1); require(!relative.contains(QChar('/')) && validateName(relative).isEmpty(), QStringLiteral("请选择同一目录的文件")); require(!QFileInfo::exists(QDir(destination).filePath(relative)), QStringLiteral("本地目标已存在：") + relative); roots.insert(relative); }
        const QDir target(QDir(destination).absolutePath());
        QTemporaryDir staging(target.filePath(QStringLiteral(".twinfile-hfx-XXXXXX"))); require(staging.isValid(), QStringLiteral("不能创建 HFX 暂存目录"));
        const QDir stage(staging.path());
        const QString stagePrefix = normalizedPath(staging.path()) + QChar('/');
        const QStorageInfo storage(staging.path());
        const qint64 available = storage.bytesAvailable();
        // Allow phone-side enumeration before the first frame; other lanes' traffic keeps waits alive.
        auto activity = std::make_shared<std::atomic<qint64>>(0);
        Wire control{state_->control, cancel, {}, state_->options.idleTimeoutMs}; control.number<qint16>(11); control.number<qint32>(qint32(paths.size())); for (const QString &path : paths) control.text(path);
        control.text(QDir::toNativeSeparators(staging.path()) + QChar('\\')); control.number<qint32>(1); control.text(source.endsWith(QChar('/')) ? source : source + QChar('/'));
        struct FileState { QString path; qint64 total = 0, modified = 0; QSet<qint32> blocks; bool directory = false; };
        QHash<qint32, FileState> files; QSet<QString> names;
        std::mutex metadata, reports;
        qint64 done = 0, total = 0; QString lastName;
        auto aborted = std::make_shared<std::atomic_bool>(false); ChannelTasks tasks;
        for (const SOCKET socket : state_->channels) tasks.start([&, socket] {
            Wire channel{socket, cancel, aborted, state_->options.idleTimeoutMs}; channel.activity = activity.get(); channel.firstReadTimeout = state_->options.startTimeoutMs;
            QFile output;
            qint32 openIndex = -1;
            QByteArray bytes; bytes.reserve(blockSize);
            for (;;) {
                cancelled(cancel); require(!aborted->load(), QStringLiteral("其它 HFX 通道失败")); const qint16 type = channel.number<qint16>(); if (type == 3) break; require(type == 0 || type == 1, QStringLiteral("手机传输中断或发送了无效帧"));
                const qint32 index = channel.number<qint32>(); const QString raw = QDir::fromNativeSeparators(channel.text()); const qint64 modified = channel.number<qint64>();
                const QString absolute = QDir::cleanPath(QDir::isAbsolutePath(raw) ? raw : stage.filePath(raw));
                const QString relative = stage.relativeFilePath(absolute);
                qint64 size = 0; qint32 block = -1, length = 0;
                if (type == 0) {
                    size = channel.number<qint64>(); block = channel.number<qint32>(); length = channel.number<qint32>();
                    require(size >= 0 && size <= 256LL * 1024 * 1024 * 1024 && block >= 0 && qint64(block) * blockSize <= size && length >= 0 && length == qMin(blockSize, size - qint64(block) * blockSize) && (length > 0 || (size == 0 && block == 0)), QStringLiteral("手机文件块尺寸无效"));
                    bytes.resizeForOverwrite(length); channel.read(bytes.data(), length);
                }
                {
                    std::lock_guard guard(metadata);
                    auto it = files.find(index);
                    if (it == files.end()) {
                        require(files.size() < 200000, QStringLiteral("HFX 文件数超限"));
                        require(index >= 0 && !safeArchiveName(relative).isEmpty() && roots.contains(relative.section(QChar('/'), 0, 0)), QStringLiteral("手机返回了越界或不安全路径"));
                        const QString key = normalizedPath(absolute);
                        require(key.startsWith(stagePrefix), QStringLiteral("手机返回了越界或不安全路径"));
                        require(!names.contains(key), QStringLiteral("手机发送了重复目标路径")); names.insert(key);
                        require(available < 0 || size <= available - total, QStringLiteral("磁盘空间不足"));
                        require(QDir().mkpath(type == 1 ? absolute : QFileInfo(absolute).absolutePath()), QStringLiteral("不能创建 HFX 目录"));
                        if (type == 0) {
                            output.close(); output.setFileName(absolute);
                            require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Unbuffered) && output.resize(size), QStringLiteral("不能创建 HFX 文件"));
                            openIndex = index;
                        }
                        it = files.insert(index, FileState{absolute, size, modified, {}, type == 1}); total += size;
                    }
                    require(it->path == absolute && it->total == size && it->directory == (type == 1) && it->modified == modified, QStringLiteral("手机文件编号、大小或类型不一致"));
                    require(!it->blocks.contains(block), QStringLiteral("手机发送了重复文件块")); it->blocks.insert(block);
                }
                // Each lane owns its handle and writes only its validated, unique block offsets.
                if (type == 0) {
                    if (openIndex != index) {
                        output.close(); output.setFileName(absolute);
                        if (!output.open(QIODevice::ReadWrite | QIODevice::ExistingOnly | QIODevice::Unbuffered))
                            throw HfxError(QStringLiteral("写入文件时发生错误：") + output.errorString());
                        openIndex = index;
                    }
                    const qint64 offset = qint64(block) * blockSize;
                    require(output.pos() == offset || output.seek(offset), QStringLiteral("写入文件时发生错误：定位失败"));
                    if (length && output.write(bytes) != length) throw HfxError(QStringLiteral("写入文件时发生错误：") + output.errorString());
                }
                activity->fetch_add(1, std::memory_order_relaxed);
                if (progress) {
                    std::lock_guard guard(reports);
                    done += length; lastName = relative;
                    // v300 does not announce a transfer-wide total before streaming the files.
                    progress(done, 0, relative);
                }
            }
        }, aborted);
        tasks.wait();
        for (const auto &file : files) { if (file.directory) continue; require(file.blocks.size() == qMax<qint64>(1, (file.total + blockSize - 1) / blockSize), QStringLiteral("手机文件块不完整")); QFile output(file.path); if (output.open(QIODevice::ReadWrite)) output.setFileTime(QDateTime::fromMSecsSinceEpoch(file.modified), QFileDevice::FileModificationTime); }
        for (const QString &root : roots) require(QFileInfo::exists(staging.filePath(root)), QStringLiteral("手机未发送所选文件：") + root);
        control.boolean(true); if (!control.boolean()) throw HfxError(QStringLiteral("手机读取失败：") + control.text()); cancelled(cancel);
        if (progress) progress(done, total, lastName);
        return publishPull(staging, target, roots, cancel);
    } catch (const std::exception &error) { state_->close(); return failure(error); }
}

}
