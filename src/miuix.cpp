#include "window.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>

namespace twinfile {
namespace {

class PaneSwitch : public QWidget {
public:
    explicit PaneSwitch(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("miuixPaneSwitch")); setFixedSize(164, 38);
        motion_.setObjectName(QStringLiteral("miuixPaneAnimation"));
        motion_.setDuration(210); motion_.setEasingCurve(QEasingCurve::OutCubic);
        auto *row = new QHBoxLayout(this); row->setContentsMargins(4, 4, 4, 4); row->setSpacing(0);
        for (int index = 0; index < 2; ++index) {
            auto *button = new QToolButton(this); button->setText(index ? QStringLiteral("右栏") : QStringLiteral("左栏"));
            button->setObjectName(index ? QStringLiteral("miuixRightPane") : QStringLiteral("miuixLeftPane"));
            button->setAccessibleName(index ? QStringLiteral("切换到右栏") : QStringLiteral("切换到左栏"));
            button->setCheckable(true); button->setCursor(Qt::PointingHandCursor);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); row->addWidget(button);
        }
        connect(&motion_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            position_ = value.toReal(); setProperty("position", position_); update();
        });
        setProperty("position", 0.0);
    }
    void select(bool right, bool animate) {
        const qreal target = right ? 1.0 : 0.0;
        findChild<QToolButton *>(QStringLiteral("miuixLeftPane"))->setChecked(!right);
        findChild<QToolButton *>(QStringLiteral("miuixRightPane"))->setChecked(right);
        if (!animate) {
            motion_.stop(); position_ = target; setProperty("position", target); update(); return;
        }
        if (target_ == target && (motion_.state() == QAbstractAnimation::Running || qFuzzyCompare(position_ + 1, target + 1))) return;
        target_ = target; motion_.stop();
        motion_.setStartValue(position_); motion_.setEndValue(target); motion_.start();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); painter.setPen(Qt::NoPen);
        painter.setBrush(palette().alternateBase()); painter.drawRoundedRect(rect(), 19, 19);
        const qreal segment = (width() - 8.0) / 2;
        painter.setBrush(palette().base()); painter.drawRoundedRect(QRectF(4 + segment * position_, 4, segment, height() - 8), 15, 15);
    }
private:
    QVariantAnimation motion_{this};
    qreal position_ = 0;
    qreal target_ = 0;
};

}

void Window::buildMiuixChrome(QHBoxLayout *heading) {
    auto *identity = new QWidget; identity->setObjectName(QStringLiteral("miuixIdentity")); identity->setMinimumWidth(0);
    identity->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *column = new QVBoxLayout(identity); column->setContentsMargins(0, 0, 0, 0); column->setSpacing(3);
    auto *title = new QLabel(QStringLiteral("文件管理")); title->setObjectName(QStringLiteral("miuixTitle")); column->addWidget(title);
    auto *location = new QLabel; location->setObjectName(QStringLiteral("miuixLocation")); location->setMinimumWidth(0);
    location->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); column->addWidget(location);
    heading->insertWidget(0, identity, 1); identity->hide();
    auto *selector = new PaneSwitch(centralWidget()); heading->insertWidget(1, selector); selector->hide();
    connect(selector->findChild<QToolButton *>(QStringLiteral("miuixLeftPane")), &QToolButton::clicked, this, [this] { setActive(left_); left_->view()->setFocus(); });
    connect(selector->findChild<QToolButton *>(QStringLiteral("miuixRightPane")), &QToolButton::clicked, this, [this] { setActive(right_); right_->view()->setFocus(); });
}

void Window::applyMiuixChrome(bool enabled) {
    findChild<QWidget *>(QStringLiteral("miuixIdentity"))->setVisible(enabled);
    findChild<QWidget *>(QStringLiteral("miuixPaneSwitch"))->setVisible(enabled);
    auto *header = findChild<QWidget *>(QStringLiteral("workspaceHeader"));
    auto *heading = qobject_cast<QHBoxLayout *>(header->layout());
    const bool mt = settings_.value(QStringLiteral("theme")).toString() == QStringLiteral("mt");
    for (int index = 0; index < heading->count(); ++index)
        if (heading->itemAt(index)->spacerItem()) heading->setStretch(index, enabled || mt ? 0 : 1);
    auto *content = findChild<QWidget *>(QStringLiteral("workspaceContent"));
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    auto *toolbar = findChild<QWidget *>(QStringLiteral("workspaceToolbar"));
    layout->removeWidget(toolbar); layout->insertWidget(enabled ? layout->count() : 0, toolbar);
    if (enabled) {
        header->layout()->setContentsMargins(24, 18, 18, 16);
        findChild<QLabel *>(QStringLiteral("workspaceHeading"))->hide();
        findChild<QLabel *>(QStringLiteral("workspaceDetail"))->hide();
        findChild<QSplitter *>(QStringLiteral("fileSplitter"))->setHandleWidth(14);
        centralWidget()->layout()->setContentsMargins(0, 0, 16, 0);
        layout->setContentsMargins(8, 0, 8, 10);
        layout->setSpacing(14);
    } else {
        header->layout()->setContentsMargins(20, 14, 2, 14);
        findChild<QLabel *>(QStringLiteral("workspaceHeading"))->show();
        findChild<QLabel *>(QStringLiteral("workspaceDetail"))->show();
        findChild<QSplitter *>(QStringLiteral("fileSplitter"))->setHandleWidth(6);
        centralWidget()->layout()->setContentsMargins(0, 0, 12, 0);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
    }
    updateMiuixContext(); adaptMiuixChrome(); adaptToolbar();
}

void Window::updateMiuixContext() {
    auto *selector = static_cast<PaneSwitch *>(findChild<QWidget *>(QStringLiteral("miuixPaneSwitch")));
    if (!selector || !active_) return;
    const bool enabled = settings_.value(QStringLiteral("theme")).toString() == QStringLiteral("miuix");
    selector->select(active_ == right_, enabled && isVisible() && !settings_.value(QStringLiteral("reduced_motion")).toBool());
    if (!enabled) return;
    auto *location = findChild<QLabel *>(QStringLiteral("miuixLocation"));
    const QString path = active_->location().display();
    location->setText(location->fontMetrics().elidedText(path, Qt::ElideMiddle, qMax(30, location->width())));
    location->setToolTip(path);
}

void Window::adaptMiuixChrome() {
    if (settings_.value(QStringLiteral("theme")).toString() != QStringLiteral("miuix")) return;
    const bool compact = centralWidget()->width() < 1000;
    sidebar_->setFixedWidth(compact ? 164 : 196);
    auto *search = findChild<QPushButton *>(QStringLiteral("workspaceSearch"));
    search->setText(compact ? QString() : QStringLiteral("搜索文件"));
    search->setMinimumWidth(compact ? 36 : 120); search->setMaximumWidth(compact ? 36 : 160);
    search->setToolTip(QStringLiteral("搜索当前栏文件 Ctrl+F"));
    updateMiuixContext();
}

}
