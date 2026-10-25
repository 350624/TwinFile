#include "properties.h"
#include "adbsync.h"
#include "zip.h"

#include <QDir>
#include <QDirIterator>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <limits>

namespace twinfile {

EntryStatistics inspectEntries(const QList<Entry> &entries, const Cancellation &cancel) {
    EntryStatistics result;
    auto stopped = [&] {
        if (cancel && cancel->load()) result.error = QStringLiteral("统计已取消");
        return !result.error.isEmpty();
    };
    auto addFile = [&](qint64 bytes) {
        if (bytes < 0 || result.bytes > std::numeric_limits<qint64>::max() - bytes) result.error = QStringLiteral("文件大小无效或总量超限");
        else { result.bytes += bytes; ++result.files; }
    };
    auto pathOf = [](const Entry &entry) {
        QString path = entry.source == Source::Archive ? entry.inner : QDir::cleanPath(entry.path);
        while (path.endsWith('/')) path.chop(1);
        return entry.source == Source::Local ? path.toCaseFolded() : path;
    };
    auto namespaceOf = [](const Entry &entry) { return QString::number(int(entry.source)) + QChar(0x1f) + entry.serial + QChar(0x1f) + (entry.source == Source::Archive ? entry.path : QString()) + QChar(0x1f); };
    if (entries.size() > 200000) { result.error = QStringLiteral("条目数量超过 200000，统计未完成"); return result; }
    QSet<QString> selectedDirectories, seen;
    for (const auto &entry : entries) if (entry.directory) selectedDirectories.insert(namespaceOf(entry) + pathOf(entry));
    QList<Entry> roots;
    for (const auto &entry : entries) {
        if (stopped()) return result;
        const QString space = namespaceOf(entry), path = pathOf(entry), key = space + path;
        if (seen.contains(key)) continue;
        seen.insert(key);
        QString parent = path; bool contained = !path.isEmpty() && selectedDirectories.contains(space);
        while (!contained && parent.contains('/')) {
            parent.truncate(parent.lastIndexOf('/'));
            contained = selectedDirectories.contains(space + parent);
        }
        if (!contained) roots.append(entry);
    }
    qint64 examined = 0;
    auto check = [&] {
        if (++examined > 200000) result.error = QStringLiteral("条目数量超过 200000，统计未完成");
        return !stopped();
    };
    QSet<QString> inspectedArchives;
    for (const auto &root : roots) {
        if (stopped()) break;
        if (root.source == Source::Archive) {
            if (inspectedArchives.contains(root.path)) continue;
            inspectedArchives.insert(root.path);
            ZipReader reader(root.path);
            if (reader.status() != ZipReader::NoError) { result.error = reader.errorString(); break; }
            QSet<QString> selectedFiles, selectedFolders, found, directories;
            for (const auto &candidate : roots) if (candidate.source == Source::Archive && candidate.path == root.path)
                (candidate.directory ? selectedFolders : selectedFiles).insert(pathOf(candidate));
            if (selectedFolders.contains(QString())) found.insert(QString());
            for (const auto &file : reader.fileInfoList()) {
                if (!check()) break;
                QString name = file.filePath; while (name.endsWith('/')) name.chop(1);
                QString base = name;
                bool inFolder = selectedFolders.contains(base);
                while (!inFolder && base.contains('/')) { base.truncate(base.lastIndexOf('/')); inFolder = selectedFolders.contains(base); }
                if (!inFolder && selectedFolders.contains(QString())) { base.clear(); inFolder = true; }
                if (!inFolder && !selectedFiles.contains(name)) continue;
                found.insert(inFolder ? base : name);
                if (safeArchiveName(file.filePath).isEmpty()) { result.error = QStringLiteral("包内路径无效"); break; }
                if (file.isSymLink) ++result.links;
                else if (file.isFile) addFile(file.size);
                if (!inFolder) continue;
                QString directory = file.isDir ? name : name.section('/', 0, -2);
                while (!directory.isEmpty() && directory != base) {
                    directories.insert(directory); directory = directory.section('/', 0, -2);
                }
            }
            result.directories += directories.size();
            if (found.size() != selectedFiles.size() + selectedFolders.size() && result.error.isEmpty()) result.error = QStringLiteral("包内项目不存在");
        } else if (root.source == Source::Local || root.source == Source::Phone) {
            struct Pending { Entry entry; int depth; };
            QList<Pending> pending{{root, 0}};
            while (!pending.isEmpty() && !stopped()) {
                const auto current = pending.takeLast(); const Entry &entry = current.entry;
                if (!check()) break;
                if (current.depth > 128) { result.error = QStringLiteral("目录层级超过 128，统计未完成"); break; }
                if (entry.source == Source::Local) {
                    const QFileInfo info(entry.path);
                    if (info.isSymLink() || info.isJunction()) { ++result.links; continue; }
                    if (!info.exists() || (!info.isFile() && !info.isDir())) { result.error = QStringLiteral("项目不存在或类型不支持：") + entry.path; break; }
                    if (info.isFile()) { addFile(info.size()); continue; }
                    if (!info.isReadable()) { result.error = QStringLiteral("无法读取目录：") + entry.path; break; }
                    if (current.depth > 0) ++result.directories;
                    QDirIterator children(entry.path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
                    while (children.hasNext() && !stopped()) {
                        const QString path = children.next();
                        if (pending.size() + examined >= 200000) { result.error = QStringLiteral("条目数量超过 200000，统计未完成"); break; }
                        Entry child; child.path = path; pending.append({child, current.depth + 1});
                    }
                } else {
                    if (entry.detail == QStringLiteral("符号链接（不跟随传输）")) { ++result.links; continue; }
                    if (!entry.directory) { addFile(entry.size); continue; }
                    if (current.depth > 0) ++result.directories;
                    const Listing listing = adbSyncList(entry.serial, entry.path, true, cancel);
                    if (!listing.error.isEmpty()) { result.error = listing.error; break; }
                    if (listing.entries.size() + pending.size() + examined > 200000) { result.error = QStringLiteral("条目数量超过 200000，统计未完成"); break; }
                    for (const auto &child : listing.entries) pending.append({child, current.depth + 1});
                }
            }
        } else result.error = QStringLiteral("此项目不提供文件大小统计");
    }
    return result;
}

PropertiesDialog::PropertiesDialog(const QList<Entry> &entries, QWidget *parent) : QDialog(parent) {
    setObjectName(QStringLiteral("fileProperties")); setAttribute(Qt::WA_DeleteOnClose); setWindowTitle(QStringLiteral("属性")); resize(560, 360);
    auto *layout = new QVBoxLayout(this); auto *form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); layout->addLayout(form);
    auto field = [&](const QString &title, const QString &text, const QString &name) {
        auto *label = new QLabel(text); label->setObjectName(name); label->setTextFormat(Qt::PlainText); label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard); label->setWordWrap(true); label->setMinimumWidth(0); form->addRow(title, label); return label;
    };
    if (entries.size() == 1) {
        const Entry &entry = entries.first();
        field(QStringLiteral("名称"), entry.name, QStringLiteral("propertyName"));
        const QString path = entry.source == Source::Archive ? entry.path + QStringLiteral("!/") + entry.inner : entry.source == Source::Phone ? entry.serial + QChar(':') + entry.path : QDir::toNativeSeparators(entry.path);
        field(QStringLiteral("位置"), path, QStringLiteral("propertyPath"));
        field(QStringLiteral("类型"), entry.directory ? QStringLiteral("文件夹") : QStringLiteral("文件"), QStringLiteral("propertyType"));
        const QDateTime modified = entry.source == Source::Local ? QFileInfo(entry.path).lastModified() : entry.modified;
        field(QStringLiteral("修改时间"), modified.isValid() ? modified.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("未知"), QStringLiteral("propertyModified"));
    } else {
        field(QStringLiteral("所选项目"), QStringLiteral("%1 项").arg(entries.size()), QStringLiteral("propertyName"));
    }
    auto *size = field(QStringLiteral("大小"), QStringLiteral("正在计算…"), QStringLiteral("propertySize"));
    auto *contains = field(QStringLiteral("包含"), QStringLiteral("正在统计…"), QStringLiteral("propertyContents"));
    auto *status = new QLabel; status->setObjectName(QStringLiteral("propertyStatus")); status->setTextFormat(Qt::PlainText); status->setWordWrap(true); layout->addWidget(status); layout->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭")); layout->addWidget(buttons); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *watcher = new QFutureWatcher<EntryStatistics>(this);
    connect(watcher, &QFutureWatcher<EntryStatistics>::finished, this, [this, watcher, size, contains, status] {
        const auto result = watcher->result();
        setProperty("statisticsFinished", true);
        if (!result.error.isEmpty()) {
            size->setText(QStringLiteral("未完成")); contains->setText(QStringLiteral("未知")); status->setText(result.error);
        } else {
            size->setText(QStringLiteral("%1（%2 字节）").arg(humanSize(result.bytes)).arg(result.bytes));
            contains->setText(QStringLiteral("%1 个文件，%2 个子文件夹").arg(result.files).arg(result.directories));
            if (result.links) status->setText(QStringLiteral("跳过 %1 个链接，未跟随链接目标").arg(result.links));
        }
    });
    watcher->setFuture(QtConcurrent::run([entries, cancel = cancel_] { return inspectEntries(entries, cancel); }));
}

PropertiesDialog::~PropertiesDialog() { cancel_->store(true); }
void PropertiesDialog::done(int result) { cancel_->store(true); QDialog::done(result); }

}
