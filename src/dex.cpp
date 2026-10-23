#include "dex.h"
#include "editor.h"
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QShortcut>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <QtEndian>

#ifdef TWINFILE_DEX_BRIDGE
static void initializeDexResources() { Q_INIT_RESOURCE(dex_resources); }
#endif

namespace twinfile {
namespace {
Result failure(const QString &error) { return {false, error, {}, {}}; }
class SmaliHighlight : public QSyntaxHighlighter {
public:
    explicit SmaliHighlight(QTextDocument *document) : QSyntaxHighlighter(document) {}
    void highlightBlock(const QString &text) override {
        const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
        const QList<QPair<QString, QColor>> rules{
            {QStringLiteral("\\.[a-z][\\w-]*|\\b(?:public|private|static|final|return-void|const-string|invoke-[\\w/-]+)\\b"), QColor(dark ? "#c4a7fa" : "#7433ad")},
            {QStringLiteral("\\b[vp][0-9]+\\b|\\b0x[0-9a-f]+\\b"), QColor(dark ? "#f4bd82" : "#9a510c")},
            {QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\""), QColor(dark ? "#9ed49a" : "#28702a")},
            {QStringLiteral("#[^\\n]*"), QColor(dark ? "#8f9ba8" : "#647180")}};
        for (const auto &rule : rules) { auto matches = QRegularExpression(rule.first).globalMatch(text); while (matches.hasNext()) { const auto match = matches.next(); setFormat(int(match.capturedStart()), int(match.capturedLength()), rule.second); } }
    }
};
}

bool isDex(const QString &name) { return name.endsWith(QStringLiteral(".dex"), Qt::CaseInsensitive); }

Result validateDex(const QString &path) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly) || input.size() < 112 || input.size() > 64 * 1024 * 1024) return failure(QStringLiteral("DEX 无法读取或超过 64 MiB"));
    const QByteArray header = input.read(112);
    const QByteArray version = header.mid(4, 3);
    if (header.left(4) != QByteArray("dex\n", 4) || header[7] != '\0' || !QList<QByteArray>{"035", "037", "038", "039", "040"}.contains(version)) return failure(QStringLiteral("仅支持标准 DEX 035–040；不支持 CompactDex、ODEX 或 DEX 041 容器"));
    if (qFromLittleEndian<quint32>(header.constData() + 32) != quint64(input.size()) || qFromLittleEndian<quint32>(header.constData() + 36) != 112 || qFromLittleEndian<quint32>(header.constData() + 40) != 0x12345678) return failure(QStringLiteral("DEX 头部或文件长度无效"));
    for (const auto &section : QList<QPair<int, int>>{{56, 4}, {64, 4}, {72, 12}, {80, 8}, {88, 8}, {96, 32}}) {
        const quint64 count = qFromLittleEndian<quint32>(header.constData() + section.first);
        const quint64 offset = qFromLittleEndian<quint32>(header.constData() + section.first + 4);
        if (count > 1000000 || (count && (offset < 112 || offset + count * section.second > quint64(input.size())))) return failure(QStringLiteral("DEX 索引越界"));
    }
    if (qFromLittleEndian<quint32>(header.constData() + 96) > 50000) return failure(QStringLiteral("超过 50000 类上限"));
    return {};
}

Result convertDex(const QString &source, const QString &workspace, const QString &output, const Cancellation &cancel) {
    if (cancel && cancel->load()) return failure(QStringLiteral("操作已取消"));
    const Result checked = validateDex(source); if (!checked.ok) return checked;
#ifdef TWINFILE_DEX_BRIDGE
    initializeDexResources();
    QTemporaryDir helper;
    if (!helper.isValid()) return failure(QStringLiteral("无法准备 DEX 工具"));
    for (const QString &name : {QStringLiteral("DexBridge.class"), QStringLiteral("DexBridge$LimitedWriter.class")}) {
        QFile resource(QStringLiteral(":/dex/") + name);
        if (!resource.copy(helper.filePath(name))) return failure(QStringLiteral("无法准备 DEX 工具"));
    }
    const QString mode = output.isEmpty() ? QStringLiteral("decode") : QStringLiteral("build");
    const auto entries = QDir(workspace).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    if (entries.size() > 50000 || (output.isEmpty() && !entries.isEmpty()) || (!output.isEmpty() && QFileInfo::exists(output))) return failure(QStringLiteral("工作区不为空、过大或编译目标已存在"));
    Result result = runProcess(toolPath(QStringLiteral("runtime/bin/java.exe")), {QStringLiteral("-Xmx768m"), QStringLiteral("-Dfile.encoding=UTF-8"), QStringLiteral("-cp"), helper.path() + QChar(';') + toolPath(QStringLiteral("apktool.jar")), QStringLiteral("DexBridge"), mode, source, workspace, output.isEmpty() ? QStringLiteral("-") : output}, 180000, cancel);
    if (result.ok && !output.isEmpty()) result = validateDex(output);
    return result;
#else
    return failure(QStringLiteral("此构建未包含 DEX 编译桥；请安装完整发行版"));
#endif
}

Result saveDex(const QString &source, const QString &compiled, const QByteArray &expected, const Entry &original, const Cancellation &cancel) {
    const Result checked = validateDex(compiled); if (!checked.ok) return checked;
    if (cancel && cancel->load()) return failure(QStringLiteral("操作已取消"));
    if (expected.isEmpty() || fileDigest(source, cancel) != expected) return failure(QStringLiteral("原 DEX 已被外部修改，未覆盖"));
    if (!original.path.isEmpty() && original.source != Source::Local) return writeBackEntry(original, compiled, expected, cancel);
    QFile input(compiled); if (!input.open(QIODevice::ReadOnly)) return failure(input.errorString());
    QSaveFile output(source); output.setDirectWriteFallback(false); if (!output.open(QIODevice::WriteOnly)) return failure(output.errorString());
    while (!input.atEnd()) { if (cancel && cancel->load()) return failure(QStringLiteral("操作已取消")); const QByteArray bytes = input.read(1024 * 1024); if (bytes.isEmpty() && input.error() != QFileDevice::NoError) return failure(input.errorString()); if (output.write(bytes) != bytes.size()) return failure(output.errorString()); }
    if (fileDigest(source, cancel) != expected || (cancel && cancel->load())) return failure(QStringLiteral("原文件已改变或操作已取消，未覆盖"));
    return output.commit() ? Result{} : failure(output.errorString());
}

DexDialog::DexDialog(const QString &path, QWidget *parent, const Entry &original, std::shared_ptr<QTemporaryDir> temporary, std::function<void()> changed)
    : QDialog(parent), path_(path), original_(original), temporary_(std::move(temporary)), workspace_(std::make_shared<QTemporaryDir>()), changed_(std::move(changed)) {
    setAttribute(Qt::WA_DeleteOnClose); setObjectName(QStringLiteral("dexEditor")); resize(1100, 740); setMinimumSize(720, 480);
    setWindowTitle(QFileInfo(path).fileName() + QStringLiteral(" — Dex 编辑器++"));
    auto *layout = new QVBoxLayout(this); auto *toolbar = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Dex 编辑器++")); toolbar->addWidget(title); toolbar->addStretch();
    save_ = new QPushButton(QStringLiteral("编译保存")); save_->setObjectName(QStringLiteral("dexSave")); export_ = new QPushButton(QStringLiteral("导出 DEX")); export_->setObjectName(QStringLiteral("dexExport")); stop_ = new QPushButton(QStringLiteral("停止"));
    toolbar->addWidget(save_); toolbar->addWidget(export_); toolbar->addWidget(stop_); layout->addLayout(toolbar);
    auto *splitter = new QSplitter; auto *navigation = new QWidget; auto *navigationLayout = new QVBoxLayout(navigation); navigationLayout->setContentsMargins(0, 0, 8, 0);
    filter_ = new QLineEdit; filter_->setPlaceholderText(QStringLiteral("筛选包名 / 类名")); filter_->setObjectName(QStringLiteral("dexFilter")); navigationLayout->addWidget(filter_);
    tree_ = new QTreeWidget; tree_->setObjectName(QStringLiteral("dexClasses")); tree_->setHeaderHidden(true); navigationLayout->addWidget(tree_); splitter->addWidget(navigation);
    auto *content = new QWidget; auto *contentLayout = new QVBoxLayout(content); contentLayout->setContentsMargins(0, 0, 0, 0); auto *search = new QHBoxLayout;
    find_ = new QLineEdit; find_->setPlaceholderText(QStringLiteral("查找代码 / 字符串")); replacement_ = new QLineEdit; replacement_->setPlaceholderText(QStringLiteral("替换为")); search->addWidget(find_); search->addWidget(replacement_);
    auto *next = new QPushButton(QStringLiteral("下一个")); auto *replace = new QPushButton(QStringLiteral("替换全部")); search->addWidget(next); search->addWidget(replace); contentLayout->addLayout(search);
    auto *members = new QComboBox; members->setObjectName(QStringLiteral("dexMembers")); members->setMinimumContentsLength(12); members->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); contentLayout->addWidget(members);
    editor_ = new QPlainTextEdit; editor_->setObjectName(QStringLiteral("dexCode")); editor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); editor_->setLineWrapMode(QPlainTextEdit::NoWrap); new SmaliHighlight(editor_->document()); contentLayout->addWidget(editor_); splitter->addWidget(content); splitter->setSizes({280, 800}); layout->addWidget(splitter, 1);
    connect(members, &QComboBox::activated, this, [this, members](int index) { QTextCursor cursor(editor_->document()); cursor.setPosition(members->itemData(index).toInt()); editor_->setTextCursor(cursor); editor_->centerCursor(); });
    status_ = new QLabel; status_->setObjectName(QStringLiteral("dexStatus")); status_->setWordWrap(true); layout->addWidget(status_);
    connect(stop_, &QPushButton::clicked, this, [this] { if (cancel_) cancel_->store(true); status_->setText(QStringLiteral("正在停止…")); });
    connect(save_, &QPushButton::clicked, this, [this] { compile(false); }); connect(export_, &QPushButton::clicked, this, [this] { compile(true); });
    connect(new QShortcut(QKeySequence::Save, this), &QShortcut::activated, this, [this] { compile(false); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) { selectClass(item); });
    connect(editor_, &QPlainTextEdit::textChanged, this, [this] { if (editor_->document()->isModified()) dirty_ = true; });
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString &text) { QTreeWidgetItemIterator items(tree_); while (*items) { auto *item = *items; item->setHidden(item->childCount() == 0 && !item->data(0, Qt::UserRole + 1).toString().contains(text, Qt::CaseInsensitive)); ++items; } });
    connect(next, &QPushButton::clicked, this, [this] { if (find_->text().isEmpty()) return; if (!editor_->find(find_->text())) { editor_->moveCursor(QTextCursor::Start); editor_->find(find_->text()); } });
    connect(find_, &QLineEdit::returnPressed, next, &QPushButton::click);
    connect(replace, &QPushButton::clicked, this, [this] { if (busy_ || current_.isEmpty() || find_->text().isEmpty()) return; const Result result = replaceTextBounded(editor_->toPlainText(), find_->text(), replacement_->text(), false, true, 2 * 1024 * 1024); if (!result.ok) { status_->setText(result.error); return; } auto cursor = editor_->textCursor(); cursor.beginEditBlock(); cursor.select(QTextCursor::Document); cursor.insertText(result.output); cursor.endEditBlock(); });
    status_->setText(QStringLiteral("正在解析 DEX…"));
    launch([path, workspace = workspace_](const Cancellation &cancel) { const Result checked = validateDex(path); if (!checked.ok) return checked; const QByteArray expected = fileDigest(path, cancel); Result result = convertDex(path, workspace->path(), {}, cancel); if (result.ok && fileDigest(path, cancel) != expected) return failure(QStringLiteral("解析时原 DEX 已改变，请重新打开")); result.output = QString::fromLatin1(expected.toHex()); return result; }, [this](const Result &result) { if (result.ok) { expected_ = QByteArray::fromHex(result.output.toLatin1()); ready_ = true; populate(); } else status_->setText(result.error.left(6000)); });
}

DexDialog::~DexDialog() { if (cancel_) cancel_->store(true); worker_.waitForFinished(); }
void DexDialog::launch(std::function<Result(const Cancellation &)> work, std::function<void(const Result &)> done) {
    busy_ = true; tree_->setEnabled(false); editor_->setReadOnly(true); save_->setEnabled(false); export_->setEnabled(false); stop_->setEnabled(true); cancel_ = std::make_shared<std::atomic_bool>(false);
    disconnect(&worker_, nullptr, this, nullptr);
    connect(&worker_, &QFutureWatcher<Result>::finished, this, [this, done] { const Result result = worker_.result(); busy_ = false; done(result); tree_->setEnabled(ready_); editor_->setReadOnly(current_.isEmpty()); save_->setEnabled(ready_); export_->setEnabled(ready_); stop_->setEnabled(false); });
    worker_.setFuture(QtConcurrent::run([work, cancel = cancel_, keep = workspace_, source = temporary_] { try { return work(cancel); } catch (const std::exception &error) { return failure(QString::fromUtf8(error.what())); } }));
}

void DexDialog::populate() {
    QMap<QString, QTreeWidgetItem *> packages;
    const auto files = QDir(workspace_->path()).entryList({QStringLiteral("*.smali")}, QDir::Files, QDir::Name);
    for (const QString &name : files) {
        QFile input(workspace_->filePath(name)); if (!input.open(QIODevice::ReadOnly)) continue;
        const QString beginning = QString::fromUtf8(input.read(8192)); const auto match = QRegularExpression(QStringLiteral("(?m)^\\.class[^\\r\\n]* (L[^;]+;)\\s*$")).match(beginning);
        const QString descriptor = match.hasMatch() ? match.captured(1) : name;
        const QString display = descriptor.startsWith(QChar('L')) ? descriptor.mid(1, descriptor.size() - 2) : descriptor;
        const QString package = display.contains(QChar('/')) ? display.section(QChar('/'), 0, -2).replace(QChar('/'), QChar('.')) : QStringLiteral("默认包");
        auto *group = packages.value(package); if (!group) { group = new QTreeWidgetItem(tree_, {package}); packages.insert(package, group); }
        auto *item = new QTreeWidgetItem(group, {display.section(QChar('/'), -1)}); item->setData(0, Qt::UserRole, name); item->setData(0, Qt::UserRole + 1, display); item->setToolTip(0, descriptor);
    }
    tree_->expandToDepth(0); status_->setText(QStringLiteral("%1 个类 · Smali · Ctrl+S 编译保存 · APK 修改后须重新签名").arg(files.size()));
    if (tree_->topLevelItemCount() && tree_->topLevelItem(0)->childCount()) tree_->setCurrentItem(tree_->topLevelItem(0)->child(0));
}

bool DexDialog::flushClass() {
    if (current_.isEmpty() || !editor_->document()->isModified()) return true;
    const QByteArray bytes = editor_->toPlainText().toUtf8(); if (bytes.size() > 2 * 1024 * 1024) { status_->setText(QStringLiteral("单个类超过 2 MiB，未保存")); return false; }
    QSaveFile file(workspace_->filePath(current_)); if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) { status_->setText(file.errorString()); return false; }
    editor_->document()->setModified(false); return true;
}

void DexDialog::selectClass(QTreeWidgetItem *item) {
    if (busy_ || !item || item->data(0, Qt::UserRole).toString().isEmpty()) return;
    if (!flushClass()) { const QSignalBlocker blocked(tree_); QTreeWidgetItemIterator items(tree_); while (*items) { if ((*items)->data(0, Qt::UserRole).toString() == current_) { tree_->setCurrentItem(*items); break; } ++items; } return; }
    const QString name = item->data(0, Qt::UserRole).toString(); QFile file(workspace_->filePath(name));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024) { status_->setText(QStringLiteral("无法读取此类或超过 2 MiB")); return; }
    current_ = name;
    { const QSignalBlocker blocked(editor_); editor_->setPlainText(QString::fromUtf8(file.readAll())); editor_->document()->setModified(false); }
    editor_->setReadOnly(false);
    auto *members = findChild<QComboBox *>(QStringLiteral("dexMembers")); members->clear();
    auto matches = QRegularExpression(QStringLiteral("(?m)^\\.(method|field)\\s+([^\\r\\n]+)")).globalMatch(editor_->toPlainText());
    while (matches.hasNext()) { const auto match = matches.next(); members->addItem(match.captured(1) + QStringLiteral(" · ") + match.captured(2).left(180), int(match.capturedStart())); }
    if (members->count() == 0) members->addItem(QStringLiteral("无字段或方法"), 0);
}

void DexDialog::compile(bool exportOnly) {
    if (busy_ || !ready_ || !flushClass()) return;
    QString target;
    if (exportOnly) { target = QFileDialog::getSaveFileName(this, QStringLiteral("导出编译后的 DEX"), QFileInfo(path_).completeBaseName() + QStringLiteral("-edited.dex"), QStringLiteral("DEX (*.dex)")); if (target.isEmpty()) return; if (QFileInfo::exists(target)) { status_->setText(QStringLiteral("导出请选择不存在的新文件")); return; } }
    else if (QMessageBox::question(this, QStringLiteral("编译保存"), original_.source == Source::Archive ? QStringLiteral("编译并替换包内 DEX？APK 修改后需要重新签名，建议先备份。") : QStringLiteral("编译并替换原 DEX？建议先备份。"), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    status_->setText(QStringLiteral("正在编译 Smali…"));
    launch([source = path_, workspace = workspace_, target, expected = expected_, original = original_](const Cancellation &cancel) {
        QTemporaryDir compiled; const QString output = compiled.filePath(QStringLiteral("classes.dex")); Result result = convertDex(source, workspace->path(), output, cancel); if (!result.ok) return result;
        if (!target.isEmpty()) { if (cancel && cancel->load()) return failure(QStringLiteral("操作已取消")); return QFile::copy(output, target) ? Result{true, {}, {}, {target}} : failure(QStringLiteral("无法导出，未覆盖现有文件")); }
        result = saveDex(source, output, expected, original, cancel);
        if (result.ok && !original.path.isEmpty() && original.source != Source::Local) {
            QFile input(output); QSaveFile local(source); if (!input.open(QIODevice::ReadOnly) || !local.open(QIODevice::WriteOnly) || local.write(input.readAll()) != input.size() || !local.commit()) return failure(QStringLiteral("已回写，但暂存副本更新失败；请关闭并重新打开"));
        }
        return result;
    }, [this, exportOnly](const Result &result) { if (!result.ok) { status_->setText(result.error.left(6000)); return; } if (!exportOnly) { expected_ = fileDigest(path_); dirty_ = false; if (changed_) changed_(); } status_->setText(exportOnly ? QStringLiteral("已导出 DEX") : QStringLiteral("已编译保存 · APK 使用前请重新签名")); });
}

void DexDialog::reject() {
    if (busy_) { if (cancel_) cancel_->store(true); status_->setText(QStringLiteral("正在停止，请稍后关闭")); return; }
    if (dirty_ && QMessageBox::question(this, QStringLiteral("放弃修改"), QStringLiteral("尚未编译保存，放弃修改？"), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    QDialog::reject();
}
void DexDialog::closeEvent(QCloseEvent *event) { event->ignore(); reject(); }

}
