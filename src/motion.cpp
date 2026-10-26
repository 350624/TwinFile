#include "motion.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QFileDialog>
#include <QHideEvent>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QStylePainter>
#include <QStyleOptionTab>
#include <QTimer>

namespace twinfile {
namespace {

class Snapshot : public QWidget {
public:
    Snapshot(QWidget *parent, const QPixmap &image, int offset, bool popup = false)
        : QWidget(parent, popup ? Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput : Qt::Widget),
          image_(image), offset_(offset) {
        setObjectName(QStringLiteral("uiSnapshotTransition"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_NoSystemBackground);
        if (popup) setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
        motion_.setObjectName(QStringLiteral("uiSnapshotAnimation"));
        motion_.setDuration(180); motion_.setStartValue(0.0); motion_.setEndValue(1.0);
        motion_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&motion_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) { progress_ = value.toReal(); update(); });
        connect(&motion_, &QVariantAnimation::finished, this, [this] { hide(); deleteLater(); });
        if (!popup) { setGeometry(parent->rect()); parent->installEventFilter(this); }
    }
    void start() { show(); raise(); motion_.start(); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setOpacity(1 - progress_);
        painter.translate(offset_ * progress_, 0);
        painter.drawPixmap(rect(), image_);
    }
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Hide
            || event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress) {
            motion_.stop(); hide(); deleteLater();
        }
        return false;
    }
private:
    QPixmap image_;
    int offset_ = 0;
    qreal progress_ = 0;
    QVariantAnimation motion_{this};
};

class NavigationIndicator : public QWidget {
public:
    explicit NavigationIndicator(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("navigationSelection"));
        setAttribute(Qt::WA_TransparentForMouseEvents); setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus); setGeometry(parent->rect()); parent->installEventFilter(this);
        motion_.setObjectName(QStringLiteral("navigationSelectionAnimation"));
        motion_.setDuration(220); motion_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&motion_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            position_ = value.toRectF(); setProperty("selectionRect", position_); update();
        });
    }
    void select(QAbstractButton *button, bool animate) {
        button_ = button;
        if (!button || !button->isVisibleTo(parentWidget())) {
            motion_.stop(); position_ = {}; target_ = {}; setProperty("selectionRect", position_); hide(); return;
        }
        const QRectF destination(button->mapTo(parentWidget(), QPoint()), button->size());
        if (target_ == destination && animate) return;
        motion_.stop(); target_ = destination; setProperty("targetRect", target_);
        setGeometry(parentWidget()->rect()); show(); raise();
        if (!animate || position_.isEmpty() || !motionAllowed(parentWidget())) {
            position_ = destination; setProperty("selectionRect", position_); update(); return;
        }
        motion_.setStartValue(position_); motion_.setEndValue(destination); motion_.start();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        if (position_.isEmpty()) return;
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); painter.setPen(Qt::NoPen);
        QColor color = palette().highlight().color(); color.setAlpha(42);
        painter.setBrush(color);
        const int radius = qApp->property("mtStyle").toBool() ? 0 : qApp->property("miuixStyle").toBool() ? 16 : 8;
        painter.drawRoundedRect(position_, radius, radius);
    }
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show)
            QTimer::singleShot(0, this, [this] {
                setGeometry(parentWidget()->rect());
                const QRectF destination = button_ ? QRectF(button_->mapTo(parentWidget(), QPoint()), button_->size()) : QRectF();
                if (destination != target_ || !motionAllowed(parentWidget())) select(button_, false);
            });
        if (event->type() == QEvent::Hide) select(button_, false);
        return false;
    }
private:
    QPointer<QAbstractButton> button_;
    QVariantAnimation motion_{this};
    QRectF position_, target_;
};

void discardSnapshots(QWidget *widget) {
    const auto snapshots = widget->findChildren<QWidget *>(QStringLiteral("uiSnapshotTransition"), Qt::FindDirectChildrenOnly);
    for (auto *snapshot : snapshots) { snapshot->hide(); delete snapshot; }
}

bool owns(const QWidget *owner, const QWidget *widget) {
    for (auto *parent = widget; parent; parent = parent->parentWidget()) if (parent == owner) return true;
    return false;
}

}

bool motionAllowed(const QWidget *widget) {
    for (auto *parent = widget; parent; parent = parent->parentWidget())
        if (parent->property("uiMotionEnabled").isValid()) return parent->property("uiMotionEnabled").toBool() && parent->isVisible() && !parent->isMinimized();
    return false;
}

void transitionSnapshot(QWidget *widget, const QPixmap &before, int offset) {
    discardSnapshots(widget);
    if (before.isNull() || !widget->isVisible() || !motionAllowed(widget)) return;
    (new Snapshot(widget, before, offset))->start();
}

MotionTabBar::MotionTabBar(QWidget *parent) : QTabBar(parent) {
    setProperty("motionTabs", true);
    motion_.setObjectName(QStringLiteral("tabSelectionAnimation"));
    motion_.setDuration(200); motion_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&motion_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        position_ = value.toRectF(); setProperty("selectionRect", position_); update();
    });
    connect(this, &QTabBar::currentChanged, this, [this] { selectTab(true); });
    connect(this, &QTabBar::tabMoved, this, [this] { selectTab(false); });
}

void MotionTabBar::selectTab(bool animate) {
    const QRectF destination = currentIndex() < 0 ? QRect() : tabRect(currentIndex());
    if (animate && destination == target_) return;
    motion_.stop(); target_ = destination;
    if (!animate || position_.isEmpty() || !motionAllowed(this) || !isVisible()) {
        position_ = target_; setProperty("selectionRect", position_); update(); return;
    }
    motion_.setStartValue(position_); motion_.setEndValue(target_); motion_.start();
}

void MotionTabBar::settleMotion() { selectTab(false); }
void MotionTabBar::resizeEvent(QResizeEvent *event) { QTabBar::resizeEvent(event); selectTab(false); }
void MotionTabBar::hideEvent(QHideEvent *event) { selectTab(false); QTabBar::hideEvent(event); }

void MotionTabBar::paintEvent(QPaintEvent *) {
    if (motion_.state() != QAbstractAnimation::Running && target_ != tabRect(currentIndex())) selectTab(false);
    QStylePainter painter(this); painter.setRenderHint(QPainter::Antialiasing); painter.setPen(Qt::NoPen);
    if (!position_.isEmpty()) {
        const bool mt = qApp->property("mtStyle").toBool();
        if (mt) {
            painter.setBrush(palette().highlight()); painter.drawRect(QRectF(position_.left(), position_.bottom() - 2, position_.width(), 2));
        } else {
            QColor color = palette().alternateBase().color();
            if (qApp->property("miuixStyle").toBool()) { color = palette().highlight().color(); color.setAlpha(38); }
            painter.setBrush(color);
            const int radius = qApp->property("miuixStyle").toBool() ? 14 : 7;
            painter.drawRoundedRect(position_.adjusted(2, 0, -2, 0), radius, radius);
        }
    }
    // Keep native labels, close buttons, focus and accessibility while painting one moving selection.
    for (int index = 0; index < count(); ++index) {
        if (!isTabVisible(index)) continue;
        QStyleOptionTab option; initStyleOption(&option, index);
        painter.drawControl(QStyle::CE_TabBarTab, option);
    }
}

MotionTabWidget::MotionTabWidget(QWidget *parent) : QTabWidget(parent) {
    setTabBar(new MotionTabBar(this));
    connect(this, &QTabWidget::currentChanged, this, [this](int index) {
        QWidget *next = widget(index);
        if (previous_ && next && previous_ != next && motionAllowed(this) && isVisible()) {
            const QPixmap before = previous_->grab();
            QPointer<QWidget> guarded = next;
            QTimer::singleShot(0, this, [guarded, before] { if (guarded) transitionSnapshot(guarded, before, -8); });
        }
        previous_ = next;
    });
}

UiMotion::UiMotion(QWidget *owner) : QObject(owner), owner_(owner) {
    setObjectName(QStringLiteral("uiMotion"));
    qApp->installEventFilter(this);
}

void UiMotion::updateNavigation() {
    auto *navigation = owner_->findChild<QWidget *>(QStringLiteral("navigationLocations"));
    if (!navigation) return;
    auto *indicator = static_cast<NavigationIndicator *>(navigation->findChild<QWidget *>(QStringLiteral("navigationSelection"), Qt::FindDirectChildrenOnly));
    if (!indicator) indicator = new NavigationIndicator(navigation);
    QAbstractButton *selected = nullptr;
    int priority = -1;
    for (auto *button : navigation->findChildren<QPushButton *>()) {
        if (!button->isChecked()) continue;
        const int score = button->property("navigationPath").isValid() ? 10000
            : button->property("navigationDevice").isValid() ? 9000 : button->property("navigationDrive").toString().size();
        if (score > priority) { priority = score; selected = button; }
    }
    indicator->select(selected, motionAllowed(owner_));
}

void UiMotion::settle() {
    if (!owner_) return;
    for (auto *bar : owner_->findChildren<QTabBar *>()) if (auto *animated = dynamic_cast<MotionTabBar *>(bar)) animated->settleMotion();
    for (auto *animation : owner_->findChildren<QVariantAnimation *>()) {
        if (animation->objectName() == QStringLiteral("navigationSelectionAnimation") || animation->objectName() == QStringLiteral("uiPopupAnimation")) {
            if (animation->state() != QAbstractAnimation::Stopped) { animation->setCurrentTime(animation->duration()); animation->stop(); }
        }
    }
    const auto snapshots = owner_->findChildren<QWidget *>(QStringLiteral("uiSnapshotTransition"));
    for (auto *snapshot : snapshots) { snapshot->hide(); delete snapshot; }
}

bool UiMotion::eventFilter(QObject *object, QEvent *event) {
    auto *widget = qobject_cast<QWidget *>(object);
    if (!owner_ || !widget || capturing_ || !owns(owner_, widget) || widget->objectName() == QStringLiteral("uiSnapshotTransition")) return false;
    if (widget == owner_ && event->type() == QEvent::Hide) { settle(); return false; }
    if (widget == owner_ && event->type() == QEvent::Show) QTimer::singleShot(0, this, &UiMotion::updateNavigation);
    const auto type = event->type();
    if (type == QEvent::Polish && qobject_cast<QMenu *>(widget)) {
        // Configure the native surface before creation; QSS rounds only the painted background.
        widget->setWindowFlags(widget->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        widget->setAttribute(Qt::WA_TranslucentBackground);
    }
    if (type == QEvent::MouseButtonPress || type == QEvent::KeyPress || type == QEvent::Wheel) {
        for (auto *parent = widget; parent; parent = parent->parentWidget()) {
            discardSnapshots(parent);
            if (parent == owner_) break;
        }
    }
    if (type == QEvent::Show && qobject_cast<QTabBar *>(widget)) {
        if (auto *bar = dynamic_cast<MotionTabBar *>(widget)) bar->settleMotion();
    }
    if (!motionAllowed(widget)) return false;
    auto capture = [this, widget] {
        capturing_ = true; const QPixmap image = widget->grab(); capturing_ = false; return image;
    };
    if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
        if (!button->isVisible() || !button->isEnabled() || button->property("navigationItem").toBool()
            || button->parentWidget()->objectName() == QStringLiteral("miuixPaneSwitch")) return false;
        if (type == QEvent::Enter || type == QEvent::Leave || type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease || type == QEvent::KeyPress) {
            const QPixmap before = capture(); QPointer<QWidget> guarded = widget;
            QTimer::singleShot(0, this, [guarded, before] { if (guarded) transitionSnapshot(guarded, before); });
        }
    }
    const bool popup = widget->isWindow() && (qobject_cast<QMenu *>(widget) || qobject_cast<QDialog *>(widget));
    // Focus/input-method events also arrive while Qt is deleting child widgets.
    // Only inspect popup children during the show/hide transitions that use them.
    if (!popup || (type != QEvent::Show && type != QEvent::Hide)) return false;
    const bool existingMotion = widget->findChild<QObject *>(QStringLiteral("mtMenuAnimation"), Qt::FindDirectChildrenOnly)
        || widget->findChild<QObject *>(QStringLiteral("mtDialogAnimation"), Qt::FindDirectChildrenOnly);
    auto *fileDialog = qobject_cast<QFileDialog *>(widget);
    if (!existingMotion && (!fileDialog || fileDialog->testOption(QFileDialog::DontUseNativeDialog))) {
        if (type == QEvent::Show) {
            auto *animation = widget->findChild<QVariantAnimation *>(QStringLiteral("uiPopupAnimation"), Qt::FindDirectChildrenOnly);
            if (!animation) {
                animation = new QVariantAnimation(widget); animation->setObjectName(QStringLiteral("uiPopupAnimation"));
                animation->setDuration(160); animation->setEasingCurve(QEasingCurve::OutCubic);
                connect(animation, &QVariantAnimation::valueChanged, widget, [widget](const QVariant &value) { widget->setWindowOpacity(value.toReal()); });
            }
            animation->stop(); animation->setStartValue(0.05); animation->setEndValue(1.0); widget->setWindowOpacity(0.05); animation->start();
        } else if (type == QEvent::Hide && owner_->isVisible()) {
            const qreal opacity = widget->windowOpacity();
            if (auto *animation = widget->findChild<QVariantAnimation *>(QStringLiteral("uiPopupAnimation"), Qt::FindDirectChildrenOnly)) animation->stop();
            const QPixmap before = capture();
            widget->setWindowOpacity(1);
            if (!before.isNull() && opacity > 0.1) {
                auto *snapshot = new Snapshot(owner_, before, 0, true);
                snapshot->setWindowOpacity(opacity); snapshot->setGeometry(QRect(widget->mapToGlobal(QPoint()), widget->size())); snapshot->start();
            }
        }
    }
    return false;
}

}
