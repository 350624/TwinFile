#include "android_editor.h"
#include "code_edit.h"
#include "editor.h"
#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <QtEndian>
#ifdef TWINFILE_ANDROID_EDITOR_BRIDGE
static void initializeAndroidEditorResources() { Q_INIT_RESOURCE(android_editor_resources); }
#endif
namespace twinfile {
namespace {
Result failure(const QString &error) { return {false, error, {}, {}}; }
bool validBinary(const QString &path, const QString &kind = {}) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly) || file.size() < 8 || file.size() > 32 * 1024 * 1024) return false;
    const QByteArray header = file.read(8); const int type = qFromLittleEndian<quint16>(header.constData());
    return (kind == QStringLiteral("arsc") ? type == 2 : kind == QStringLiteral("axml") ? type == 3 : type == 2 || type == 3) && qFromLittleEndian<quint32>(header.constData() + 4) == file.size();
}
}
bool isAndroidBinary(const QString &name) {
    const QString file = QFileInfo(name).fileName();
    if (file.compare(QStringLiteral("resources.arsc"), Qt::CaseInsensitive) == 0) return true;
    if (file.compare(QStringLiteral("AndroidManifest.xml"), Qt::CaseInsensitive) != 0) return false;
    if (!QFileInfo::exists(name)) return true;
    QFile input(name); return input.open(QIODevice::ReadOnly) && input.read(2) == QByteArray("\x03\x00", 2);
}
Result convertAndroidBinary(const QString &source, const QString &document, const QString &output, const QString &kind, const Cancellation &cancel) {
    if (!validBinary(source, kind) || (kind != QStringLiteral("arsc") && kind != QStringLiteral("axml"))) return failure(QStringLiteral("Android 二进制头部、长度无效或超过 32 MiB"));
#ifdef TWINFILE_ANDROID_EDITOR_BRIDGE
    initializeAndroidEditorResources(); QTemporaryDir helper; if (!helper.isValid()) return failure(QStringLiteral("无法创建 Android 编辑器工作区"));
    QFile bridge(QStringLiteral(":/android-editor/AndroidBinaryBridge.class")); if (!bridge.copy(helper.filePath(QStringLiteral("AndroidBinaryBridge.class")))) return failure(QStringLiteral("无法准备二进制编译器"));
    Result result = runProcess(toolPath(QStringLiteral("runtime/bin/java.exe")), {QStringLiteral("-Xmx768m"), QStringLiteral("-Dfile.encoding=UTF-8"), QStringLiteral("-Djavax.xml.accessExternalDTD="), QStringLiteral("-Djavax.xml.accessExternalSchema="), QStringLiteral("-cp"), helper.path() + QChar(';') + toolPath(QStringLiteral("arsclib.jar")), QStringLiteral("AndroidBinaryBridge"), output.isEmpty() ? QStringLiteral("decode") : QStringLiteral("build"), kind, source, document, output.isEmpty() ? QStringLiteral("-") : output}, 120000, cancel);
    if (result.ok && !output.isEmpty() && !validBinary(output, kind)) return failure(QStringLiteral("编译后二进制校验失败，未保存"));
    return result;
#else
    Q_UNUSED(document); Q_UNUSED(output); Q_UNUSED(cancel); return failure(QStringLiteral("缺少 Android 二进制编辑组件，请使用完整发行版"));
#endif
}
Result saveAndroidBinary(const QString &source, const QString &compiled, const QByteArray &expected, const Entry &original, const Cancellation &cancel) {
    if (!validBinary(compiled)) return failure(QStringLiteral("编译产物无效，未覆盖"));
    if (expected.isEmpty() || fileDigest(source, cancel) != expected) return failure(QStringLiteral("原文件已被外部修改，未覆盖"));
    if (cancel && cancel->load()) return failure(QStringLiteral("操作已取消"));
    if (!original.path.isEmpty() && original.source != Source::Local) return writeBackEntry(original, compiled, expected, cancel);
    QFile input(compiled); if (!input.open(QIODevice::ReadOnly)) return failure(input.errorString());
    QSaveFile output(source); output.setDirectWriteFallback(false); if (!output.open(QIODevice::WriteOnly)) return failure(output.errorString());
    while (!input.atEnd()) { if (cancel && cancel->load()) return failure(QStringLiteral("操作已取消")); const QByteArray bytes = input.read(1024 * 1024); if ((bytes.isEmpty() && input.error() != QFileDevice::NoError) || output.write(bytes) != bytes.size()) return failure(QStringLiteral("写入编译产物失败")); }
    if (fileDigest(source, cancel) != expected || (cancel && cancel->load())) return failure(QStringLiteral("原文件改变或已取消，未覆盖"));
    return output.commit() ? Result{} : failure(output.errorString());
}
AndroidEditorDialog::AndroidEditorDialog(const QString &path, QWidget *parent, const Entry &original, std::shared_ptr<QTemporaryDir> temporary, std::function<void()> changed)
    : QDialog(parent), path_(path), kind_(QFileInfo(path).fileName().compare(QStringLiteral("resources.arsc"), Qt::CaseInsensitive) == 0 ? QStringLiteral("arsc") : QStringLiteral("axml")), original_(original), temporary_(std::move(temporary)), workspace_(std::make_shared<QTemporaryDir>()), changed_(std::move(changed)) {
    setAttribute(Qt::WA_DeleteOnClose); setObjectName(QStringLiteral("androidBinaryEditor")); resize(1100, 740); setMinimumSize(640, 460);
    setWindowTitle(QFileInfo(path).fileName() + (kind_ == QStringLiteral("arsc") ? QStringLiteral(" · ARSC 编辑器") : QStringLiteral(" · AXML 编辑器")));
    auto *layout = new QVBoxLayout(this); auto *bar = new QHBoxLayout; bar->addWidget(new QLabel(QFileInfo(path).fileName())); bar->addStretch();
    save_ = new QPushButton(QStringLiteral("编译保存")); save_->setObjectName(QStringLiteral("androidBinarySave")); export_ = new QPushButton(QStringLiteral("导出")); stop_ = new QPushButton(QStringLiteral("停止")); bar->addWidget(save_); bar->addWidget(export_); bar->addWidget(stop_); layout->addLayout(bar);
    auto *search = new QHBoxLayout; auto *find = new QLineEdit; find->setPlaceholderText(QStringLiteral("查找")); find->setClearButtonEnabled(true); auto *next = new QPushButton(QStringLiteral("下一处")); search->addWidget(find); search->addWidget(next); layout->addLayout(search);
    auto *split = new QSplitter; auto *navigation = new QWidget; auto *nav = new QVBoxLayout(navigation); nav->setContentsMargins(0, 0, 4, 0); auto *filter = new QLineEdit; filter->setPlaceholderText(QStringLiteral("筛选类型 / 名称 / 配置 / ID")); filter->setClearButtonEnabled(true); nav->addWidget(filter);
    tree_ = new QTreeWidget; tree_->setObjectName(QStringLiteral("androidResources")); tree_->setHeaderHidden(true); nav->addWidget(tree_); split->addWidget(navigation); navigation->setVisible(kind_ == QStringLiteral("arsc"));
    editor_ = new CodeEdit; editor_->setObjectName(QStringLiteral("androidBinaryCode")); highlightTextDocument(editor_->document()); split->addWidget(editor_); split->setSizes({300, 780}); layout->addWidget(split, 1);
    status_ = new QLabel(QStringLiteral("正在解码…")); status_->setObjectName(QStringLiteral("androidBinaryStatus")); status_->setWordWrap(true); layout->addWidget(status_);
    connect(editor_, &QPlainTextEdit::textChanged, this, [this] { if (editor_->document()->isModified()) dirty_ = true; });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) { selectResource(item); });
    connect(filter, &QLineEdit::textChanged, this, [this](const QString &query) { for (int i = 0; i < tree_->topLevelItemCount(); ++i) { auto *group = tree_->topLevelItem(i); bool any = false; for (int j = 0; j < group->childCount(); ++j) { auto *item = group->child(j); const bool visible = (group->text(0) + item->text(0) + item->toolTip(0)).contains(query, Qt::CaseInsensitive); item->setHidden(!visible); any |= visible; } group->setHidden(!any); } });
    connect(next, &QPushButton::clicked, this, [this, find] { if (find->text().isEmpty()) return; if (!editor_->find(find->text())) { editor_->moveCursor(QTextCursor::Start); editor_->find(find->text()); } }); connect(find, &QLineEdit::returnPressed, next, &QPushButton::click);
    connect(new QShortcut(QKeySequence::Find, this), &QShortcut::activated, find, [find] { find->setFocus(); find->selectAll(); });
    connect(save_, &QPushButton::clicked, this, [this] { compile(false); }); connect(export_, &QPushButton::clicked, this, [this] { compile(true); }); connect(new QShortcut(QKeySequence::Save, this), &QShortcut::activated, this, [this] { compile(false); });
    connect(stop_, &QPushButton::clicked, this, [this] { if (cancel_) cancel_->store(true); status_->setText(QStringLiteral("正在停止…")); });
    launch([this](const Cancellation &cancel) { if (!workspace_->isValid()) return failure(QStringLiteral("无法创建工作区")); const QByteArray expected = fileDigest(path_, cancel); Result result = convertAndroidBinary(path_, workspace_->filePath(QStringLiteral("document.txt")), {}, kind_, cancel); if (result.ok && fileDigest(path_, cancel) != expected) return failure(QStringLiteral("解码时原文件已改变，请重新打开")); result.output = QString::fromLatin1(expected.toHex()); return result; }, [this](const Result &result) {
        if (!result.ok) { status_->setText(result.error.left(6000)); return; }
        expected_ = QByteArray::fromHex(result.output.toLatin1()); QFile file(workspace_->filePath(QStringLiteral("document.txt"))); if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024 * 1024) { status_->setText(QStringLiteral("解码文档无法读取或过大")); return; }
        const QByteArray text = file.readAll();
        if (kind_ == QStringLiteral("arsc")) {
            QJsonParseError error; const auto doc = QJsonDocument::fromJson(text, &error); if (error.error != QJsonParseError::NoError || !doc.isArray()) { status_->setText(QStringLiteral("资源表解码结果无效")); return; } resources_ = doc.array(); QMap<QString, QTreeWidgetItem *> groups;
            for (int i = 0; i < resources_.size(); ++i) { const auto row = resources_[i].toObject(); const QString type = row.value(QStringLiteral("type")).toString(); auto *group = groups.value(type); if (!group) { group = new QTreeWidgetItem(tree_, {type}); groups.insert(type, group); } const QString config = row.value(QStringLiteral("config")).toString(); auto *item = new QTreeWidgetItem(group, {row.value(QStringLiteral("name")).toString() + QStringLiteral(" [") + (config.isEmpty() ? QStringLiteral("default") : config) + QChar(']')}); item->setData(0, Qt::UserRole, i); item->setToolTip(0, QStringLiteral("0x%1").arg(quint32(row.value(QStringLiteral("id")).toInteger()), 8, 16, QChar('0'))); }
            tree_->expandToDepth(0); status_->setText(QStringLiteral("%1 个资源配置 · JSON 资源值").arg(resources_.size()));
        } else { editor_->setPlainText(QString::fromUtf8(text)); editor_->document()->setModified(false); status_->setText(QStringLiteral("AXML · XML")); }
        ready_ = true; if (tree_->topLevelItemCount() && tree_->topLevelItem(0)->childCount()) selectResource(tree_->topLevelItem(0)->child(0));
    });
}
AndroidEditorDialog::~AndroidEditorDialog() { if (cancel_) cancel_->store(true); worker_.waitForFinished(); }
void AndroidEditorDialog::launch(std::function<Result(const Cancellation &)> work, std::function<void(const Result &)> done) {
    busy_ = true; editor_->setReadOnly(true); tree_->setEnabled(false); save_->setEnabled(false); export_->setEnabled(false); stop_->setEnabled(true); cancel_ = std::make_shared<std::atomic_bool>(false); disconnect(&worker_, nullptr, this, nullptr);
    connect(&worker_, &QFutureWatcher<Result>::finished, this, [this, done] { busy_ = false; done(worker_.result()); tree_->setEnabled(ready_); editor_->setReadOnly(!ready_ || (kind_ == QStringLiteral("arsc") && selected_ < 0)); save_->setEnabled(ready_); export_->setEnabled(ready_); stop_->setEnabled(false); });
    worker_.setFuture(QtConcurrent::run([work, cancel = cancel_, workspace = workspace_, temporary = temporary_] { try { return work(cancel); } catch (const std::exception &error) { return failure(QString::fromUtf8(error.what())); } }));
}
bool AndroidEditorDialog::flush() {
    if (kind_ != QStringLiteral("arsc") || selected_ < 0 || !editor_->document()->isModified()) return true;
    const QByteArray text = editor_->toPlainText().toUtf8(); if (text.size() > 2 * 1024 * 1024) { status_->setText(QStringLiteral("单个资源超过 2 MiB")); return false; }
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(text, &error); if (error.error != QJsonParseError::NoError || !doc.isObject()) { status_->setText(QStringLiteral("JSON 语法错误：") + error.errorString()); return false; }
    auto row = resources_[selected_].toObject(); row.insert(QStringLiteral("value"), doc.object()); resources_[selected_] = row; editor_->document()->setModified(false); return true;
}
void AndroidEditorDialog::selectResource(QTreeWidgetItem *item) {
    if (busy_ || !item || !item->parent()) return;
    if (!flush()) { QSignalBlocker block(tree_); QTreeWidgetItemIterator all(tree_); while (*all) { if ((*all)->parent() && (*all)->data(0, Qt::UserRole).toInt() == selected_) { tree_->setCurrentItem(*all); break; } ++all; } return; }
    selected_ = item->data(0, Qt::UserRole).toInt(); editor_->setPlainText(QString::fromUtf8(QJsonDocument(resources_[selected_].toObject().value(QStringLiteral("value")).toObject()).toJson(QJsonDocument::Indented))); editor_->document()->setModified(false); editor_->setReadOnly(false);
}
void AndroidEditorDialog::compile(bool exportOnly) {
    if (busy_ || !ready_ || !flush()) return;
    QString target;
    if (exportOnly) { target = QFileDialog::getSaveFileName(this, QStringLiteral("导出编译产物"), QFileInfo(path_).fileName()); if (target.isEmpty()) return; if (QFileInfo::exists(target)) { status_->setText(QStringLiteral("请选择不存在的新文件")); return; } }
    else if (QMessageBox::question(this, QStringLiteral("编译保存"), QStringLiteral("编译并替换原文件？APK 修改后需要重新签名，建议先备份。"), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    const QByteArray text = kind_ == QStringLiteral("arsc") ? QJsonDocument(resources_).toJson(QJsonDocument::Compact) : editor_->toPlainText().toUtf8(); if (text.size() > 32 * 1024 * 1024) { status_->setText(QStringLiteral("文档超过 32 MiB")); return; }
    status_->setText(QStringLiteral("正在编译并校验…"));
    launch([this, text, target](const Cancellation &cancel) {
        QTemporaryDir compiled; if (!compiled.isValid()) return failure(QStringLiteral("无法创建编译目录")); const QString document = compiled.filePath(QStringLiteral("edited.txt")), output = compiled.filePath(QStringLiteral("compiled.bin")); QFile file(document); if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size()) return failure(QStringLiteral("无法写入编辑内容")); file.close(); Result result = convertAndroidBinary(path_, document, output, kind_, cancel); if (!result.ok) return result;
        if (!target.isEmpty()) { if (cancel && cancel->load()) return failure(QStringLiteral("操作已取消")); return QFile::copy(output, target) ? Result{} : failure(QStringLiteral("导出失败，未覆盖现有文件")); }
        result = saveAndroidBinary(path_, output, expected_, original_, cancel); if (!result.ok) return result;
        if (!original_.path.isEmpty() && original_.source != Source::Local) { QFile input(output); QSaveFile local(path_); local.setDirectWriteFallback(false); if (!input.open(QIODevice::ReadOnly) || !local.open(QIODevice::WriteOnly) || local.write(input.readAll()) != input.size() || !local.commit()) return failure(QStringLiteral("已回写，但暂存副本更新失败；请关闭并重新打开")); } return result;
    }, [this, exportOnly](const Result &result) { if (!result.ok) { status_->setText(result.error.left(6000)); return; } if (!exportOnly) { expected_ = fileDigest(path_); dirty_ = false; editor_->document()->setModified(false); if (changed_) changed_(); } status_->setText(exportOnly ? QStringLiteral("已导出编译产物") : QStringLiteral("已编译保存 · APK 使用前请重新签名")); });
}
void AndroidEditorDialog::reject() { if (busy_) { if (cancel_) cancel_->store(true); status_->setText(QStringLiteral("正在停止，请稍后关闭")); return; } if (dirty_ && QMessageBox::question(this, QStringLiteral("放弃修改"), QStringLiteral("尚未编译保存，放弃修改？"), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return; QDialog::reject(); }
void AndroidEditorDialog::closeEvent(QCloseEvent *event) { event->ignore(); reject(); }
}
