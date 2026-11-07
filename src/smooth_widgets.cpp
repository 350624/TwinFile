#include "smooth_widgets.h"

#include "appearance.h"
#include "motion.h"
#include "miuix_theme.h"

#include <QApplication>
#include <QEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QStyleOptionButton>
#include <QtMath>

namespace twinfile {

QPainterPath continuousRoundedRect(const QRectF &bounds, qreal cornerExtent) {
    QPainterPath path;
    if (bounds.isEmpty() || !qIsFinite(bounds.x()) || !qIsFinite(bounds.y())
        || !qIsFinite(bounds.width()) || !qIsFinite(bounds.height()) || !qIsFinite(cornerExtent)) return path;
    const qreal extent = qBound(qreal(0), cornerExtent, qMin(bounds.width(), bounds.height()) / 2);
    if (qFuzzyIsNull(extent)) { path.addRect(bounds); return path; }
    // Two symmetric cubics per corner: zero curvature at each straight edge,
    // matching first and second derivatives at the diagonal join.
    const qreal k = 0.22;
    auto corner = [&](QPointF origin, QPointF across, QPointF down) {
        auto point = [&](qreal x, qreal y) { return origin + across * (x * extent) + down * (y * extent); };
        path.cubicTo(point(1 - 4 * k, 0), point(1 - 2 * k, 0), point(1 - k, k));
        path.cubicTo(point(1, 2 * k), point(1, 4 * k), point(1, 1));
    };
    path.moveTo(bounds.left() + extent, bounds.top());
    path.lineTo(bounds.right() - extent, bounds.top());
    corner({bounds.right() - extent, bounds.top()}, {1, 0}, {0, 1});
    path.lineTo(bounds.right(), bounds.bottom() - extent);
    corner({bounds.right(), bounds.bottom() - extent}, {0, 1}, {-1, 0});
    path.lineTo(bounds.left() + extent, bounds.bottom());
    corner({bounds.left() + extent, bounds.bottom()}, {-1, 0}, {0, -1});
    path.lineTo(bounds.left(), bounds.top() + extent);
    corner({bounds.left(), bounds.top() + extent}, {0, -1}, {1, 0});
    path.closeSubpath();
    return path;
}

void paintMiuixSurface(QWidget *widget) {
    if (!qApp->property("miuixStyle").toBool()) return;
    const QString name = widget->objectName();
    const bool menu = qobject_cast<QMenu *>(widget);
    const bool pane = name == QStringLiteral("filePane");
    const bool path = name == QStringLiteral("panePath") && qobject_cast<QLineEdit *>(widget);
    const bool composer = name == QStringLiteral("aiComposer");
    if (!menu && !pane && !path && !composer) return;
    const auto t = miuixTokens(widget->palette());
    const bool focused = widget->hasFocus() || widget->property("focused").toBool();
    QColor background = menu ? t.surface4 : pane || focused ? t.surface2 : t.surface3;
    QColor border = focused ? t.primary : t.border;
    if (pane) { border = widget->property("active").toBool() ? t.primary : t.divider; border.setAlpha(110); }
    QPainter painter(widget); painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(background); painter.setPen(QPen(border, 1));
    painter.drawPath(continuousRoundedRect(QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5), pane || composer ? 16 : 12));
}

SmoothActionButton::SmoothActionButton(const QString &text, QWidget *parent) : QPushButton(text, parent) {
    setAttribute(Qt::WA_Hover);
    motion_.setObjectName(QStringLiteral("continuousButtonAnimation"));
    motion_.setEasingCurve(miuixEase());
    connect(&motion_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        color_ = value.value<QColor>(); update();
    });
    feedback_.setObjectName(QStringLiteral("continuousButtonFeedback"));
    feedback_.setEasingCurve(miuixEase());
    connect(&feedback_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        feedbackPosition_ = value.toPointF(); update();
    });
    ripple_.setObjectName(QStringLiteral("continuousButtonRipple"));
    ripple_.setDuration(500); ripple_.setEasingCurve(miuixEase());
    connect(&ripple_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        rippleProgress_ = value.toReal(); update();
    });
    connect(this, &QPushButton::toggled, this, [this] { updateSurface(true); });
}

QColor SmoothActionButton::surfaceColor() const {
    const auto t = miuixTokens(palette());
    const bool primary = property("primaryAction").toBool();
    if (!isEnabled()) return primary ? t.primaryDisabled : QColor(Qt::transparent);
    if (primary) {
        return isDown() ? t.primaryActive : underMouse() ? t.primaryHover : t.primary;
    }
    if (objectName() == QStringLiteral("workspaceSearch")) return t.surface3;
    if (isDown()) return t.active;
    if (isChecked() && !property("navigationItem").toBool()) return t.selection;
    return underMouse() ? t.hover : QColor(Qt::transparent);
}

void SmoothActionButton::updateSurface(bool animate) {
    const bool animated = animate && motionAllowed(this) && qApp->property("miuixStyle").toBool();
    const QPointF feedbackTarget(isDown() && isEnabled() ? 0.95 : 1.0,
        !isDown() && isEnabled() && underMouse() && property("primaryAction").toBool() ? -1 : 0);
    if (feedback_.endValue().toPointF() != feedbackTarget || !animated) {
        feedback_.stop();
        if (!animated) feedbackPosition_ = feedbackTarget;
        else {
            feedback_.setDuration(isDown() ? 100 : underMouse() ? 200 : 320);
            feedback_.setStartValue(feedbackPosition_); feedback_.setEndValue(feedbackTarget); feedback_.start();
        }
    }
    const QColor target = surfaceColor();
    if (target_ == target && color_.isValid()) return;
    target_ = target; motion_.stop();
    if (!animated || !color_.isValid()) {
        color_ = target; update(); return;
    }
    // Interpolate opacity without passing through black when entering/leaving a transparent surface.
    if (color_.alpha() == 0) color_ = QColor(target.red(), target.green(), target.blue(), 0);
    QColor end = target;
    if (end.alpha() == 0) end = QColor(color_.red(), color_.green(), color_.blue(), 0);
    motion_.setDuration(isDown() ? 100 : underMouse() ? 200 : 320);
    motion_.setStartValue(color_); motion_.setEndValue(end); motion_.start();
}

void SmoothActionButton::settleMotion() {
    motion_.stop(); target_ = surfaceColor(); color_ = target_; update();
    feedback_.stop(); feedbackPosition_ = {isDown() && isEnabled() ? 0.95 : 1.0, 0};
    feedback_.setEndValue(feedbackPosition_);
    ripple_.stop(); rippleProgress_ = 1;
}

bool SmoothActionButton::event(QEvent *event) {
    const QPointer<SmoothActionButton> guard(this);
    const bool result = QPushButton::event(event);
    if (!guard) return result;
    if (event->type() == QEvent::MouseButtonPress && isDown() && motionAllowed(this) && qApp->property("miuixStyle").toBool()) {
        rippleOrigin_ = static_cast<QMouseEvent *>(event)->position();
        ripple_.stop(); ripple_.setStartValue(0.0); ripple_.setEndValue(1.0); ripple_.start();
    }
    switch (event->type()) {
    case QEvent::Enter: case QEvent::Leave:
    case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease:
    case QEvent::KeyPress: case QEvent::KeyRelease:
        updateSurface(true); break;
    case QEvent::Show: case QEvent::Hide: case QEvent::EnabledChange:
    case QEvent::PaletteChange: case QEvent::StyleChange:
        settleMotion(); break;
    default: break;
    }
    return result;
}

void SmoothActionButton::paintEvent(QPaintEvent *event) {
    if (!qApp->property("miuixStyle").toBool()) { QPushButton::paintEvent(event); return; }
    if (!color_.isValid()) settleMotion();
    QStyleOptionButton option; initStyleOption(&option);
    const auto t = miuixTokens(palette());
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(width() / 2.0, height() / 2.0 + feedbackPosition_.y());
    painter.scale(feedbackPosition_.x(), feedbackPosition_.x()); painter.translate(-width() / 2.0, -height() / 2.0);
    const auto path = continuousRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12);
    painter.fillPath(path, color_);
    if (objectName() == QStringLiteral("workspaceSearch")) {
        painter.setPen(QPen(underMouse() || hasFocus() ? t.primary : t.border, 1)); painter.setBrush(Qt::NoBrush); painter.drawPath(path);
    }
    if (rippleProgress_ < 1) {
        painter.save(); painter.setClipPath(path);
        QColor rippleColor = property("primaryAction").toBool() ? QColor(Qt::white) : t.text;
        rippleColor.setAlphaF((1 - rippleProgress_) * 0.14);
        painter.setPen(Qt::NoPen); painter.setBrush(rippleColor);
        const qreal radius = qSqrt(width() * width() + height() * height()) * rippleProgress_;
        painter.drawEllipse(rippleOrigin_, radius, radius); painter.restore();
    }
    if (hasFocus() && option.state.testFlag(QStyle::State_KeyboardFocusChange)) {
        painter.setPen(QPen(palette().highlight(), 1.5)); painter.setBrush(Qt::NoBrush); painter.drawPath(path);
    }
    const bool primary = property("primaryAction").toBool(), navigation = property("navigationItem").toBool();
    const QColor foreground = !isEnabled() ? primary ? t.onPrimaryDisabled : t.disabledText
        : primary ? QColor(Qt::white) : isChecked() ? t.primary : underMouse() ? t.text : t.secondaryText;
    option.palette.setColor(QPalette::ButtonText, foreground);
    const QString symbol = property("symbol").toString();
    if (!symbol.isEmpty()) option.icon = actionSymbol(symbol, foreground);
    else if (navigation && isChecked()) option.icon = icon();
    style()->drawControl(QStyle::CE_PushButtonLabel, &option, &painter, this);
}

void SmoothToolbar::paintEvent(QPaintEvent *event) {
    if (!qApp->property("miuixStyle").toBool()) { QWidget::paintEvent(event); return; }
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    const auto t = miuixTokens(palette());
    painter.fillRect(rect(), t.background);
    painter.setPen(t.divider); painter.drawLine(8, height() - 1, width() - 8, height() - 1);
}

}
