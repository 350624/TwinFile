#include "window.h"
#include "appearance.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace twinfile {
namespace {

class PathLabel : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setFont(font()); painter.setPen(palette().windowText().color());
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideMiddle, contentsRect().width()));
    }
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
    auto *shade = new QPushButton(centralWidget()); shade->setObjectName(QStringLiteral("mtDrawerShade")); shade->setFocusPolicy(Qt::NoFocus);
    shade->setAccessibleName(QStringLiteral("收起位置侧栏")); shade->hide();
    connect(shade, &QPushButton::clicked, this, [this] { sidebar_->hide(); positionMtDrawer(); });
    auto *drawer = new QPushButton; drawer->setObjectName(QStringLiteral("mtDrawer")); drawer->setProperty("symbol", QStringLiteral("menu"));
    drawer->setFixedSize(42, 44); drawer->setToolTip(QStringLiteral("位置与工具")); drawer->setAccessibleName(drawer->toolTip());
    connect(drawer, &QPushButton::clicked, this, [this] { sidebar_->setVisible(!sidebar_->isVisible()); positionMtDrawer(); }); heading->insertWidget(0, drawer); drawer->hide();
    auto *location = new QPushButton; location->setObjectName(QStringLiteral("mtLocation")); location->setToolTip(QStringLiteral("最近与书签"));
    location->setAccessibleName(location->toolTip()); location->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred); location->setMinimumWidth(0);
    auto *labels = new QVBoxLayout(location); labels->setContentsMargins(4, 0, 4, 0); labels->setSpacing(3);
    auto *path = new PathLabel; path->setObjectName(QStringLiteral("mtPath")); path->setMinimumWidth(0); path->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *statistics = new PathLabel; statistics->setObjectName(QStringLiteral("mtStatistics")); statistics->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    for (auto *label : {path, statistics}) { label->setAttribute(Qt::WA_TransparentForMouseEvents); labels->addWidget(label); }
    heading->insertWidget(1, location, 1); location->hide(); connect(location, &QPushButton::clicked, this, &Window::showMtPlaces);
    auto *more = new QPushButton; more->setObjectName(QStringLiteral("mtOverflow")); more->setProperty("symbol", QStringLiteral("more-vertical"));
    more->setFixedSize(38, 44); more->setToolTip(QStringLiteral("菜单")); more->setAccessibleName(more->toolTip());
    connect(more, &QPushButton::clicked, this, &Window::showMtMenu); heading->addWidget(more); more->hide();

    auto *bottom = new QWidget; bottom->setObjectName(QStringLiteral("mtBottomBar")); auto *navigation = new QHBoxLayout(bottom);
    navigation->setContentsMargins(16, 0, 16, 0); navigation->setSpacing(0); bottom->setFixedHeight(52);
    auto add = [&](const QString &symbol, const QString &tip, auto handler) {
        auto *button = new QToolButton; button->setObjectName(QStringLiteral("mt_") + symbol); button->setProperty("symbol", symbol);
        button->setToolTip(tip); button->setAccessibleName(tip); button->setIconSize(QSize(24, 24)); button->setFixedSize(64, 50);
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
    if (mtPlaces_) mtPlaces_->close();
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
    auto *shade = findChild<QWidget *>(QStringLiteral("mtDrawerShade"));
    if (!shade) return;
    if (!sidebar_->property("mtStyle").toBool()) { shade->hide(); return; }
    const int top = findChild<QWidget *>(QStringLiteral("workspaceHeader"))->geometry().bottom() + 1;
    const int bottom = findChild<QWidget *>(QStringLiteral("mtBottomBar"))->geometry().top();
    shade->setGeometry(0, top, centralWidget()->width(), qMax(0, bottom - top));
    shade->setVisible(sidebar_->isVisible()); shade->raise();
    sidebar_->setGeometry(0, top, 232, qMax(0, bottom - top)); sidebar_->raise();
}

void Window::styleNavigation() {
    const bool mt = settings_.value(QStringLiteral("theme")).toString() == QStringLiteral("mt");
    for (auto *button : sidebar_->findChildren<QPushButton *>()) {
        if (!button->property("navigationItem").toBool()) continue;
        if (!button->property("navigationOriginalIcon").isValid()) button->setProperty("navigationOriginalIcon", button->icon());
        const QString symbol = button->property("symbol").toString();
        const QIcon icon = symbol.isEmpty() ? qvariant_cast<QIcon>(button->property("navigationOriginalIcon")) : actionSymbol(symbol, qApp->palette().color(QPalette::Text));
        button->setIcon(mt ? navigationBadge(icon) : icon); button->setIconSize(QSize(mt ? 30 : 18, mt ? 30 : 18));
        if (!button->property("mtDrawerConnected").toBool()) {
            button->setProperty("mtDrawerConnected", true);
            connect(button, &QPushButton::clicked, this, [this] { if (sidebar_->property("mtStyle").toBool()) { sidebar_->hide(); positionMtDrawer(); } });
        }
    }
}

void Window::updateMtHeader() {
    if (!active_ || settings_.value(QStringLiteral("theme")).toString() != QStringLiteral("mt")) return;
    auto *path = findChild<QLabel *>(QStringLiteral("mtPath")); if (!path) return;
    const auto location = active_->location(); path->setText(location.display()); path->setToolTip(location.display());
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
    QMenu menu(this); menu.setObjectName(QStringLiteral("mtMenu"));
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
    auto *tools = menu.addMenu(actionSymbol(QStringLiteral("more"), palette().color(QPalette::Text)), QStringLiteral("更多工具")); populateMoreMenu(*tools);
    menu.addSeparator(); add(QStringLiteral("退出"), QStringLiteral("move"), [this] { close(); });
    auto *anchor = findChild<QWidget *>(QStringLiteral("mtOverflow")); menu.exec(anchor->mapToGlobal(QPoint(anchor->width(), anchor->height())));
}

void Window::createMtEntry() {
    const Location target = active_->location();
    QDialog dialog(this); dialog.setObjectName(QStringLiteral("mtCreate")); dialog.setWindowTitle(QStringLiteral("新建")); dialog.setMinimumWidth(430);
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
    sidebar_->hide(); positionMtDrawer();
    auto *overlay = new QPushButton(centralWidget()); overlay->setObjectName(QStringLiteral("mtPlacesShade")); overlay->setAttribute(Qt::WA_DeleteOnClose);
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
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), sheet); escape->setContext(Qt::WidgetWithChildrenShortcut); connect(escape, &QShortcut::activated, overlay, &QWidget::close);
    const QRect area = centralWidget()->rect(); const int width = qMin(650, area.width()), height = qMin(440, area.height() - 120);
    sheet->setGeometry((area.width() - width) / 2, area.height() - height, width, height); overlay->show(); overlay->raise(); sheet->show(); tabs->setFocus();
}

}
