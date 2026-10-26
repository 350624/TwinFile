#pragma once

#include <QPointer>
#include <QTabBar>
#include <QTabWidget>
#include <QVariantAnimation>

namespace twinfile {

bool motionAllowed(const QWidget *widget);
void transitionSnapshot(QWidget *widget, const QPixmap &before, int offset = 0);

class MotionTabBar : public QTabBar {
public:
    explicit MotionTabBar(QWidget *parent = nullptr);
    void settleMotion();
protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;
private:
    void selectTab(bool animate);
    QVariantAnimation motion_{this};
    QRectF position_, target_;
};

class MotionTabWidget : public QTabWidget {
public:
    explicit MotionTabWidget(QWidget *parent = nullptr);
private:
    QPointer<QWidget> previous_;
};

class UiMotion : public QObject {
public:
    explicit UiMotion(QWidget *owner);
    void updateNavigation();
    void settle();
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    QPointer<QWidget> owner_;
    bool capturing_ = false;
};

}
