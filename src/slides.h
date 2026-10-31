#pragma once
#include "core.h"
#include <QDialog>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
class QGraphicsView;
class QGraphicsPixmapItem;
class QSpinBox;
class QLabel;
class QToolButton;
class QPlainTextEdit;
namespace twinfile {
bool isPresentation(const QString &name);
class SlidesDialog : public QDialog {
public:
    SlidesDialog(const QString &path, QWidget *parent, std::shared_ptr<QTemporaryDir> temporary = {});
    ~SlidesDialog() override;
private:
    void readOutput();
    void requestPage();
    void fail(const QString &message);
    void fit();
    void updateIcons();
    std::shared_ptr<QTemporaryDir> temporary_;
    QTemporaryDir workspace_;
    QProcess process_;
    QTimer timeout_;
    QByteArray pending_;
    QGraphicsView *view_;
    QGraphicsPixmapItem *image_;
    QSpinBox *page_;
    QLabel *count_, *status_;
    QToolButton *previous_, *next_;
    QPlainTextEdit *text_;
    int pages_ = 0, rendering_ = -1;
    bool failed_ = false, fitting_ = true;
protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
};
}
