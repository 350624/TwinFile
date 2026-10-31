#pragma once
#include <QPlainTextEdit>
namespace twinfile {
class CodeEdit : public QPlainTextEdit {
public:
    explicit CodeEdit(QWidget *parent = nullptr);
protected:
    void resizeEvent(QResizeEvent *event) override;
private:
    void updateGutter();
    void paintGutter();
    QWidget *gutter_;
};
}
