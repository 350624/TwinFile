#include "dex.h"
#include "editor.h"
#include "code_edit.h"
#include "appearance.h"
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
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QShortcut>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QTabWidget>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
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
    tree_ = new QTreeWidget; tree_->setObjectName(QStringLiteral("dexClasses")); tree_->setHeaderHidden(true); navigationLayout->addWidget(tree_, 3);
    auto *tabs = new QTabWidget; tabs->setObjectName(QStringLiteral("dexNavigationTabs"));
    outline_ = new QTreeWidget; outline_->setObjectName(QStringLiteral("dexOutline")); outline_->setHeaderHidden(true);
    strings_ = new QTreeWidget; strings_->setObjectName(QStringLiteral("dexStrings")); strings_->setHeaderHidden(true);
    results_ = new QTreeWidget; results_->setObjectName(QStringLiteral("dexResults")); results_->setHeaderHidden(true);
    tabs->addTab(outline_, QStringLiteral("成员")); tabs->addTab(strings_, QStringLiteral("字符串")); tabs->addTab(results_, QStringLiteral("搜索")); navigationLayout->addWidget(tabs, 2); splitter->addWidget(navigation);
    auto *content = new QWidget; auto *contentLayout = new QVBoxLayout(content); contentLayout->setContentsMargins(0, 0, 0, 0); auto *search = new QHBoxLayout;
    find_ = new QLineEdit; find_->setPlaceholderText(QStringLiteral("查找代码 / 字符串")); replacement_ = new QLineEdit; replacement_->setPlaceholderText(QStringLiteral("替换为")); search->addWidget(find_); search->addWidget(replacement_);
    find_->setObjectName(QStringLiteral("dexFind")); find_->setClearButtonEnabled(true); replacement_->setObjectName(QStringLiteral("dexReplacement"));
    auto tool = [this, search](const QString &symbol, const QString &tip) { auto *button = new QToolButton; button->setIcon(actionSymbol(symbol, palette().color(QPalette::ButtonText))); button->setToolTip(tip); button->setAccessibleName(tip); button->setFixedSize(32, 32); button->setIconSize(QSize(20, 20)); button->setStyleSheet(QStringLiteral("QToolButton { padding: 0px; border-radius: 4px; }")); button->setProperty("dexSymbol", symbol); search->addWidget(button); return button; };
    auto *previous = tool(QStringLiteral("up"), QStringLiteral("上一处")); auto *next = tool(QStringLiteral("down"), QStringLiteral("下一处")); auto *replace = new QPushButton(QStringLiteral("替换全部")); replace->setObjectName(QStringLiteral("dexReplaceAll")); search->addWidget(replace); contentLayout->addLayout(search);
    auto *filebar = new QHBoxLayout; className_ = new QLabel; className_->setObjectName(QStringLiteral("dexClassName")); className_->setTextInteractionFlags(Qt::TextSelectableByMouse); className_->setMinimumWidth(0); className_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); filebar->addWidget(className_, 1); searchAll_ = new QPushButton(QStringLiteral("全局搜索")); searchAll_->setObjectName(QStringLiteral("dexSearchAll")); filebar->addWidget(searchAll_); contentLayout->addLayout(filebar);
    auto *members = new QComboBox; members->setObjectName(QStringLiteral("dexMembers")); members->setMinimumContentsLength(12); members->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); contentLayout->addWidget(members);
    editor_ = new CodeEdit; editor_->setObjectName(QStringLiteral("dexCode")); new SmaliHighlight(editor_->document()); contentLayout->addWidget(editor_); splitter->addWidget(content); splitter->setSizes({300, 780}); layout->addWidget(splitter, 1);
    connect(members, &QComboBox::activated, this, [this, members](int index) { QTextCursor cursor(editor_->document()); cursor.setPosition(members->itemData(index).toInt()); editor_->setTextCursor(cursor); editor_->centerCursor(); });
    status_ = new QLabel; status_->setObjectName(QStringLiteral("dexStatus")); status_->setWordWrap(true); layout->addWidget(status_);
    position_ = new QLabel; position_->setObjectName(QStringLiteral("dexPosition")); layout->addWidget(position_);
    auto *outlineTimer = new QTimer(this); outlineTimer->setSingleShot(true); outlineTimer->setInterval(250); connect(outlineTimer, &QTimer::timeout, this, [this] { updateOutline(); });
    connect(editor_, &QPlainTextEdit::textChanged, outlineTimer, [outlineTimer] { outlineTimer->start(); });
    connect(editor_, &QPlainTextEdit::cursorPositionChanged, this, [this] { const auto cursor = editor_->textCursor(); position_->setText(QStringLiteral("Smali · %1:%2 · %3 行%4").arg(cursor.blockNumber() + 1).arg(cursor.positionInBlock() + 1).arg(editor_->blockCount()).arg(dirty_ ? QStringLiteral(" · 未编译") : QString())); });
    for (auto *list : {outline_, strings_}) { connect(list, &QTreeWidget::itemClicked, list, &QTreeWidget::itemActivated); connect(list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) { if (item->data(0, Qt::UserRole).isValid()) jumpTo(item->data(0, Qt::UserRole).toInt()); }); }
    connect(results_, &QTreeWidget::itemClicked, results_, &QTreeWidget::itemActivated);
    connect(results_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) { if (busy_) return; const QString file = item->data(0, Qt::UserRole).toString(); const int offset = item->data(0, Qt::UserRole + 1).toInt(); QTreeWidgetItemIterator all(tree_); while (*all) { if ((*all)->data(0, Qt::UserRole).toString() == file) { tree_->setCurrentItem(*all); if (current_ == file) { jumpTo(offset); auto cursor = editor_->textCursor(); if (editor_->toPlainText().mid(offset, find_->text().size()).compare(find_->text(), Qt::CaseInsensitive) == 0) { cursor.setPosition(offset + find_->text().size(), QTextCursor::KeepAnchor); editor_->setTextCursor(cursor); } } break; } ++all; } });
    connect(searchAll_, &QPushButton::clicked, this, [this] { searchAll(); });
    connect(new QShortcut(QKeySequence::Find, this), &QShortcut::activated, find_, [this] { find_->setFocus(); find_->selectAll(); });
    connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_H), this), &QShortcut::activated, replacement_, [this] { replacement_->setFocus(); replacement_->selectAll(); });
    connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_G), this), &QShortcut::activated, this, [this] { if (busy_) return; bool ok = false; const int line = QInputDialog::getInt(this, QStringLiteral("跳转行"), QStringLiteral("行号"), editor_->textCursor().blockNumber() + 1, 1, editor_->blockCount(), 1, &ok); if (ok) jumpTo(editor_->document()->findBlockByNumber(line - 1).position()); });
    connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), this), &QShortcut::activated, this, [this] { searchAll(); });
    connect(stop_, &QPushButton::clicked, this, [this] { if (cancel_) cancel_->store(true); status_->setText(QStringLiteral("正在停止…")); });
    connect(save_, &QPushButton::clicked, this, [this] { compile(false); }); connect(export_, &QPushButton::clicked, this, [this] { compile(true); });
    connect(new QShortcut(QKeySequence::Save, this), &QShortcut::activated, this, [this] { compile(false); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) { selectClass(item); });
    connect(editor_, &QPlainTextEdit::textChanged, this, [this] { if (editor_->document()->isModified()) dirty_ = true; });
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString &text) { for (int i = 0; i < tree_->topLevelItemCount(); ++i) { auto *group = tree_->topLevelItem(i); bool any = false; for (int j = 0; j < group->childCount(); ++j) { auto *item = group->child(j); const bool visible = item->data(0, Qt::UserRole + 1).toString().contains(text, Qt::CaseInsensitive); item->setHidden(!visible); any |= visible; } group->setHidden(!any); } });
    connect(next, &QToolButton::clicked, this, [this] { if (find_->text().isEmpty()) return; if (!editor_->find(find_->text())) { editor_->moveCursor(QTextCursor::Start); editor_->find(find_->text()); } });
    connect(previous, &QToolButton::clicked, this, [this] { if (find_->text().isEmpty()) return; if (!editor_->find(find_->text(), QTextDocument::FindBackward)) { editor_->moveCursor(QTextCursor::End); editor_->find(find_->text(), QTextDocument::FindBackward); } });
    connect(find_, &QLineEdit::returnPressed, next, &QToolButton::click);
    connect(replace, &QPushButton::clicked, this, [this] { if (busy_ || current_.isEmpty() || find_->text().isEmpty()) return; const Result result = replaceTextBounded(editor_->toPlainText(), find_->text(), replacement_->text(), false, true, 2 * 1024 * 1024); if (!result.ok) { status_->setText(result.error); return; } auto cursor = editor_->textCursor(); cursor.beginEditBlock(); cursor.select(QTextCursor::Document); cursor.insertText(result.output); cursor.endEditBlock(); });
    status_->setText(QStringLiteral("正在解析 DEX…"));
    launch([path, workspace = workspace_](const Cancellation &cancel) { const Result checked = validateDex(path); if (!checked.ok) return checked; const QByteArray expected = fileDigest(path, cancel); Result result = convertDex(path, workspace->path(), {}, cancel); if (result.ok && fileDigest(path, cancel) != expected) return failure(QStringLiteral("解析时原 DEX 已改变，请重新打开")); result.output = QString::fromLatin1(expected.toHex()); return result; }, [this](const Result &result) { if (result.ok) { expected_ = QByteArray::fromHex(result.output.toLatin1()); ready_ = true; populate(); } else status_->setText(result.error.left(6000)); });
}

DexDialog::~DexDialog() { if (cancel_) cancel_->store(true); worker_.waitForFinished(); }
void DexDialog::launch(std::function<Result(const Cancellation &)> work, std::function<void(const Result &)> done) {
    busy_ = true; tree_->setEnabled(false); searchAll_->setEnabled(false); editor_->setReadOnly(true); save_->setEnabled(false); export_->setEnabled(false); stop_->setEnabled(true); cancel_ = std::make_shared<std::atomic_bool>(false);
    disconnect(&worker_, nullptr, this, nullptr);
    connect(&worker_, &QFutureWatcher<Result>::finished, this, [this, done] { const Result result = worker_.result(); busy_ = false; done(result); tree_->setEnabled(ready_); searchAll_->setEnabled(ready_); editor_->setReadOnly(current_.isEmpty()); save_->setEnabled(ready_); export_->setEnabled(ready_); stop_->setEnabled(false); });
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
    tree_->expandToDepth(0); status_->setText(QStringLiteral("%1 个类 · Smali · APK 修改后须重新签名").arg(files.size()));
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
    className_->setText(item->toolTip(0)); className_->setToolTip(item->toolTip(0)); updateOutline();
}

void DexDialog::jumpTo(int position) {
    if (busy_) return;
    QTextCursor cursor(editor_->document()); cursor.setPosition(qBound(0, position, editor_->document()->characterCount() - 1)); editor_->setTextCursor(cursor); editor_->centerCursor(); editor_->setFocus();
}
void DexDialog::updateOutline() {
    auto *members = findChild<QComboBox *>(QStringLiteral("dexMembers")); members->clear(); outline_->clear(); strings_->clear();
    auto matches = QRegularExpression(QStringLiteral("(?m)^\\.(method|field)\\s+([^\\r\\n]+)")).globalMatch(editor_->toPlainText());
    while (matches.hasNext()) { const auto match = matches.next(); const QString text = match.captured(1) + QStringLiteral(" · ") + match.captured(2).left(180); members->addItem(text, int(match.capturedStart())); auto *item = new QTreeWidgetItem(outline_, {text}); item->setData(0, Qt::UserRole, int(match.capturedStart())); item->setToolTip(0, text); }
    if (members->count() == 0) members->addItem(QStringLiteral("无字段或方法"), 0);
    auto literals = QRegularExpression(QStringLiteral("(?m)^\\s*const-string(?:/jumbo)?[^\\r\\n]*?\"((?:[^\"\\\\]|\\\\.)*)\"")).globalMatch(editor_->toPlainText());
    int count = 0; while (literals.hasNext() && count++ < 10000) { const auto match = literals.next(); auto *item = new QTreeWidgetItem(strings_, {match.captured(1).left(240)}); item->setData(0, Qt::UserRole, int(match.capturedStart())); item->setToolTip(0, match.captured(1)); }
}
void DexDialog::searchAll() {
    if (busy_ || !ready_ || find_->text().isEmpty() || !flushClass()) return;
    const QString query = find_->text(); status_->setText(QStringLiteral("正在搜索全部类…")); results_->clear();
    launch([workspace = workspace_, query](const Cancellation &cancel) {
        QJsonArray hits; const auto files = QDir(workspace->path()).entryList({QStringLiteral("*.smali")}, QDir::Files, QDir::Name);
        for (const QString &name : files) {
            if (cancel && cancel->load()) return failure(QStringLiteral("搜索已取消"));
            QFile file(workspace->filePath(name)); if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024) continue; const QString code = QString::fromUtf8(file.readAll()).replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QChar('\r'), QChar('\n')); int from = 0, line = 1, counted = 0;
            while (hits.size() < 2000) { const int found = int(code.indexOf(query, from, Qt::CaseInsensitive)); if (found < 0) break; line += code.mid(counted, found - counted).count(QChar('\n')); counted = found; const int start = int(code.lastIndexOf(QChar('\n'), found)) + 1; hits.append(QJsonObject{{QStringLiteral("file"), name}, {QStringLiteral("offset"), found}, {QStringLiteral("line"), line}, {QStringLiteral("text"), code.mid(start, 200).section(QChar('\n'), 0, 0).trimmed()}}); from = found + query.size(); }
            if (hits.size() >= 2000) break;
        }
        Result result; result.output = QString::fromUtf8(QJsonDocument(hits).toJson(QJsonDocument::Compact)); return result;
    }, [this](const Result &result) {
        if (!result.ok) { status_->setText(result.error); return; } const auto hits = QJsonDocument::fromJson(result.output.toUtf8()).array();
        QMap<QString, QString> names; QTreeWidgetItemIterator classes(tree_); while (*classes) { if ((*classes)->data(0, Qt::UserRole).isValid()) names.insert((*classes)->data(0, Qt::UserRole).toString(), (*classes)->data(0, Qt::UserRole + 1).toString()); ++classes; }
        for (const auto &value : hits) { const auto hit = value.toObject(); const QString file = hit.value(QStringLiteral("file")).toString(); auto *item = new QTreeWidgetItem(results_, {names.value(file, file).section(QChar('/'), -1) + QChar(':') + QString::number(hit.value(QStringLiteral("line")).toInt()) + QStringLiteral("  ") + hit.value(QStringLiteral("text")).toString()}); item->setToolTip(0, names.value(file) + QChar('\n') + hit.value(QStringLiteral("text")).toString()); item->setData(0, Qt::UserRole, file); item->setData(0, Qt::UserRole + 1, hit.value(QStringLiteral("offset")).toInt()); }
        findChild<QTabWidget *>(QStringLiteral("dexNavigationTabs"))->setCurrentWidget(results_); status_->setText(QStringLiteral("%1 处匹配%2").arg(hits.size()).arg(hits.size() >= 2000 ? QStringLiteral(" · 已达 2000 条上限") : QString()));
    });
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
void DexDialog::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) for (auto *button : findChildren<QToolButton *>()) { const QString symbol = button->property("dexSymbol").toString(); if (!symbol.isEmpty()) button->setIcon(actionSymbol(symbol, palette().color(QPalette::ButtonText))); }
}

}
