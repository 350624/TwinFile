#include "editor.h"
#include "operations.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QShortcut>
#include <QStringConverter>
#include <QSyntaxHighlighter>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <windows.h>
#include <algorithm>
#include <vector>

namespace twinfile {
namespace {
Result failed(const QString &error) { return {false, error, {}, {}}; }
UINT codePage(const QString &name) { if (name == QStringLiteral("GB18030")) return 54936; if (name == QStringLiteral("GBK")) return 936; if (name == QStringLiteral("Big5")) return 950; if (name == QStringLiteral("Windows-1252")) return 1252; return 0; }
class Highlight : public QSyntaxHighlighter {
public:
    explicit Highlight(QTextDocument *document) : QSyntaxHighlighter(document) {
        qApp->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == qApp && event->type() == QEvent::ApplicationPaletteChange) rehighlight();
        return QSyntaxHighlighter::eventFilter(object, event);
    }
    void highlightBlock(const QString &text) override {
        const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
        const QColor comment(dark ? QStringLiteral("#a5b0c0") : QStringLiteral("#59687c"));
        const QList<QPair<QString, QColor>> rules{
            {QStringLiteral("\\b(?:class|struct|namespace|public|private|protected|const|static|void|int|bool|auto|return|if|else|for|while|switch|case|break|new|delete|true|false|null|nullptr|import|from|def|function|let|var|async|await|try|catch|throw)\\b"), QColor(dark ? QStringLiteral("#d4a1fa") : QStringLiteral("#7933aa"))},
            {QStringLiteral("\\b[0-9]+(?:\\.[0-9]+)?\\b"), QColor(dark ? QStringLiteral("#e6b477") : QStringLiteral("#8c500d"))},
            {QStringLiteral("</?[^>]+>"), QColor(dark ? QStringLiteral("#78cbe9") : QStringLiteral("#096580"))},
            {QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\"|'(?:[^'\\\\]|\\\\.)*'"), QColor(dark ? QStringLiteral("#a1cd8d") : QStringLiteral("#37672a"))},
            {QStringLiteral("//[^\\n]*|#[^\\n]*"), comment}};
        for (const auto &rule : rules) { const QRegularExpression pattern(rule.first); auto matches = pattern.globalMatch(text); while (matches.hasNext()) { const auto match = matches.next(); setFormat(int(match.capturedStart()), int(match.capturedLength()), rule.second); } }
        setCurrentBlockState(0); int start = previousBlockState() == 1 ? 0 : int(text.indexOf(QStringLiteral("/*")));
        while (start >= 0) { const int end = int(text.indexOf(QStringLiteral("*/"), start + (previousBlockState() == 1 && start == 0 ? 0 : 2))); const int count = end < 0 ? int(text.size()) - start : end - start + 2; setFormat(start, count, comment); if (end < 0) { setCurrentBlockState(1); break; } start = int(text.indexOf(QStringLiteral("/*"), end + 2)); }
    }
};
}

void highlightTextDocument(QTextDocument *document) { new Highlight(document); }

Result decodeText(const QByteArray &bytes, const QString &encoding, TextDocument *document) {
    if (!document || bytes.size() > 16 * 1024 * 1024) return failed(QStringLiteral("文本超过 16 MiB 编辑上限"));
    TextDocument result; result.encoding = encoding;
    if (encoding.isEmpty()) {
        if (bytes.startsWith(QByteArray::fromHex("fffe"))) result.encoding = QStringLiteral("UTF-16LE");
        else if (bytes.startsWith(QByteArray::fromHex("feff"))) result.encoding = QStringLiteral("UTF-16BE");
        else result.encoding = QStringLiteral("UTF-8");
    }
    QByteArray content = bytes;
    const QByteArray marker = result.encoding == QStringLiteral("UTF-8") ? QByteArray::fromHex("efbbbf") : result.encoding == QStringLiteral("UTF-16LE") ? QByteArray::fromHex("fffe") : result.encoding == QStringLiteral("UTF-16BE") ? QByteArray::fromHex("feff") : QByteArray();
    if (!marker.isEmpty() && content.startsWith(marker)) { result.bom = true; content.remove(0, marker.size()); }
    if (const UINT page = codePage(result.encoding)) {
        const int count = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, content.constData(), int(content.size()), nullptr, 0);
        if (!content.isEmpty() && count == 0) return failed(QStringLiteral("不是有效的 ") + result.encoding + QStringLiteral(" 文本"));
        result.text.resize(count); if (count) MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, content.constData(), int(content.size()), reinterpret_cast<LPWSTR>(result.text.data()), count);
    } else {
        const auto format = QStringConverter::encodingForName(result.encoding.toLatin1()); if (!format) return failed(QStringLiteral("不支持的编码"));
        QStringDecoder decoder(*format); result.text = decoder(content); if (decoder.hasError()) return failed(QStringLiteral("文本不符合所选编码，请选择正确编码后重新载入"));
    }
    if (result.text.contains(QChar(0))) return failed(QStringLiteral("文本包含 NUL；拒绝将二进制文件当作文本编辑"));
    const qsizetype crlf = result.text.count(QStringLiteral("\r\n"));
    const qsizetype lf = result.text.count(QChar('\n')) - crlf;
    const qsizetype cr = result.text.count(QChar('\r')) - crlf;
    if (crlf > 0 && crlf >= lf && crlf >= cr) result.newline = QStringLiteral("\r\n");
    else if (cr > lf) result.newline = QStringLiteral("\r");
    *document = result; return {};
}

TextDocument editedText(const TextDocument &original, const QString &text) {
    TextDocument result = original;
    QString normalized = original.text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QChar('\r'), QChar('\n'));
    if (normalized == text) return result;
    result.text = text;
    result.text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QChar('\r'), QChar('\n'));
    if (original.newline != QStringLiteral("\n")) result.text.replace(QStringLiteral("\n"), original.newline);
    return result;
}

Result encodeText(const TextDocument &document, QByteArray *bytes) {
    if (!bytes) return failed(QStringLiteral("无效的文本输出"));
    if (document.text.size() > 16 * 1024 * 1024) return failed(QStringLiteral("文本超过编辑上限，未保存"));
    if (const UINT page = codePage(document.encoding)) {
        const int count = WideCharToMultiByte(page, 0, reinterpret_cast<LPCWCH>(document.text.utf16()), int(document.text.size()), nullptr, 0, nullptr, nullptr);
        if (!document.text.isEmpty() && count == 0) return failed(QStringLiteral("编码转换失败"));
        bytes->resize(count); if (count) WideCharToMultiByte(page, 0, reinterpret_cast<LPCWCH>(document.text.utf16()), int(document.text.size()), bytes->data(), count, nullptr, nullptr);
        TextDocument decoded; const Result check = decodeText(*bytes, document.encoding, &decoded); if (!check.ok || decoded.text != document.text) return failed(QStringLiteral("当前编码无法无损保存这些字符，请改用 UTF-8"));
    } else {
        const auto format = QStringConverter::encodingForName(document.encoding.toLatin1()); if (!format) return failed(QStringLiteral("不支持的编码"));
        QStringEncoder encoder(*format, document.bom ? QStringConverter::Flag::WriteBom : QStringConverter::Flag::Default); *bytes = encoder(document.text); if (encoder.hasError()) return failed(QStringLiteral("编码转换失败"));
    }
    if (bytes->size() > 16 * 1024 * 1024) return failed(QStringLiteral("编码后文本超过 16 MiB，未保存"));
    return {};
}

Result replaceTextBounded(const QString &text, const QString &pattern, const QString &replacement, bool regular, bool sensitive, qsizetype limit) {
    if (limit < 0 || limit > 16 * 1024 * 1024 || text.size() > limit) return failed(QStringLiteral("文本或替换结果超过上限"));
    const QRegularExpression expression(regular ? pattern : QRegularExpression::escape(pattern), sensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
    if (!expression.isValid()) return failed(expression.errorString());
    QString output;
    auto append = [&](QStringView part) { if (part.size() > limit - output.size()) return false; output.append(part); return true; };
    auto matches = expression.globalMatch(text);
    qsizetype copied = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (!append(QStringView(text).mid(copied, match.capturedStart() - copied))) return failed(QStringLiteral("替换结果过大，未更改"));
        for (qsizetype index = 0; index < replacement.size(); ++index) {
            if (regular && replacement[index] == QChar('\\') && index + 1 < replacement.size() && replacement[index + 1] >= QChar('1') && replacement[index + 1] <= QChar('9')) {
                const int group = replacement[++index].digitValue(); if (!append(match.capturedView(group))) return failed(QStringLiteral("替换结果过大，未更改"));
            } else if (!append(QStringView(replacement).mid(index, 1))) return failed(QStringLiteral("替换结果过大，未更改"));
        }
        copied = match.capturedEnd();
    }
    if (!append(QStringView(text).mid(copied))) return failed(QStringLiteral("替换结果过大，未更改"));
    return {true, {}, output, {}};
}

QStringList textDifference(const QString &left, const QString &right, const Cancellation &cancel) {
    const QStringList before = left.split(QChar('\n')), after = right.split(QChar('\n')); const int beforeCount = int(before.size()), afterCount = int(after.size());
    int prefix = 0; while (prefix < beforeCount && prefix < afterCount && before[prefix] == after[prefix]) ++prefix;
    int suffix = 0; while (suffix < beforeCount - prefix && suffix < afterCount - prefix && before[beforeCount - suffix - 1] == after[afterCount - suffix - 1]) ++suffix;
    if (prefix == beforeCount && prefix == afterCount) return {};
    const int leftCount = beforeCount - prefix - suffix, rightCount = afterCount - prefix - suffix; QStringList output;
    if (qint64(leftCount + 1) * (rightCount + 1) > 16000000) {
        output.append(QStringLiteral("@@ 大差异块（为限制内存，未做逐行对齐）@@")); for (int index = prefix; index < beforeCount - suffix; ++index) output.append(QStringLiteral("- ") + before[index]); for (int index = prefix; index < afterCount - suffix; ++index) output.append(QStringLiteral("+ ") + after[index]); return output;
    }
    const qsizetype columns = rightCount + 1; std::vector<quint32> lengths(size_t(leftCount + 1) * size_t(columns), 0);
    for (int leftIndex = leftCount - 1; leftIndex >= 0; --leftIndex) {
        if (cancel && cancel->load()) return {QStringLiteral("已取消")};
        for (int rightIndex = rightCount - 1; rightIndex >= 0; --rightIndex) lengths[size_t(leftIndex * columns + rightIndex)] = before[prefix + leftIndex] == after[prefix + rightIndex] ? 1 + lengths[size_t((leftIndex + 1) * columns + rightIndex + 1)] : qMax(lengths[size_t((leftIndex + 1) * columns + rightIndex)], lengths[size_t(leftIndex * columns + rightIndex + 1)]);
    }
    int leftIndex = 0, rightIndex = 0; bool block = false;
    while (leftIndex < leftCount || rightIndex < rightCount) {
        if (leftIndex < leftCount && rightIndex < rightCount && before[prefix + leftIndex] == after[prefix + rightIndex]) { ++leftIndex; ++rightIndex; block = false; continue; }
        if (!block) { output.append(QStringLiteral("@@ 左 %1 / 右 %2 @@").arg(prefix + leftIndex + 1).arg(prefix + rightIndex + 1)); block = true; }
        if (leftIndex < leftCount && (rightIndex == rightCount || lengths[size_t((leftIndex + 1) * columns + rightIndex)] >= lengths[size_t(leftIndex * columns + rightIndex + 1)])) output.append(QStringLiteral("- ") + before[prefix + leftIndex++]); else output.append(QStringLiteral("+ ") + after[prefix + rightIndex++]);
    }
    return output;
}

EditorDialog::EditorDialog(const QString &path, QWidget *parent, const Entry &original, std::shared_ptr<QTemporaryDir> temporary, std::function<void()> changed)
    : QDialog(parent), path_(path), original_(original), temporary_(std::move(temporary)), changed_(std::move(changed)) {
    setAttribute(Qt::WA_DeleteOnClose); setWindowTitle(QFileInfo(path).fileName() + QStringLiteral(" — 文本编辑器")); resize(980, 720); auto *layout = new QVBoxLayout(this);
    auto *tools = new QHBoxLayout; encoding_ = new QComboBox; encoding_->addItems({QStringLiteral("UTF-8"), QStringLiteral("UTF-16LE"), QStringLiteral("UTF-16BE"), QStringLiteral("GB18030"), QStringLiteral("GBK"), QStringLiteral("Big5"), QStringLiteral("Windows-1252")});
    tools->addWidget(new QLabel(QStringLiteral("保存编码"))); tools->addWidget(encoding_); auto *reload = new QPushButton(QStringLiteral("按所选编码重新载入")); tools->addWidget(reload); tools->addStretch(); layout->addLayout(tools);
    auto *search = new QHBoxLayout; find_ = new QLineEdit; replacement_ = new QLineEdit; find_->setPlaceholderText(QStringLiteral("查找")); replacement_->setPlaceholderText(QStringLiteral("替换为")); search->addWidget(find_); search->addWidget(replacement_); case_ = new QCheckBox(QStringLiteral("区分大小写")); regular_ = new QCheckBox(QStringLiteral("正则")); search->addWidget(case_); search->addWidget(regular_);
    auto button = [&](const QString &name, auto action) { auto *control = new QPushButton(name); search->addWidget(control); connect(control, &QPushButton::clicked, this, action); };
    button(QStringLiteral("上一个"), [this] { find(true); }); button(QStringLiteral("下一个"), [this] { find(false); }); button(QStringLiteral("替换"), [this] { replace(false); }); button(QStringLiteral("全部替换"), [this] { replace(true); }); layout->addLayout(search);
    editor_ = new QPlainTextEdit; editor_->setFont(QFont(QStringLiteral("Consolas"), 11)); editor_->setLineWrapMode(QPlainTextEdit::NoWrap); layout->addWidget(editor_, 1); new Highlight(editor_->document());
    status_ = new QLabel; layout->addWidget(status_); auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close); layout->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked, this, [this] { save(); }); connect(buttons, &QDialogButtonBox::rejected, this, &EditorDialog::reject);
    connect(reload, &QPushButton::clicked, this, [this] { if (!saving_ && (!editor_->document()->isModified() || QMessageBox::question(this, QStringLiteral("重新载入"), QStringLiteral("放弃尚未保存的修改？")) == QMessageBox::Yes)) load(encoding_->currentText()); });
    auto *shortcut = new QShortcut(QKeySequence::Save, this); connect(shortcut, &QShortcut::activated, this, [this] { save(); }); auto *findShortcut = new QShortcut(QKeySequence::Find, this); connect(findShortcut, &QShortcut::activated, find_, [this] { find_->setFocus(); find_->selectAll(); });
    connect(find_, &QLineEdit::returnPressed, this, [this] { find(false); }); load({});
}
bool EditorDialog::load(const QString &encoding) {
    QFile input(path_); if (!input.open(QIODevice::ReadOnly) || input.size() > 16 * 1024 * 1024) { status_->setText(QStringLiteral("无法打开或超过 16 MiB 编辑上限")); editor_->setReadOnly(true); return false; }
    const QByteArray bytes = input.readAll(); TextDocument document; const Result decoded = decodeText(bytes, encoding, &document);
    if (!decoded.ok) { status_->setText(decoded.error); editor_->setReadOnly(true); return false; }
    document_ = document; originalHash_ = fileDigest(path_); encoding_->setCurrentText(document.encoding); editor_->setPlainText(document.text); editor_->setReadOnly(false); editor_->document()->setModified(false); status_->setText(QStringLiteral("保留原编码与 BOM；保存使用原子替换，Ctrl+S 保存。")); return true;
}
void EditorDialog::save(bool closeAfter) {
    if (saving_ || editor_->isReadOnly()) return;
    TextDocument saved = editedText(document_, editor_->toPlainText()); saved.encoding = encoding_->currentText(); if (saved.encoding != document_.encoding) saved.bom = saved.encoding.startsWith(QStringLiteral("UTF-16")); QByteArray bytes; const Result encoded = encodeText(saved, &bytes);
    if (!encoded.ok) { QMessageBox::warning(this, QStringLiteral("不能保存"), encoded.error); return; }
    if (!temporary_ && fileDigest(path_) != originalHash_) { QMessageBox::warning(this, QStringLiteral("外部修改冲突"), QStringLiteral("原文件已被其它程序修改，请先备份编辑内容，未覆盖原文件。")); return; }
    QSaveFile output(path_); output.setDirectWriteFallback(false); if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) { QMessageBox::warning(this, QStringLiteral("保存失败"), output.errorString()); return; }
    if (!temporary_) { document_ = saved; originalHash_ = fileDigest(path_); editor_->document()->setModified(false); if (changed_) changed_(); if (closeAfter) QDialog::reject(); return; }
    saving_ = true; editor_->setReadOnly(true); status_->setText(QStringLiteral("正在回写并校验原文件…")); cancel_ = std::make_shared<std::atomic_bool>(false); auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, saved, closeAfter] {
        const Result result = watcher->result(); watcher->deleteLater(); saving_ = false; editor_->setReadOnly(false);
        if (!result.ok) { status_->setText(result.error); QMessageBox::warning(this, QStringLiteral("回写失败，编辑内容仍保留"), result.error); return; }
        document_ = saved; originalHash_ = fileDigest(path_); editor_->document()->setModified(false); status_->setText(QStringLiteral("已保存并回写")); if (changed_) changed_(); if (closeAfter) QDialog::reject();
    });
    watcher->setFuture(QtConcurrent::run([entry = original_, path = path_, expected = originalHash_, cancel = cancel_, keep = temporary_] { return writeBackEntry(entry, path, expected, cancel); }));
}
void EditorDialog::reject() {
    if (saving_) { status_->setText(QStringLiteral("请等待回写完成后关闭")); return; }
    if (editor_->document()->isModified()) { const auto choice = QMessageBox::question(this, QStringLiteral("未保存"), QStringLiteral("保存修改后关闭？"), QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel); if (choice == QMessageBox::Cancel) return; if (choice == QMessageBox::Save) { save(true); return; } }
    QDialog::reject();
}
void EditorDialog::closeEvent(QCloseEvent *event) { event->ignore(); reject(); }
void EditorDialog::find(bool backwards) {
    if (find_->text().isEmpty()) return; QTextDocument::FindFlags flags; if (backwards) flags |= QTextDocument::FindBackward; if (case_->isChecked()) flags |= QTextDocument::FindCaseSensitively;
    QRegularExpression expression(regular_->isChecked() ? find_->text() : QRegularExpression::escape(find_->text()), case_->isChecked() ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
    if (!expression.isValid()) { status_->setText(expression.errorString()); return; }
    QTextCursor found = editor_->document()->find(expression, editor_->textCursor(), flags);
    if (found.isNull()) { QTextCursor edge(editor_->document()); if (backwards) edge.movePosition(QTextCursor::End); found = editor_->document()->find(expression, edge, flags); }
    if (found.isNull()) status_->setText(QStringLiteral("没有找到")); else { editor_->setTextCursor(found); status_->setText(QStringLiteral("已找到")); }
}
void EditorDialog::replace(bool all) {
    if (editor_->isReadOnly() || find_->text().isEmpty()) return;
    QRegularExpression expression(regular_->isChecked() ? find_->text() : QRegularExpression::escape(find_->text()), case_->isChecked() ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
    if (!expression.isValid()) { status_->setText(expression.errorString()); return; }
    if (!all) { QTextCursor cursor = editor_->textCursor(); const auto match = expression.match(cursor.selectedText()); if (match.hasMatch() && match.capturedStart() == 0 && match.capturedLength() == cursor.selectedText().size()) cursor.insertText(replacement_->text()); find(false); return; }
    const Result replaced = replaceTextBounded(editor_->toPlainText(), find_->text(), replacement_->text(), regular_->isChecked(), case_->isChecked());
    if (!replaced.ok) { status_->setText(replaced.error); return; }
    const QString text = replaced.output;
    QTextCursor cursor(editor_->document()); cursor.beginEditBlock(); cursor.select(QTextCursor::Document); cursor.insertText(text); cursor.endEditBlock();
}

}
