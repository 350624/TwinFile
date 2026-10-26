#include "core.h"
#include "zip.h"
#include "operations.h"
#include "adbsync.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonArray>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSet>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace twinfile {

static bool cancelled(const Cancellation &cancel) { return cancel && cancel->load(); }
static Result failure(const QString &error) { return {false, error, {}, {}}; }

QString Entry::key() const {
    return QString::number(int(source)) + QChar(0x1f) + serial + QChar(0x1f) + path + QChar(0x1f) + inner;
}

QString Location::display() const {
    if (source == Source::Drive) return QStringLiteral("此电脑");
    if (source == Source::Archive) return path + QStringLiteral("!/") + inner;
    if (source == Source::Phone) return path + QChar(':') + inner;
    return QDir::toNativeSeparators(path);
}

QJsonObject Location::json() const {
    return {{QStringLiteral("source"), int(source)}, {QStringLiteral("path"), path},
            {QStringLiteral("inner"), inner}, {QStringLiteral("label"), label}};
}

Location Location::fromJson(const QJsonObject &object) {
    const int value = object.value(QStringLiteral("source")).toInt(int(Source::Drive));
    if (value < 0 || value > int(Source::Phone) || value == int(Source::Device)) return {};
    Location location{Source(value), object.value(QStringLiteral("path")).toString(),
                      object.value(QStringLiteral("inner")).toString(), object.value(QStringLiteral("label")).toString()};
    if (location.source == Source::Local && !QFileInfo(location.path).isDir()) return {};
    if (location.source == Source::Archive && !QFileInfo(location.path).isFile()) return {};
    if (location.source == Source::Phone && (location.path.isEmpty() || !location.inner.startsWith(QChar('/')))) return {};
    return location;
}

bool isApk(const QString &name) {
    static const QRegularExpression expression(QStringLiteral(R"(\.apk(?:\.[0-9]+)?$)"), QRegularExpression::CaseInsensitiveOption);
    return expression.match(name).hasMatch();
}

bool isArchive(const QString &name) {
    static const QStringList extensions{QStringLiteral("zip"), QStringLiteral("jar"), QStringLiteral("apks"), QStringLiteral("xapk"), QStringLiteral("kmz"), QStringLiteral("whl"), QStringLiteral("epub"), QStringLiteral("crx")};
    return isApk(name) || extensions.contains(QFileInfo(name).suffix().toLower());
}
bool isText(const QString &name) {
    static const QStringList extensions{QStringLiteral("txt"), QStringLiteral("md"), QStringLiteral("log"), QStringLiteral("ini"), QStringLiteral("cfg"), QStringLiteral("conf"), QStringLiteral("json"), QStringLiteral("xml"), QStringLiteral("yml"), QStringLiteral("yaml"), QStringLiteral("toml"), QStringLiteral("py"), QStringLiteral("js"), QStringLiteral("ts"), QStringLiteral("java"), QStringLiteral("kt"), QStringLiteral("c"), QStringLiteral("cpp"), QStringLiteral("h"), QStringLiteral("hpp"), QStringLiteral("cs"), QStringLiteral("go"), QStringLiteral("rs"), QStringLiteral("smali"), QStringLiteral("html"), QStringLiteral("css"), QStringLiteral("sh"), QStringLiteral("bat"), QStringLiteral("properties"), QStringLiteral("gradle"), QStringLiteral("pro"), QStringLiteral("sql"), QStringLiteral("proto"), QStringLiteral("gitignore"), QStringLiteral("lock"), QStringLiteral("ps1")};
    return extensions.contains(name.section(QChar('.'), -1).toLower());
}

bool isImage(const QString &name) {
    static const QStringList extensions{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("webp"), QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("ico"), QStringLiteral("avif")};
    return extensions.contains(QFileInfo(name).suffix().toLower());
}

QString humanSize(qint64 bytes) {
    const QStringList units{QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB"), QStringLiteral("TB")};
    double value = double(bytes);
    int unit = 0;
    while (value >= 1024 && unit < units.size() - 1) { value /= 1024; ++unit; }
    return QString::number(value, 'f', unit ? 1 : 0) + QChar(' ') + units[unit];
}

QString shellQuote(const QString &value) {
    QString escaped = value;
    escaped.replace(QChar('\''), QStringLiteral("'\\''"));
    return QChar('\'') + escaped + QChar('\'');
}

QString toolPath(const QString &relative) {
    const QString base = QCoreApplication::applicationDirPath();
    const QStringList roots{base, base + QStringLiteral("/tools"), QString::fromWCharArray(TWINFILE_TOOL_ROOT)};
    for (const QString &root : roots) {
        const QString path = QDir(root).filePath(relative);
        if (QFileInfo(path).isFile()) return path;
    }
    return QStandardPaths::findExecutable(QFileInfo(relative).fileName());
}

QString safeArchiveName(const QString &name) {
    if (name.isEmpty() || name.startsWith(QChar('/')) || name.startsWith(QChar('\\'))
        || name.contains(QChar('\\')) || name.contains(QChar(':')) || name.contains(QChar(0))) return {};
    const QStringList parts = name.split(QChar('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QStringLiteral("..") || part == QStringLiteral(".") || !validateName(part).isEmpty()) return {};
    }
    return parts.join(QChar('/'));
}

QString normalizedPath(const QString &path) {
    const QFileInfo info(path);
    QString canonical = info.canonicalFilePath();
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(info.absoluteFilePath());
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (required && required < 32768) {
            QString resolved(int(required) + 1, Qt::Uninitialized);
            const DWORD written = GetFinalPathNameByHandleW(handle, reinterpret_cast<LPWSTR>(resolved.data()), DWORD(resolved.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (written && written < DWORD(resolved.size())) {
                resolved.truncate(written);
                if (resolved.startsWith(QStringLiteral("\\\\?\\UNC\\"), Qt::CaseInsensitive)) resolved = QStringLiteral("\\\\") + resolved.mid(8);
                else if (resolved.startsWith(QStringLiteral("\\\\?\\"))) resolved.remove(0, 4);
                canonical = QDir::fromNativeSeparators(resolved);
            }
        }
        CloseHandle(handle);
    }
#endif
    if (canonical.isEmpty() && !info.fileName().isEmpty() && info.absolutePath() != info.absoluteFilePath()) canonical = QDir(normalizedPath(info.absolutePath())).filePath(info.fileName());
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical).toCaseFolded();
}

QString validateName(const QString &name) {
    static const QRegularExpression forbidden(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])"));
    static const QRegularExpression reserved(QStringLiteral(R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$))"), QRegularExpression::CaseInsensitiveOption);
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")
        || name.endsWith(QChar('.')) || name.endsWith(QChar(' ')) || name.size() > 255
        || forbidden.match(name).hasMatch() || reserved.match(name).hasMatch()) return QStringLiteral("非法文件名：") + name;
    return {};
}

Listing listLocal(const QString &path, bool hidden, const QString &sort, bool descending) {
    Listing result;
    if (path.isEmpty()) {
        for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
            if (!storage.isValid() || !storage.isReady()) continue;
            Entry entry;
            entry.path = storage.rootPath();
            entry.name = storage.displayName().isEmpty() ? entry.path : storage.displayName() + QStringLiteral(" (") + entry.path + QChar(')');
            entry.directory = true;
            entry.source = Source::Drive;
            entry.detail = QStringLiteral("可用 %1 / 共 %2").arg(humanSize(storage.bytesAvailable()), humanSize(storage.bytesTotal()));
            result.entries.append(entry);
        }
        return result;
    }
    const QDir directory(path);
    if (!directory.exists() || !QFileInfo(path).isReadable()) return {{}, QStringLiteral("无法读取文件夹：") + path};
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (hidden) filters |= QDir::Hidden;
    for (const QFileInfo &info : directory.entryInfoList(filters, QDir::NoSort)) {
        Entry entry;
        entry.name = info.fileName(); entry.path = info.absoluteFilePath();
        entry.directory = info.isDir(); entry.size = info.size(); entry.modified = info.lastModified();
        result.entries.append(entry);
    }
    std::stable_sort(result.entries.begin(), result.entries.end(), [&](const Entry &first, const Entry &second) {
        if (first.directory != second.directory) return first.directory;
        int comparison = QString::compare(first.name, second.name, Qt::CaseInsensitive);
        if (sort == QStringLiteral("size") && first.size != second.size) comparison = first.size < second.size ? -1 : 1;
        if (sort == QStringLiteral("mtime") && first.modified != second.modified) comparison = first.modified < second.modified ? -1 : 1;
        return descending ? comparison > 0 : comparison < 0;
    });
    return result;
}

Listing listArchive(const QString &path, const QString &inner) {
    ZipReader reader(path);
    const auto files = reader.fileInfoList();
    if (reader.status() != ZipReader::NoError) return {{}, reader.errorString()};
    QString prefix = inner;
    if (!prefix.isEmpty() && !prefix.endsWith(QChar('/'))) prefix += QChar('/');
    QMap<QString, Entry> entries;
    for (const auto &file : files) {
        if (file.isSymLink || safeArchiveName(file.filePath).isEmpty() || !file.filePath.startsWith(prefix)) continue;
        QString relative = file.filePath.mid(prefix.size());
        if (relative.isEmpty()) continue;
        const QString first = relative.section(QChar('/'), 0, 0);
        const bool directory = relative.contains(QChar('/')) || file.isDir;
        if (entries.contains(first) && !directory) continue;
        Entry entry;
        entry.name = first; entry.path = path; entry.inner = prefix + first;
        entry.source = Source::Archive; entry.directory = directory;
        entry.size = directory ? 0 : file.size; entry.modified = file.lastModified;
        entries.insert(first, entry);
    }
    Listing result;
    result.entries = entries.values();
    std::stable_sort(result.entries.begin(), result.entries.end(), [](const Entry &first, const Entry &second) {
        return first.directory != second.directory ? first.directory : first.name.toCaseFolded() < second.name.toCaseFolded();
    });
    return result;
}

Result runProcess(const QString &program, const QStringList &arguments, int timeoutMs, const Cancellation &cancel) {
    if (cancelled(cancel)) return failure(QStringLiteral("操作已取消"));
    if (program.isEmpty()) return failure(QStringLiteral("未找到所需外部工具"));
    QProcess process;
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) { arguments->flags |= CREATE_NO_WINDOW; });
#endif
    process.setProgram(program); process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted(5000)) return failure(process.errorString());
    QElapsedTimer elapsed; elapsed.start();
    QByteArray output;
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(100);
        output += process.readAll();
        if (output.size() > 16 * 1024 * 1024) {
            process.kill(); process.waitForFinished(3000); return failure(QStringLiteral("外部工具输出超过 16 MiB 上限"));
        }
        if (cancelled(cancel) || elapsed.elapsed() > timeoutMs) {
            process.kill(); process.waitForFinished(3000);
            return failure(cancelled(cancel) ? QStringLiteral("操作已取消") : QStringLiteral("外部工具执行超时"));
        }
    }
    output += process.readAll();
    if (output.size() > 16 * 1024 * 1024) return failure(QStringLiteral("外部工具输出超过 16 MiB 上限"));
    const QString text = QString::fromUtf8(output);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return {false, text.isEmpty() ? process.errorString() : text, text, {}};
    return {true, {}, text, {}};
}

Listing listPhone(const QString &serial, const QString &path, bool hidden) {
    const Listing synced = adbSyncList(serial, path, hidden);
    if (synced.error.isEmpty()) return synced;
    const Result output = runProcess(toolPath(QStringLiteral("adb/adb.exe")),
        {QStringLiteral("-s"), serial, QStringLiteral("shell"), QStringLiteral("ls -lan -- ") + shellQuote(path)}, 20000);
    if (!output.ok) return {{}, output.error};
    static const QRegularExpression linePattern(QStringLiteral(R"(^([dl-][rwxstST-]{9})\s+\d+\s+\S+\s+\S+\s+(\d+)\s+(\d{4}-\d\d-\d\d)\s+(\d\d:\d\d)(?::\d\d)?\s+(.*)$)"));
    Listing listing;
    for (const QString &line : output.output.split(QChar('\n'))) {
        const auto match = linePattern.match(line.trimmed());
        if (!match.hasMatch()) continue;
        QString name = match.captured(5);
        if (name == QStringLiteral(".") || name == QStringLiteral("..") || (!hidden && name.startsWith(QChar('.')))) continue;
        if (match.captured(1).startsWith(QChar('l'))) name = name.section(QStringLiteral(" -> "), 0, 0);
        Entry entry;
        entry.name = name; entry.path = QDir::cleanPath(path + QChar('/') + name); entry.serial = serial;
        entry.directory = match.captured(1).startsWith(QChar('d')); entry.size = match.captured(2).toLongLong();
        entry.modified = QDateTime::fromString(match.captured(3) + QChar(' ') + match.captured(4), QStringLiteral("yyyy-MM-dd HH:mm"));
        entry.source = Source::Phone; listing.entries.append(entry);
    }
    std::stable_sort(listing.entries.begin(), listing.entries.end(), [](const Entry &first, const Entry &second) {
        return first.directory != second.directory ? first.directory : first.name.toCaseFolded() < second.name.toCaseFolded();
    });
    return listing;
}

Listing searchLocal(const QString &path, const QString &pattern, bool hidden, const Cancellation &cancel) {
    Listing result;
    QDir::Filters filters = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot;
    if (hidden) filters |= QDir::Hidden;
    QDirIterator iterator(path, filters, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        if (cancelled(cancel)) return {{}, QStringLiteral("搜索已取消")};
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (!info.fileName().contains(pattern, Qt::CaseInsensitive)) continue;
        Entry entry; entry.name = info.fileName(); entry.path = info.absoluteFilePath();
        entry.directory = info.isDir(); entry.size = info.size(); entry.modified = info.lastModified(); entry.detail = entry.path;
        result.entries.append(entry);
        if (result.entries.size() >= 100000) { result.error = QStringLiteral("结果达到 100000 项上限，请缩小范围"); break; }
    }
    return result;
}

Listing searchLocation(const Location &location, const QString &pattern, bool hidden, const Cancellation &cancel) {
    if (location.source == Source::Local) return searchLocal(location.path, pattern, hidden, cancel);
    Listing result;
    if (location.source == Source::Archive) {
        ZipReader reader(location.path); if (reader.status() != ZipReader::NoError) return {{}, reader.errorString()};
        const QString prefix = location.inner.isEmpty() ? QString() : location.inner + QChar('/');
        for (const auto &file : reader.fileInfoList()) {
            if (cancelled(cancel)) return {{}, QStringLiteral("已取消")};
            QString inner = file.filePath; if (inner.endsWith(QChar('/'))) inner.chop(1);
            const QString relative = inner.mid(prefix.size());
            if (!inner.startsWith(prefix) || safeArchiveName(inner).isEmpty() || relative.isEmpty() || (!hidden && relative.split(QChar('/')).filter(QRegularExpression(QStringLiteral("^\\."))).size()) || !QFileInfo(inner).fileName().contains(pattern, Qt::CaseInsensitive)) continue;
            Entry entry; entry.source = Source::Archive; entry.path = location.path; entry.inner = inner; entry.name = QFileInfo(inner).fileName(); entry.directory = file.isDir; entry.size = file.size; entry.modified = file.lastModified; entry.detail = inner; result.entries.append(entry);
        }
        return result;
    }
    if (location.source != Source::Phone) return {{}, QStringLiteral("请选择本地、手机或压缩包目录")};
    QList<QPair<QString, int>> pending{{location.inner, 0}};
    qsizetype next = 0; int visited = 0;
    while (next < pending.size()) {
        if (cancelled(cancel)) return {{}, QStringLiteral("已取消")};
        const auto current = pending[next++];
        const Listing listing = adbSyncList(location.path, current.first, hidden, cancel);
        if (!listing.error.isEmpty()) return {{}, listing.error};
        for (auto entry : listing.entries) {
            if (++visited > 200000 || result.entries.size() >= 100000) return {result.entries, QStringLiteral("搜索达到数量上限，请缩小范围")};
            if (entry.name.contains(pattern, Qt::CaseInsensitive)) { entry.detail = entry.path; result.entries.append(entry); }
            if (entry.directory) { if (current.second >= 128) return {result.entries, QStringLiteral("目录深度超过 128")}; pending.append({entry.path, current.second + 1}); }
        }
    }
    return result;
}

static Result copyTree(const QString &source, const QString &destination, const Cancellation &cancel, bool *created = nullptr, const Progress &progress = {}) {
    if (cancelled(cancel)) return failure(QStringLiteral("操作已取消"));
    const QFileInfo info(source);
    if (info.isSymLink() || info.isJunction()) return failure(QStringLiteral("暂不复制符号链接或联接点：") + source);
    if (QFileInfo::exists(destination)) return failure(QStringLiteral("目标已存在：") + destination);
    if (info.isDir()) {
        if (!QDir().mkdir(destination)) return failure(QStringLiteral("无法创建：") + destination);
        if (created) *created = true;
        for (const QFileInfo &child : QDir(source).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
            Result result = copyTree(child.absoluteFilePath(), QDir(destination).filePath(child.fileName()), cancel, nullptr, progress);
            if (!result.ok) return result;
        }
    } else {
        QFile input(source);
        QFile output(destination);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return failure(QStringLiteral("无法读写或目标已经存在：") + source);
        if (created) *created = true;
        while (!input.atEnd()) {
            if (cancelled(cancel)) return failure(QStringLiteral("操作已取消"));
            const QByteArray block = input.read(1024 * 1024);
            if ((block.isEmpty() && input.error() != QFile::NoError) || output.write(block) != block.size()) return failure(QStringLiteral("复制失败：") + source);
            if (progress) progress(block.size(), 0, info.fileName());
        }
        if (!output.flush()) return failure(output.errorString());
        output.setFileTime(info.lastModified(), QFileDevice::FileModificationTime);
        output.setPermissions(info.permissions());
    }
#ifdef Q_OS_WIN
    const HANDLE sourceHandle = CreateFileW(reinterpret_cast<LPCWSTR>(source.utf16()), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    const HANDLE targetHandle = CreateFileW(reinterpret_cast<LPCWSTR>(destination.utf16()), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (sourceHandle != INVALID_HANDLE_VALUE && targetHandle != INVALID_HANDLE_VALUE) {
        FILETIME createdTime{}, accessedTime{}, modifiedTime{};
        if (GetFileTime(sourceHandle, &createdTime, &accessedTime, &modifiedTime)) SetFileTime(targetHandle, &createdTime, &accessedTime, &modifiedTime);
    }
    if (sourceHandle != INVALID_HANDLE_VALUE) CloseHandle(sourceHandle);
    if (targetHandle != INVALID_HANDLE_VALUE) CloseHandle(targetHandle);
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(source.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(reinterpret_cast<LPCWSTR>(destination.utf16()), attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE));
#endif
    return {};
}

Result transferFiles(const QStringList &sources, const QString &destination, bool move,
                     const Cancellation &cancel, const Progress &progress) {
    if (!QFileInfo(destination).isDir()) return failure(QStringLiteral("目标不是本地文件夹"));
    QStringList targets;
    QSet<QString> unique;
    const QString destinationKey = normalizedPath(destination);
    for (const QString &source : sources) {
        const QFileInfo info(source);
        if (!info.exists() || info.isSymLink() || info.isJunction()) return failure(QStringLiteral("来源不存在或为链接：") + source);
        const QString sourceKey = normalizedPath(source);
        for (const QString &other : sources) {
            if (normalizedPath(other).startsWith(sourceKey + QChar('/'))) return failure(QStringLiteral("不能同时传输父目录与其子项"));
        }
        if (info.isDir() && (destinationKey == sourceKey || destinationKey.startsWith(sourceKey + QChar('/')))) return failure(QStringLiteral("不能复制或移动到自身及子文件夹"));
        const QString target = QDir(destination).filePath(info.fileName());
        const QString targetKey = normalizedPath(target);
        if (QFileInfo::exists(target) || unique.contains(targetKey)) return failure(QStringLiteral("目标冲突：") + target);
        unique.insert(targetKey); targets.append(target);
    }
    if (move) {
        bool differentVolume = false;
        const QStorageInfo targetVolume(destination);
        for (const QString &source : sources) if (QStorageInfo(source).device() != targetVolume.device()) differentVolume = true;
        if (differentVolume) {
            Result copied = transferFiles(sources, destination, false, cancel, progress);
            if (!copied.ok) return copied;
            QStringList staged;
            for (qsizetype index = 0; index < sources.size(); ++index) {
                const QString temporary = QFileInfo(sources[index]).dir().filePath(QStringLiteral(".twinfile-move-") + QUuid::createUuid().toString(QUuid::Id128));
                if (cancelled(cancel) || !QDir().rename(sources[index], temporary)) {
                    QStringList recovery;
                    for (qsizetype restored = 0; restored < staged.size(); ++restored) if (!QDir().rename(staged[restored], sources[restored])) recovery.append(staged[restored]);
                    for (const QString &target : targets) { if (QFileInfo(target).isDir()) QDir(target).removeRecursively(); else QFile::remove(target); }
                    return failure(QStringLiteral("跨卷移动已取消或来源无法删除。未恢复项目：\n") + recovery.join(QChar('\n')));
                }
                staged.append(temporary);
            }
            QStringList retained;
            for (const QString &temporary : staged) { const bool removed = QFileInfo(temporary).isDir() ? QDir(temporary).removeRecursively() : QFile::remove(temporary); if (!removed) retained.append(temporary); }
            if (!retained.isEmpty()) return failure(QStringLiteral("文件已复制到目标，但来源暂存文件未能删除：\n") + retained.join(QChar('\n')));
            return copied;
        }
        qsizetype finished = 0;
        for (; finished < sources.size() && !cancelled(cancel); ++finished) {
#ifdef Q_OS_WIN
            if (!MoveFileW(reinterpret_cast<LPCWSTR>(sources[finished].utf16()), reinterpret_cast<LPCWSTR>(targets[finished].utf16()))) break;
#else
            if (!QDir().rename(sources[finished], targets[finished])) break;
#endif
        }
        if (finished != sources.size()) {
            QStringList failed;
            while (finished > 0) {
                --finished;
                if (!QDir().rename(targets[finished], sources[finished])) failed.append(targets[finished]);
            }
            return failure(QStringLiteral("移动取消或失败；未恢复项目：\n") + failed.join(QChar('\n')));
        }
        return {true, {}, {}, targets};
    }
    qint64 bytesDone = 0, bytesTotal = 0;
    if (progress) {
        for (const QString &source : sources) {
            if (cancelled(cancel)) return failure(QStringLiteral("操作已取消"));
            if (QFileInfo(source).isFile()) bytesTotal += QFileInfo(source).size();
            else {
                QDirIterator iterator(source, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
                while (iterator.hasNext()) {
                    if (cancelled(cancel)) return failure(QStringLiteral("操作已取消"));
                    iterator.next(); const QFileInfo info = iterator.fileInfo();
                    if (info.isFile() && !info.isSymLink() && !info.isJunction()) bytesTotal += info.size();
                }
            }
        }
        progress(0, bytesTotal, QStringLiteral("复制文件"));
    }
    const Progress copiedBytes = [&](qint64 count, qint64, const QString &name) { bytesDone += count; if (progress) progress(bytesDone, bytesTotal, name); };
    QStringList createdTargets;
    for (qsizetype index = 0; index < sources.size(); ++index) {
        bool created = false;
        Result result = copyTree(sources[index], targets[index], cancel, &created, progress ? copiedBytes : Progress{});
        if (created) createdTargets.append(targets[index]);
        if (!result.ok) {
            for (const QString &target : createdTargets) {
                if (QFileInfo(target).isDir()) QDir(target).removeRecursively();
                else QFile::remove(target);
            }
            return result;
        }
    }
    return {true, {}, {}, targets};
}

Result batchRename(const QStringList &paths, const QStringList &names) {
    if (paths.isEmpty() || paths.size() != names.size()) return failure(QStringLiteral("重命名列表无效"));
    QSet<QString> sources, targets;
    QStringList destinations, staged;
    for (const QString &path : paths) sources.insert(normalizedPath(path));
    if (sources.size() != paths.size()) return failure(QStringLiteral("重复来源"));
    for (qsizetype index = 0; index < paths.size(); ++index) {
        const QFileInfo info(paths[index]);
        if (!info.exists() || info.isSymLink() || info.isJunction()) return failure(QStringLiteral("无效来源：") + paths[index]);
        const QString invalid = validateName(names[index]);
        if (!invalid.isEmpty()) return failure(invalid);
        const QString target = info.dir().filePath(names[index]);
        const QString key = normalizedPath(target);
        if (targets.contains(key) || (QFileInfo::exists(target) && !sources.contains(key))) return failure(QStringLiteral("重名冲突：") + target);
        for (const QString &other : paths) {
            if (normalizedPath(other).startsWith(normalizedPath(paths[index]) + QChar('/'))) return failure(QStringLiteral("不能同时重命名父目录及其子项"));
        }
        targets.insert(key); destinations.append(target);
        staged.append(info.dir().filePath(QStringLiteral(".twinfile-") + QUuid::createUuid().toString(QUuid::WithoutBraces)));
    }
    qsizetype stagedCount = 0;
    for (; stagedCount < paths.size(); ++stagedCount) {
        if (!QDir().rename(paths[stagedCount], staged[stagedCount])) break;
    }
    if (stagedCount != paths.size()) {
        QStringList failed;
        for (qsizetype index = stagedCount; index > 0; --index) if (!QDir().rename(staged[index - 1], paths[index - 1])) failed.append(staged[index - 1]);
        return failure(QStringLiteral("准备重命名失败。回滚失败时文件保留在：") + failed.join(QChar('\n')));
    }
    qsizetype finished = 0;
    for (; finished < paths.size(); ++finished) if (!QDir().rename(staged[finished], destinations[finished])) break;
    if (finished != paths.size()) {
        QStringList failed;
        for (qsizetype index = 0; index < finished; ++index) if (!QDir().rename(destinations[index], staged[index])) failed.append(destinations[index]);
        for (qsizetype index = 0; index < paths.size(); ++index) if (!QDir().rename(staged[index], paths[index])) failed.append(staged[index]);
        return failure(QStringLiteral("重命名失败；未恢复的文件位置：\n") + failed.join(QChar('\n')));
    }
    return {true, {}, {}, destinations};
}

Result recycleFiles(const QStringList &paths) {
    for (const QString &path : paths) {
        if (!QFile::moveToTrash(path)) return failure(QStringLiteral("无法移入回收站，未执行永久删除：") + path);
    }
    return {};
}

QByteArray readArchiveFile(const QString &path, const QString &inner, qint64 limit, QString *error) {
    ZipReader reader(path);
    for (const auto &file : reader.fileInfoList()) {
        if (file.filePath != inner) continue;
        if (!file.isFile || file.isSymLink || file.size < 0 || file.size > limit) { *error = QStringLiteral("文件过大或类型不支持"); return {}; }
        const QByteArray data = reader.fileData(inner, limit);
        if (data.size() != file.size || reader.status() != ZipReader::NoError) *error = QStringLiteral("压缩文件损坏");
        return data;
    }
    *error = QStringLiteral("压缩包内文件不存在");
    return {};
}

Result extractArchive(const QString &path, const QString &destination, const QString &prefix, const Cancellation &cancel, const Progress &progress) {
    ZipReader reader(path);
    const auto files = reader.fileInfoList();
    if (reader.status() != ZipReader::NoError) return failure(reader.errorString());
    if (!QFileInfo(destination).isDir()) return failure(QStringLiteral("目标文件夹不存在"));
    qint64 total = 0;
    QSet<QString> targets, regularFiles;
    for (const auto &file : files) {
        if (!prefix.isEmpty() && file.filePath != prefix && !file.filePath.startsWith(prefix + QChar('/'))) continue;
        if (safeArchiveName(file.filePath).isEmpty() || file.isSymLink || file.size > 64LL * 1024 * 1024 * 1024
            || file.size < 0 || (total += file.size) > 256LL * 1024 * 1024 * 1024) return failure(QStringLiteral("不安全的路径、链接或超出解压限制"));
        QString target = QDir(destination).filePath(file.filePath);
        const QString key = QDir::cleanPath(target).toCaseFolded();
        if (targets.contains(key)) return failure(QStringLiteral("压缩包路径重复或大小写冲突：") + file.filePath);
        targets.insert(key); if (!file.isDir) regularFiles.insert(key);
        if (QFileInfo::exists(target) && (!file.isDir || !QFileInfo(target).isDir())) return failure(QStringLiteral("目标已存在：") + target);
        QString parent = QFileInfo(target).absolutePath();
        while (parent.size() >= QFileInfo(destination).absoluteFilePath().size()) {
            const QFileInfo info(parent);
            if (info.isSymLink() || info.isJunction()) return failure(QStringLiteral("解压路径包含链接：") + parent);
            const QString next = info.absolutePath(); if (next == parent) break; parent = next;
        }
    }
    for (const QString &target : targets) {
        QString parent = QFileInfo(target).absolutePath().toCaseFolded();
        while (parent.size() > QFileInfo(destination).absoluteFilePath().size()) {
            if (regularFiles.contains(parent)) return failure(QStringLiteral("压缩包文件与父目录路径冲突"));
            const QString next = QFileInfo(parent).absolutePath(); if (next == parent) break; parent = next;
        }
    }
    const QStorageInfo storage(destination);
    if (storage.isValid() && storage.bytesAvailable() >= 0 && total > storage.bytesAvailable()) return failure(QStringLiteral("目标磁盘可用空间不足"));
    qint64 done = 0;
    for (const auto &file : files) {
        if (!prefix.isEmpty() && file.filePath != prefix && !file.filePath.startsWith(prefix + QChar('/'))) continue;
        if (cancelled(cancel)) return failure(QStringLiteral("解压已取消；已完成的文件保留在目标目录"));
        const QString target = QDir(destination).filePath(file.filePath);
        if (file.isDir) { if (!QDir().mkpath(target)) return failure(QStringLiteral("无法创建：") + target); continue; }
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) return failure(QStringLiteral("无法创建父目录"));
        QFile output(target);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return failure(QStringLiteral("无法创建或目标已存在：") + target);
        if (!reader.streamFile(file.filePath, &output, cancel, [&](qint64 current, qint64, const QString &name) { if (progress) progress(done + current, total, name); }) || !output.flush()) { output.close(); QFile::remove(target); return failure(reader.errorString().isEmpty() ? QStringLiteral("写入失败：") + target : reader.errorString()); }
        done += file.size;
    }
    return {};
}

Result createArchive(const QStringList &paths, const QString &destination, const Cancellation &cancel, const Progress &progress) {
    if (paths.isEmpty()) return failure(QStringLiteral("没有压缩来源"));
    if (QFileInfo::exists(destination)) return failure(QStringLiteral("压缩包目标已存在"));
    const QFileInfo destinationInfo(destination);
    const QString destinationParent = destinationInfo.dir().canonicalPath();
    if (destinationParent.isEmpty()) return failure(QStringLiteral("压缩包目标文件夹不存在"));
    const QString invalid = validateName(destinationInfo.fileName());
    if (!invalid.isEmpty()) return failure(invalid);
    const QString destinationKey = normalizedPath(QDir(destinationParent).filePath(destinationInfo.fileName()));
    QSet<QString> sourceKeys;
    for (const QString &path : paths) {
        const QString key = normalizedPath(path);
        if (sourceKeys.contains(key)) return failure(QStringLiteral("重复压缩来源"));
        sourceKeys.insert(key);
        for (const QString &other : paths) if (normalizedPath(other).startsWith(key + QChar('/'))) return failure(QStringLiteral("不能同时压缩父目录与其子项"));
        if (QFileInfo(path).isDir() && destinationKey.startsWith(key + QChar('/'))) return failure(QStringLiteral("不能将压缩包输出到来源文件夹内部"));
    }
    // QTemporaryFile cannot be created reliably when the final path traverses
    // a Windows junction. Stage in the system temp directory, then copy
    // through the lexical destination path after the archive is complete.
    QTemporaryFile output(QDir(QDir::tempPath()).filePath(QStringLiteral(".twinfile-XXXXXX.tmp")));
    if (!output.open()) return failure(QStringLiteral("无法创建临时压缩包：") + output.errorString());
    ZipWriter writer(&output);
    QStringList files;
    QSet<QString> names;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (names.contains(info.fileName().toCaseFolded())) return failure(QStringLiteral("存在重复文件名"));
        names.insert(info.fileName().toCaseFolded()); files.append(path);
        if (info.isDir()) {
            QDirIterator iterator(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
            while (iterator.hasNext()) files.append(iterator.next());
        }
    }
    qint64 done = 0, total = 0;
    for (const QString &path : files) if (QFileInfo(path).isFile()) total += QFileInfo(path).size();
    for (const QString &path : files) {
        if (cancelled(cancel)) return failure(QStringLiteral("操作已取消"));
        const QFileInfo info(path);
        if (!info.exists() || info.isSymLink() || info.isJunction()) return failure(QStringLiteral("来源不存在或为链接"));
        QString relative;
        for (const QString &root : paths) {
            if (path == root || normalizedPath(path).startsWith(normalizedPath(root) + QChar('/'))) {
                relative = QFileInfo(root).fileName() + (path == root ? QString() : QChar('/') + QDir(root).relativeFilePath(path)); break;
            }
        }
        if (safeArchiveName(relative).isEmpty()) return failure(QStringLiteral("无效压缩路径"));
        if (info.isDir()) writer.addDirectory(relative);
        else { QFile input(path); if (!input.open(QIODevice::ReadOnly)) return failure(input.errorString()); writer.addFile(relative, &input, cancel, nullptr, [&](qint64 current, qint64, const QString &name) { if (progress) progress(done + current, total, name); }); done += info.size(); }
        if (writer.status() != ZipWriter::NoError) return failure(QStringLiteral("压缩失败或已取消"));
    }
    writer.close();
    if (writer.status() != ZipWriter::NoError) return failure(QStringLiteral("压缩包保存失败"));
    const QString temporaryPath = output.fileName();
    output.close();
    bool copied = false;
#ifdef Q_OS_WIN
    const QString nativeTemporaryPath = QDir::toNativeSeparators(temporaryPath);
    const QString nativeDestination = QDir::toNativeSeparators(destination);
    copied = CopyFileW(reinterpret_cast<LPCWSTR>(nativeTemporaryPath.utf16()),
                       reinterpret_cast<LPCWSTR>(nativeDestination.utf16()), FALSE) != FALSE;
#else
    copied = QFile::copy(temporaryPath, destination);
#endif
    if (!copied) return failure(QStringLiteral("保存失败或目标已存在：") + destination);
    return {true, {}, {}, {destination}};
}

Result checksum(const QStringList &paths, const Cancellation &cancel) {
    QStringList output;
    for (const QString &path : paths) {
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly)) return failure(input.errorString());
        QCryptographicHash md5(QCryptographicHash::Md5), sha1(QCryptographicHash::Sha1), sha256(QCryptographicHash::Sha256);
        while (!input.atEnd()) {
            if (cancelled(cancel)) return failure(QStringLiteral("操作已取消"));
            const QByteArray block = input.read(1024 * 1024);
            if (input.error() != QFile::NoError) return failure(input.errorString());
            md5.addData(block); sha1.addData(block); sha256.addData(block);
        }
        output.append(QStringLiteral("%1\nMD5    %2\nSHA1   %3\nSHA256 %4\n").arg(path, QString::fromLatin1(md5.result().toHex()), QString::fromLatin1(sha1.result().toHex()), QString::fromLatin1(sha256.result().toHex())));
    }
    return {true, {}, output.join(QChar('\n')), {}};
}

Result phoneTransfer(const QList<Entry> &entries, const Location &destination, bool move, const Cancellation &cancel, const Progress &progress) {
    const QString adb = toolPath(QStringLiteral("adb/adb.exe"));
    qint64 completedBytes = 0;
    for (const Entry &entry : entries) {
        const QString invalid = validateName(entry.name);
        if (!invalid.isEmpty()) return failure(invalid);
    }
    for (const Entry &entry : entries) {
        Result result;
        qint64 entryBytes = 0;
        const Progress entryProgress = [&](qint64 done, qint64 total, const QString &stage) {
            entryBytes = qMax(entryBytes, done);
            if (progress) progress(completedBytes + done, entries.size() == 1 && completedBytes == 0 ? total : 0, stage);
        };
        if (entry.source == Source::Local && destination.source == Source::Phone) {
            const QString target = destination.inner + QChar('/') + entry.name;
            Result exists = runProcess(adb, {QStringLiteral("-s"), destination.path, QStringLiteral("shell"), QStringLiteral("if test -e ") + shellQuote(target) + QStringLiteral(" || test -L ") + shellQuote(target) + QStringLiteral("; then printf exists; else printf missing; fi")}, 15000, cancel);
            if (!exists.ok) return exists;
            if (exists.output.trimmed() != QStringLiteral("missing")) return failure(QStringLiteral("手机目标已存在或无法确认：") + target);
            const QString staged = destination.inner + QStringLiteral("/.twinfile-push-") + QUuid::createUuid().toString(QUuid::Id128);
            result = adbSyncPush(destination.path, entry.path, staged, cancel, entryProgress);
            if (result.ok) result = phoneCommand(destination.path, QStringLiteral("if test -e ") + shellQuote(target) + QStringLiteral(" || test -L ") + shellQuote(target) + QStringLiteral("; then exit 1; else mv -n -- ") + shellQuote(staged) + QChar(' ') + shellQuote(target) + QStringLiteral(" && ! test -e ") + shellQuote(staged) + QStringLiteral("; fi"), cancel);
            if (!result.ok) phoneCommand(destination.path, QStringLiteral("rm -rf -- ") + shellQuote(staged));
        } else if (entry.source == Source::Phone && destination.source == Source::Local) {
            const QString target = QDir(destination.path).filePath(entry.name);
            const QFileInfo targetInfo(target);
            if (targetInfo.exists() || targetInfo.isSymLink() || targetInfo.isJunction()) return failure(QStringLiteral("本地目标已存在：") + target);
            QTemporaryDir staging(QDir(destination.path).filePath(QStringLiteral(".twinfile-pull-XXXXXX")));
            if (!staging.isValid()) return failure(QStringLiteral("无法创建手机下载暂存目录"));
            const QString staged = staging.filePath(entry.name);
            result = adbSyncPull(entry.serial, entry.path, staged, cancel, entryProgress);
            if (!result.ok) return result;
            result = transferFiles({staged}, destination.path, true, cancel);
        } else if (entry.source == Source::Phone && destination.source == Source::Phone && entry.serial != destination.path) {
            QTemporaryDir staging;
            if (!staging.isValid()) return failure(QStringLiteral("无法创建跨设备暂存目录"));
            const QString local = staging.filePath(entry.name);
            result = adbSyncPull(entry.serial, entry.path, local, cancel, entryProgress);
            if (!result.ok) return result;
            completedBytes += entryBytes; entryBytes = 0;
            Entry stagedEntry; stagedEntry.source = Source::Local; stagedEntry.name = entry.name; stagedEntry.path = local; stagedEntry.directory = entry.directory;
            result = phoneTransfer({stagedEntry}, destination, false, cancel, entryProgress);
        } else return failure(QStringLiteral("无效的手机传输来源或目标；请选择文件目录而不是设备列表"));
        if (!result.ok) return result;
        completedBytes += entryBytes;
        if (move) {
            if (cancelled(cancel)) return failure(QStringLiteral("复制已完成，取消后保留源文件"));
            const Result removed = removeEntries({entry}, cancel); if (!removed.ok) return removed;
        }
    }
    return {};
}

QString settingsPath() {
    const QString overridden = qEnvironmentVariable("TWINFILE_SETTINGS");
    if (!overridden.isEmpty()) return overridden;
    return QDir(qEnvironmentVariable("APPDATA", QDir::homePath())).filePath(QStringLiteral("TwinFileNative/settings.json"));
}

QJsonObject readSettings(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QJsonObject migrateLegacySettings(const QJsonObject &legacy) {
    QJsonObject result;
    const QMap<QString, QStringList> choices{
        {QStringLiteral("theme"), {QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("mt"), QStringLiteral("cream"), QStringLiteral("bataUI01")}},
        {QStringLiteral("accent"), {QStringLiteral("blue"), QStringLiteral("teal"), QStringLiteral("green"), QStringLiteral("purple"), QStringLiteral("orange"), QStringLiteral("indigo"), QStringLiteral("mtblue")}},
        {QStringLiteral("sort"), {QStringLiteral("name"), QStringLiteral("size"), QStringLiteral("mtime")}}};
    for (auto choice = choices.cbegin(); choice != choices.cend(); ++choice) result.insert(choice.key(), choice.value().contains(legacy.value(choice.key()).toString()) ? legacy.value(choice.key()).toString() : choice.value().first());
    result.insert(QStringLiteral("show_hidden"), legacy.value(QStringLiteral("show_hidden")).toBool());
    result.insert(QStringLiteral("descending"), legacy.value(QStringLiteral("desc")).toBool());
    const QString geometry = legacy.value(QStringLiteral("geometry")).toString(); if (geometry.size() <= 65536) result.insert(QStringLiteral("geometry"), geometry);
    const QStringList orders{QStringLiteral("list"), QStringLiteral("one"), QStringLiteral("shuffle")}; result.insert(QStringLiteral("player_order"), qMax(0, int(orders.indexOf(legacy.value(QStringLiteral("player_order")).toString()))));
    QJsonArray favorites;
    for (const auto &value : legacy.value(QStringLiteral("favorites")).toArray()) if (value.isString() && favorites.size() < 1000 && QFileInfo(value.toString()).isDir() && !favorites.contains(value)) favorites.append(value);
    result.insert(QStringLiteral("favorites"), favorites);
    const auto workspace = legacy.value(QStringLiteral("workspace")).toObject(); QJsonObject converted;
    for (const QString &side : {QStringLiteral("left"), QStringLiteral("right")}) {
        const auto panel = workspace.value(side).toObject(); QJsonArray tabs;
        for (const auto &value : panel.value(QStringLiteral("tabs")).toArray()) {
            if (tabs.size() >= 32) break;
            Location location;
            const auto stack = value.toArray();
            if (stack.size() > 256) continue;
            for (const auto &node : stack) {
                const auto object = node.toObject(); const QString kind = object.value(QStringLiteral("kind")).toString();
                const Source source = kind == QStringLiteral("dir") ? Source::Local : kind == QStringLiteral("zip") ? Source::Archive : kind == QStringLiteral("device") ? Source::Phone : Source::Drive;
                const Location candidate = Location::fromJson({{QStringLiteral("source"), int(source)}, {QStringLiteral("path"), object.value(QStringLiteral("path"))}, {QStringLiteral("inner"), object.value(QStringLiteral("inner"))}, {QStringLiteral("label"), object.value(QStringLiteral("label"))}});
                if (candidate.source != Source::Drive) location = candidate;
            }
            tabs.append(location.json());
        }
        if (tabs.isEmpty()) tabs.append(Location().json());
        converted.insert(side, QJsonObject{{QStringLiteral("tabs"), tabs}, {QStringLiteral("current"), qBound(0, panel.value(QStringLiteral("current")).toInt(), int(tabs.size()) - 1)}});
    }
    converted.insert(QStringLiteral("active"), workspace.value(QStringLiteral("active")).toString() == QStringLiteral("right") ? QStringLiteral("right") : QStringLiteral("left"));
    result.insert(QStringLiteral("workspace"), converted);
    return result;
}

bool writeSettings(const QString &path, const QJsonObject &settings, QString *error) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) { if (error) *error = QStringLiteral("无法创建配置目录"); return false; }
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(settings).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) { if (error) *error = file.errorString(); return false; }
    return true;
}

}
