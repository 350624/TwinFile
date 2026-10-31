#pragma once

#include "core.h"
#include <QDialog>
#include <QTemporaryDir>

class QPlainTextEdit;
class QComboBox;
class QLabel;
class QLineEdit;
class QCheckBox;
class QTextDocument;

namespace twinfile {
void highlightTextDocument(QTextDocument *document);

struct TextDocument {
    QString text;
    QString encoding = QStringLiteral("UTF-8");
    bool bom = false;
    QString newline = QStringLiteral("\n");
};
Result decodeText(const QByteArray &bytes, const QString &encoding, TextDocument *document);
Result encodeText(const TextDocument &document, QByteArray *bytes);
TextDocument editedText(const TextDocument &original, const QString &text);
Result replaceTextBounded(const QString &text, const QString &pattern, const QString &replacement, bool regular, bool sensitive, qsizetype limit = 16 * 1024 * 1024);
QStringList textDifference(const QString &left, const QString &right, const Cancellation &cancel = {});
struct DiffRow { int left = 0, right = 0, kind = 0; QString text; int changedStart = -1, changedLength = 0; };
struct TextComparison { QList<DiffRow> rows; QString error; bool coarse = false; };
TextComparison alignedDifference(const QString &left, const QString &right, int ignore, const Cancellation &cancel = {});
class DifferenceDialog : public QDialog {
public:
    DifferenceDialog(const QString &left, const QString &right, QWidget *parent);
    ~DifferenceDialog() override;
private:
    Cancellation cancel_;
};

class EditorDialog : public QDialog {
public:
    EditorDialog(const QString &path, QWidget *parent, const Entry &original = {}, std::shared_ptr<QTemporaryDir> temporary = {}, std::function<void()> changed = {});
    void reject() override;
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    bool load(const QString &encoding);
    void save(bool closeAfter = false);
    void find(bool backwards);
    void replace(bool all);
    QString path_;
    Entry original_;
    std::shared_ptr<QTemporaryDir> temporary_;
    std::function<void()> changed_;
    TextDocument document_;
    QByteArray originalHash_;
    QPlainTextEdit *editor_;
    QComboBox *encoding_;
    QLabel *status_;
    QLineEdit *find_, *replacement_;
    QCheckBox *case_, *regular_;
    bool saving_ = false;
    Cancellation cancel_;
};

}
