#pragma once

#include <QPainterPath>
#include <QPushButton>
#include <QVariantAnimation>

namespace twinfile {

QPainterPath continuousRoundedRect(const QRectF &bounds, qreal cornerExtent);
void paintMiuixSurface(QWidget *widget);

class SmoothActionButton : public QPushButton {
public:
    explicit SmoothActionButton(const QString &text = {}, QWidget *parent = nullptr);
    void settleMotion();
protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    QColor surfaceColor() const;
    void updateSurface(bool animate);
    QVariantAnimation motion_{this};
    QVariantAnimation feedback_{this}, ripple_{this};
    QColor color_, target_;
    QPointF feedbackPosition_{1, 0}, rippleOrigin_;
    qreal rippleProgress_ = 1;
};

class SmoothToolbar : public QWidget {
public:
    explicit SmoothToolbar(QWidget *parent = nullptr) : QWidget(parent) {}
protected:
    void paintEvent(QPaintEvent *event) override;
};

}
