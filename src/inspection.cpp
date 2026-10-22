#include "inspection.h"
#include "operations.h"
#include "editor.h"
#include "zip.h"
#include "adbsync.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

namespace twinfile {
namespace {
class HashSink : public QIODevice {
public:
    QCryptographicHash md5{QCryptographicHash::Md5}, sha1{QCryptographicHash::Sha1}, sha256{QCryptographicHash::Sha256};
    HashSink() { open(QIODevice::WriteOnly); }
protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 size) override { const QByteArrayView bytes(data, size); md5.addData(bytes); sha1.addData(bytes); sha256.addData(bytes); return size; }
};
Result failed(const QString &error) { return {false, error, {}, {}}; }
QByteArray digest(const QString &path, QCryptographicHash::Algorithm algorithm, const Cancellation &cancel) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(algorithm);
    while (!input.atEnd()) {
        if (cancel && cancel->load()) return {};
        const QByteArray block = input.read(1024 * 1024);
        if (input.error() != QFile::NoError) return {};
        hash.addData(block);
    }
    return hash.result().toHex();
}
bool boundedFile(const QString &base, const QString &relative) {
    if (safeArchiveName(relative).isEmpty()) return false;
    QString current = QFileInfo(base).absoluteFilePath();
    for (const QString &part : relative.split(QChar('/'))) {
        current = QDir(current).filePath(part);
        const QFileInfo info(current);
        if (info.isSymLink() || info.isJunction()) return false;
    }
    return QFileInfo(current).isFile();
}
}
QByteArray normalizedChecksum(const QString &text) {
    if (text.size() > 65536) return {};
    static const QRegularExpression pattern(QStringLiteral("(?<![A-Fa-f0-9])([A-Fa-f0-9]{64}|[A-Fa-f0-9]{40}|[A-Fa-f0-9]{32})(?![A-Fa-f0-9])"));
    auto matches = pattern.globalMatch(text);
    QByteArray result;
    while (matches.hasNext()) { const QByteArray value = matches.next().captured(1).toLatin1().toLower(); if (!result.isEmpty() && result != value) return {}; result = value; }
    return result;
}
Result checksumEntries(const QList<Entry> &entries, const Cancellation &cancel, const QString &expected) {
    const QByteArray normalized = normalizedChecksum(expected);
    if (!expected.isEmpty() && (normalized.isEmpty() || entries.size() != 1)) return failed(QStringLiteral("请只选一个文件，并提供唯一的 MD5、SHA1 或 SHA256 校验值"));
    QStringList output;
    for (const auto &entry : entries) {
        if (entry.directory) return failed(QStringLiteral("请选择普通文件"));
        if (cancel && cancel->load()) return failed(QStringLiteral("已取消"));
        HashSink sink;
        if (entry.source == Source::Local) {
            QFile input(entry.path); if (!input.open(QIODevice::ReadOnly)) return failed(input.errorString());
            while (!input.atEnd()) { if (cancel && cancel->load()) return failed(QStringLiteral("已取消")); const QByteArray block = input.read(1024 * 1024); if (input.error() != QFile::NoError) return failed(input.errorString()); sink.write(block); }
        } else if (entry.source == Source::Archive) { ZipReader reader(entry.path); if (!reader.streamFile(entry.inner, &sink, cancel)) return failed(reader.errorString()); }
        else if (entry.source == Source::Phone) { const Result result = adbSyncRead(entry.serial, entry.path, &sink, cancel); if (!result.ok) return result; }
        else return failed(QStringLiteral("不支持此来源"));
        output.append(QStringLiteral("%1%2\nMD5    %3\nSHA1   %4\nSHA256 %5\n").arg(entry.path, entry.inner.isEmpty() ? QString() : QStringLiteral("!/") + entry.inner, QString::fromLatin1(sink.md5.result().toHex()), QString::fromLatin1(sink.sha1.result().toHex()), QString::fromLatin1(sink.sha256.result().toHex())));
        if (!normalized.isEmpty()) {
            const QByteArray actual = normalized.size() == 32 ? sink.md5.result().toHex() : normalized.size() == 40 ? sink.sha1.result().toHex() : sink.sha256.result().toHex();
            output.append(QStringLiteral("预期：%1\n%2").arg(QString::fromLatin1(normalized), actual == normalized ? QStringLiteral("校验通过：一致") : QStringLiteral("校验失败：不一致")));
        }
    }
    return {true, {}, output.join(QChar('\n')), {}};
}
Result checksumManifest(const QStringList &paths, const QString &base, QCryptographicHash::Algorithm algorithm, const Cancellation &cancel) {
    if (algorithm != QCryptographicHash::Md5 && algorithm != QCryptographicHash::Sha1 && algorithm != QCryptographicHash::Sha256) return failed(QStringLiteral("不支持的校验算法"));
    QStringList lines;
    QSet<QString> seen;
    for (const QString &path : paths) {
        const QString relative = QDir(base).relativeFilePath(path);
        if (relative.contains(QChar('\n')) || relative.contains(QChar('\r')) || !boundedFile(base, relative) || seen.contains(relative.toCaseFolded())) return failed(QStringLiteral("清单只能包含基准目录内不重复的普通文件"));
        seen.insert(relative.toCaseFolded());
        const QByteArray hash = digest(path, algorithm, cancel);
        if (hash.isEmpty()) return failed(QStringLiteral("校验读取失败或已取消：") + path);
        lines.append(QString::fromLatin1(hash) + QStringLiteral(" *") + relative);
    }
    if (lines.isEmpty()) return failed(QStringLiteral("没有可导出的文件"));
    return {true, {}, lines.join(QChar('\n')) + QChar('\n'), {}};
}
Result verifyChecksumManifest(const QString &manifest, const QString &base, const Cancellation &cancel) {
    QFile input(manifest);
    if (!input.open(QIODevice::ReadOnly) || input.size() > 16 * 1024 * 1024 || !QFileInfo(base).isDir()) return failed(QStringLiteral("无法读取清单，超过 16 MiB，或基准目录无效"));
    TextDocument document;
    const QByteArray bytes = input.readAll();
    if (!decodeText(bytes, {}, &document).ok && !decodeText(bytes, QStringLiteral("GB18030"), &document).ok) return failed(QStringLiteral("清单编码无效"));
    static const QRegularExpression standard(QStringLiteral("^([A-Fa-f0-9]{32}|[A-Fa-f0-9]{40}|[A-Fa-f0-9]{64})[ \\t]+[ *]?(.+)$"));
    static const QRegularExpression bsd(QStringLiteral("^(MD5|SHA1|SHA256) \\((.+)\\) = ([A-Fa-f0-9]+)$"), QRegularExpression::CaseInsensitiveOption);
    QList<QPair<QString, QByteArray>> items;
    for (QString line : document.text.split(QChar('\n'))) {
        if (line.endsWith(QChar('\r'))) line.chop(1);
        if (line.trimmed().isEmpty() || line.startsWith(QChar('#')) || line.startsWith(QChar(';'))) continue;
        const auto match = standard.match(line), alternate = bsd.match(line);
        QString name;
        QByteArray expected;
        if (match.hasMatch()) { expected = match.captured(1).toLatin1().toLower(); name = match.captured(2); }
        else if (alternate.hasMatch()) {
            name = alternate.captured(2); expected = alternate.captured(3).toLatin1().toLower();
            const QString kind = alternate.captured(1).toUpper();
            if (expected.size() != (kind == QStringLiteral("MD5") ? 32 : kind == QStringLiteral("SHA1") ? 40 : 64)) return failed(QStringLiteral("BSD 校验算法与摘要长度不匹配"));
        } else return failed(QStringLiteral("清单格式无法识别，未执行：") + line.left(120));
        name.replace(QChar('\\'), QChar('/'));
        if (safeArchiveName(name).isEmpty() || items.size() >= 100000) return failed(QStringLiteral("清单含不安全路径或条目过多，未执行"));
        items.append({name, expected});
    }
    if (items.isEmpty()) return failed(QStringLiteral("清单没有有效条目"));
    QStringList output;
    int passed = 0;
    for (const auto &item : items) {
        if (cancel && cancel->load()) return failed(QStringLiteral("校验已取消"));
        if (!boundedFile(base, item.first)) { output.append(QStringLiteral("缺失/链接/非普通文件：") + item.first); continue; }
        const auto algorithm = item.second.size() == 32 ? QCryptographicHash::Md5 : item.second.size() == 40 ? QCryptographicHash::Sha1 : QCryptographicHash::Sha256;
        const QByteArray actual = digest(QDir(base).filePath(item.first), algorithm, cancel);
        if (actual == item.second) { ++passed; output.append(QStringLiteral("通过：") + item.first); }
        else output.append((actual.isEmpty() ? QStringLiteral("无法读取：") : QStringLiteral("不一致：")) + item.first);
    }
    output.prepend(QStringLiteral("通过 %1 / %2；失败 %3").arg(passed).arg(items.size()).arg(items.size() - passed));
    return {true, {}, output.join(QChar('\n')), {}};
}
DirectoryComparison inspectDirectories(const QString &left, const QString &right, int mode, const Cancellation &cancel) {
    auto invalid = [](const QString &error) { DirectoryComparison result; result.error = error; return result; };
    if (!QFileInfo(left).isDir() || !QFileInfo(right).isDir()) return invalid(QStringLiteral("请选择两个文件夹"));
    if (mode < 0 || mode > 2) return invalid(QStringLiteral("无效的比较方式"));
    QMap<QString, QFileInfo> first, second;
    auto collect = [&](const QString &root, QMap<QString, QFileInfo> &files) {
        QDirIterator iterator(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            if ((cancel && cancel->load()) || files.size() >= 200000) return false;
            iterator.next();
            files.insert(QDir(root).relativeFilePath(iterator.filePath()), iterator.fileInfo());
        }
        return true;
    };
    if (!collect(left, first) || !collect(right, second)) return invalid(QStringLiteral("已取消或目录超过 20 万条目"));
    DirectoryComparison result;
    QStringList output;
    QSet<QString> unique;
    for (auto item = first.cbegin(); item != first.cend(); ++item) unique.insert(item.key());
    for (auto item = second.cbegin(); item != second.cend(); ++item) unique.insert(item.key());
    QStringList names(unique.cbegin(), unique.cend()); names.sort(Qt::CaseInsensitive);
    for (const QString &name : names) {
        if (cancel && cancel->load()) return invalid(QStringLiteral("已取消"));
        const QFileInfo before = first.value(name), after = second.value(name);
        DirectoryRow row{name, {}, first.contains(name) ? before.filePath() : QString(), second.contains(name) ? after.filePath() : QString(), before.isDir() || after.isDir()};
        const bool beforeLink = before.isSymLink() || before.isJunction(), afterLink = after.isSymLink() || after.isJunction();
        if (!first.contains(name)) row.status = QStringLiteral("仅右侧");
        else if (!second.contains(name)) row.status = QStringLiteral("仅左侧");
        else if (beforeLink != afterLink || before.isDir() != after.isDir() || before.isFile() != after.isFile()) row.status = QStringLiteral("类型不同");
        else if (beforeLink) { if (before.symLinkTarget() != after.symLinkTarget()) row.status = QStringLiteral("链接目标不同"); }
        else if (before.isDir()) {}
        else if (before.size() != after.size()) row.status = QStringLiteral("大小不同");
        else if (mode == 1 && before.lastModified() != after.lastModified()) row.status = QStringLiteral("修改时间不同");
        else if (mode == 2) {
            const auto beforeHash = fileDigest(before.filePath(), cancel), afterHash = fileDigest(after.filePath(), cancel);
            if (beforeHash.isEmpty() || afterHash.isEmpty()) return invalid(QStringLiteral("读取失败或已取消：") + name);
            if (beforeHash != afterHash) row.status = QStringLiteral("内容不同");
        }
        if (beforeLink || afterLink) row.directory = false;
        if (!row.status.isEmpty()) output.append(row.status + QStringLiteral("：") + name);
        result.rows.append(std::move(row));
    }
    const QString criterion = mode == 2 ? QStringLiteral("类型、名称、空目录、大小和 SHA256 内容") : mode == 1 ? QStringLiteral("类型、名称、空目录、大小和修改时间") : QStringLiteral("类型、名称、空目录和大小（未读取内容）");
    result.summary = QStringLiteral("%1 项 · %2 处差异 · %3").arg(result.rows.size()).arg(output.size()).arg(criterion);
    result.output = criterion + QStringLiteral("\n") + (output.isEmpty() ? QStringLiteral("一致") : output.join(QChar('\n')));
    return result;
}
Result compareDirectories(const QString &left, const QString &right, int mode, const Cancellation &cancel) {
    const auto result = inspectDirectories(left, right, mode, cancel);
    return {result.error.isEmpty(), result.error, result.output, {}};
}
}
