#include "window.h"
#include "appearance.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QTouchEvent>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace twinfile {
namespace {

class MotionShade : public QPushButton {
public:
    MotionShade(QWidget *parent, std::function<bool()> motion)
        : QPushButton(parent), motion_(std::move(motion)) {
        animation_ = new QVariantAnimation(this); animation_->setObjectName(QStringLiteral("mtOverlayAnimation"));
        connect(animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) { progress_ = value.toReal(); render(); });
        connect(animation_, &QVariantAnimation::finished, this, [this] { if (closing_) finishClose(); });
    }
    bool closing() const { return closing_; }
    void bounds(const QRect &destination, const QRect &collapsed) {
        destination_ = destination; collapsed_ = collapsed; render();
    }
    void open(QWidget *panel, const QRect &destination, const QRect &collapsed) {
        panel_ = panel; bounds(destination, collapsed); closing_ = false;
        setProperty("mtClosing", false); panel->setEnabled(true); show(); raise(); panel->show(); panel->raise();
        animate(1);
    }
    void settle() {
        if (!isVisible()) return;
        animation_->stop();
        if (closing_) finishClose();
        else { progress_ = 1; render(); }
    }
    void finishClose() {
        animation_->stop(); closing_ = true; setProperty("mtClosing", true); progress_ = 0; render();
        if (panel_) { panel_->hide(); panel_->setEnabled(true); }
        hide();
        if (testAttribute(Qt::WA_DeleteOnClose)) deleteLater();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.fillRect(rect(), QColor(0, 0, 0, qRound(95 * progress_)));
    }
    void closeEvent(QCloseEvent *event) override {
        event->ignore();
        if (closing_) return;
        closing_ = true; setProperty("mtClosing", true);
        if (panel_) panel_->setEnabled(false);
        animate(0);
    }
private:
    void animate(qreal target) {
        animation_->stop();
        if (!motion_() || qFuzzyCompare(progress_ + 1, target + 1)) {
            progress_ = target; render(); if (closing_) finishClose(); return;
        }
        const qreal from = progress_;
        {
            const QSignalBlocker blocker(animation_);
            animation_->setDuration(qMax(1, qRound((target ? 240 : 200) * qAbs(target - from))));
            animation_->setEasingCurve(target ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
            animation_->setStartValue(from); animation_->setEndValue(target);
        }
        animation_->start();
    }
    void render() {
        setProperty("mtMotionProgress", progress_);
        if (panel_) {
            const QPoint delta = destination_.topLeft() - collapsed_.topLeft();
            panel_->setGeometry(QRect(collapsed_.topLeft() + QPoint(qRound(delta.x() * progress_), qRound(delta.y() * progress_)), destination_.size()));
        }
        update();
    }
    std::function<bool()> motion_;
    QVariantAnimation *animation_;
    QPointer<QWidget> panel_;
    QRect destination_, collapsed_;
    qreal progress_ = 0;
    bool closing_ = false;
};

class DialogShade : public QWidget {
public:
    explicit DialogShade(QWidget *parent) : QWidget(parent) {}
    void setProgress(qreal progress) { progress_ = progress; update(); }
protected:
    void paintEvent(QPaintEvent *) override { QPainter painter(this); painter.fillRect(rect(), QColor(0, 0, 0, qRound(95 * progress_))); }
private:
    qreal progress_ = 1;
};

class MotionDialog : public QDialog {
public:
    MotionDialog(QWidget *parent, std::function<bool()> motion)
        : QDialog(parent), motion_(std::move(motion)) {
        animation_ = new QVariantAnimation(this); animation_->setObjectName(QStringLiteral("mtDialogAnimation"));
        connect(animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            const qreal progress = value.toReal(); setWindowOpacity(progress);
            move(destination_ + QPoint(0, qRound((1 - progress) * 12)));
            if (shade_) {
                shade_->setGeometry(parentWidget()->rect()); shade_->setProgress(progress);
            }
        });
        connect(animation_, &QVariantAnimation::finished, this, [this] {
            if (closing_) QDialog::done(result_);
        });
    }
    ~MotionDialog() override { delete shade_; }
    void done(int result) override {
        if (closing_) return;
        animation_->stop(); closing_ = true; result_ = result;
        if (!motion_()) { QDialog::done(result); return; }
        setProperty("mtClosing", true); setEnabled(false);
        const qreal from = windowOpacity();
        {
            const QSignalBlocker blocker(animation_);
            animation_->setStartValue(from); animation_->setEndValue(0.0);
            animation_->setDuration(140); animation_->setEasingCurve(QEasingCurve::InCubic);
        }
        animation_->start();
    }
protected:
    void showEvent(QShowEvent *event) override {
        QDialog::showEvent(event); destination_ = pos();
        shade_ = new DialogShade(parentWidget()); shade_->setObjectName(QStringLiteral("mtDialogShade"));
        shade_->setGeometry(parentWidget()->rect()); shade_->show(); shade_->raise();
        if (!motion_()) return;
        setWindowOpacity(0);
        animation_->setDuration(180); animation_->setEasingCurve(QEasingCurve::OutCubic);
        animation_->setStartValue(0.0); animation_->setEndValue(1.0); animation_->start();
    }
private:
    std::function<bool()> motion_;
    QVariantAnimation *animation_;
    QPoint destination_;
    QPointer<DialogShade> shade_;
    int result_ = QDialog::Rejected;
    bool closing_ = false;
};

// A non-interactive snapshot lets QMenu release its popup grab immediately on dismissal.
class MenuFade : public QWidget {
public:
    MenuFade(QWidget *owner, const QPixmap &image, const QRect &geometry, qreal opacity)
        : QWidget(owner, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowTransparentForInput), image_(image), opacity_(opacity) {
        setObjectName(QStringLiteral("mtMenuFade")); setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents); setAttribute(Qt::WA_ShowWithoutActivating); setFocusPolicy(Qt::NoFocus);
        setGeometry(geometry);
        auto *animation = new QVariantAnimation(this); animation->setObjectName(QStringLiteral("mtMenuExitAnimation"));
        animation->setDuration(120); animation->setStartValue(opacity); animation->setEndValue(0.0);
        connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) { opacity_ = value.toReal(); update(); });
        connect(animation, &QVariantAnimation::finished, this, &QObject::deleteLater);
        show(); animation->start();
    }
protected:
    void paintEvent(QPaintEvent *) override { QPainter painter(this); painter.setOpacity(opacity_); painter.drawPixmap(rect(), image_); }
private:
    QPixmap image_;
    qreal opacity_;
};

class MotionMenu : public QMenu {
public:
    MotionMenu(QWidget *parent, std::function<bool()> motion) : QMenu(parent), motion_(std::move(motion)) {
        animation_ = new QPropertyAnimation(this, "windowOpacity", this); animation_->setObjectName(QStringLiteral("mtMenuAnimation"));
        animation_->setDuration(160); animation_->setEasingCurve(QEasingCurve::OutCubic);
    }
protected:
    void showEvent(QShowEvent *event) override {
        QMenu::showEvent(event);
        if (!motion_()) { setWindowOpacity(1); return; }
        setWindowOpacity(0); animation_->setStartValue(0.0); animation_->setEndValue(1.0); animation_->start();
    }
    void hideEvent(QHideEvent *event) override {
        animation_->stop();
        if (motion_() && windowOpacity() > 0) new MenuFade(parentWidget(), grab(), geometry(), windowOpacity());
        QMenu::hideEvent(event);
    }
private:
    std::function<bool()> motion_;
    QPropertyAnimation *animation_;
};

class PathLabel : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setFont(font()); painter.setPen(palette().windowText().color());
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideMiddle, contentsRect().width()));
    }
};

class SwipeHandler : public QObject {
public:
    SwipeHandler(QWidget *parent, bool upwards, std::function<void()> action)
        : QObject(parent), upwards_(upwards), action_(std::move(action)) { watch(parent); }
    void watch(QWidget *widget) { widget->setAttribute(Qt::WA_AcceptTouchEvents); widget->installEventFilter(this); }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        auto *widget = qobject_cast<QWidget *>(object);
        if (!widget) return false;
        if (event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse) tracking_ = false;
        if (!widget->isVisible()) return false;
        if (event->type() == QEvent::Wheel && upwards_) {
            const auto *wheel = static_cast<QWheelEvent *>(event);
            if (wheel->angleDelta().y() > 0 || wheel->pixelDelta().y() > 0) { action_(); return true; }
        }
        if (event->type() == QEvent::MouseButtonPress) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) { start_ = mouse->globalPosition(); tracking_ = true; swiped_ = false; }
        } else if (event->type() == QEvent::MouseMove && tracking_) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            swiped_ = swiped_ || matches(mouse->globalPosition());
            if (swiped_) { if (auto *button = qobject_cast<QAbstractButton *>(widget)) button->setDown(false); return true; }
        } else if (event->type() == QEvent::MouseButtonRelease && tracking_) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() != Qt::LeftButton) return false;
            tracking_ = false;
            if (swiped_ || matches(mouse->globalPosition())) {
                if (auto *button = qobject_cast<QAbstractButton *>(widget)) button->setDown(false);
                action_(); return true;
            }
        } else if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate || event->type() == QEvent::TouchEnd) {
            const auto *touch = static_cast<QTouchEvent *>(event);
            if (touch->points().size() != 1) { tracking_ = false; event->accept(); return true; }
            const auto &point = touch->points().first();
            if (event->type() == QEvent::TouchBegin) { start_ = point.globalPosition(); tracking_ = true; swiped_ = false; }
            if (tracking_) swiped_ = swiped_ || matches(point.globalPosition());
            if (event->type() == QEvent::TouchEnd && tracking_) {
                tracking_ = false;
                if (swiped_) action_();
                else if (auto *button = qobject_cast<QAbstractButton *>(widget); button && widget->rect().contains(point.position().toPoint())) button->click();
                else if (auto *tabs = qobject_cast<QTabBar *>(widget)) {
                    const int index = tabs->tabAt(point.position().toPoint()); if (index >= 0) tabs->setCurrentIndex(index);
                }
            }
            event->accept(); return true;
        } else if (event->type() == QEvent::TouchCancel) { tracking_ = false; event->accept(); return true; }
        return false;
    }
private:
    bool matches(const QPointF &point) const {
        const QPointF delta = point - start_;
        return (upwards_ ? -delta.y() : delta.y()) >= 28 && qAbs(delta.y()) > qAbs(delta.x()) * 1.3;
    }
    QPointF start_;
    bool upwards_, tracking_ = false, swiped_ = false;
    std::function<void()> action_;
};

QIcon navigationBadge(const QIcon &icon) {
    QPixmap glyph = icon.pixmap(40, 40);
    QPainter ink(&glyph); ink.setCompositionMode(QPainter::CompositionMode_SourceIn); ink.fillRect(glyph.rect(), Qt::white); ink.end();
    QPixmap badge(64, 64); badge.fill(Qt::transparent);
    QPainter painter(&badge); painter.setRenderHint(QPainter::Antialiasing); painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#303030"))); painter.drawEllipse(QRectF(1, 1, 62, 62));
    painter.drawPixmap(QRect(15, 15, 34, 34), glyph); return QIcon(badge);
}

}

void Window::buildMtChrome(QVBoxLayout *root, QHBoxLayout *heading) {
    auto *shade = new MotionShade(centralWidget(), [this] { return !settings_.value(QStringLiteral("reduced_motion")).toBool(); }); shade->setObjectName(QStringLiteral("mtDrawerShade")); shade->setFocusPolicy(Qt::NoFocus);
    shade->setAccessibleName(QStringLiteral("收起位置侧栏")); shade->hide();
    connect(shade, &QPushButton::clicked, shade, &QWidget::close);
    auto *drawer = new QPushButton; drawer->setObjectName(QStringLiteral("mtDrawer")); drawer->setProperty("symbol", QStringLiteral("menu"));
    drawer->setFixedSize(42, 44); drawer->setToolTip(QStringLiteral("位置与工具")); drawer->setAccessibleName(drawer->toolTip());
    connect(drawer, &QPushButton::clicked, this, [this, shade] { setMtDrawerVisible(!shade->isVisible() || shade->closing()); }); heading->insertWidget(0, drawer); drawer->hide();
    auto *location = new QWidget; location->setObjectName(QStringLiteral("mtLocation"));
    location->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred); location->setMinimumWidth(0);
    auto *labels = new QVBoxLayout(location); labels->setContentsMargins(4, 0, 4, 0); labels->setSpacing(3);
    auto *path = new QLineEdit; path->setObjectName(QStringLiteral("mtPath")); path->setMinimumWidth(0); path->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    path->setAccessibleName(QStringLiteral("当前路径，可输入地址跳转")); path->installEventFilter(this); labels->addWidget(path);
    auto *statistics = new PathLabel; statistics->setObjectName(QStringLiteral("mtStatistics")); statistics->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statistics->setAttribute(Qt::WA_TransparentForMouseEvents); labels->addWidget(statistics);
    heading->insertWidget(1, location, 1); location->hide();
    connect(path, &QLineEdit::returnPressed, this, [this, path] {
        if (!active_->navigatePath(path->text())) {
            path->setToolTip(QStringLiteral("目录不存在或路径无效")); statusBar()->showMessage(path->toolTip(), 5000); path->selectAll(); return;
        }
        path->clearFocus(); updateMtHeader(); active_->view()->setFocus();
    });
    connect(path, &QLineEdit::editingFinished, this, &Window::updateMtHeader);
    auto *more = new QPushButton; more->setObjectName(QStringLiteral("mtOverflow")); more->setProperty("symbol", QStringLiteral("more-vertical"));
    more->setFixedSize(38, 44); more->setToolTip(QStringLiteral("菜单")); more->setAccessibleName(more->toolTip());
    connect(more, &QPushButton::clicked, this, &Window::showMtMenu); heading->addWidget(more); more->hide();

    auto *bottom = new QWidget; bottom->setObjectName(QStringLiteral("mtBottomBar")); auto *navigation = new QHBoxLayout(bottom);
    navigation->setContentsMargins(16, 0, 16, 0); navigation->setSpacing(0); bottom->setFixedHeight(52);
    auto *swipe = new SwipeHandler(bottom, true, [this] { if (!mtPlaces_) showMtPlaces(); });
    auto add = [&](const QString &symbol, const QString &tip, auto handler) {
        auto *button = new QToolButton; button->setObjectName(QStringLiteral("mt_") + symbol); button->setProperty("symbol", symbol);
        button->setToolTip(tip); button->setAccessibleName(tip); button->setIconSize(QSize(24, 24)); button->setFixedSize(64, 50);
        swipe->watch(button);
        connect(button, &QToolButton::clicked, this, handler); navigation->addWidget(button); return button;
    };
    add(QStringLiteral("back"), QStringLiteral("后退"), [this] { active_->goBack(); }); navigation->addStretch();
    add(QStringLiteral("forward"), QStringLiteral("前进"), [this] { active_->goForward(); }); navigation->addStretch();
    add(QStringLiteral("add"), QStringLiteral("新建文件或文件夹"), &Window::createMtEntry); navigation->addStretch();
    add(QStringLiteral("swap"), QStringLiteral("交换左右窗口"), &Window::swapPanes); navigation->addStretch();
    add(QStringLiteral("up"), QStringLiteral("上一级"), [this] { active_->goUp(); });
    root->addWidget(bottom); bottom->hide();
}

void Window::applyMtChrome(bool enabled) {
    closeMtPlaces(false);
    setMtDrawerVisible(false, false);
    centralWidget()->layout()->setContentsMargins(0, 0, enabled ? 0 : 12, 0);
    auto *header = findChild<QWidget *>(QStringLiteral("workspaceHeader"));
    header->layout()->setContentsMargins(enabled ? 10 : 20, enabled ? 10 : 14, enabled ? 10 : 2, enabled ? 10 : 14);
    for (const QString &name : {QStringLiteral("mtDrawer"), QStringLiteral("mtLocation"), QStringLiteral("mtOverflow"), QStringLiteral("mtBottomBar")})
        findChild<QWidget *>(name)->setVisible(enabled);
    for (const QString &name : {QStringLiteral("workspaceHeading"), QStringLiteral("workspaceDetail")})
        findChild<QWidget *>(name)->setVisible(!enabled);
    auto *search = findChild<QPushButton *>(QStringLiteral("workspaceSearch"));
    search->setText(enabled ? QString() : QStringLiteral("搜索文件    Ctrl+F"));
    search->setMinimumWidth(enabled ? 0 : 156); search->setMaximumWidth(enabled ? 38 : QWIDGETSIZE_MAX);
    // Keep the path area flexible; the original header's spacer otherwise takes half its width.
    auto *heading = qobject_cast<QHBoxLayout *>(header->layout());
    heading->setStretch(heading->indexOf(findChild<QWidget *>(QStringLiteral("mtLocation"))), enabled ? 1 : 0);
    for (int i = 0; i < heading->count(); ++i) if (heading->itemAt(i)->spacerItem()) heading->setStretch(i, enabled ? 0 : 1);
    const bool entering = !sidebar_->property("mtStyle").toBool() && enabled;
    sidebar_->setProperty("mtStyle", enabled); sidebar_->setFixedWidth(enabled ? 232 : 184);
    if (entering) sidebar_->hide();
    if (!enabled) sidebar_->show();
    auto *body = qobject_cast<QHBoxLayout *>(centralWidget()->layout()->itemAt(1)->layout());
    if (body) {
        if (enabled) body->removeWidget(sidebar_);
        else if (body->indexOf(sidebar_) < 0) body->insertWidget(0, sidebar_);
        body->setSpacing(enabled ? 0 : 12);
    }
    findChild<QWidget *>(QStringLiteral("workspaceContent"))->layout()->setSpacing(enabled ? 0 : 12);
    for (auto *button : header->findChildren<QPushButton *>()) {
        const auto symbol = button->property("symbol").toString();
        if (!symbol.isEmpty()) button->setIcon(actionSymbol(symbol, enabled ? QColor(Qt::white) : qApp->palette().color(QPalette::Text)));
    }
    for (auto *button : findChild<QWidget *>(QStringLiteral("mtBottomBar"))->findChildren<QToolButton *>())
        button->setIconSize(QSize(24, 24));
    styleNavigation(); updateMtHeader(); centralWidget()->layout()->activate(); positionMtDrawer();
}

void Window::positionMtDrawer() {
    auto *shade = static_cast<MotionShade *>(findChild<QPushButton *>(QStringLiteral("mtDrawerShade")));
    if (!shade) return;
    if (!sidebar_->property("mtStyle").toBool()) { shade->hide(); return; }
    const int top = findChild<QWidget *>(QStringLiteral("workspaceHeader"))->geometry().bottom() + 1;
    const int bottom = findChild<QWidget *>(QStringLiteral("mtBottomBar"))->geometry().top();
    shade->setGeometry(0, top, centralWidget()->width(), qMax(0, bottom - top));
    const QRect destination(0, top, 232, qMax(0, bottom - top));
    shade->bounds(destination, destination.translated(-232, 0));
    if (shade->isVisible()) { shade->raise(); sidebar_->raise(); }
}

void Window::setMtDrawerVisible(bool visible, bool animated) {
    auto *shade = static_cast<MotionShade *>(findChild<QPushButton *>(QStringLiteral("mtDrawerShade")));
    if (!shade) return;
    if (!visible) { if (animated) shade->close(); else shade->finishClose(); return; }
    closeMtPlaces(false); positionMtDrawer();
    const int top = shade->geometry().top();
    const QRect destination(0, top, 232, shade->height());
    shade->open(sidebar_, destination, destination.translated(-232, 0));
    if (!animated) shade->settle();
}

void Window::closeMtPlaces(bool animated) {
    if (!mtPlaces_) return;
    auto *shade = static_cast<MotionShade *>(mtPlaces_.data());
    if (animated) shade->close(); else { mtPlaces_ = nullptr; shade->finishClose(); }
}

void Window::settleMtAnimations() {
    for (const auto &name : {QStringLiteral("mtDrawerShade"), QStringLiteral("mtPlacesShade")})
        if (auto *shade = static_cast<MotionShade *>(findChild<QPushButton *>(name))) shade->settle();
    for (auto *animation : findChildren<QVariantAnimation *>()) {
        if (animation->objectName() == QStringLiteral("mtDialogAnimation") || animation->objectName() == QStringLiteral("mtMenuAnimation") || animation->objectName() == QStringLiteral("mtMenuExitAnimation"))
            if (animation->state() == QAbstractAnimation::Running) animation->setCurrentTime(animation->duration());
    }
}

void Window::styleNavigation() {
    const bool mt = settings_.value(QStringLiteral("theme")).toString() == QStringLiteral("mt");
    const int iconSize = mt ? 30 : settings_.value(QStringLiteral("theme")).toString() == QStringLiteral("miuix") ? 20 : 18;
    for (auto *button : sidebar_->findChildren<QPushButton *>()) {
        if (!button->property("navigationItem").toBool()) continue;
        if (!button->property("navigationOriginalIcon").isValid()) button->setProperty("navigationOriginalIcon", button->icon());
        const QString symbol = button->property("symbol").toString();
        const QIcon icon = symbol.isEmpty() ? qvariant_cast<QIcon>(button->property("navigationOriginalIcon")) : actionSymbol(symbol, qApp->palette().color(QPalette::Text));
        button->setIcon(mt ? navigationBadge(icon) : icon); button->setIconSize(QSize(iconSize, iconSize));
        if (!button->property("mtDrawerConnected").toBool()) {
            button->setProperty("mtDrawerConnected", true);
            connect(button, &QPushButton::clicked, this, [this] { if (sidebar_->property("mtStyle").toBool()) setMtDrawerVisible(false); });
        }
    }
}

void Window::updateMtHeader() {
    if (!active_ || settings_.value(QStringLiteral("theme")).toString() != QStringLiteral("mt")) return;
    auto *path = findChild<QLineEdit *>(QStringLiteral("mtPath")); if (!path) return;
    const auto location = active_->location();
    if (!path->hasFocus()) { path->setText(location.display()); path->setToolTip(location.display()); }
    int folders = 0, files = 0;
    for (const Entry &entry : active_->fileModel()->entries()) entry.directory ? ++folders : ++files;
    QString detail = QStringLiteral("文件夹: %1   文件: %2").arg(folders).arg(files);
    for (const Entry &drive : navigationDrives_) if (location.source == Source::Local && location.path.startsWith(drive.path, Qt::CaseInsensitive) && !drive.detail.isEmpty()) { detail += QStringLiteral("   ") + drive.detail; break; }
    findChild<QLabel *>(QStringLiteral("mtStatistics"))->setText(detail);
    findChild<QToolButton *>(QStringLiteral("mt_back"))->setEnabled(active_->canGoBack());
    findChild<QToolButton *>(QStringLiteral("mt_forward"))->setEnabled(active_->canGoForward());
    findChild<QToolButton *>(QStringLiteral("mt_up"))->setEnabled(location.source != Source::Drive);
}

void Window::swapPanes() {
    auto *splitter = findChild<QSplitter *>(QStringLiteral("fileSplitter")); const QList<int> sizes = splitter->sizes();
    splitter->insertWidget(0, right_); std::swap(left_, right_); splitter->setSizes(sizes);
    setActive(active_); updateTransferButtons();
}

void Window::showMtMenu() {
    MotionMenu menu(this, [this] { return !settings_.value(QStringLiteral("reduced_motion")).toBool(); }); menu.setObjectName(QStringLiteral("mtMenu"));
    auto add = [&](const QString &text, const QString &symbol, auto handler) { return menu.addAction(actionSymbol(symbol, palette().color(QPalette::Text)), text, this, handler); };
    add(QStringLiteral("刷新"), QStringLiteral("refresh"), &Window::refreshActive);
    add(QStringLiteral("搜索"), QStringLiteral("search"), &Window::searchFiles);
    add(QStringLiteral("全选"), QStringLiteral("select-all"), [this] { active_->view()->selectAll(); active_->view()->setFocus(); });
    add(QStringLiteral("取消全选"), QStringLiteral("select-none"), [this] { active_->deselect(); });
    add(QStringLiteral("排序方式"), QStringLiteral("sort"), [this] { sortMenu(findChild<QWidget *>(QStringLiteral("mtOverflow"))); });
    auto *hidden = add(QStringLiteral("隐藏文件"), QStringLiteral("select-all"), [this] {
        settings_.insert(QStringLiteral("show_hidden"), !settings_.value(QStringLiteral("show_hidden")).toBool());
        for (Pane *pane : {left_, right_}) pane->setOptions(settings_.value(QStringLiteral("show_hidden")).toBool(), settings_.value(QStringLiteral("sort")).toString(QStringLiteral("name")), settings_.value(QStringLiteral("descending")).toBool());
    });
    hidden->setCheckable(true); hidden->setChecked(settings_.value(QStringLiteral("show_hidden")).toBool());
    add(QStringLiteral("添加书签"), QStringLiteral("bookmark"), [this] {
        const auto location = active_->location(); if (location.source != Source::Local) return;
        QJsonArray favorites = settings_.value(QStringLiteral("favorites")).toArray(); if (!favorites.contains(location.path)) favorites.append(location.path);
        settings_.insert(QStringLiteral("favorites"), favorites); statusBar()->showMessage(QStringLiteral("已添加书签"), 5000);
    })->setEnabled(active_->location().source == Source::Local);
    add(QStringLiteral("最近 / 书签"), QStringLiteral("bookmark"), &Window::showMtPlaces);
    add(QStringLiteral("交换窗口"), QStringLiteral("swap"), &Window::swapPanes);
    add(QStringLiteral("设置"), QStringLiteral("theme"), &Window::showAppearanceSettings);
    auto *tools = new MotionMenu(&menu, [this] { return !settings_.value(QStringLiteral("reduced_motion")).toBool(); });
    tools->setTitle(QStringLiteral("更多工具")); tools->setIcon(actionSymbol(QStringLiteral("more"), palette().color(QPalette::Text)));
    menu.addMenu(tools); populateMoreMenu(*tools);
    menu.addSeparator(); add(QStringLiteral("退出"), QStringLiteral("move"), [this] { close(); });
    auto *anchor = findChild<QWidget *>(QStringLiteral("mtOverflow")); menu.exec(anchor->mapToGlobal(QPoint(anchor->width(), anchor->height())));
}

void Window::createMtEntry() {
    const Location target = active_->location();
    MotionDialog dialog(this, [this] { return !settings_.value(QStringLiteral("reduced_motion")).toBool(); }); dialog.setObjectName(QStringLiteral("mtCreate")); dialog.setWindowTitle(QStringLiteral("新建")); dialog.setMinimumWidth(430);
    auto *layout = new QVBoxLayout(&dialog); layout->setContentsMargins(28, 24, 28, 20); layout->setSpacing(18);
    auto *title = new QLabel(QStringLiteral("新建")); title->setObjectName(QStringLiteral("mtCreateTitle")); layout->addWidget(title);
    auto *name = new QLineEdit; name->setObjectName(QStringLiteral("mtCreateName")); name->setAccessibleName(QStringLiteral("名称")); layout->addWidget(name);
    auto *error = new QLabel; error->setObjectName(QStringLiteral("subtle")); error->setWordWrap(true); error->hide(); layout->addWidget(error);
    auto *buttons = new QHBoxLayout; auto *cancel = new QPushButton(QStringLiteral("取消")); cancel->setAutoDefault(false); buttons->addWidget(cancel); buttons->addStretch();
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    bool directory = true;
    for (bool folder : {false, true}) {
        auto *button = new QPushButton(folder ? QStringLiteral("文件夹") : QStringLiteral("文件")); button->setObjectName(folder ? QStringLiteral("mtCreateFolder") : QStringLiteral("mtCreateFile")); button->setAutoDefault(false); button->setEnabled(false); buttons->addWidget(button);
        connect(name, &QLineEdit::textChanged, button, [button](const QString &text) { button->setEnabled(validateName(text).isEmpty()); });
        connect(button, &QPushButton::clicked, &dialog, [&, folder] { directory = folder; dialog.accept(); });
    }
    connect(name, &QLineEdit::returnPressed, &dialog, [&] {
        const auto invalid = validateName(name->text()); error->setText(invalid); error->setVisible(!invalid.isEmpty());
    });
    layout->addLayout(buttons); name->setFocus();
    if (dialog.exec() != QDialog::Accepted) return;
    const QString value = name->text(); if (!validateName(value).isEmpty()) return;
    runJob(QStringLiteral("新建"), [target, value, directory](const Cancellation &cancel) { return createEntry(target, value, directory, cancel); }, {}, {}, true);
}

void Window::showMtPlaces() {
    if (mtPlaces_) { mtPlaces_->close(); return; }
    setMtDrawerVisible(false, false);
    auto *overlay = new MotionShade(centralWidget(), [this] { return !settings_.value(QStringLiteral("reduced_motion")).toBool(); }); overlay->setObjectName(QStringLiteral("mtPlacesShade")); overlay->setAttribute(Qt::WA_DeleteOnClose);
    overlay->setGeometry(centralWidget()->rect()); overlay->setFocusPolicy(Qt::NoFocus); overlay->setAccessibleName(QStringLiteral("收起最近与书签"));
    connect(overlay, &QPushButton::clicked, overlay, &QWidget::close); mtPlaces_ = overlay;
    auto *sheet = new QFrame(overlay); sheet->setObjectName(QStringLiteral("mtPlaces"));
    auto *layout = new QVBoxLayout(sheet); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    auto *tabs = new QTabWidget; tabs->setObjectName(QStringLiteral("mtPlacesTabs")); layout->addWidget(tabs, 1);
    const QPointer<Pane> target = active_;
    auto list = [&](const QString &name, const QList<Location> &locations) {
        auto *view = new QListWidget; view->setObjectName(QStringLiteral("mtPlacesList")); view->setIconSize(QSize(36, 36)); view->setAccessibleName(name);
        for (const auto &location : locations) {
            QString label = location.source == Source::Local ? QFileInfo(location.path).fileName() : location.label;
            if (label.isEmpty()) label = location.display();
            Entry folder; folder.directory = true;
            auto *item = new QListWidgetItem(entryIcon(folder), label + QChar('\n') + location.display(), view); item->setData(Qt::UserRole, location.json()); item->setToolTip(location.display());
        }
        if (locations.isEmpty()) { auto *empty = new QListWidgetItem(QStringLiteral("暂无记录"), view); empty->setFlags(Qt::NoItemFlags); }
        connect(view, &QListWidget::itemClicked, sheet, [overlay, target](QListWidgetItem *item) {
            if (target && item->data(Qt::UserRole).isValid()) target->navigate(Location::fromJson(item->data(Qt::UserRole).toJsonObject()));
            overlay->close();
        });
        tabs->addTab(view, name);
    };
    list(QStringLiteral("最近"), recentLocations_);
    QList<Location> favorites; for (const auto &value : settings_.value(QStringLiteral("favorites")).toArray()) if (!value.toString().isEmpty()) favorites.append({Source::Local, value.toString(), {}, {}});
    list(QStringLiteral("书签"), favorites);
    auto *collapse = new QToolButton; collapse->setObjectName(QStringLiteral("mtPlacesClose")); collapse->setIcon(actionSymbol(QStringLiteral("down"), palette().color(QPalette::Text)));
    collapse->setToolTip(QStringLiteral("收起")); collapse->setAccessibleName(collapse->toolTip()); collapse->setFixedHeight(36); collapse->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(collapse); connect(collapse, &QToolButton::clicked, overlay, &QWidget::close);
    auto *swipe = new SwipeHandler(collapse, false, [overlay] { overlay->close(); }); swipe->watch(tabs->tabBar());
    const QRect area = centralWidget()->rect(); const int height = qMin(440, area.height() - 120);
    const QRect destination(0, area.height() - height, area.width(), height);
    overlay->open(sheet, destination, destination.translated(0, height)); tabs->setFocus();
}

}
