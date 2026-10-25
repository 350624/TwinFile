#include "operations.h"
#include "zip.h"
#include "adbsync.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QUuid>
#include <algorithm>

namespace twinfile {
namespace {
Result failed(const QString &error) { return {false, error, {}, {}}; }
bool stopped(const Cancellation &cancel) { return cancel && cancel->load(); }
bool selectedPath(const QString &name, const QString &root) { const QString trimmed = root.endsWith(QChar('/')) ? root.chopped(1) : root; return name == trimmed || name.startsWith(trimmed + QChar('/')); }
QString remoteChild(const QString &parent, const QString &name) { return parent.endsWith(QChar('/')) ? parent + name : parent + QChar('/') + name; }
bool allowedRemote(const QString &path) {
    return path.startsWith(QChar('/')) && !path.contains(QChar(0)) && !QStringList{QStringLiteral("/"), QStringLiteral("/sdcard"), QStringLiteral("/storage"), QStringLiteral("/storage/emulated"), QStringLiteral("/storage/emulated/0")}.contains(QDir::cleanPath(path));
}
}

QByteArray fileDigest(const QString &path, const Cancellation &cancel) {
    QFile input(path); if (!input.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!input.atEnd()) { if (stopped(cancel)) return {}; const QByteArray block = input.read(1024 * 1024); if (input.error() != QFile::NoError) return {}; hash.addData(block); }
    return hash.result();
}

Result commitArchiveMounts(const ArchiveMounts &mounts, const Cancellation &cancel) {
    std::function<Result(const QString &, QSet<QString>)> commit = [&](const QString &path, QSet<QString> visiting) -> Result {
        if (!mounts.contains(path)) return {}; if (visiting.contains(path) || visiting.size() >= 8) return failed(QStringLiteral("嵌套包循环或层级超过 8")); visiting.insert(path);
        const auto &mount = mounts[path]; std::lock_guard guard(mount->mutex); const QByteArray current = fileDigest(path, cancel);
        if (current.isEmpty()) return failed(QStringLiteral("嵌套包读取失败或已取消"));
        if (current != mount->hash) { const Result saved = writeBackEntry(mount->original, path, mount->hash, cancel); if (!saved.ok) { mount->temporary->setAutoRemove(false); return failed(saved.error + QStringLiteral("\n未回写的副本保留在：") + path); } mount->hash = current; }
        return mount->original.source == Source::Archive ? commit(mount->original.path, visiting) : Result{};
    };
    for (const QString &path : mounts.keys()) { const Result saved = commit(path, {}); if (!saved.ok) return saved; }
    return {};
}

Result editArchive(const QString &path, const ArchiveChanges &changes, const Cancellation &cancel, const Progress &progress) {
    if (stopped(cancel)) return failed(QStringLiteral("操作已取消"));
    qint64 processed = 0;
    const auto streamProgress = [&]() -> Progress {
        auto previous = std::make_shared<qint64>(0);
        return [&, previous](qint64 done, qint64, const QString &name) {
            processed += done - *previous; *previous = done;
            if (progress) progress(processed, 0, name);
        };
    };
    QLockFile lock(path + QStringLiteral(".twinfile-lock")); if (!lock.tryLock(0)) return failed(QStringLiteral("压缩包正被其它操作编辑"));
    const QFileInfo before(path); if (!before.isFile() || before.isSymLink() || before.isJunction()) return failed(QStringLiteral("无效的压缩包来源"));
    const QByteArray originalHash = fileDigest(path, cancel); if (originalHash.isEmpty()) return failed(QStringLiteral("无法校验压缩包或已取消"));
    QTemporaryDir staging(QDir(before.absolutePath()).filePath(QStringLiteral(".twinfile-zip-XXXXXX"))); if (!staging.isValid()) return failed(QStringLiteral("无法创建 ZIP 暂存目录"));
    QStringList names; QList<ZipInfo> files;
    {
        ZipReader reader(path); if (reader.status() != ZipReader::NoError) return failed(reader.errorString()); files = reader.fileInfoList();
        for (auto expected = changes.expected.cbegin(); expected != changes.expected.cend(); ++expected) {
            QTemporaryFile original(staging.filePath(QStringLiteral("expected-XXXXXX")));
            if (!original.open() || !reader.streamFile(expected.key(), &original, cancel) || !original.flush() || expected.value().isEmpty() || fileDigest(original.fileName(), cancel) != expected.value()) return failed(QStringLiteral("原条目已被外部修改或读取失败，未覆盖：") + expected.key());
        }
        QSet<QString> originalNames; for (const auto &file : files) originalNames.insert(file.filePath.endsWith(QChar('/')) ? file.filePath.chopped(1) : file.filePath);
        auto contains = [&](const QString &root) { for (const QString &name : originalNames) if (selectedPath(name, root)) return true; return false; };
        for (const QString &name : changes.remove) if (safeArchiveName(name).isEmpty() || !contains(name)) return failed(QStringLiteral("删除的条目不存在或不安全：") + name);
        for (auto item = changes.rename.cbegin(); item != changes.rename.cend(); ++item) {
            if (safeArchiveName(item.key()).isEmpty() || safeArchiveName(item.value()).isEmpty() || !contains(item.key()) || selectedPath(item.value(), item.key())) return failed(QStringLiteral("重命名条目不存在、不安全或移动到自身"));
            for (auto other = changes.rename.cbegin(); other != item; ++other) if (selectedPath(item.key(), other.key()) || selectedPath(other.key(), item.key())) return failed(QStringLiteral("不能同时重命名父条目和子条目"));
        }
        QMap<QString, QString> finalFiles, finalDirectories; QSet<QString> keys, regularKeys;
        auto registerName = [&](QString name, bool directory) {
            if (name.endsWith(QChar('/'))) name.chop(1); const QString key = name.toCaseFolded();
            if (safeArchiveName(name).isEmpty() || keys.contains(key)) return false;
            keys.insert(key); if (!directory) regularKeys.insert(key); return true;
        };
        for (const auto &file : files) {
            if (file.isSymLink || safeArchiveName(file.filePath).isEmpty() || file.size < 0 || file.size > 64LL * 1024 * 1024 * 1024) return failed(QStringLiteral("压缩包含不安全条目或超限文件"));
            QString name = file.filePath; if (name.endsWith(QChar('/'))) name.chop(1); bool removed = false;
            for (const QString &root : changes.remove) if (selectedPath(name, root)) { removed = true; break; }
            if (removed || changes.files.contains(name) || changes.directories.contains(name)) continue;
            for (auto item = changes.rename.cbegin(); item != changes.rename.cend(); ++item) if (selectedPath(name, item.key())) { name = item.value() + name.mid(item.key().size()); break; }
            if (!registerName(name, file.isDir)) return failed(QStringLiteral("ZIP 目标路径冲突：") + name);
            if (file.isDir) finalDirectories.insert(name, file.filePath); else finalFiles.insert(name, file.filePath);
        }
        for (auto item = changes.files.cbegin(); item != changes.files.cend(); ++item) {
            const QFileInfo info(item.value()); if (!info.isFile() || info.isSymLink() || info.isJunction() || !registerName(item.key(), false)) return failed(QStringLiteral("新增 ZIP 文件无效或冲突：") + item.key());
        }
        for (const QString &name : changes.directories) if (!registerName(name, true)) return failed(QStringLiteral("ZIP 文件夹路径冲突：") + name);
        if (keys.size() > 200000) return failed(QStringLiteral("ZIP 条目数量超过限制"));
        for (const QString &key : keys) { QString parent = key.section(QChar('/'), 0, -2); while (!parent.isEmpty()) { if (regularKeys.contains(parent)) return failed(QStringLiteral("ZIP 父路径是文件：") + parent); parent = parent.section(QChar('/'), 0, -2); } }
        ZipWriter writer(staging.filePath(QStringLiteral("updated.zip")));
        for (auto item = finalDirectories.cbegin(); item != finalDirectories.cend(); ++item) { const auto original = std::find_if(files.cbegin(), files.cend(), [&](const ZipInfo &info) { return info.filePath == item.value(); }); writer.addDirectory(item.key(), original != files.cend() ? &*original : nullptr); }
        for (const QString &name : changes.directories) writer.addDirectory(name);
        for (auto item = finalFiles.cbegin(); item != finalFiles.cend(); ++item) {
            if (stopped(cancel)) return failed(QStringLiteral("操作已取消，原包未修改"));
            QTemporaryFile inflated(staging.filePath(QStringLiteral("entry-XXXXXX"))); if (!inflated.open() || !reader.streamFile(item.value(), &inflated, cancel, streamProgress()) || !inflated.seek(0)) return failed(QStringLiteral("读取 ZIP 条目失败，原包未修改：") + item.value());
            const auto original = std::find_if(files.cbegin(), files.cend(), [&](const ZipInfo &info) { return info.filePath == item.value(); });
            writer.addFile(item.key(), &inflated, cancel, original != files.cend() ? &*original : nullptr, streamProgress()); if (writer.status() != ZipWriter::NoError) return failed(QStringLiteral("写入 ZIP 失败，原包未修改"));
        }
        for (auto item = changes.files.cbegin(); item != changes.files.cend(); ++item) {
            QFile input(item.value()); if (!input.open(QIODevice::ReadOnly)) return failed(input.errorString()); const auto original = std::find_if(files.cbegin(), files.cend(), [&](const ZipInfo &info) { return info.filePath == item.key(); }); writer.addFile(item.key(), &input, cancel, original != files.cend() ? &*original : nullptr, streamProgress());
        }
        writer.close(); if (writer.status() != ZipWriter::NoError) return failed(QStringLiteral("ZIP 保存失败，原包未修改"));
    }
    if (stopped(cancel) || fileDigest(path, cancel) != originalHash) return failed(QStringLiteral("操作已取消或原包被外部修改，未覆盖原包"));
    QFile input(staging.filePath(QStringLiteral("updated.zip"))); QSaveFile output(path); output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return failed(QStringLiteral("无法原子替换压缩包"));
    while (!input.atEnd()) { if (stopped(cancel)) return failed(QStringLiteral("操作已取消，原包未修改")); const QByteArray block = input.read(1024 * 1024); if (input.error() != QFile::NoError || output.write(block) != block.size()) return failed(QStringLiteral("压缩包回写失败，原包未修改")); }
    if (!output.commit()) return failed(output.errorString()); return {true, {}, {}, {path}};
}

Result phoneCommand(const QString &serial, const QString &command, const Cancellation &cancel) {
    if (serial.isEmpty()) return failed(QStringLiteral("没有设备序列号"));
    return runProcess(toolPath(QStringLiteral("adb/adb.exe")), {QStringLiteral("-s"), serial, QStringLiteral("shell"), command}, 3600000, cancel);
}

Result createEntry(const Location &location, const QString &name, bool directory, const Cancellation &cancel) {
    const QString invalid = validateName(name); if (!invalid.isEmpty()) return failed(invalid);
    if (location.source == Source::Local) {
        const QString target = QDir(location.path).filePath(name); if (QFileInfo::exists(target)) return failed(QStringLiteral("名称已经存在"));
        if (stopped(cancel)) return failed(QStringLiteral("操作已取消"));
        const bool ok = directory ? QDir().mkdir(target) : QFile(target).open(QIODevice::WriteOnly | QIODevice::NewOnly); return ok ? Result{} : failed(QStringLiteral("创建失败"));
    }
    if (location.source == Source::Phone) {
        const QString target = remoteChild(location.inner, name); if (!allowedRemote(target)) return failed(QStringLiteral("不安全的手机路径"));
        return phoneCommand(location.path, QStringLiteral("if test -e ") + shellQuote(target) + QStringLiteral(" || test -L ") + shellQuote(target) + QStringLiteral("; then echo '目标已存在' >&2; exit 1; else ") + (directory ? QStringLiteral("mkdir -- ") + shellQuote(target) : QStringLiteral("(set -C; : > ") + shellQuote(target) + QChar(')')) + QStringLiteral("; fi"), cancel);
    }
    if (location.source == Source::Archive) {
        ArchiveChanges changes; const QString target = location.inner.isEmpty() ? name : location.inner + QChar('/') + name;
        if (directory) changes.directories.append(target);
        QTemporaryFile empty; if (!directory) { if (!empty.open()) return failed(empty.errorString()); empty.flush(); changes.files.insert(target, empty.fileName()); }
        const Listing listing = listArchive(location.path, location.inner); for (const auto &entry : listing.entries) if (entry.name.compare(name, Qt::CaseInsensitive) == 0) return failed(QStringLiteral("名称已经存在"));
        return editArchive(location.path, changes, cancel);
    }
    return failed(QStringLiteral("此位置不能创建文件"));
}

Result removeEntries(const QList<Entry> &entries, const Cancellation &cancel) {
    if (entries.isEmpty()) return {};
    const Source source = entries.first().source; QStringList local; ArchiveChanges changes;
    for (const auto &entry : entries) {
        if (entry.source != source) return failed(QStringLiteral("不能混合不同来源删除"));
        if (source == Source::Local) local.append(entry.path);
        else if (source == Source::Archive) { if (entry.path != entries.first().path) return failed(QStringLiteral("不能混合多个压缩包")); changes.remove.append(entry.inner); }
        else if (source == Source::Phone && !allowedRemote(entry.path)) return failed(QStringLiteral("禁止删除手机根目录或存储根目录"));
        else if (source != Source::Phone) return failed(QStringLiteral("不能删除此项目"));
    }
    if (source == Source::Local) return recycleFiles(local);
    if (source == Source::Archive) return editArchive(entries.first().path, changes, cancel);
    for (const auto &entry : entries) { const Result result = phoneCommand(entry.serial, QStringLiteral("rm -rf -- ") + shellQuote(entry.path), cancel); if (!result.ok) return result; }
    return {};
}

Result renameEntry(const Entry &entry, const QString &name, const Cancellation &cancel) {
    const QString invalid = validateName(name); if (!invalid.isEmpty()) return failed(invalid); if (name == entry.name) return {};
    if (entry.source == Source::Local) return batchRename({entry.path}, {name});
    if (entry.source == Source::Archive) { ArchiveChanges changes; const QString parent = entry.inner.section(QChar('/'), 0, -2); changes.rename.insert(entry.inner, parent.isEmpty() ? name : parent + QChar('/') + name); return editArchive(entry.path, changes, cancel); }
    if (entry.source == Source::Phone && allowedRemote(entry.path)) {
        const QString target = remoteChild(QFileInfo(entry.path).path(), name);
        return phoneCommand(entry.serial, QStringLiteral("if test -e ") + shellQuote(target) + QStringLiteral(" || test -L ") + shellQuote(target) + QStringLiteral("; then exit 1; else mv -n -- ") + shellQuote(entry.path) + QChar(' ') + shellQuote(target) + QStringLiteral("; fi"), cancel);
    }
    return failed(QStringLiteral("此项目不能重命名"));
}

Result materializeEntry(const Entry &entry, const QString &destination, const Cancellation &cancel, const Progress &progress) {
    if (entry.directory || !validateName(entry.name).isEmpty()) return failed(QStringLiteral("请选择普通文件"));
    const QString target = QDir(destination).filePath(entry.name);
    if (QFileInfo::exists(target)) return failed(QStringLiteral("暂存文件已经存在"));
    if (entry.source == Source::Local) return transferFiles({entry.path}, destination, false, cancel, progress);
    if (entry.source == Source::Phone) return phoneTransfer({entry}, {Source::Local, destination, {}, {}}, false, cancel, progress);
    if (entry.source == Source::Archive) {
        ZipReader reader(entry.path); if (reader.status() != ZipReader::NoError) return failed(reader.errorString());
        const auto files = reader.fileInfoList(); const auto info = std::find_if(files.cbegin(), files.cend(), [&](const auto &file) { return file.filePath == entry.inner; });
        const QStorageInfo storage(destination);
        if (info == files.cend() || !info->isFile || info->isSymLink || info->size < 0 || info->size > 64LL * 1024 * 1024 * 1024 || (storage.bytesAvailable() >= 0 && info->size > storage.bytesAvailable())) return failed(QStringLiteral("包内文件不存在、为链接、超过 64 GiB 或暂存空间不足"));
        QFile output(target); if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return failed(output.errorString());
        if (!reader.streamFile(entry.inner, &output, cancel, progress) || !output.flush()) { output.close(); QFile::remove(target); return failed(QStringLiteral("压缩包文件读取失败")); }
        return {true, {}, {}, {target}};
    }
    return failed(QStringLiteral("不支持的文件来源"));
}

Result writeBackEntry(const Entry &entry, const QString &local, const QByteArray &expectedHash, const Cancellation &cancel) {
    QTemporaryDir staging; if (!staging.isValid() || expectedHash.isEmpty()) return failed(QStringLiteral("缺少原始校验值，拒绝覆盖"));
    if (entry.source == Source::Archive) { ArchiveChanges changes; changes.files.insert(entry.inner, local); changes.expected.insert(entry.inner, expectedHash); return editArchive(entry.path, changes, cancel); }
    const Result read = materializeEntry(entry, staging.path(), cancel); if (!read.ok) return read;
    if (fileDigest(read.paths.first(), cancel) != expectedHash) return failed(QStringLiteral("原文件已被其它程序修改，请重新打开，未覆盖"));
    if (entry.source == Source::Phone) {
        const QString temporary = entry.path + QStringLiteral(".twinfile-") + QUuid::createUuid().toString(QUuid::Id128);
        const Result pushed = adbSyncPush(entry.serial, local, temporary, cancel);
        if (!pushed.ok) { phoneCommand(entry.serial, QStringLiteral("rm -f -- ") + shellQuote(temporary)); return pushed; }
        const QString expected = QString::fromLatin1(expectedHash.toHex());
        const QString command = QStringLiteral("test -f ") + shellQuote(entry.path) + QStringLiteral(" && ! test -L ") + shellQuote(entry.path) + QStringLiteral(" && actual=$(sha256sum -- ") + shellQuote(entry.path) + QStringLiteral(") && test \"${actual%% *}\" = ") + shellQuote(expected) + QStringLiteral(" && mv -f -- ") + shellQuote(temporary) + QChar(' ') + shellQuote(entry.path);
        const Result replaced = phoneCommand(entry.serial, command, cancel);
        if (!replaced.ok) phoneCommand(entry.serial, QStringLiteral("rm -f -- ") + shellQuote(temporary)); return replaced;
    }
    return failed(QStringLiteral("本地编辑请直接原子保存"));
}

Result transferEntries(const QList<Entry> &entries, const Location &destination, bool move, const Cancellation &cancel, const Progress &progress) {
    if (entries.isEmpty()) return {};
    if (move && destination.source == Source::Archive && entries.first().source == Source::Archive && entries.first().path == destination.path) {
        ArchiveChanges changes;
        for (const auto &entry : entries) { if (entry.source != Source::Archive || entry.path != destination.path) return failed(QStringLiteral("不能混合多个压缩包内部移动")); const QString target = destination.inner.isEmpty() ? entry.name : destination.inner + QChar('/') + entry.name; if (target != entry.inner) changes.rename.insert(entry.inner, target); }
        return changes.rename.isEmpty() ? Result{} : editArchive(destination.path, changes, cancel, progress);
    }
    bool allLocal = true; for (const auto &entry : entries) allLocal &= entry.source == Source::Local;
    if (allLocal && destination.source == Source::Local) { QStringList paths; for (const auto &entry : entries) paths.append(entry.path); return transferFiles(paths, destination.path, move, cancel, progress); }
    if (destination.source == Source::Phone && entries.first().source == Source::Phone && entries.first().serial == destination.path) {
        for (const auto &entry : entries) {
            const QString target = remoteChild(destination.inner, entry.name);
            if (entry.source != Source::Phone || entry.serial != destination.path || !allowedRemote(entry.path) || target == entry.path || (entry.directory && target.startsWith(entry.path + QChar('/')))) return failed(QStringLiteral("无效的手机内部传输"));
            const Result result = phoneCommand(destination.path, QStringLiteral("if test -e ") + shellQuote(target) + QStringLiteral(" || test -L ") + shellQuote(target) + QStringLiteral("; then exit 1; else ") + (move ? QStringLiteral("mv -n -- ") : QStringLiteral("cp -Rpn -- ")) + shellQuote(entry.path) + QChar(' ') + shellQuote(target) + QStringLiteral("; fi"), cancel);
            if (!result.ok) return result;
        }
        return {};
    }
    if (destination.source != Source::Archive && entries.first().source != Source::Archive) return phoneTransfer(entries, destination, move, cancel, progress);
    QTemporaryDir staging; if (!staging.isValid()) return failed(QStringLiteral("无法创建传输暂存目录"));
    QList<Entry> materialized; ArchiveChanges additions;
    if (allLocal) materialized = entries;
    else for (const auto &entry : entries) {
        if (entry.source == Source::Archive && entry.directory) {
            Result result = extractArchive(entry.path, staging.path(), entry.inner, cancel, progress); if (!result.ok) return result;
            Entry local; local.path = QDir(staging.path()).filePath(entry.inner); local.name = entry.name; local.directory = true; materialized.append(local);
        } else if (entry.source == Source::Phone && entry.directory) {
            const QString target = staging.filePath(entry.name);
            const Result result = adbSyncPull(entry.serial, entry.path, target, cancel, progress); if (!result.ok) return result;
            Entry local; local.path = target; local.name = entry.name; local.directory = true; materialized.append(local);
        } else { Result result = materializeEntry(entry, staging.path(), cancel, progress); if (!result.ok) return result; Entry local; local.path = result.paths.first(); local.name = entry.name; materialized.append(local); }
    }
    Result copied;
    if (destination.source == Source::Archive) {
        const Listing listing = listArchive(destination.path, destination.inner); if (!listing.error.isEmpty()) return failed(listing.error);
        for (const auto &entry : materialized) {
            for (const auto &existing : listing.entries) if (existing.name.compare(entry.name, Qt::CaseInsensitive) == 0) return failed(QStringLiteral("ZIP 目标已经存在：") + entry.name);
            const QString root = destination.inner.isEmpty() ? entry.name : destination.inner + QChar('/') + entry.name;
            if (!entry.directory) additions.files.insert(root, entry.path);
            else {
                additions.directories.append(root); QDirIterator iterator(entry.path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
                while (iterator.hasNext()) { iterator.next(); const auto info = iterator.fileInfo(); if (info.isSymLink() || info.isJunction()) return failed(QStringLiteral("不压缩符号链接")); const QString name = root + QChar('/') + QDir(entry.path).relativeFilePath(iterator.filePath()); if (info.isDir()) additions.directories.append(name); else additions.files.insert(name, iterator.filePath()); }
            }
        }
        copied = editArchive(destination.path, additions, cancel, progress);
    } else { QStringList paths; for (const auto &entry : materialized) paths.append(entry.path); copied = destination.source == Source::Local ? transferFiles(paths, destination.path, false, cancel, progress) : phoneTransfer(materialized, destination, false, cancel, progress); }
    if (!copied.ok || !move) return copied;
    if (entries.first().source == Source::Archive && destination.source == Source::Archive && entries.first().path == destination.path) return failed(QStringLiteral("复制已完成；同包移动请使用重命名，源文件已保留"));
    if (stopped(cancel)) return failed(QStringLiteral("复制完成但取消了删除，源文件已保留"));
    return removeEntries(entries, cancel);
}

}
