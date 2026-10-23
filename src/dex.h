#pragma once

#include "operations.h"
#include <QDialog>
#include <QFutureWatcher>

class QTreeWidget;
class QTreeWidgetItem;
class QPlainTextEdit;
class QLineEdit;
class QLabel;
class QPushButton;

namespace twinfile {

bool isDex(const QString &name);
Result validateDex(const QString &path);
Result convertDex(const QString &source, const QString &workspace, const QString &output, const Cancellation &cancel = {});
Result saveDex(const QString &source, const QString &compiled, const QByteArray &expected, const Entry &original = {}, const Cancellation &cancel = {});

class DexDialog : public QDialog {
public:
    DexDialog(const QString &path, QWidget *parent, const Entry &original = {}, std::shared_ptr<QTemporaryDir> temporary = {}, std::function<void()> changed = {});
    ~DexDialog() override;
    void reject() override;
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void selectClass(QTreeWidgetItem *item);
    bool flushClass();
    void compile(bool exportOnly);
    void launch(std::function<Result(const Cancellation &)> work, std::function<void(const Result &)> done);
    void populate();
    QString path_, current_;
    Entry original_;
    QByteArray expected_;
    std::shared_ptr<QTemporaryDir> temporary_, workspace_;
    std::function<void()> changed_;
    Cancellation cancel_;
    QFutureWatcher<Result> worker_;
    QTreeWidget *tree_;
    QPlainTextEdit *editor_;
    QLineEdit *filter_, *find_, *replacement_;
    QLabel *status_;
    QPushButton *save_, *export_, *stop_;
    bool busy_ = false, dirty_ = false, ready_ = false;
};

}
