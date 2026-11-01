#include "hfx_provider.h"
#include "operations.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>

namespace twinfile {
namespace {
std::mutex endpointsMutex;
QMap<QString, std::weak_ptr<HfxSession>> endpoints;
Result failed(const QString &message) { return {false, message, {}, {}}; }
bool stopped(const Cancellation &cancel) { return cancel && cancel->load(); }
QString child(const QString &parent, const QString &name) { return QDir::cleanPath(parent + QChar('/') + name); }
using Fingerprint = QMap<QString, QByteArray>;
struct RemoteStamp {
    bool directory;
    qint64 size, modified;
    bool operator==(const RemoteStamp &) const = default;
};
using Snapshot = QMap<QString, RemoteStamp>;
Result remoteSnapshot(const std::shared_ptr<HfxSession> &session, const QStringList &paths, Snapshot &result, const Cancellation &cancel) {
    std::function<Result(const Entry &, const QString &, int)> visit = [&](const Entry &entry, const QString &relative, int depth) -> Result {
        if (stopped(cancel)) return failed(QStringLiteral("已取消，源文件已保留"));
        if (depth > 128 || result.size() >= 200000 || result.contains(relative)) return failed(QStringLiteral("手机目录深度、条目数量或重复路径超限"));
        result.insert(relative, {entry.directory, entry.directory ? 0 : entry.size, entry.modified.toMSecsSinceEpoch()});
        if (!entry.directory) return {};
        const Listing children = session->list(entry.path, cancel); if (!children.error.isEmpty()) return failed(children.error);
        for (const auto &item : children.entries) { const Result checked = visit(item, relative + QChar('/') + item.name, depth + 1); if (!checked.ok) return checked; }
        return {};
    };
    for (const QString &path : paths) {
        const QString clean = QDir::cleanPath(path);
        if (!path.startsWith(QChar('/')) || path != clean || path == session->home() || QStringList{QStringLiteral("/"), QStringLiteral("/sdcard"), QStringLiteral("/storage"), QStringLiteral("/storage/emulated"), QStringLiteral("/storage/emulated/0")}.contains(path)) return failed(QStringLiteral("禁止移动手机根目录或存储根目录"));
        const Listing parent = session->list(QFileInfo(path).path(), cancel); if (!parent.error.isEmpty()) return failed(parent.error);
        bool found = false; for (const auto &entry : parent.entries) if (entry.path == path) { found = true; const Result checked = visit(entry, entry.name, 0); if (!checked.ok) return checked; break; }
        if (!found) return failed(QStringLiteral("手机来源已不存在，未移动：") + path);
    }
    return {};
}
Result fingerprint(const QStringList &paths, Fingerprint &result, const Cancellation &cancel) {
    auto add = [&](const QString &path, const QString &relative) -> Result {
        if (stopped(cancel)) return failed(QStringLiteral("已取消，源文件已保留"));
        const QFileInfo info(path);
        if (!info.exists() || info.isSymLink() || info.isJunction() || (!info.isDir() && !info.isFile()) || result.size() >= 200000 || result.contains(relative)) return failed(QStringLiteral("来源不存在、重复、包含链接或条目超限"));
        const QByteArray hash = info.isDir() ? QByteArray("directory") : fileDigest(path, cancel);
        if (hash.isEmpty()) return failed(QStringLiteral("无法校验文件，源文件已保留：") + path);
        result.insert(relative, hash); return {};
    };
    for (const QString &path : paths) {
        const QString root = QFileInfo(path).fileName();
        Result checked = add(path, root); if (!checked.ok) return checked;
        if (!QFileInfo(path).isDir()) continue;
        QDirIterator iterator(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (iterator.hasNext()) { iterator.next(); checked = add(iterator.filePath(), root + QChar('/') + QDir(path).relativeFilePath(iterator.filePath())); if (!checked.ok) return checked; }
    }
    return {};
}
}

bool isHfxEndpoint(const QString &endpoint) { return endpoint.startsWith(QStringLiteral("hfx:")); }
QString registerHfxSession(const std::shared_ptr<HfxSession> &session) {
    if (!session || !session->connected()) return {};
    const QString endpoint = QStringLiteral("hfx:") + QUuid::createUuid().toString(QUuid::Id128);
    std::lock_guard guard(endpointsMutex);
    for (auto it = endpoints.begin(); it != endpoints.end();) { if (it.value().expired()) it = endpoints.erase(it); else ++it; }
    endpoints.insert(endpoint, session); return endpoint;
}
void unregisterHfxSession(const QString &endpoint) { std::lock_guard guard(endpointsMutex); endpoints.remove(endpoint); }
std::shared_ptr<HfxSession> findHfxSession(const QString &endpoint) {
    std::lock_guard guard(endpointsMutex); const auto session = endpoints.value(endpoint).lock();
    return session && session->connected() ? session : std::shared_ptr<HfxSession>{};
}
Listing listHfx(const QString &endpoint, const QString &path, bool hidden, const Cancellation &cancel) {
    const auto session = findHfxSession(endpoint);
    if (!session) return {{}, QStringLiteral("HFX 已断开，请重新连接多轨快传并打开手机目录")};
    Listing result = session->list(path, cancel);
    for (auto it = result.entries.begin(); it != result.entries.end();) {
        if (!hidden && it->name.startsWith(QChar('.'))) it = result.entries.erase(it);
        else { it->serial = endpoint; ++it; }
    }
    return result;
}
bool usesHfx(const QList<Entry> &entries, const Location &destination) {
    if (destination.source == Source::Phone && isHfxEndpoint(destination.path)) return true;
    for (const auto &entry : entries) if (entry.source == Source::Phone && isHfxEndpoint(entry.serial)) return true;
    return false;
}

Result transferHfx(const QList<Entry> &entries, const Location &destination, bool move, const Cancellation &cancel, const Progress &progress) {
    if (entries.isEmpty()) return {};
    const bool upload = destination.source == Source::Phone && isHfxEndpoint(destination.path);
    const QString endpoint = upload ? destination.path : entries.first().serial;
    const auto session = findHfxSession(endpoint);
    if (!session) return failed(QStringLiteral("HFX 已断开，请重新连接多轨快传；未改用 ADB"));
    if (upload && (!destination.inner.startsWith(QChar('/')) || destination.inner != QDir::cleanPath(destination.inner) || destination.inner.contains(QChar(0)))) return failed(QStringLiteral("无效的 HFX 目标目录"));
    if ((!upload && destination.source != Source::Local) || (upload && entries.first().source != Source::Local)) return failed(QStringLiteral("多轨快传目前支持电脑与手机之间复制／移动；手机内部操作请使用 ADB"));
    QStringList paths, remote, copied; QSet<QString> names; QString parent;
    for (const auto &entry : entries) {
        if (entry.source != (upload ? Source::Local : Source::Phone) || (!upload && entry.serial != endpoint) || !validateName(entry.name).isEmpty() || names.contains(entry.name.toCaseFolded())) return failed(QStringLiteral("不能混合不同设备、重复名称或无效的来源"));
        names.insert(entry.name.toCaseFolded());
        if (QFileInfo(entry.path).fileName() != entry.name) return failed(QStringLiteral("所选名称与来源路径不一致"));
        paths.append(entry.path);
        if (upload) { remote.append(child(destination.inner, entry.name)); copied.append(entry.path); }
        else {
            const QString current = QFileInfo(entry.path).path();
            if (!parent.isEmpty() && parent != current) return failed(QStringLiteral("多轨快传请选择同一手机目录中的项目"));
            parent = current; remote.append(entry.path); copied.append(QDir(destination.path).filePath(entry.name));
        }
    }
    if (stopped(cancel)) return failed(QStringLiteral("已取消，源文件已保留"));
    Fingerprint before;
    Snapshot beforeRemote;
    if (upload && move) { if (progress) progress(0, 0, QStringLiteral("校验移动来源")); const Result checked = fingerprint(paths, before, cancel); if (!checked.ok) return checked; }
    if (!upload && move) { const Result checked = remoteSnapshot(session, remote, beforeRemote, cancel); if (!checked.ok) return checked; }
    Result copiedResult = upload ? session->push(paths, destination.inner, cancel, progress) : session->pull(remote, parent, destination.path, cancel, progress);
    if (!copiedResult.ok || !move) return copiedResult;
    // HFX v300 has no remote hash command: read back before deleting a move source.
    QTemporaryDir verification;
    if (!verification.isValid()) return failed(QStringLiteral("复制已完成，无法创建校验目录，源文件已保留"));
    if (progress) progress(0, 0, QStringLiteral("回读校验，完成后删除来源"));
    Result checked = session->pull(remote, upload ? destination.inner : parent, verification.path(), cancel, [progress](qint64 done, qint64 total, const QString &name) { if (progress) progress(done, total, QStringLiteral("回读校验 · ") + name); });
    if (!checked.ok) return failed(QStringLiteral("复制已完成，但回读校验失败；源文件已保留：") + checked.error);
    QStringList verified; for (const auto &entry : entries) verified.append(verification.filePath(entry.name));
    Fingerprint actual, current; checked = fingerprint(verified, actual, cancel); if (!checked.ok) return checked;
    checked = fingerprint(copied, current, cancel); if (!checked.ok) return checked;
    if (actual != current || (upload && before != current)) return failed(QStringLiteral("复制已完成，但内容变化或校验不一致；源文件已保留"));
    if (!upload) {
        Snapshot after; checked = remoteSnapshot(session, remote, after, cancel);
        if (!checked.ok || beforeRemote != after || after.keys() != actual.keys()) return failed(QStringLiteral("复制已完成，但手机来源目录或文件信息变化；源文件已保留"));
        for (auto item = after.cbegin(); item != after.cend(); ++item) {
            const QFileInfo local(QDir(destination.path).filePath(item.key()));
            if (item->directory != local.isDir() || (!item->directory && item->size != local.size())) return failed(QStringLiteral("复制已完成，但手机来源类型或大小变化；源文件已保留"));
        }
    }
    if (stopped(cancel)) return failed(QStringLiteral("复制已完成，操作已取消；源文件已保留"));
    Result removed = removeEntries(entries, cancel);
    if (!removed.ok) return failed(QStringLiteral("复制和校验已完成，但来源删除失败：") + removed.error);
    return copiedResult;
}

}
