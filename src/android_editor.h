#pragma once
#include "operations.h"
#include <QDialog>
#include <QFutureWatcher>
#include <QJsonArray>
class QPlainTextEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QPushButton;
namespace twinfile {
bool isAndroidBinary(const QString &name);
Result convertAndroidBinary(const QString &source, const QString &document, const QString &output, const QString &kind, const Cancellation &cancel = {});
Result saveAndroidBinary(const QString &source, const QString &compiled, const QByteArray &expected, const Entry &original = {}, const Cancellation &cancel = {});
class AndroidEditorDialog : public QDialog {
public:
    AndroidEditorDialog(const QString &path, QWidget *parent, const Entry &original = {}, std::shared_ptr<QTemporaryDir> temporary = {}, std::function<void()> changed = {});
    ~AndroidEditorDialog() override;
    void reject() override;
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void launch(std::function<Result(const Cancellation &)> work, std::function<void(const Result &)> done);
    bool flush();
    void selectResource(QTreeWidgetItem *item);
    void compile(bool exportOnly);
    QString path_, kind_;
    Entry original_;
    QByteArray expected_;
    std::shared_ptr<QTemporaryDir> temporary_, workspace_;
    std::function<void()> changed_;
    QJsonArray resources_;
    int selected_ = -1;
    Cancellation cancel_;
    QFutureWatcher<Result> worker_;
    QPlainTextEdit *editor_;
    QTreeWidget *tree_;
    QLabel *status_;
    QPushButton *save_, *export_, *stop_;
    bool busy_ = false, ready_ = false, dirty_ = false;
};
}
