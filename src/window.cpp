#include "window.h"
#include "android.h"
#include "operations.h"
#include "payload.h"
#include "editor.h"
#include "dex.h"
#include "media.h"
#include "inspection.h"
#include "adbsync.h"
#include "shell.h"
#include "extensions.h"
#include "appearance.h"
#include "updates.h"
#include "mcp.h"
#include "download.h"
#include "task_progress.h"

#include <QApplication>
#include <QActionGroup>
#include <QButtonGroup>
#include <QAudioOutput>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QClipboard>
#include <QColorDialog>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMediaPlayer>
#include <QMenu>
#include <QMap>
#include <QNetworkInterface>
#include <QListWidget>
#include <QPointer>
#include <QPainter>
#include <QRegularExpression>
#include <QRubberBand>
#include <QMessageBox>
#include <QMovie>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QSignalBlocker>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringDecoder>
#include <QStyle>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QWheelEvent>
#include <QtConcurrent>
#include <mutex>

namespace twinfile {

static QStringList localPaths(const QList<Entry> &entries) {
    QStringList paths;
    for (const Entry &entry : entries) { if (entry.source != Source::Local) return {}; paths.append(entry.path); }
    return paths;
}

static QString readText(const QString &path, qint64 limit, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = file.errorString(); return {}; }
    if (file.size() > limit) { *error = QStringLiteral("文件超过读取上限：") + humanSize(limit); return {}; }
    const QByteArray bytes = file.readAll();
    TextDocument document;
    if (!decodeText(bytes, {}, &document).ok && !decodeText(bytes, QStringLiteral("GB18030"), &document).ok) { *error = QStringLiteral("无法无损解码文本；请在编辑器中选择正确编码"); return {}; }
    return document.text;
}

class ImageCanvas : public QGraphicsView {
public:
    using QGraphicsView::QGraphicsView;
    std::shared_ptr<QImage> artwork;
    QGraphicsPixmapItem *item = nullptr;
    bool brush = false;
    bool cropping = false;
    std::function<void(const QRect &)> crop;
    QColor color = Qt::red;
    int brushWidth = 8;
    std::function<void()> beforeStroke;
protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (cropping && event->button() == Qt::LeftButton) { origin_ = event->position().toPoint(); if (!band_) band_ = new QRubberBand(QRubberBand::Rectangle, viewport()); band_->setGeometry(QRect(origin_, QSize())); band_->show(); event->accept(); return; }
        if (brush && artwork && event->button() == Qt::LeftButton) { if (beforeStroke) beforeStroke(); drawing_ = true; previous_ = mapToScene(event->position().toPoint()); paint(previous_); event->accept(); return; } QGraphicsView::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override { if (cropping && band_ && band_->isVisible()) { band_->setGeometry(QRect(origin_, event->position().toPoint()).normalized()); event->accept(); return; } if (drawing_) { paint(mapToScene(event->position().toPoint())); event->accept(); return; } QGraphicsView::mouseMoveEvent(event); }
    void mouseReleaseEvent(QMouseEvent *event) override { if (cropping && band_ && band_->isVisible() && event->button() == Qt::LeftButton) { const QRect area = mapToScene(band_->geometry()).boundingRect().toAlignedRect().intersected(artwork->rect()); band_->hide(); cropping = false; if (area.isValid() && crop) crop(area); event->accept(); return; } if (drawing_ && event->button() == Qt::LeftButton) { paint(mapToScene(event->position().toPoint())); drawing_ = false; event->accept(); return; } QGraphicsView::mouseReleaseEvent(event); }
    void wheelEvent(QWheelEvent *event) override {
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        const qreal current = transform().m11(); const qreal factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
        if (current * factor > 0.01 && current * factor < 100) scale(factor, factor);
        event->accept();
    }
private:
    void paint(const QPointF &point) { QPainter painter(artwork.get()); painter.setRenderHint(QPainter::Antialiasing); painter.setPen(QPen(color, brushWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); painter.drawLine(previous_, point); painter.end(); previous_ = point; if (item) item->setPixmap(QPixmap::fromImage(*artwork)); }
    bool drawing_ = false;
    QPointF previous_;
    QPoint origin_;
    QRubberBand *band_ = nullptr;
};

class ImageDialog : public QDialog {
public:
    using QDialog::QDialog;
    std::function<bool()> allowClose;
    void reject() override { if (!allowClose || allowClose()) QDialog::reject(); }
protected:
    void closeEvent(QCloseEvent *event) override { event->ignore(); reject(); }
};

Window::Window(QWidget *parent, const QUrl &updateTestEndpoint) : QMainWindow(parent) {
    settings_ = readSettings(settingsPath());
    normalizeAppearance(settings_);
    setWindowTitle(QStringLiteral("TwinFile")); setWindowIcon(QIcon(QStringLiteral(":/app.ico"))); resize(1520, 900); setMinimumSize(1100, 650);
    auto *central = new QWidget; central->setObjectName(QStringLiteral("workspace")); setCentralWidget(central); central->installEventFilter(this);
    auto *root = new QVBoxLayout(central); root->setContentsMargins(0, 0, 12, 0); root->setSpacing(0);
    auto *heading = new QWidget; heading->setObjectName(QStringLiteral("workspaceHeader"));
    auto *headingLayout = new QHBoxLayout(heading); headingLayout->setContentsMargins(20, 14, 2, 14); headingLayout->setSpacing(12);
    auto *headingTitle = new QLabel(QStringLiteral("文件")); headingTitle->setObjectName(QStringLiteral("workspaceHeading")); headingLayout->addWidget(headingTitle);
    auto *headingDetail = new QLabel(QStringLiteral("工作空间")); headingDetail->setObjectName(QStringLiteral("workspaceDetail")); headingLayout->addWidget(headingDetail); headingLayout->addStretch();
    auto *search = new QPushButton(QStringLiteral("搜索文件    Ctrl+F")); search->setObjectName(QStringLiteral("workspaceSearch")); search->setProperty("symbol", QStringLiteral("search")); search->setAccessibleName(QStringLiteral("搜索当前栏文件")); search->setCursor(Qt::PointingHandCursor); connect(search, &QPushButton::clicked, this, &Window::searchFiles); headingLayout->addWidget(search);
    auto *appearance = new QPushButton; appearance->setObjectName(QStringLiteral("appearanceButton")); appearance->setProperty("symbol", QStringLiteral("theme")); appearance->setToolTip(QStringLiteral("外观与明暗模式")); appearance->setAccessibleName(appearance->toolTip()); appearance->setFixedSize(34, 34); connect(appearance, &QPushButton::clicked, this, &Window::showAppearanceSettings); headingLayout->addWidget(appearance);
    root->addWidget(heading);
    auto *body = new QHBoxLayout; body->setContentsMargins(0, 0, 0, 0); body->setSpacing(12); sidebar_ = new QWidget; sidebar_->setObjectName(QStringLiteral("navigationSidebar")); sidebar_->setFixedWidth(184); sidebar_->installEventFilter(this); buildSidebar(sidebar_); body->addWidget(sidebar_);
    auto *content = new QWidget; content->setObjectName(QStringLiteral("workspaceContent")); auto *contentLayout = new QVBoxLayout(content); contentLayout->setContentsMargins(0, 0, 0, 0); contentLayout->setSpacing(12);
    auto *toolbarWidget = new QWidget; toolbarWidget->setObjectName(QStringLiteral("workspaceToolbar")); toolbarWidget->installEventFilter(this); auto *toolbar = new QHBoxLayout(toolbarWidget); toolbar->setContentsMargins(4, 0, 4, 0); toolbar->setSpacing(4);
    auto addButton = [&](const QString &text, const QString &symbol, const QString &tip, auto handler) {
        auto *button = new QPushButton(text); button->setProperty("symbol", symbol); button->setProperty("toolbarText", text); button->setObjectName(QStringLiteral("action_") + symbol); button->setToolTip(tip); button->setAccessibleName(tip); button->setCursor(Qt::PointingHandCursor); connect(button, &QPushButton::clicked, this, handler); toolbar->addWidget(button); return button;
    };
    auto *createButton = addButton(QStringLiteral("新建"), QStringLiteral("add"), QStringLiteral("新建文件夹 F7"), [this] { create(true); }); createButton->setProperty("primaryAction", true);
    addButton(QStringLiteral("复制"), QStringLiteral("copy"), QStringLiteral("复制到另一栏 F5"), [this] { transfer(false); });
    addButton(QStringLiteral("移动"), QStringLiteral("move"), QStringLiteral("移动到另一栏 F6"), [this] { transfer(true); });
    addButton(QStringLiteral("删除"), QStringLiteral("delete"), QStringLiteral("删除所选项目 Delete"), &Window::remove);
    taskProgress_ = new TaskProgress(toolbarWidget); toolbar->addWidget(taskProgress_, 1);
    toolbar->addStretch();
    auto *sort = addButton(QStringLiteral("排序"), QStringLiteral("sort"), QStringLiteral("排序与隐藏文件"), [] {}); connect(sort, &QPushButton::clicked, this, [this, sort] { sortMenu(sort); });
    auto *assistant = addButton(QStringLiteral("助手"), QStringLiteral("assistant"), QStringLiteral("AI / 扩展 Ctrl+Shift+A"), [this] { extensionDock_->setVisible(!extensionDock_->isVisible()); }); assistant->setObjectName(QStringLiteral("assistantToggle")); assistant->setCheckable(true);
    addButton(QString(), QStringLiteral("more"), QStringLiteral("更多工具"), &Window::moreMenu)->setFixedWidth(36);
    contentLayout->addWidget(toolbarWidget);
    auto *splitter = new QSplitter; splitter->setObjectName(QStringLiteral("fileSplitter")); splitter->setChildrenCollapsible(false); left_ = new Pane; right_ = new Pane; splitter->addWidget(left_); splitter->addWidget(right_); contentLayout->addWidget(splitter, 1); body->addWidget(content, 1); root->addLayout(body, 1);
    for (Pane *pane : {left_, right_}) {
        pane->setAnimationsEnabled(!settings_.value(QStringLiteral("reduced_motion")).toBool());
        connect(pane, &Pane::activated, this, &Window::setActive);
        connect(pane, &Pane::openRequested, this, &Window::open);
        connect(pane, &Pane::previewRequested, this, &Window::preview);
        connect(pane, &Pane::menuRequested, this, &Window::menu);
        connect(pane, &Pane::locationChanged, this, &Window::updateNavigation);
    }
    buildActions(); setActive(left_);
    const QJsonObject workspace = settings_.value(QStringLiteral("workspace")).toObject();
    if (!workspace.isEmpty()) { left_->restore(workspace.value(QStringLiteral("left")).toObject()); right_->restore(workspace.value(QStringLiteral("right")).toObject()); }
    else left_->navigate({Source::Local, QDir::homePath(), {}, {}});
    if (workspace.value(QStringLiteral("active")).toString() == QStringLiteral("right")) setActive(right_);
    setTheme(settings_.value(QStringLiteral("theme")).toString(QStringLiteral("dark")));
    const QString geometry = settings_.value(QStringLiteral("geometry")).toString(); if (geometry.size() <= 65536) restoreGeometry(QByteArray::fromHex(geometry.toLatin1()));
    for (Pane *pane : {left_, right_}) pane->setOptions(settings_.value(QStringLiteral("show_hidden")).toBool(), settings_.value(QStringLiteral("sort")).toString(QStringLiteral("name")), settings_.value(QStringLiteral("descending")).toBool());
    monitor_ = new DeviceMonitor(this);
    connect(monitor_, &DeviceMonitor::changed, left_, &Pane::setDevices); connect(monitor_, &DeviceMonitor::changed, right_, &Pane::setDevices);
    connect(monitor_, &DeviceMonitor::changed, this, [this](const QList<Entry> &entries) { navigationPhones_ = entries; updateDeviceNavigation(); });
    connect(monitor_, &DeviceMonitor::status, this, [this](const QString &message) { statusBar()->showMessage(message, 8000); });
    QTimer::singleShot(0, monitor_, &DeviceMonitor::start);
    storageMonitor_ = new StorageMonitor(this);
    connect(storageMonitor_, &StorageMonitor::changed, this, [this](const QList<Entry> &entries) { navigationDrives_ = entries; updateDeviceNavigation(); });
    QTimer::singleShot(0, storageMonitor_, &StorageMonitor::start);
    statusBar()->showMessage(QStringLiteral("就绪"));
    auto *statusHint = new QLabel(QStringLiteral("空格预览   ·   F2 刷新   ·   F5 复制   ·   F6 移动")); statusHint->setObjectName(QStringLiteral("workspaceShortcuts")); statusBar()->addPermanentWidget(statusHint);
    buildExtensions();
    mcp_ = new McpServer(this);
    connect(mcp_, &McpServer::filesChanged, this, [this] { left_->refresh(); right_->refresh(); });
    updates_ = new UpdateChecker(QStringLiteral(TWINFILE_VERSION), this, updateTestEndpoint);
    connect(updates_, &UpdateChecker::stateChanged, this, [this] {
        if (!closing_ && updates_->state() == UpdateChecker::State::Available) showUpdateSettings(false);
    });
    if (!qEnvironmentVariableIsSet("TWINFILE_TEST_MODE") || !updateTestEndpoint.isEmpty()) updates_->scheduleStartup(settings_.value(QStringLiteral("check_updates_on_startup")).toBool(true));
}

void Window::buildExtensions() {
    extensionDock_ = new QDockWidget(QStringLiteral("AI / 扩展"), this);
    extensionDock_->setObjectName(QStringLiteral("extensionDock"));
    extensionDock_->setAllowedAreas(Qt::RightDockWidgetArea);
    extensionDock_->setFeatures(QDockWidget::DockWidgetClosable);
    auto *dockTitle = new QWidget(extensionDock_); dockTitle->setFixedHeight(0); extensionDock_->setTitleBarWidget(dockTitle);
    extensions_ = new ExtensionSidebar(QFileInfo(settingsPath()).dir().filePath(QStringLiteral("extensions")), extensionDock_);
    connect(extensions_, &ExtensionSidebar::closeRequested, extensionDock_, &QWidget::hide);
    extensions_->contextProvider = [this] {
        QJsonArray selected;
        const auto entries = active_ ? active_->selected() : QList<Entry>();
        for (const auto &entry : entries.mid(0, 256)) {
            selected.append(QJsonObject{{QStringLiteral("name"), entry.name}, {QStringLiteral("path"), entry.path},
                                        {QStringLiteral("inner"), entry.inner}, {QStringLiteral("source"), int(entry.source)},
                                        {QStringLiteral("directory"), entry.directory}, {QStringLiteral("size"), double(entry.size)}});
        }
        return QJsonObject{{QStringLiteral("location"), active_ ? active_->location().json() : QJsonObject()},
                           {QStringLiteral("selected"), selected}, {QStringLiteral("truncated"), entries.size() > 256}};
    };
    extensions_->refresh = [this] { refreshActive(); };
    extensions_->notify = [this](const QString &message) { statusBar()->showMessage(message, 8000); };
    extensions_->manager()->restore();
    extensionDock_->setWidget(extensions_); addDockWidget(Qt::RightDockWidgetArea, extensionDock_);
    if (auto *toggle = findChild<QPushButton *>(QStringLiteral("assistantToggle"))) {
        connect(extensionDock_, &QDockWidget::visibilityChanged, toggle, &QPushButton::setChecked);
        toggle->setChecked(settings_.value(QStringLiteral("extension_visible")).toBool(true));
    }
    resizeDocks({extensionDock_}, {qBound(330, settings_.value(QStringLiteral("extension_width")).toInt(370), 700)}, Qt::Horizontal);
    extensionDock_->setVisible(settings_.value(QStringLiteral("extension_visible")).toBool(true));
    auto *shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")), this);
    connect(shortcut, &QShortcut::activated, this, [this] { extensionDock_->setVisible(!extensionDock_->isVisible()); });
}

void Window::buildSidebar(QWidget *sidebar) {
    auto *layout = new QVBoxLayout(sidebar); layout->setContentsMargins(14, 12, 12, 14); layout->setSpacing(4);
    auto *brand = new QHBoxLayout; brand->setContentsMargins(8, 0, 0, 0); brand->setSpacing(9); auto *logo = new QLabel; logo->setPixmap(QIcon(QStringLiteral(":/app.ico")).pixmap(26, 26)); brand->addWidget(logo);
    auto *title = new QLabel(QStringLiteral("TwinFile")); title->setObjectName(QStringLiteral("brandTitle")); brand->addWidget(title, 1); layout->addLayout(brand);
    auto *navigation = new QWidget; navigation->setObjectName(QStringLiteral("navigationLocations")); navigation->installEventFilter(this);
    auto *locationsLayout = new QVBoxLayout(navigation); locationsLayout->setContentsMargins(0, 0, 0, 0); locationsLayout->setSpacing(4);
    auto *scroll = new QScrollArea; scroll->setObjectName(QStringLiteral("navigationScroll")); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true); scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); scroll->setWidget(navigation); layout->addWidget(scroll, 1);
    auto addLocation = [&](const QString &label, const QString &path) {
        auto *button = new QPushButton; button->setText(button->fontMetrics().elidedText(label, Qt::ElideMiddle, 104)); button->setAccessibleName(label); button->setProperty("navigationItem", true); button->setProperty("navigationPath", path); button->setCheckable(true); button->setIcon(sidebarSymbol(label)); button->setIconSize(QSize(18, 18)); button->setMinimumHeight(36); button->setFlat(true); button->setCursor(Qt::PointingHandCursor); button->setToolTip(path.isEmpty() ? QStringLiteral("驱动器与已连接设备") : QDir::toNativeSeparators(path));
        connect(button, &QPushButton::clicked, this, [this, path] { active_->navigate(path.isEmpty() ? Location() : Location{Source::Local, path, {}, {}}); }); locationsLayout->addWidget(button);
    };
    auto addSection = [&](const QString &text) { auto *label = new QLabel(text); label->setObjectName(QStringLiteral("navigationSection")); locationsLayout->addWidget(label); };
    locationsLayout->addSpacing(22); addSection(QStringLiteral("位置"));
    addLocation(QStringLiteral("此电脑"), {});
    const QList<QPair<QString, QStandardPaths::StandardLocation>> locations{
        {QStringLiteral("桌面"), QStandardPaths::DesktopLocation}, {QStringLiteral("下载"), QStandardPaths::DownloadLocation},
        {QStringLiteral("文档"), QStandardPaths::DocumentsLocation}, {QStringLiteral("图片"), QStandardPaths::PicturesLocation},
        {QStringLiteral("音乐"), QStandardPaths::MusicLocation}, {QStringLiteral("视频"), QStandardPaths::MoviesLocation}};
    for (const auto &location : locations) addLocation(location.first, QStandardPaths::writableLocation(location.second));
    locationsLayout->addSpacing(20); addSection(QStringLiteral("设备与磁盘"));
    deviceNavigation_ = new QWidget; deviceNavigation_->setObjectName(QStringLiteral("navigationDevices")); deviceNavigation_->installEventFilter(this);
    auto *devicesLayout = new QVBoxLayout(deviceNavigation_); devicesLayout->setContentsMargins(0, 0, 0, 0); devicesLayout->setSpacing(4); locationsLayout->addWidget(deviceNavigation_);
    updateDeviceNavigation();
    locationsLayout->addSpacing(20); addSection(QStringLiteral("收藏夹"));
    for (const QJsonValue &value : settings_.value(QStringLiteral("favorites")).toArray()) {
        const QString path = value.toString(); if (QFileInfo(path).isDir()) addLocation(QFileInfo(path).fileName(), path);
    }
    locationsLayout->addStretch();
    auto *tools = new QPushButton(QStringLiteral("扩展与工具")); tools->setFlat(true); tools->setProperty("navigationItem", true); tools->setIcon(actionSymbol(QStringLiteral("assistant"), palette().color(QPalette::PlaceholderText))); tools->setProperty("symbol", QStringLiteral("assistant")); tools->setCursor(Qt::PointingHandCursor); connect(tools, &QPushButton::clicked, this, [this] { extensionDock_->show(); extensions_->showExtensions(); }); layout->addWidget(tools);
}

void Window::updateNavigation() {
    if (!active_) return;
    for (auto *button : sidebar_->findChildren<QPushButton *>()) {
        const auto location = active_->location();
        if (button->property("navigationDevice").isValid()) {
            button->setChecked(location.source == Source::Phone && location.path == button->property("navigationDevice").toString());
            continue;
        }
        if (button->property("navigationDrive").isValid()) {
            const QString root = QDir::cleanPath(button->property("navigationDrive").toString());
            const QString path = QDir::cleanPath(location.path);
            button->setChecked(location.source == Source::Local && (path.compare(root, Qt::CaseInsensitive) == 0 || path.startsWith(root.endsWith('/') ? root : root + '/', Qt::CaseInsensitive)));
            continue;
        }
        if (!button->property("navigationPath").isValid()) continue;
        const QString path = button->property("navigationPath").toString();
        button->setChecked(path.isEmpty() ? location.source == Source::Drive : location.source == Source::Local && QDir::cleanPath(location.path).compare(QDir::cleanPath(path), Qt::CaseInsensitive) == 0);
    }
}

void Window::updateDeviceNavigation() {
    QStringList signature{QStringLiteral("devices")};
    for (const auto &entries : {navigationPhones_, navigationDrives_}) {
        for (const Entry &entry : entries) signature << QString::number(int(entry.source)) << entry.path << entry.serial << entry.name << entry.state << entry.detail;
        signature << QStringLiteral("|");
    }
    if (signature == navigationSignature_) return;
    navigationSignature_ = signature;
    auto *scroll = sidebar_->findChild<QScrollArea *>(QStringLiteral("navigationScroll"));
    const int offset = scroll->verticalScrollBar()->value();
    auto *layout = deviceNavigation_->layout();
    while (auto *item = layout->takeAt(0)) { delete item->widget(); delete item; }
    auto add = [&](const QString &label, const QString &symbol, const QString &tip) {
        auto *button = new QPushButton; button->setProperty("navigationItem", true); button->setProperty("symbol", symbol);
        button->setText(button->fontMetrics().elidedText(label, Qt::ElideMiddle, 104)); button->setAccessibleName(label); button->setToolTip(tip);
        button->setIcon(actionSymbol(symbol, qApp->palette().color(QPalette::Text))); button->setIconSize(QSize(18, 18)); button->setMinimumHeight(36); button->setFlat(true); button->setCheckable(true); button->setCursor(Qt::PointingHandCursor); layout->addWidget(button); return button;
    };
    if (navigationPhones_.isEmpty()) {
        auto *button = add(QStringLiteral("手机 · 未连接"), QStringLiteral("phone"), QStringLiteral("连接手机并开启 USB 调试，点击重新检测"));
        button->setObjectName(QStringLiteral("navigationPhoneDetect")); button->setCheckable(false);
        connect(button, &QPushButton::clicked, this, [this] { if (monitor_) monitor_->detect(); statusBar()->showMessage(QStringLiteral("正在检测手机，请允许 USB 调试"), 5000); });
    }
    for (const Entry &entry : navigationPhones_) {
        const QString label = entry.name + (entry.state == QStringLiteral("device") ? QString() : entry.state == QStringLiteral("unauthorized") ? QStringLiteral(" · 未授权") : QStringLiteral(" · 离线"));
        auto *button = add(label, QStringLiteral("phone"), entry.name + '\n' + entry.serial + '\n' + entry.detail);
        button->setProperty("navigationDevice", entry.serial);
        connect(button, &QPushButton::clicked, this, [this, entry] {
            if (entry.state == QStringLiteral("device")) active_->navigate({Source::Phone, entry.serial, QStringLiteral("/sdcard"), entry.name});
            else { statusBar()->showMessage(entry.detail, 8000); updateNavigation(); if (monitor_) monitor_->detect(); }
        });
    }
    for (const Entry &entry : navigationDrives_) {
        auto *button = add(entry.name, entry.state == QStringLiteral("usb") ? QStringLiteral("usb") : QStringLiteral("drive"), entry.name + '\n' + QDir::toNativeSeparators(entry.path) + (entry.detail.isEmpty() ? QString() : '\n' + entry.detail));
        button->setProperty("navigationDrive", entry.path);
        connect(button, &QPushButton::clicked, this, [this, entry] { active_->navigate({Source::Local, entry.path, {}, {}}); });
    }
    updateNavigation();
    if (auto *navigationLayout = deviceNavigation_->parentWidget()->layout()) navigationLayout->activate();
    scroll->verticalScrollBar()->setValue(offset);
}

void Window::setActive(Pane *pane) {
    if (!pane) return;
    const bool changed = active_ != pane;
    active_ = pane;
    updateNavigation();
    for (Pane *item : {left_, right_}) {
        item->setProperty("active", item == pane); item->style()->unpolish(item); item->style()->polish(item);
        if (auto *indicator = item->findChild<QLabel *>(QStringLiteral("paneIndicator"))) { indicator->style()->unpolish(indicator); indicator->style()->polish(indicator); indicator->update(); }
    }
    if (changed && !settings_.value(QStringLiteral("reduced_motion")).toBool()) {
        auto *effect = qobject_cast<QGraphicsOpacityEffect *>(pane->graphicsEffect());
        if (!effect) { effect = new QGraphicsOpacityEffect(pane); pane->setGraphicsEffect(effect); }
        if (auto *previous = effect->findChild<QPropertyAnimation *>()) previous->stop();
        auto *animation = new QPropertyAnimation(effect, "opacity", effect); animation->setDuration(120); animation->setStartValue(0.88); animation->setEndValue(1.0); animation->start(QAbstractAnimation::DeleteWhenStopped);
    }
}
Pane *Window::otherPane() const { return active_ == left_ ? right_ : left_; }
void Window::refreshActive() { active_->refresh(); }
void Window::report(const QString &message) { QMessageBox::warning(this, QStringLiteral("TwinFile"), message); }

void Window::setReducedMotion(bool enabled) {
    settings_.insert(QStringLiteral("reduced_motion"), enabled);
    for (Pane *pane : {left_, right_}) {
        pane->setAnimationsEnabled(!enabled);
        if (enabled) pane->setGraphicsEffect(nullptr);
    }
}

void Window::setTheme(const QString &theme) {
    settings_.insert(QStringLiteral("theme"), theme);
    normalizeAppearance(settings_);
    const QString current = settings_.value(QStringLiteral("theme")).toString();
    const bool dark = settings_.value(QStringLiteral("color_mode")).toString() == QStringLiteral("dark");
    const bool mac = current == QStringLiteral("macos");
    qApp->setProperty("macosStyle", mac);
    const QPalette palette = appearancePalette(current, dark, settings_.value(QStringLiteral("accent")).toString());
    qApp->setPalette(palette);
    qApp->setStyleSheet(appearanceStyleSheet(current, dark, palette));
    findChild<QSplitter *>(QStringLiteral("fileSplitter"))->setHandleWidth(12);
    findChild<QWidget *>(QStringLiteral("workspaceHeader"))->show();
    sidebar_->setFixedWidth(184);
    for (auto *button : findChildren<QAbstractButton *>()) {
        const QString symbol = button->property("symbol").toString();
        if (!symbol.isEmpty()) { button->setIcon(actionSymbol(symbol, palette.color(QPalette::Text))); button->setIconSize(QSize(18, 18)); }
    }
    for (Pane *pane : {left_, right_}) {
        pane->layout()->setContentsMargins(10, 10, 10, 0);
        pane->layout()->setSpacing(6);
        pane->view()->stopTransition(); pane->view()->doItemsLayout(); pane->view()->viewport()->update();
    }
}

void Window::setColorMode(const QString &mode) {
    if (mode != QStringLiteral("dark") && mode != QStringLiteral("light")) return;
    settings_.insert(QStringLiteral("color_mode"), mode);
    setTheme(settings_.value(QStringLiteral("theme")).toString());
}

void Window::showAppearanceSettings() {
    QDialog dialog(this); dialog.setObjectName(QStringLiteral("appearanceSettings")); dialog.setWindowTitle(QStringLiteral("外观")); dialog.resize(510, 420);
    auto *layout = new QVBoxLayout(&dialog); layout->setContentsMargins(22, 20, 22, 20); layout->setSpacing(14);
    auto *title = new QLabel(QStringLiteral("选择你的工作空间风格")); title->setObjectName(QStringLiteral("appearanceHeading")); layout->addWidget(title);
    auto *description = new QLabel(QStringLiteral("每套主题都支持亮色与暗色，切换立即生效。")); description->setObjectName(QStringLiteral("subtle")); layout->addWidget(description);
    auto *themes = new QComboBox; themes->setObjectName(QStringLiteral("appearanceTheme"));
    for (const QString &theme : themeIds()) themes->addItem(themeName(theme), theme);
    themes->setCurrentIndex(themeIds().indexOf(settings_.value(QStringLiteral("theme")).toString())); layout->addWidget(themes);
    auto *detail = new QLabel(QStringLiteral("macOS 风格：Finder 式侧栏、工作空间标题、紧凑文件行、分段标签和统一控件。保留 Windows 原生标题栏与窗口操作，不伪装系统毛玻璃。")); detail->setWordWrap(true); detail->setObjectName(QStringLiteral("subtle")); layout->addWidget(detail);
    layout->addWidget(new QLabel(QStringLiteral("显示模式")));
    auto *modes = new QHBoxLayout; auto *modeGroup = new QButtonGroup(&dialog); modeGroup->setExclusive(true);
    for (const auto &mode : {QStringLiteral("light"), QStringLiteral("dark")}) {
        auto *button = new QPushButton(mode == QStringLiteral("light") ? QStringLiteral("☀  亮色") : QStringLiteral("☾  暗色")); button->setObjectName(QStringLiteral("appearanceMode_") + mode); button->setAutoDefault(false); button->setCheckable(true); button->setMinimumHeight(38); button->setChecked(settings_.value(QStringLiteral("color_mode")).toString() == mode); modeGroup->addButton(button); modes->addWidget(button);
        connect(button, &QPushButton::clicked, &dialog, [this, mode] { setColorMode(mode); });
    }
    layout->addLayout(modes);
    auto *motion = new QCheckBox(QStringLiteral("减少动画")); motion->setChecked(settings_.value(QStringLiteral("reduced_motion")).toBool()); layout->addWidget(motion); connect(motion, &QCheckBox::toggled, this, &Window::setReducedMotion);
    auto *updates = new QPushButton(QStringLiteral("软件更新…")); updates->setObjectName(QStringLiteral("appearanceUpdates")); updates->setAutoDefault(false); layout->addWidget(updates);
    connect(updates, &QPushButton::clicked, this, [this] { showUpdateSettings(); });
    connect(themes, &QComboBox::currentIndexChanged, &dialog, [this, themes] { setTheme(themes->currentData().toString()); });
    layout->addStretch(); auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("完成")); layout->addWidget(buttons); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec(); saveSettings();
}

void Window::showUpdateSettings(bool checkNow) {
    if (!updateDialog_) {
        updateDialog_ = new UpdateDialog(updates_, settings_.value(QStringLiteral("check_updates_on_startup")).toBool(true), this);
        updateDialog_->setAttribute(Qt::WA_DeleteOnClose);
        connect(updateDialog_, &UpdateDialog::startupChanged, this, [this](bool enabled) {
            settings_.insert(QStringLiteral("check_updates_on_startup"), enabled); updates_->setStartupEnabled(enabled); saveSettings();
        });
    }
    updateDialog_->show(); updateDialog_->raise(); updateDialog_->activateWindow();
    if (checkNow) updates_->check();
}

void Window::showMcpSettings() {
    showMcpDialog(mcp_, McpSettings::fromJson(settings_.value(QStringLiteral("mcp")).toObject()), this, [this](const McpSettings &value) {
        settings_.insert(QStringLiteral("mcp"), value.json());
        saveSettings();
    });
}

void Window::showDownload() {
    if (taskProgress_->active()) { statusBar()->showMessage(QStringLiteral("当前任务正在顶部进度栏运行"), 5000); return; }
    auto *dialog = findChild<DownloadDialog *>(QStringLiteral("directDownloadDialog"), Qt::FindDirectChildrenOnly);
    if (!dialog) {
        const auto location = active_->location();
        const QString fallback = location.source == Source::Local ? location.path : QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        const QString directory = settings_.value(QStringLiteral("download_directory")).toString(fallback);
        dialog = new DownloadDialog(directory, this); dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setTaskProgress(taskProgress_);
        connect(dialog, &DownloadDialog::destinationSelected, this, [this](const QString &path) {
            settings_.insert(QStringLiteral("download_directory"), QFileInfo(path).absolutePath()); saveSettings();
        });
        connect(dialog, &QObject::destroyed, this, [this] { if (!closing_) refreshActive(); });
    }
    dialog->show(); dialog->raise(); dialog->activateWindow();
}

void Window::buildActions() {
    auto bind = [&](const QKeySequence &sequence, auto function) { auto *shortcut = new QShortcut(sequence, centralWidget()); shortcut->setContext(Qt::WidgetWithChildrenShortcut); connect(shortcut, &QShortcut::activated, this, function); };
    bind(QKeySequence(Qt::Key_F2), &Window::refreshActive);
    bind(QKeySequence(QStringLiteral("Ctrl+R")), &Window::refreshActive);
    bind(QKeySequence(QStringLiteral("Ctrl+T")), [this] { active_->newTab(); });
    bind(QKeySequence(QStringLiteral("Ctrl+W")), [this] { active_->closeTab(); });
    bind(QKeySequence(QStringLiteral("Ctrl+Tab")), [this] { active_->cycleTab(1); });
    bind(QKeySequence(QStringLiteral("Ctrl+Shift+Tab")), [this] { active_->cycleTab(-1); });
    bind(QKeySequence(QStringLiteral("Ctrl+L")), [this] { active_->focusPath(); });
    bind(QKeySequence(QStringLiteral("Alt+Left")), [this] { active_->goBack(); });
    bind(QKeySequence(QStringLiteral("Alt+Right")), [this] { active_->goForward(); });
    bind(QKeySequence(Qt::Key_Backspace), [this] { if (!qobject_cast<QLineEdit *>(QApplication::focusWidget())) active_->goUp(); });
    bind(QKeySequence::Copy, [this] { if (active_->view()->hasFocus()) transfer(false, true); });
    bind(QKeySequence::Cut, [this] { if (active_->view()->hasFocus()) transfer(true, true); });
    bind(QKeySequence::Paste, [this] { if (active_->view()->hasFocus()) paste(); });
    bind(QKeySequence(Qt::Key_Delete), [this] { if (active_->view()->hasFocus()) remove(); });
    bind(QKeySequence(Qt::Key_F5), [this] { transfer(false); });
    bind(QKeySequence(Qt::Key_F6), [this] { transfer(true); });
    bind(QKeySequence(Qt::Key_F7), [this] { create(true); });
    bind(QKeySequence(QStringLiteral("Ctrl+M")), [this] { rename(true); });
    bind(QKeySequence::Find, &Window::searchFiles);
    bind(QKeySequence(Qt::Key_Escape), [this] { active_->deselect(); });
}

void Window::searchFiles() {
    bool accepted;
    const QString pattern = QInputDialog::getText(this, QStringLiteral("搜索文件"), QStringLiteral("在当前栏目录及子目录中按名称搜索"), QLineEdit::Normal, {}, &accepted);
    if (accepted && !pattern.trimmed().isEmpty()) active_->search(pattern.trimmed());
}

void Window::sortMenu(QWidget *anchor) {
    QMenu popup(this); auto *group = new QActionGroup(&popup);
    auto apply = [this] { for (auto *pane : {left_, right_}) pane->setOptions(settings_.value(QStringLiteral("show_hidden")).toBool(), settings_.value(QStringLiteral("sort")).toString(QStringLiteral("name")), settings_.value(QStringLiteral("descending")).toBool()); };
    const QList<QPair<QString, QString>> orders{{QStringLiteral("name"), QStringLiteral("名称")}, {QStringLiteral("mtime"), QStringLiteral("修改时间")}, {QStringLiteral("size"), QStringLiteral("大小")}};
    for (const auto &order : orders) {
        auto *action = popup.addAction(order.second, this, [this, order, apply] { settings_.insert(QStringLiteral("sort"), order.first); apply(); }); action->setCheckable(true); action->setChecked(settings_.value(QStringLiteral("sort")).toString(QStringLiteral("name")) == order.first); group->addAction(action);
    }
    popup.addSeparator();
    auto *descending = popup.addAction(QStringLiteral("降序")); descending->setCheckable(true); descending->setChecked(settings_.value(QStringLiteral("descending")).toBool()); connect(descending, &QAction::toggled, this, [this, apply](bool checked) { settings_.insert(QStringLiteral("descending"), checked); apply(); });
    auto *hidden = popup.addAction(QStringLiteral("显示隐藏文件")); hidden->setCheckable(true); hidden->setChecked(settings_.value(QStringLiteral("show_hidden")).toBool()); connect(hidden, &QAction::toggled, this, [this, apply](bool checked) { settings_.insert(QStringLiteral("show_hidden"), checked); apply(); });
    popup.exec(anchor->mapToGlobal(QPoint(0, anchor->height() + 5)));
}

void Window::runJob(const QString &title, std::function<Result(const Cancellation &)> function, std::function<void(const Result &)> completed, std::function<void()> settled, bool writeBack) {
    runJob(title, [function = std::move(function)](const Cancellation &cancel, const Progress &) { return function(cancel); }, std::move(completed), std::move(settled), writeBack);
}

void Window::runJob(const QString &title, std::function<Result(const Cancellation &, const Progress &)> function, std::function<void(const Result &)> completed, std::function<void()> settled, bool writeBack) {
    if (busy_ || taskProgress_->active()) { statusBar()->showMessage(QStringLiteral("请等待当前操作完成"), 5000); return; }
    busy_ = true; cancellation_ = std::make_shared<std::atomic_bool>(false);
    struct JobProgress { std::mutex mutex; qint64 done = 0, total = 0; QString stage; bool changed = false; };
    const auto state = std::make_shared<JobProgress>();
    taskProgress_->begin(title, [cancel = cancellation_] { cancel->store(true); }); adaptToolbar();
    auto *timer = new QTimer(this); timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [this, state] {
        qint64 done, total; QString stage;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->changed) return;
            state->changed = false; done = state->done; total = state->total; stage = state->stage;
        }
        taskProgress_->updateProgress(done, total, stage);
    });
    timer->start();
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, timer, completed, settled] {
        const Result result = watcher->result(); watcher->deleteLater(); timer->stop(); timer->deleteLater(); busy_ = false;
        taskProgress_->finish(result.ok ? QStringLiteral("操作完成") : result.error, result.ok);
        left_->refresh(); right_->refresh();
        if (settled) settled();
        if (!result.ok) statusBar()->showMessage(result.error, 15000);
        else { statusBar()->showMessage(QStringLiteral("操作完成"), 5000); if (completed) completed(result); }
    });
    const auto cancel = cancellation_;
    watcher->setFuture(QtConcurrent::run([function = std::move(function), cancel, state, mounts = mounts_, writeBack] {
        const Progress notify = [state](qint64 done, qint64 total, const QString &stage) {
            std::lock_guard<std::mutex> lock(state->mutex); state->done = done; state->total = total; state->stage = stage; state->changed = true;
        };
        try { Result result = function(cancel, notify); if (result.ok && writeBack) { const Result committed = commitArchiveMounts(mounts, cancel); if (!committed.ok) return committed; } return result; }
        catch (const std::exception &exception) { return Result{false, QString::fromUtf8(exception.what()), {}, {}}; }
    }));
}

void Window::transfer(bool move, bool clipboard) {
    const QList<Entry> entries = active_->selected(); if (entries.isEmpty()) return;
    if (clipboard) { clipboard_ = entries; cut_ = move; statusBar()->showMessage(QStringLiteral("已记录 %1 个项目").arg(entries.size()), 4000); return; }
    const Location target = otherPane()->location();
    runJob(move ? QStringLiteral("移动文件") : QStringLiteral("复制文件"), [entries, target, move](const Cancellation &cancel, const Progress &progress) { return transferEntries(entries, target, move, cancel, progress); }, {}, {}, true);
}

void Window::paste() {
    if (clipboard_.isEmpty()) return;
    const QList<Entry> entries = clipboard_; const Location target = active_->location(); const bool move = cut_;
    auto completed = [this, move](const Result &) { if (move) clipboard_.clear(); };
    runJob(QStringLiteral("粘贴文件"), [entries, target, move](const Cancellation &cancel, const Progress &progress) { return transferEntries(entries, target, move, cancel, progress); }, completed, {}, true);
}

void Window::remove() {
    const QList<Entry> entries = active_->selected(); if (entries.isEmpty()) return;
    const bool local = entries.first().source == Source::Local;
    if (QMessageBox::question(this, QStringLiteral("确认删除"), local ? QStringLiteral("将选中的 %1 项移到回收站？").arg(entries.size()) : QStringLiteral("永久删除选中的 %1 项？手机和压缩包没有回收站。").arg(entries.size())) != QMessageBox::Yes) return;
    runJob(QStringLiteral("删除"), [entries](const Cancellation &cancel) { return removeEntries(entries, cancel); }, {}, {}, true);
}

void Window::rename(bool batch) {
    const auto entries = active_->selected(); if (entries.isEmpty()) return;
    const QStringList paths = localPaths(entries);
    if (paths.isEmpty()) {
        if (entries.size() != 1) { report(QStringLiteral("手机/包内条目请逐项重命名")); return; }
        bool accepted; const QString name = QInputDialog::getText(this, QStringLiteral("重命名"), QStringLiteral("新名称"), QLineEdit::Normal, entries.first().name, &accepted);
        if (accepted) runJob(QStringLiteral("重命名"), [entry = entries.first(), name](const Cancellation &cancel) { return renameEntry(entry, name, cancel); }, {}, {}, true); return;
    }
    if (!batch && paths.size() == 1) {
        bool accepted; const QString name = QInputDialog::getText(this, QStringLiteral("重命名"), QStringLiteral("新名称"), QLineEdit::Normal, QFileInfo(paths.first()).fileName(), &accepted);
        if (accepted) runJob(QStringLiteral("重命名"), [paths, name](const Cancellation &) { return batchRename(paths, {name}); });
        return;
    }
    QDialog dialog(this); dialog.setWindowTitle(QStringLiteral("批量重命名 · 确认后执行")); dialog.resize(820, 500);
    auto *layout = new QVBoxLayout(&dialog); auto *options = new QHBoxLayout;
    QLineEdit find, replacement, prefix, suffix; find.setPlaceholderText(QStringLiteral("查找文字")); replacement.setPlaceholderText(QStringLiteral("替换为")); prefix.setPlaceholderText(QStringLiteral("前缀")); suffix.setPlaceholderText(QStringLiteral("后缀（保留扩展名）"));
    for (QLineEdit *input : {&find, &replacement, &prefix, &suffix}) options->addWidget(input);
    layout->addLayout(options);
    auto *numbering = new QHBoxLayout; QCheckBox numbered(QStringLiteral("添加编号")); QSpinBox start, step, width;
    start.setRange(0, 1000000000); start.setValue(1); step.setRange(1, 1000000); step.setValue(1); width.setRange(1, 12); width.setValue(3);
    numbering->addWidget(&numbered); numbering->addWidget(new QLabel(QStringLiteral("起始"))); numbering->addWidget(&start); numbering->addWidget(new QLabel(QStringLiteral("步长"))); numbering->addWidget(&step); numbering->addWidget(new QLabel(QStringLiteral("位数"))); numbering->addWidget(&width); layout->addLayout(numbering);
    auto *table = new QTableWidget(int(paths.size()), 3); table->setHorizontalHeaderLabels({QStringLiteral("原名称"), QStringLiteral("新名称（可直接编辑）"), QStringLiteral("检查")}); table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); layout->addWidget(table);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(&buttons);
    auto validate = [&] {
        const QSignalBlocker blocked(table);
        QSet<QString> sources, targets;
        QHash<QString, int> counts;
        for (const QString &path : paths) sources.insert(normalizedPath(path));
        for (int row = 0; row < table->rowCount(); ++row) if (table->item(row, 1)) ++counts[normalizedPath(QFileInfo(paths[row]).dir().filePath(table->item(row, 1)->text()))];
        bool valid = true;
        for (int row = 0; row < table->rowCount(); ++row) {
            if (!table->item(row, 1)) { valid = false; continue; }
            const QString name = table->item(row, 1)->text(), target = QFileInfo(paths[row]).dir().filePath(name), key = normalizedPath(target);
            QString error = validateName(name);
            if (counts.value(key) > 1) error = QStringLiteral("新名称重复");
            else if (QFileInfo::exists(target) && !sources.contains(key)) error = QStringLiteral("目标已存在");
            auto *status = new QTableWidgetItem(error.isEmpty() ? QStringLiteral("可执行") : error); status->setFlags(status->flags() & ~Qt::ItemIsEditable); table->setItem(row, 2, status);
            if (!error.isEmpty()) valid = false;
        }
        buttons.button(QDialogButtonBox::Ok)->setEnabled(valid);
    };
    auto update = [&] {
        const QSignalBlocker blocked(table);
        for (qsizetype index = 0; index < paths.size(); ++index) {
            const QFileInfo info(paths[index]); QString base = info.isDir() ? info.fileName() : info.completeBaseName();
            if (!find.text().isEmpty()) base.replace(find.text(), replacement.text());
            const QString extension = !info.isDir() && !info.suffix().isEmpty() ? QChar('.') + info.suffix() : QString();
            auto *original = new QTableWidgetItem(info.fileName()); original->setFlags(original->flags() & ~Qt::ItemIsEditable); table->setItem(int(index), 0, original);
            const QString number = numbered.isChecked() ? QString::number(qint64(start.value()) + index * step.value()).rightJustified(width.value(), QChar('0')) : QString();
            table->setItem(int(index), 1, new QTableWidgetItem(prefix.text() + base + number + suffix.text() + extension));
        }
        validate();
    };
    for (QLineEdit *input : {&find, &replacement, &prefix, &suffix}) connect(input, &QLineEdit::textChanged, &dialog, update);
    connect(&numbered, &QCheckBox::toggled, &dialog, update);
    for (QSpinBox *spin : {&start, &step, &width}) connect(spin, &QSpinBox::valueChanged, &dialog, update);
    connect(table, &QTableWidget::itemChanged, &dialog, [&validate](QTableWidgetItem *item) { if (item->column() == 1) validate(); });
    update();
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    QStringList names; for (int row = 0; row < table->rowCount(); ++row) names.append(table->item(row, 1)->text());
    runJob(QStringLiteral("批量重命名"), [paths, names](const Cancellation &) { return batchRename(paths, names); });
}

void Window::create(bool directory) {
    const Location target = active_->location();
    bool accepted; const QString name = QInputDialog::getText(this, directory ? QStringLiteral("新建文件夹") : QStringLiteral("新建文件"), QStringLiteral("名称"), QLineEdit::Normal, {}, &accepted);
    if (!accepted) return;
    const QString invalid = validateName(name); if (!invalid.isEmpty()) { report(invalid); return; }
    runJob(QStringLiteral("新建"), [target, name, directory](const Cancellation &cancel) { return createEntry(target, name, directory, cancel); }, {}, {}, true);
}

void Window::compress() {
    const QStringList paths = localPaths(active_->selected()); if (paths.isEmpty()) return;
    const QString destination = QFileDialog::getSaveFileName(this, QStringLiteral("压缩为 ZIP"), QFileInfo(paths.first()).absolutePath() + QStringLiteral("/archive.zip"), QStringLiteral("ZIP (*.zip)"));
    if (destination.isEmpty()) return;
    runJob(QStringLiteral("创建 ZIP"), [paths, destination](const Cancellation &cancel, const Progress &progress) { return createArchive(paths, destination, cancel, progress); });
}

void Window::extract() {
    const QList<Entry> selected = active_->selected();
    QString archive, prefix;
    if (active_->location().source == Source::Archive) { archive = active_->location().path; if (selected.size() == 1) prefix = selected.first().inner; }
    else if (selected.size() == 1 && selected.first().source == Source::Local && isArchive(selected.first().name)) archive = selected.first().path;
    if (archive.isEmpty()) return;
    const QString destination = QFileDialog::getExistingDirectory(this, QStringLiteral("解压到文件夹")); if (destination.isEmpty()) return;
    runJob(QStringLiteral("解压 ZIP/APK"), [archive, destination, prefix](const Cancellation &cancel, const Progress &progress) { return extractArchive(archive, destination, prefix, cancel, progress); });
}

void Window::showText(const QString &title, const QString &text) {
    auto *dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowTitle(title); dialog->resize(850, 560);
    auto *layout = new QVBoxLayout(dialog); auto *search = new QLineEdit; search->setPlaceholderText(QStringLiteral("查找结果，按回车定位下一处")); layout->addWidget(search); auto *viewer = new QPlainTextEdit(text); viewer->setReadOnly(true); viewer->setFont(QFont(QStringLiteral("Consolas"), 10)); layout->addWidget(viewer);
    connect(search, &QLineEdit::returnPressed, viewer, [viewer, search] { if (!viewer->find(search->text())) { viewer->moveCursor(QTextCursor::Start); viewer->find(search->text()); } });
    auto *exportResult = new QPushButton(QStringLiteral("导出结果")); layout->addWidget(exportResult); connect(exportResult, &QPushButton::clicked, dialog, [dialog, text] { const QString path = QFileDialog::getSaveFileName(dialog, QStringLiteral("导出结果"), {}, QStringLiteral("文本 (*.txt)")); if (path.isEmpty()) return; QSaveFile output(path); const QByteArray bytes = text.toUtf8(); if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) QMessageBox::warning(dialog, QStringLiteral("保存失败"), output.errorString()); });
    dialog->show();
}

void Window::edit(const QString &path) {
    auto changed = [this] { left_->refresh(); right_->refresh(); };
    if (isDex(path)) (new DexDialog(path, this, {}, {}, changed))->show();
    else (new EditorDialog(path, this, {}, {}, changed))->show();
}

void Window::open(const Entry &entry) {
    if (entry.source == Source::Device) {
        if (entry.state == QStringLiteral("device")) active_->navigate({Source::Phone, entry.serial, QStringLiteral("/sdcard"), entry.name});
        else report(entry.detail);
        return;
    }
    if (entry.directory) {
        if (entry.source == Source::Archive) active_->navigate({Source::Archive, entry.path, entry.inner, {}});
        else if (entry.source == Source::Phone) active_->navigate({Source::Phone, entry.serial, entry.path, active_->location().label});
        else active_->navigate({Source::Local, entry.path, {}, {}});
        return;
    }
    if (entry.source != Source::Local) {
        if (isMedia(entry.name)) { media(entry.name, active_->fileModel()->entries(), entry); return; }
        auto temporary = std::make_shared<QTemporaryDir>(); if (!temporary->isValid()) { report(QStringLiteral("无法创建打开文件的暂存目录")); return; }
        const Location parent = active_->location(); QPointer<Pane> targetPane(active_); const auto neighbors = active_->fileModel()->entries();
        runJob(QStringLiteral("读取手机/包内文件"), [entry, temporary](const Cancellation &cancel, const Progress &progress) { return materializeEntry(entry, temporary->path(), cancel, progress); }, [this, entry, temporary, parent, targetPane, neighbors](const Result &result) {
            const QString path = result.paths.first();
            if (isArchive(entry.name)) {
                auto mount = std::make_shared<MountedArchive>(); mount->original = entry; mount->local = path; mount->temporary = temporary; mount->hash = fileDigest(path); mounts_.insert(path, mount);
                left_->registerArchiveParent(path, parent); right_->registerArchiveParent(path, parent); if (targetPane) targetPane->navigate({Source::Archive, path, {}, entry.name});
            } else if (isImage(entry.name) || isMedia(entry.name)) {
                auto mount = std::make_shared<MountedArchive>(); mount->original = entry; mount->local = path; mount->temporary = temporary; mount->hash = fileDigest(path); mounts_.insert(path, mount); if (isImage(entry.name)) image(path, neighbors); else media(path, neighbors);
            } else {
                auto changed = [this] { runJob(QStringLiteral("同步嵌套容器"), [](const Cancellation &) { return Result{}; }, {}, {}, true); };
                if (isDex(entry.name)) (new DexDialog(path, this, entry, temporary, changed))->show();
                else (new EditorDialog(path, this, entry, temporary, changed))->show();
            }
        });
        return;
    }
    if (isArchive(entry.name)) { active_->navigate({Source::Archive, entry.path, {}, {}}); return; }
    if (isImage(entry.name)) { image(entry.path); return; }
    if (isMedia(entry.name)) { media(entry.path); return; }
    if (isDex(entry.name) || isText(entry.name)) edit(entry.path);
    else QDesktopServices::openUrl(QUrl::fromLocalFile(entry.path));
}

void Window::preview() {
    const QList<Entry> entries = active_->selected(); if (entries.size() != 1) return;
    const Entry entry = entries.first();
    if (!entry.directory && (isDex(entry.name) || isImage(entry.name) || isMedia(entry.name) || isText(entry.name))) { open(entry); return; }
    QString text = QStringLiteral("%1\n%2\n大小：%3\n修改：%4\n").arg(entry.name, entry.path, humanSize(entry.size), entry.modified.toString(Qt::ISODate));
    if (entry.source == Source::Local && !entry.directory) { QString error; const QString contents = readText(entry.path, 1024 * 1024, &error); if (error.isEmpty()) text += QStringLiteral("\n") + contents; }
    showText(QStringLiteral("快速预览"), text);
}
void Window::properties() {
    const QList<Entry> entries = active_->selected(); QStringList text;
    for (const Entry &entry : entries) text.append(QStringLiteral("名称：%1\n路径：%2\n包内：%3\n大小：%4\n修改：%5\n").arg(entry.name, entry.path, entry.inner, humanSize(entry.size), entry.modified.toString(Qt::ISODate)));
    if (text.isEmpty()) text.append(active_->location().display());
    showText(QStringLiteral("属性"), text.join(QChar('\n')));
}

void Window::image(const QString &path, const QList<Entry> &sourceEntries) {
    QImageReader reader(path); reader.setAutoTransform(true); const QSize size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > 64000000 || QFileInfo(path).size() > 128 * 1024 * 1024) { report(QStringLiteral("图片过大或格式无法识别")); return; }
    const QImage loaded = reader.read(); if (loaded.isNull()) { report(reader.errorString()); return; }
    auto *dialog = new ImageDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowTitle(QFileInfo(path).fileName()); dialog->resize(1250, 780);
    auto *layout = new QVBoxLayout(dialog); auto *toolbar = new QHBoxLayout; layout->addLayout(toolbar);
    auto *canvas = new ImageCanvas; auto *scene = new QGraphicsScene(canvas); canvas->setScene(scene); canvas->setDragMode(QGraphicsView::ScrollHandDrag); canvas->setRenderHint(QPainter::SmoothPixmapTransform);
    auto *item = scene->addPixmap(QPixmap::fromImage(loaded)); layout->addWidget(canvas, 1);
    auto artwork = std::make_shared<QImage>(loaded);
    canvas->artwork = artwork; canvas->item = item;
    struct ImageState { QList<QImage> undo, redo; bool dirty = false; bool saving = false; bool exportOnly = false; QByteArray hash; };
    auto state = std::make_shared<ImageState>(); state->hash = fileDigest(path); state->exportOnly = reader.supportsAnimation() || QFileInfo(path).suffix().compare(QStringLiteral("gif"), Qt::CaseInsensitive) == 0;
    auto *movie = new QMovie(path, {}, dialog);
    if (reader.supportsAnimation()) { movie->setCacheMode(QMovie::CacheNone); connect(movie, &QMovie::frameChanged, dialog, [movie, item] { item->setPixmap(movie->currentPixmap()); }); movie->start(); }
    auto remember = [state, artwork, movie] { movie->stop(); state->redo.clear(); state->undo.append(*artwork); while (state->undo.size() > 8 || (state->undo.size() > 1 && qint64(state->undo.size()) * artwork->sizeInBytes() > 256 * 1024 * 1024)) state->undo.removeFirst(); state->dirty = true; };
    canvas->beforeStroke = remember;
    canvas->crop = [artwork, item, remember](const QRect &rectangle) { remember(); *artwork = artwork->copy(rectangle); item->setPixmap(QPixmap::fromImage(*artwork)); };
    auto button = [&](const QString &title, auto handler) { auto *control = new QPushButton(title); toolbar->addWidget(control); connect(control, &QPushButton::clicked, dialog, handler); };
    button(QStringLiteral("适应窗口"), [canvas, item] { canvas->fitInView(item, Qt::KeepAspectRatio); });
    button(QStringLiteral("原始大小"), [canvas] { canvas->resetTransform(); });
    button(QStringLiteral("放大 +"), [canvas] { canvas->scale(1.25, 1.25); }); button(QStringLiteral("缩小 −"), [canvas] { canvas->scale(0.8, 0.8); });
    button(QStringLiteral("旋转 90°"), [artwork, item, remember] { remember(); *artwork = artwork->transformed(QTransform().rotate(90)); item->setPixmap(QPixmap::fromImage(*artwork)); });
    button(QStringLiteral("水平翻转"), [artwork, item, remember] { remember(); *artwork = artwork->flipped(Qt::Horizontal); item->setPixmap(QPixmap::fromImage(*artwork)); });
    button(QStringLiteral("另存为"), [dialog, artwork, state, path] {
        const QString destination = QFileDialog::getSaveFileName(dialog, QStringLiteral("另存图片"), {}, QStringLiteral("PNG (*.png);;JPEG (*.jpg);;WebP (*.webp)"));
        if (destination.isEmpty()) return;
        if (normalizedPath(destination) == normalizedPath(path) || (!QFileInfo(destination).canonicalFilePath().isEmpty() && normalizedPath(QFileInfo(destination).canonicalFilePath()) == normalizedPath(QFileInfo(path).canonicalFilePath()))) { QMessageBox::warning(dialog, QStringLiteral("请选择新文件名"), QStringLiteral("另存为不能覆盖当前源文件；请使用保存原图，以保留冲突检查和动画保护。")); return; }
        QSaveFile output(destination); const QByteArray format = QFileInfo(destination).suffix().toLatin1();
        if (!output.open(QIODevice::WriteOnly) || !artwork->save(&output, format.constData()) || !output.commit()) QMessageBox::warning(dialog, QStringLiteral("保存失败"), output.errorString());
        else state->dirty = false;
    });
    const auto neighbors = QFileInfo(path).dir().entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase);
    QStringList images;
    for (const auto &neighbor : neighbors) if (isImage(neighbor.fileName()) && images.size() < 5000) images.append(neighbor.absoluteFilePath());
    const int currentImage = images.indexOf(QFileInfo(path).absoluteFilePath());
    auto navigateImage = [this, dialog, state, images, currentImage](int step) {
        if (state->saving || currentImage < 0 || images.size() < 2) return;
        const QString target = images[(currentImage + step + images.size()) % images.size()];
        dialog->close();
        if (!dialog->isVisible()) image(target);
    };
    button(QStringLiteral("上一张"), [navigateImage] { navigateImage(-1); });
    button(QStringLiteral("下一张"), [navigateImage] { navigateImage(1); });
    if (!sourceEntries.isEmpty() && mounts_.contains(path)) {
        QList<Entry> neighbors; const QString originalKey = mounts_.value(path)->original.key(); int current = -1;
        for (const auto &entry : sourceEntries) if (!entry.directory && isImage(entry.name) && neighbors.size() < 5000) { if (entry.key() == originalKey) current = int(neighbors.size()); neighbors.append(entry); }
        auto remoteImage = [this, dialog, state, neighbors, current](int step) {
            if (state->saving || current < 0 || neighbors.size() < 2) return;
            const Entry target = neighbors[(current + step + neighbors.size()) % neighbors.size()]; dialog->close(); if (dialog->isVisible()) return;
            auto temporary = std::make_shared<QTemporaryDir>(); if (!temporary->isValid()) { report(QStringLiteral("不能创建图片暂存目录")); return; }
            runJob(QStringLiteral("读取下一张手机/包内图片"), [target, temporary](const Cancellation &cancel) { return materializeEntry(target, temporary->path(), cancel); }, [this, target, temporary, neighbors](const Result &result) { const QString local = result.paths.first(); auto mount = std::make_shared<MountedArchive>(); mount->original = target; mount->local = local; mount->temporary = temporary; mount->hash = fileDigest(local); mounts_.insert(local, mount); image(local, neighbors); });
        };
        button(QStringLiteral("源目录上一张"), [remoteImage] { remoteImage(-1); }); button(QStringLiteral("源目录下一张"), [remoteImage] { remoteImage(1); });
    }
    auto *editing = new QHBoxLayout; layout->insertLayout(1, editing);
    auto editButton = [&](const QString &title, auto handler) { auto *control = new QPushButton(title); editing->addWidget(control); connect(control, &QPushButton::clicked, dialog, handler); return control; };
    editButton(QStringLiteral("框选裁剪"), [canvas] { canvas->cropping = true; canvas->brush = false; canvas->setDragMode(QGraphicsView::NoDrag); });
    editButton(QStringLiteral("调整大小"), [dialog, artwork, item, remember] { bool accepted; const QString dimensions = QInputDialog::getText(dialog, QStringLiteral("调整大小"), QStringLiteral("宽度 × 高度（像素），例如 1920x1080"), QLineEdit::Normal, QStringLiteral("%1x%2").arg(artwork->width()).arg(artwork->height()), &accepted); if (!accepted) return; const auto parts = dimensions.split(QRegularExpression(QStringLiteral("[xX×,， ]+")), Qt::SkipEmptyParts); const int width = parts.value(0).toInt(), height = parts.value(1).toInt(); if (parts.size() != 2 || width <= 0 || height <= 0 || qint64(width) * height > 64000000) { QMessageBox::warning(dialog, QStringLiteral("尺寸无效"), QStringLiteral("最多 6400 万像素，宽高必须为正整数")); return; } remember(); *artwork = artwork->scaled(width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation); item->setPixmap(QPixmap::fromImage(*artwork)); });
    editButton(QStringLiteral("裁剪"), [dialog, artwork, item, remember] { bool accepted; const QString text = QInputDialog::getText(dialog, QStringLiteral("裁剪矩形"), QStringLiteral("左上角 X,Y,宽,高（像素）"), QLineEdit::Normal, QStringLiteral("0,0,%1,%2").arg(artwork->width()).arg(artwork->height()), &accepted); if (!accepted) return; const auto parts = text.split(QRegularExpression(QStringLiteral("[,， ]+")), Qt::SkipEmptyParts); QList<int> values; for (const QString &part : parts) { bool ok = false; const int value = part.toInt(&ok); if (!ok) return; values.append(value); } if (values.size() != 4) return; const QRect rectangle(values[0], values[1], values[2], values[3]); if (!rectangle.isValid() || !artwork->rect().contains(rectangle)) return; remember(); *artwork = artwork->copy(rectangle); item->setPixmap(QPixmap::fromImage(*artwork)); });
    auto *brush = editButton(QStringLiteral("画笔"), [canvas] { canvas->brush = !canvas->brush; canvas->setDragMode(canvas->brush ? QGraphicsView::NoDrag : QGraphicsView::ScrollHandDrag); }); brush->setCheckable(true);
    editButton(QStringLiteral("颜色"), [dialog, canvas] { const QColor color = QColorDialog::getColor(canvas->color, dialog, QStringLiteral("画笔颜色"), QColorDialog::ShowAlphaChannel); if (color.isValid()) canvas->color = color; });
    auto *width = new QSpinBox; width->setRange(1, 256); width->setValue(8); width->setSuffix(QStringLiteral(" px")); editing->addWidget(width); connect(width, &QSpinBox::valueChanged, canvas, [canvas](int value) { canvas->brushWidth = value; });
    editButton(QStringLiteral("撤销"), [state, artwork, item, movie] { if (state->undo.isEmpty()) return; movie->stop(); state->redo.append(*artwork); *artwork = state->undo.takeLast(); state->dirty = true; item->setPixmap(QPixmap::fromImage(*artwork)); });
    editButton(QStringLiteral("重做"), [state, artwork, item] { if (state->redo.isEmpty()) return; state->undo.append(*artwork); *artwork = state->redo.takeLast(); state->dirty = true; item->setPixmap(QPixmap::fromImage(*artwork)); });
    editButton(QStringLiteral("复制图片"), [artwork] { QApplication::clipboard()->setImage(*artwork); });
    auto saveOriginal = [this, dialog, state, artwork, path]() -> bool {
        if (state->saving) return false;
        if (state->exportOnly) { QMessageBox::information(dialog, QStringLiteral("动画保护"), QStringLiteral("动画只允许另存为静态图片，不覆盖原始动画。请使用“另存为”。")); return false; }
        if (fileDigest(path) != state->hash) { QMessageBox::warning(dialog, QStringLiteral("文件已被外部修改"), QStringLiteral("未覆盖原图，请使用另存为")); return false; }
        QSaveFile output(path); const QByteArray format = QFileInfo(path).suffix().toLatin1(); if (!output.open(QIODevice::WriteOnly) || !artwork->save(&output, format.constData()) || !output.commit()) { QMessageBox::warning(dialog, QStringLiteral("保存失败"), output.errorString()); return false; }
        state->hash = fileDigest(path); if (mounts_.contains(path)) { if (busy_) return false; state->saving = true; runJob(QStringLiteral("回写图片及嵌套容器"), [](const Cancellation &) { return Result{}; }, [state](const Result &) { state->dirty = false; }, [state] { state->saving = false; }, true); return false; }
        state->dirty = false; return true;
    };
    editButton(QStringLiteral("保存"), saveOriginal);
    auto *saveShortcut = new QShortcut(QKeySequence::Save, dialog); connect(saveShortcut, &QShortcut::activated, dialog, saveOriginal);
    dialog->allowClose = [dialog, state, saveOriginal] { if (state->saving) return false; if (!state->dirty) return true; const auto choice = QMessageBox::question(dialog, QStringLiteral("图片未保存"), QStringLiteral("保存修改后关闭？"), QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel); if (choice == QMessageBox::Discard) return true; return choice == QMessageBox::Save && saveOriginal(); };
    if (reader.supportsAnimation()) button(QStringLiteral("播放 / 暂停动画"), [movie] { movie->setPaused(movie->state() == QMovie::Running); });
    dialog->show(); QTimer::singleShot(0, dialog, [canvas, item] { canvas->fitInView(item, Qt::KeepAspectRatio); });
}

void Window::media(const QString &path, const QList<Entry> &neighbors, const Entry &current) {
    (new MediaDialog(path, this, neighbors, current))->show();
}

void Window::hashes() {
    const auto entries = active_->selected(); if (entries.isEmpty()) return;
    runJob(QStringLiteral("流式计算校验值"), [entries](const Cancellation &cancel) { return checksumEntries(entries, cancel); }, [this](const Result &result) { showText(QStringLiteral("MD5 / SHA1 / SHA256"), result.output); });
}

void Window::verifyHash() {
    const auto entries = active_->selected();
    if (entries.size() != 1 || entries.first().directory) { report(QStringLiteral("请只选择一个普通文件")); return; }
    bool accepted = false;
    const QString expected = QInputDialog::getMultiLineText(this, QStringLiteral("验证文件校验值"), QStringLiteral("粘贴 MD5 / SHA1 / SHA256，可带算法前缀或文件名"), {}, &accepted);
    if (!accepted) return;
    if (normalizedChecksum(expected).isEmpty()) { report(QStringLiteral("没有识别到唯一的有效校验值")); return; }
    runJob(QStringLiteral("读取文件并核对校验值"), [entries, expected](const Cancellation &cancel) { return checksumEntries(entries, cancel, expected); }, [this](const Result &result) { showText(QStringLiteral("文件校验结果"), result.output); });
}

void Window::checksumList(bool verify) {
    if (active_->location().source != Source::Local) { report(QStringLiteral("请选择本地目录")); return; }
    const QString base = active_->location().path;
    if (verify) {
        const QString source = QFileDialog::getOpenFileName(this, QStringLiteral("导入校验清单；相对路径以当前目录为基准"), base, QStringLiteral("校验清单 (*.md5 *.sha1 *.sha256 *.txt);;所有文件 (*)"));
        if (source.isEmpty()) return;
        runJob(QStringLiteral("验证校验清单"), [source, base](const Cancellation &cancel) { return verifyChecksumManifest(source, base, cancel); }, [this](const Result &result) { showText(QStringLiteral("校验结果"), result.output); });
        return;
    }
    const QStringList paths = localPaths(active_->selected());
    if (paths.isEmpty()) return;
    bool accepted = false;
    const QString algorithm = QInputDialog::getItem(this, QStringLiteral("导出校验清单"), QStringLiteral("算法（相对路径以当前文件夹为基准）"), {QStringLiteral("SHA256"), QStringLiteral("SHA1"), QStringLiteral("MD5")}, 0, false, &accepted);
    if (!accepted) return;
    const QString destination = QFileDialog::getSaveFileName(this, QStringLiteral("保存校验清单"), QDir(base).filePath(QStringLiteral("checksums.") + algorithm.toLower()));
    if (destination.isEmpty()) return;
    const auto kind = algorithm == QStringLiteral("MD5") ? QCryptographicHash::Md5 : algorithm == QStringLiteral("SHA1") ? QCryptographicHash::Sha1 : QCryptographicHash::Sha256;
    runJob(QStringLiteral("导出校验清单"), [paths, base, kind, destination](const Cancellation &cancel) {
        const Result result = checksumManifest(paths, base, kind, cancel);
        if (!result.ok) return result;
        QSaveFile output(destination); const QByteArray bytes = result.output.toUtf8();
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) return Result{false, output.errorString(), {}, {}};
        return Result{true, {}, {}, {destination}};
    });
}

void Window::compare(bool directories) {
    const QList<Entry> first = left_->selected(), second = right_->selected();
    if (first.size() != 1 || second.size() != 1) { report(QStringLiteral("请在左右栏各选择一个文件或文件夹")); return; }
    const Entry leftEntry = first.first(), rightEntry = second.first();
    if (leftEntry.directory != directories || rightEntry.directory != directories) { report(directories ? QStringLiteral("请选择两个文件夹") : QStringLiteral("请选择两个文本文件")); return; }
    if ((leftEntry.source != Source::Local || rightEntry.source != Source::Local) && QMessageBox::question(this, QStringLiteral("暂存比较内容"), QStringLiteral("比较手机或包内内容时会暂存到电脑，目录比较可能读取大量数据。完成后删除暂存副本，原文件不变。继续？")) != QMessageBox::Yes) return;
    int mode = 2;
    if (directories) {
        bool accepted = false;
        const QStringList modes{QStringLiteral("类型 / 名称 / 大小"), QStringLiteral("类型 / 名称 / 大小 / 修改时间"), QStringLiteral("类型 / 名称 / 大小 / SHA256 内容")};
        const QString choice = QInputDialog::getItem(this, QStringLiteral("目录比较"), QStringLiteral("包括空目录；不遍历链接"), modes, 2, false, &accepted);
        if (!accepted) return;
        mode = modes.indexOf(choice);
    }
    auto temporary = std::make_shared<QTemporaryDir>();
    auto directoryResult = std::make_shared<DirectoryComparison>();
    runJob(QStringLiteral("比较"), [leftEntry, rightEntry, directories, mode, temporary, directoryResult](const Cancellation &cancel) {
        if (!temporary->isValid()) return Result{false, QStringLiteral("无法创建比较暂存目录"), {}, {}};
        auto materialize = [&](const Entry &entry, const QString &side) -> Result {
            if (entry.source == Source::Local) return {true, {}, {}, {entry.path}};
            const QString directory = temporary->filePath(side); if (!QDir().mkpath(directory)) return {false, QStringLiteral("无法创建暂存目录"), {}, {}};
            if (!entry.directory) return materializeEntry(entry, directory, cancel);
            if (entry.source == Source::Phone) return adbSyncPull(entry.serial, entry.path, QDir(directory).filePath(entry.name), cancel);
            if (entry.source == Source::Archive) { const Result result = extractArchive(entry.path, directory, entry.inner, cancel); if (!result.ok) return result; return {true, {}, {}, {QDir(directory).filePath(entry.inner)}}; }
            return {false, QStringLiteral("此来源无法比较"), {}, {}};
        };
        const Result before = materialize(leftEntry, QStringLiteral("left")); if (!before.ok) return before;
        const Result after = materialize(rightEntry, QStringLiteral("right")); if (!after.ok) return after;
        const QString left = before.paths.first(), right = after.paths.first();
        if (!directories) {
            QString error; const QString first = readText(left, 16 * 1024 * 1024, &error); if (!error.isEmpty()) return Result{false, error, {}, {}};
            const QString second = readText(right, 16 * 1024 * 1024, &error); if (!error.isEmpty()) return Result{false, error, {}, {}};
            if (cancel && cancel->load()) return Result{false, QStringLiteral("已取消"), {}, {}};
            return Result{true, {}, {}, {first, second}};
        }
        *directoryResult = inspectDirectories(left, right, mode, cancel);
        return Result{directoryResult->error.isEmpty(), directoryResult->error, {}, {}};
    }, [this, directories, temporary, directoryResult](const Result &result) { if (!directories && result.paths.size() == 2) (new DifferenceDialog(result.paths[0], result.paths[1], this))->show(); else (new DirectoryComparisonDialog(std::move(*directoryResult), temporary, this))->show(); });
}

void Window::apkDecompile() {
    const QList<Entry> entries = active_->selected(); if (entries.size() != 1 || entries.first().source != Source::Local || !isApk(entries.first().name)) return;
    const QString source = entries.first().path; const QString parent = QFileDialog::getExistingDirectory(this, QStringLiteral("选择反编译输出父目录")); if (parent.isEmpty()) return;
    const QString destination = QDir(parent).filePath(QFileInfo(source).completeBaseName() + QStringLiteral("_decoded"));
    if (QFileInfo::exists(destination)) { report(QStringLiteral("工程目录已存在，不会覆盖")); return; }
    runJob(QStringLiteral("APK 反编译"), [source, destination](const Cancellation &cancel) {
        return runProcess(toolPath(QStringLiteral("runtime/bin/java.exe")), {QStringLiteral("-Xmx2g"), QStringLiteral("-Dfile.encoding=UTF-8"), QStringLiteral("-jar"), toolPath(QStringLiteral("apktool.jar")), QStringLiteral("d"), source, QStringLiteral("-o"), destination}, 3600000, cancel);
    });
}

void Window::apkBuild() {
    const Location location = active_->location(); if (location.source != Source::Local) return;
    const QString source = QFileInfo(QDir(location.path).filePath(QStringLiteral("apktool.yml"))).isFile() ? location.path : QFileDialog::getExistingDirectory(this, QStringLiteral("选择 apktool 工程"));
    if (source.isEmpty()) return;
    const QString destination = QFileDialog::getSaveFileName(this, QStringLiteral("回编译并签名输出"), {}, QStringLiteral("APK (*.apk)")); if (destination.isEmpty()) return;
    if (QFileInfo::exists(destination)) { report(QStringLiteral("目标已存在，请使用新的文件名")); return; }
    if (QMessageBox::question(this, QStringLiteral("调试签名"), QStringLiteral("回编译后使用独立调试密钥签名。不能直接覆盖原作者签名的已安装应用。继续？")) != QMessageBox::Yes) return;
    runJob(QStringLiteral("APK 回编译、签名与校验"), [source, destination](const Cancellation &cancel) { return buildApk(source, destination, true, cancel); });
}

void Window::apkEdit() {
    const auto entries = active_->selected(); if (entries.size() != 1 || entries.first().source != Source::Local || !isApk(entries.first().name)) { report(QStringLiteral("请先选择一个本地 APK；包内 APK 可先打开后复制到本地")); return; }
    const QString source = entries.first().path; QString error; const QByteArray bytes = readArchiveFile(source, QStringLiteral("AndroidManifest.xml"), 4 * 1024 * 1024, &error); if (!error.isEmpty()) { report(error); return; }
    const QDomDocument document = androidXml(bytes, &error); if (document.isNull()) { report(error); return; } const auto manifest = document.documentElement();
    QDialog dialog(this); dialog.setWindowTitle(QStringLiteral("APK 快速编辑 · 原文件保持不变")); auto *layout = new QVBoxLayout(&dialog); layout->addWidget(new QLabel(QStringLiteral("修改二进制清单并重新调试签名。改包名时将相对组件类名补全。\n共享 UID、硬编码包名或签名校验可能让修改后的应用无法工作。")));
    QMap<QString, QLineEdit *> inputs; const auto application = manifest.firstChildElement(QStringLiteral("application"));
    for (const QString &key : {QStringLiteral("package"), QStringLiteral("versionCode"), QStringLiteral("versionName"), QStringLiteral("label")}) {
        const auto element = key == QStringLiteral("label") ? application : manifest; const QString value = element.attribute(key, element.attribute(QStringLiteral("android:") + key)); layout->addWidget(new QLabel(key)); auto *input = new QLineEdit(value); input->setProperty("original", value); inputs.insert(key, input); layout->addWidget(input);
    }
    QMap<QString, QString> icons;
    auto *replaceIcon = new QPushButton(QStringLiteral("选择包内位图图标替换目标…")); layout->addWidget(replaceIcon);
    connect(replaceIcon, &QPushButton::clicked, &dialog, [&dialog, source, &icons, replaceIcon] {
        const QString image = QFileDialog::getOpenFileName(&dialog, QStringLiteral("选择新图标"), {}, QStringLiteral("图片 (*.png *.webp *.jpg *.jpeg)")); if (image.isEmpty()) return;
        QDialog targets(&dialog); targets.resize(700, 500); targets.setWindowTitle(QStringLiteral("选择现有图标资源；不确定时使用工程编辑")); auto *layout = new QVBoxLayout(&targets); auto *list = new QListWidget; layout->addWidget(list);
        ZipReader reader(source);
        for (const auto &entry : reader.fileInfoList()) if (!entry.isDir && entry.filePath.startsWith(QStringLiteral("res/")) && isImage(entry.filePath) && !entry.filePath.endsWith(QStringLiteral(".9.png")) && list->count() < 20000) { auto *item = new QListWidgetItem(entry.filePath, list); item->setCheckState(icons.contains(entry.filePath) ? Qt::Checked : Qt::Unchecked); }
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(&buttons); connect(&buttons, &QDialogButtonBox::accepted, &targets, &QDialog::accept); connect(&buttons, &QDialogButtonBox::rejected, &targets, &QDialog::reject);
        if (targets.exec() != QDialog::Accepted) return;
        icons.clear(); for (int index = 0; index < list->count(); ++index) if (list->item(index)->checkState() == Qt::Checked) icons.insert(list->item(index)->text(), image);
        replaceIcon->setText(QStringLiteral("已选择 %1 个位图（保留各目标尺寸）").arg(icons.size()));
    });
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(&buttons); connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); if (dialog.exec() != QDialog::Accepted) return;
    QMap<QString, QString> fields; for (auto item = inputs.cbegin(); item != inputs.cend(); ++item) if (item.value()->text() != item.value()->property("original").toString()) fields.insert(item.key(), item.value()->text()); if (fields.isEmpty() && icons.isEmpty()) return;
    const QString destination = QFileDialog::getSaveFileName(this, QStringLiteral("保存新的签名 APK"), source + QStringLiteral(".edited.apk"), QStringLiteral("APK (*.apk)")); if (destination.isEmpty()) return;
    runJob(QStringLiteral("修改 APK 清单并签名校验"), [source, destination, fields, icons](const Cancellation &cancel) { return quickEditApk(source, destination, fields, icons, cancel); });
}

void Window::apkProjectEdit() {
    QString source = active_->location().source == Source::Local ? active_->location().path : QString();
    if (!QFileInfo(QDir(source).filePath(QStringLiteral("apktool.yml"))).isFile()) source = QFileDialog::getExistingDirectory(this, QStringLiteral("选择反编译工程"));
    if (source.isEmpty()) return;
    QFile manifestFile(QDir(source).filePath(QStringLiteral("AndroidManifest.xml")));
    if (!manifestFile.open(QIODevice::ReadOnly) || manifestFile.size() > 4 * 1024 * 1024) { report(QStringLiteral("工程清单无法读取")); return; }
    QString error; const auto document = androidXml(repairResourceXml(QString::fromUtf8(manifestFile.readAll()), true).toUtf8(), &error);
    if (document.isNull()) { report(error); return; }
    const auto manifest = document.documentElement(), application = manifest.firstChildElement(QStringLiteral("application"));
    QDialog dialog(this); dialog.setWindowTitle(QStringLiteral("工程编辑并生成 APK · 不改动原工程")); dialog.resize(640, 500); auto *layout = new QVBoxLayout(&dialog);
    auto *notice = new QLabel(QStringLiteral("在临时副本修改包名、版本、应用名和图标；自动修复资源 XML，并恢复混淆组件的原始二进制清单。\n新图标生成五档密度；可保留自适应图标背景。原工程可继续用文本/图片编辑器修改。")); notice->setWordWrap(true); layout->addWidget(notice);
    QMap<QString, QLineEdit *> inputs;
    for (const QString &key : {QStringLiteral("package"), QStringLiteral("versionCode"), QStringLiteral("versionName"), QStringLiteral("label")}) {
        const auto element = key == QStringLiteral("label") ? application : manifest;
        const QString value = element.attribute(QStringLiteral("android:") + key, element.attribute(key));
        auto *input = new QLineEdit(value); input->setProperty("original", value); inputs.insert(key, input); layout->addWidget(new QLabel(key)); layout->addWidget(input);
    }
    QString icon;
    QCheckBox preserveAdaptive(QStringLiteral("保留自适应图标背景，仅替换前景")); preserveAdaptive.setChecked(true); layout->addWidget(&preserveAdaptive);
    auto *choose = new QPushButton(QStringLiteral("选择新图标（可选）")); layout->addWidget(choose);
    connect(choose, &QPushButton::clicked, &dialog, [&icon, &dialog, choose] { const QString selected = QFileDialog::getOpenFileName(&dialog, QStringLiteral("选择图片"), {}, QStringLiteral("图片 (*.png *.webp *.jpg *.jpeg)")); if (!selected.isEmpty()) { icon = selected; choose->setText(QFileInfo(icon).fileName()); } });
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(&buttons); connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    QMap<QString, QString> fields; for (auto input = inputs.cbegin(); input != inputs.cend(); ++input) if (input.value()->text() != input.value()->property("original").toString()) fields.insert(input.key(), input.value()->text());
    const QString destination = QFileDialog::getSaveFileName(this, QStringLiteral("输出新的调试签名 APK"), source + QStringLiteral("-edited.apk"), QStringLiteral("APK (*.apk)")); if (destination.isEmpty()) return;
    if (QMessageBox::question(this, QStringLiteral("调试签名"), QStringLiteral("输出将用 TwinFile 的独立调试密钥签名，不能覆盖原作者签名的应用。继续？")) != QMessageBox::Yes) return;
    runJob(QStringLiteral("编辑工程、编译、恢复清单、对齐并签名"), [source, destination, fields, icon, preserve = preserveAdaptive.isChecked()](const Cancellation &cancel) { return buildApk(source, destination, true, cancel, fields, icon, preserve); });
}

void Window::apkInstall() {
    const QList<Entry> entries = active_->selected(); if (entries.isEmpty()) return;
    QStringList serials; for (const Entry &entry : monitor_->entries()) if (entry.state == QStringLiteral("device")) serials.append(entry.serial);
    if (serials.isEmpty()) { report(QStringLiteral("没有已授权的手机")); return; }
    bool accepted = true; const QString serial = serials.size() == 1 ? serials.first() : QInputDialog::getItem(this, QStringLiteral("选择手机"), QStringLiteral("设备"), serials, 0, false, &accepted); if (!accepted) return;
    if (QMessageBox::question(this, QStringLiteral("安装 APK"), QStringLiteral("将选中的 APK 安装到手机 %1？").arg(serial)) != QMessageBox::Yes) return;
    runJob(QStringLiteral("安装 APK"), [entries, serial](const Cancellation &cancel) {
        QTemporaryDir staging;
        if (!staging.isValid()) return Result{false, QStringLiteral("无法创建安装临时目录"), {}, {}};
        for (const Entry &entry : entries) {
            if ((entry.source != Source::Local && entry.source != Source::Archive) || entry.directory || !isApk(entry.name)) return Result{false, QStringLiteral("选择包含非本地/包内 APK"), {}, {}};
        }
        for (const Entry &entry : entries) {
            const QString staged = staging.filePath(QStringLiteral("install.apk"));
            if (entry.source == Source::Local) { if (!QFile::copy(entry.path, staged)) return Result{false, QStringLiteral("无法创建 APK 安装副本"), {}, {}}; }
            else { QTemporaryDir extracted; const Result read = materializeEntry(entry, extracted.path(), cancel); if (!read.ok) return read; if (!QFile::copy(read.paths.first(), staged)) return Result{false, QStringLiteral("无法创建包内 APK 安装副本"), {}, {}}; }
            const Result result = runProcess(toolPath(QStringLiteral("adb/adb.exe")), {QStringLiteral("-s"), serial, QStringLiteral("install"), QStringLiteral("-r"), staged}, 300000, cancel);
            QFile::remove(staged); if (!result.ok) return result;
        }
        return Result{};
    });
}

void Window::payload() {
    bool accepted = false;
    const QString method = QInputDialog::getItem(this, QStringLiteral("Payload 分区提取"), QStringLiteral("来源"), {QStringLiteral("本地 payload.bin / OTA ZIP"), QStringLiteral("远程 HTTP(S) Range")}, 0, false, &accepted);
    if (!accepted) return;
    QString source;
    PayloadOptions options;
    const bool remote = method.startsWith(QStringLiteral("远程"));
    int threads = qBound(1, settings_.value(QStringLiteral("payload_threads")).toInt(16), 512);
    QString outputDirectory = settings_.value(QStringLiteral("payload_output_directory")).toString(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (method.startsWith(QStringLiteral("本地"))) source = QFileDialog::getOpenFileName(this, QStringLiteral("选择 Payload 或 OTA ZIP"), {}, QStringLiteral("Payload / OTA (*.bin *.zip);;所有文件 (*)"));
    else {
        QDialog dialog(this); dialog.setObjectName(QStringLiteral("remotePayloadDialog")); dialog.setWindowTitle(QStringLiteral("远程 Payload · HTTP(S) Range")); dialog.resize(720, 300); auto *layout = new QVBoxLayout(&dialog);
        QLineEdit url(settings_.value(QStringLiteral("payload_last_url")).toString()); url.setObjectName(QStringLiteral("payloadUrl")); url.setPlaceholderText(QStringLiteral("https://…/payload.bin 或 OTA.zip")); layout->addWidget(&url);
        layout->addWidget(new QLabel(QStringLiteral("下载线程数量"))); QSpinBox workers; workers.setObjectName(QStringLiteral("payloadThreads")); workers.setRange(1, 512); workers.setValue(threads); layout->addWidget(&workers);
        layout->addWidget(new QLabel(QStringLiteral("保存位置")));
        QLineEdit output(outputDirectory); output.setObjectName(QStringLiteral("payloadOutputDirectory"));
        auto *outputRow = new QHBoxLayout; outputRow->addWidget(&output);
        QPushButton browse(QStringLiteral("选择位置…")); browse.setObjectName(QStringLiteral("payloadBrowse")); outputRow->addWidget(&browse); layout->addLayout(outputRow);
        connect(&browse, &QPushButton::clicked, &dialog, [&] { const QString chosen = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("选择分区输出文件夹"), output.text()); if (!chosen.isEmpty()) output.setText(chosen); });
        QCheckBox remember(QStringLiteral("记住直链（可能含令牌，将以明文保存到本机设置）")); remember.setChecked(settings_.value(QStringLiteral("payload_remember_url")).toBool()); layout->addWidget(&remember);
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(&buttons); connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        source = url.text().trimmed(); threads = workers.value();
        const QUrl parsed(source); if (!parsed.isValid() || (parsed.scheme() != QStringLiteral("http") && parsed.scheme() != QStringLiteral("https"))) { report(QStringLiteral("请输入有效的 HTTP(S) 直链")); return; }
        if (output.text().trimmed().isEmpty() || !QFileInfo(output.text().trimmed()).isDir()) { report(QStringLiteral("请选择有效的输出文件夹")); return; }
        outputDirectory = QDir::cleanPath(output.text().trimmed()); options.downloadConnections = threads;
        settings_.remove(QStringLiteral("payload_user_agent")); settings_.insert(QStringLiteral("payload_threads"), threads); settings_.insert(QStringLiteral("payload_remember_url"), remember.isChecked());
        settings_.insert(QStringLiteral("payload_output_directory"), outputDirectory);
        if (remember.isChecked()) settings_.insert(QStringLiteral("payload_last_url"), source); else settings_.remove(QStringLiteral("payload_last_url"));
        saveSettings();
    }
    if (source.isEmpty() || (method.startsWith(QStringLiteral("远程")) && !accepted)) return;
    runJob(QStringLiteral("解析 Payload 分区"), [source, options](const Cancellation &cancel, const Progress &progress) { return inspectPayload(source, cancel, options, progress); }, [this, source, options, threads, remote, outputDirectory](const Result &result) mutable {
        QDialog dialog(this); dialog.setObjectName(QStringLiteral("payloadPartitionsDialog")); dialog.setWindowTitle(QStringLiteral("选择要提取的分区")); dialog.resize(650, 550); auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(QStringLiteral("完整 OTA · 支持 REPLACE / BZ2 / XZ / ZSTD / ZERO\n输出为 .img，已有文件不会覆盖；含增量操作的 OTA 将拒绝提取。")));
        QListWidget choices; const QStringList descriptions = result.output.split(QChar('\n'), Qt::SkipEmptyParts);
        for (qsizetype index = 0; index < result.paths.size(); ++index) { auto *item = new QListWidgetItem(descriptions.value(index, result.paths[index]), &choices); item->setData(Qt::UserRole, result.paths[index]); item->setCheckState(Qt::Checked); }
        layout->addWidget(&choices); QSpinBox workers; workers.setObjectName(QStringLiteral("payloadThreads")); workers.setRange(1, remote ? 512 : 8); workers.setValue(remote ? threads : settings_.value(QStringLiteral("payload_partition_workers")).toInt(4)); layout->addWidget(new QLabel(remote ? QStringLiteral("下载线程数量") : QStringLiteral("分区并行数量"))); layout->addWidget(&workers);
        layout->addWidget(new QLabel(QStringLiteral("保存位置")));
        QLineEdit output(outputDirectory);
        output.setObjectName(QStringLiteral("payloadOutputDirectory"));
        auto *outputRow = new QHBoxLayout; outputRow->addWidget(&output);
        QPushButton browse(QStringLiteral("选择位置…")); browse.setObjectName(QStringLiteral("payloadBrowse")); outputRow->addWidget(&browse); layout->addLayout(outputRow);
        connect(&browse, &QPushButton::clicked, &dialog, [&] { const QString chosen = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("选择分区输出文件夹"), output.text()); if (!chosen.isEmpty()) output.setText(chosen); });
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(&buttons); connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return; QStringList names; for (int index = 0; index < choices.count(); ++index) if (choices.item(index)->checkState() == Qt::Checked) names.append(choices.item(index)->data(Qt::UserRole).toString()); if (names.isEmpty()) return;
        const int partitionWorkers = remote ? 4 : workers.value();
        if (remote) { options.downloadConnections = workers.value(); settings_.insert(QStringLiteral("payload_threads"), workers.value()); }
        else settings_.insert(QStringLiteral("payload_partition_workers"), workers.value());
        const QString destination = QDir::cleanPath(output.text().trimmed()); if (output.text().trimmed().isEmpty() || !QFileInfo(destination).isDir()) { report(QStringLiteral("请选择有效的输出文件夹")); return; }
        settings_.insert(QStringLiteral("payload_output_directory"), destination); saveSettings();
        runJob(QStringLiteral("提取并校验 Payload 分区"), [source, names, destination, options, partitionWorkers](const Cancellation &cancel, const Progress &progress) { return extractPayload(source, names, destination, partitionWorkers, cancel, progress, options); }, [this, destination](const Result &extracted) { taskProgress_->finish(extracted.output + QStringLiteral(" · ") + destination, true); });
    });
}

void Window::hfx() {
    if (!hfxSession_) hfxSession_ = std::make_shared<HfxSession>(); const auto session = hfxSession_;
    auto *dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowTitle(QStringLiteral("HFX USB + WiFi 多轨传输")); dialog->resize(740, 640); QPointer<QDialog> guarded(dialog);
    auto *layout = new QVBoxLayout(dialog); layout->addWidget(new QLabel(QStringLiteral("需安装配套 HybridFileXfer 手机端并授予文件权限。\nWiFi 模式是未加密的局域网传输，仅在可信网络使用；程序不会自动修改防火墙。")));
    auto *devices = new QComboBox; devices->addItem(QStringLiteral("仅 WLAN（手机端手动连接电脑 IP:5740）"), QString());
    for (const Entry &entry : monitor_->entries()) if (entry.state == QStringLiteral("device")) devices->addItem(entry.name + QStringLiteral(" · USB"), entry.serial);
    if (devices->count() > 1) devices->setCurrentIndex(1); layout->addWidget(devices);
    auto *interfaces = new QListWidget; interfaces->setMaximumHeight(100);
    for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp) || (interface.flags() & QNetworkInterface::IsLoopBack)) continue;
        for (const auto &address : interface.addressEntries()) if (address.ip().protocol() == QAbstractSocket::IPv4Protocol) {
            auto *item = new QListWidgetItem(interface.humanReadableName() + QStringLiteral(" · ") + address.ip().toString(), interfaces); item->setData(Qt::UserRole, address.ip().toString()); item->setCheckState(Qt::Unchecked);
        }
    }
    layout->addWidget(interfaces); auto *controls = new QHBoxLayout; auto *install = new QPushButton(QStringLiteral("安装配套手机端")); auto *connectButton = new QPushButton(QStringLiteral("连接")); auto *disconnect = new QPushButton(QStringLiteral("断开")); controls->addWidget(install); controls->addWidget(connectButton); controls->addWidget(disconnect); layout->addLayout(controls);
    auto *status = new QLabel(session->connected() ? QStringLiteral("已连接") : QStringLiteral("尚未连接")); status->setWordWrap(true); layout->addWidget(status);
    auto *path = new QLineEdit(session->connected() ? session->home() : QStringLiteral("/storage/emulated/0")); layout->addWidget(path);
    auto *listing = new QListWidget; listing->setSelectionMode(QAbstractItemView::ExtendedSelection); layout->addWidget(listing, 1);
    auto *actions = new QHBoxLayout; auto *refresh = new QPushButton(QStringLiteral("读取目录")); auto *push = new QPushButton(QStringLiteral("电脑所选 → 手机")); auto *pull = new QPushButton(QStringLiteral("手机所选 → 电脑")); auto *mkdir = new QPushButton(QStringLiteral("新建文件夹")); auto *remove = new QPushButton(QStringLiteral("删除"));
    for (auto *button : {refresh, push, pull, mkdir, remove}) actions->addWidget(button); layout->addLayout(actions);
    auto refreshDirectory = [this, session, guarded, path, listing, status] {
        const QString current = path->text();
        runJob(QStringLiteral("读取 HFX 目录"), [session, current](const Cancellation &cancel) { const Listing result = session->list(current, cancel); Result output; output.ok = result.error.isEmpty(); output.error = result.error; for (const auto &entry : result.entries) { QJsonObject item{{QStringLiteral("name"), entry.name}, {QStringLiteral("path"), entry.path}, {QStringLiteral("directory"), entry.directory}, {QStringLiteral("size"), double(entry.size)}}; output.paths.append(QString::fromUtf8(QJsonDocument(item).toJson(QJsonDocument::Compact))); } return output; }, [guarded, listing, status](const Result &result) {
            if (!guarded) return; listing->clear(); for (const QString &row : result.paths) { const auto object = QJsonDocument::fromJson(row.toUtf8()).object(); auto *item = new QListWidgetItem((object.value(QStringLiteral("directory")).toBool() ? QStringLiteral("📁 ") : QString()) + object.value(QStringLiteral("name")).toString(), listing); item->setData(Qt::UserRole, object); } status->setText(QStringLiteral("已读取 %1 项").arg(listing->count()));
        });
    };
    connect(refresh, &QPushButton::clicked, dialog, refreshDirectory);
    connect(path, &QLineEdit::returnPressed, dialog, refreshDirectory);
    connect(listing, &QListWidget::itemDoubleClicked, dialog, [path, refreshDirectory](QListWidgetItem *item) { const auto object = item->data(Qt::UserRole).toJsonObject(); if (object.value(QStringLiteral("directory")).toBool()) { path->setText(object.value(QStringLiteral("path")).toString()); refreshDirectory(); } });
    connect(install, &QPushButton::clicked, dialog, [this, devices] {
        const QString serial = devices->currentData().toString(); if (serial.isEmpty()) { report(QStringLiteral("安装需要选择已授权 USB 手机")); return; }
        if (QMessageBox::question(this, QStringLiteral("安装手机端"), QStringLiteral("将配套 HybridFileXfer APK 安装到手机？此操作会修改手机上已安装的应用。")) != QMessageBox::Yes) return;
        runJob(QStringLiteral("安装 HFX 手机端"), [serial](const Cancellation &cancel) { return runProcess(toolPath(QStringLiteral("adb/adb.exe")), {QStringLiteral("-s"), serial, QStringLiteral("install"), QStringLiteral("-r"), toolPath(QStringLiteral("hfx/HybridFileXfer-phone.apk"))}, 300000, cancel); });
    });
    connect(connectButton, &QPushButton::clicked, dialog, [this, session, guarded, devices, interfaces, status, path] {
        HfxOptions options; options.serial = devices->currentData().toString(); if (!options.serial.isEmpty()) options.interfaces.append({QStringLiteral("USB"), QHostAddress::LocalHost});
        for (int index = 0; index < interfaces->count(); ++index) if (interfaces->item(index)->checkState() == Qt::Checked) options.interfaces.append({interfaces->item(index)->text(), QHostAddress(interfaces->item(index)->data(Qt::UserRole).toString())});
        if (options.interfaces.isEmpty()) { report(QStringLiteral("请选择 USB 手机或至少一个 WLAN 通道")); return; }
        status->setText(QStringLiteral("等待手机连接；请解锁手机。WLAN 手动连接电脑 IP:5740。"));
        runJob(QStringLiteral("等待 HFX 手机端连接（最长 90 秒，可取消）"), [session, options](const Cancellation &cancel) { return session->connect(options, cancel); }, [session, guarded, status, path](const Result &result) { if (guarded) { status->setText(result.output); path->setText(session->home()); } });
    });
    connect(disconnect, &QPushButton::clicked, dialog, [this, session, guarded, status] { runJob(QStringLiteral("断开 HFX"), [session](const Cancellation &) { session->close(); return Result{}; }, [guarded, status](const Result &) { if (guarded) status->setText(QStringLiteral("已断开")); }); });
    connect(push, &QPushButton::clicked, dialog, [this, session, path, dialog] { QStringList files = localPaths(active_->selected()); if (files.isEmpty()) files = localPaths(otherPane()->selected()); if (files.isEmpty()) { report(QStringLiteral("请先在文件栏选择本地文件")); return; } const QString target = path->text(); if (busy_ || taskProgress_->active()) return; dialog->hide(); runJob(QStringLiteral("HFX 推送到手机"), [session, files, target](const Cancellation &cancel, const Progress &progress) { return session->push(files, target, cancel, progress); }); });
    connect(pull, &QPushButton::clicked, dialog, [this, session, path, listing, dialog] { QStringList files; for (const auto *item : listing->selectedItems()) files.append(item->data(Qt::UserRole).toJsonObject().value(QStringLiteral("path")).toString()); if (files.isEmpty()) return; const QString target = QFileDialog::getExistingDirectory(this, QStringLiteral("接收到电脑文件夹")); if (target.isEmpty()) return; const QString source = path->text(); if (busy_ || taskProgress_->active()) return; dialog->hide(); runJob(QStringLiteral("HFX 接收手机文件"), [session, files, source, target](const Cancellation &cancel, const Progress &progress) { return session->pull(files, source, target, cancel, progress); }); });
    connect(mkdir, &QPushButton::clicked, dialog, [this, session, path] { bool accepted; const QString name = QInputDialog::getText(this, QStringLiteral("手机新建文件夹"), QStringLiteral("名称"), QLineEdit::Normal, {}, &accepted); if (!accepted) return; const QString parent = path->text(); runJob(QStringLiteral("HFX 新建文件夹"), [session, parent, name](const Cancellation &cancel) { return session->mkdir(parent, name, cancel); }); });
    connect(remove, &QPushButton::clicked, dialog, [this, session, listing] { QStringList files; for (const auto *item : listing->selectedItems()) files.append(item->data(Qt::UserRole).toJsonObject().value(QStringLiteral("path")).toString()); if (files.isEmpty() || QMessageBox::question(this, QStringLiteral("永久删除"), QStringLiteral("永久删除手机所选项目？不能撤销。")) != QMessageBox::Yes) return; runJob(QStringLiteral("HFX 删除"), [session, files](const Cancellation &cancel) { for (const QString &path : files) { const Result result = session->remove(path, cancel); if (!result.ok) return result; } return Result{}; }); });
    dialog->show();
}

void Window::menu(const QPoint &position) {
    QMenu popup(this);
    popup.addAction(QStringLiteral("打开"), this, [this] { const auto entries = active_->selected(); if (entries.size() == 1) open(entries.first()); });
    popup.addAction(QStringLiteral("快速预览（空格）"), this, &Window::preview);
    const auto selected = active_->selected();
    if (selected.size() == 1 && selected.first().source == Source::Local && !batchPlan(selected.first().path).program.isEmpty()) popup.addAction(QStringLiteral("运行脚本（内置 ADB / Fastboot，新控制台）"), this, [this, path = selected.first().path] { if (QMessageBox::warning(this, QStringLiteral("运行本地脚本"), QStringLiteral("只运行可信脚本。脚本可执行任意命令，包括刷机或删除文件。继续？"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return; const Result result = launchBatch(path); if (!result.ok) report(result.error); });
    popup.addSeparator();
    popup.addAction(QStringLiteral("复制"), this, [this] { transfer(false, true); }); popup.addAction(QStringLiteral("剪切"), this, [this] { transfer(true, true); }); popup.addAction(QStringLiteral("粘贴"), this, &Window::paste);
    popup.addAction(QStringLiteral("重命名"), this, [this] { rename(false); }); popup.addAction(QStringLiteral("批量重命名"), this, [this] { rename(true); }); popup.addAction(QStringLiteral("移到回收站"), this, &Window::remove);
    popup.addSeparator();
    popup.addAction(QStringLiteral("压缩为 ZIP"), this, &Window::compress); popup.addAction(QStringLiteral("解压 ZIP/APK"), this, &Window::extract);
    auto *apk = popup.addMenu(QStringLiteral("APK 工具")); apk->addAction(QStringLiteral("快速修改清单并签名"), this, &Window::apkEdit); apk->addAction(QStringLiteral("反编译"), this, &Window::apkDecompile); apk->addAction(QStringLiteral("回编译并签名"), this, &Window::apkBuild); apk->addAction(QStringLiteral("编辑工程 / 更换图标并生成 APK"), this, &Window::apkProjectEdit); apk->addAction(QStringLiteral("安装到手机"), this, &Window::apkInstall);
    popup.addAction(QStringLiteral("校验值"), this, &Window::hashes); popup.addAction(QStringLiteral("属性"), this, &Window::properties);
    popup.addSeparator(); popup.addAction(QStringLiteral("刷新当前栏"), this, &Window::refreshActive); popup.exec(position);
}

void Window::moreMenu() {
    QMenu popup(this);
    popup.addAction(QStringLiteral("直链下载…"), this, &Window::showDownload);
    popup.addSeparator();
    auto *settings = popup.addMenu(QStringLiteral("设置"));
    settings->addAction(QStringLiteral("外观…"), this, &Window::showAppearanceSettings);
    settings->addAction(QStringLiteral("软件更新…"), this, [this] { showUpdateSettings(); });
    settings->addAction(QStringLiteral("MCP 服务…"), this, &Window::showMcpSettings);
    popup.addSeparator();
    popup.addAction(QStringLiteral("AI / 扩展侧栏  Ctrl+Shift+A"), this, [this] { extensionDock_->show(); extensionDock_->raise(); });
    popup.addAction(QStringLiteral("管理插件"), this, [this] { extensionDock_->show(); extensions_->showExtensions(); });
    popup.addSeparator();
    popup.addAction(QStringLiteral("刷新当前栏"), this, &Window::refreshActive);
    popup.addAction(QStringLiteral("立即检测手机"), monitor_, &DeviceMonitor::detect);
    popup.addAction(QStringLiteral("新建文件"), this, [this] { create(false); }); popup.addAction(QStringLiteral("新建文件夹"), this, [this] { create(true); });
    popup.addAction(QStringLiteral("比较文本（左右各选一项）"), this, [this] { compare(false); }); popup.addAction(QStringLiteral("比较目录"), this, [this] { compare(true); });
    popup.addAction(QStringLiteral("导入并验证校验清单"), this, [this] { checksumList(true); });
    popup.addAction(QStringLiteral("粘贴校验值并验证所选文件"), this, &Window::verifyHash);
    popup.addAction(QStringLiteral("导出所选文件校验清单"), this, [this] { checksumList(false); });
    auto *theme = popup.addMenu(QStringLiteral("外观"));
    theme->addAction(QStringLiteral("外观设置…"), this, &Window::showAppearanceSettings);
    theme->addSeparator(); auto *themeGroup = new QActionGroup(theme);
    for (const QString &style : themeIds()) {
        auto *action = theme->addAction(themeName(style), this, [this, style] { setTheme(style); }); action->setCheckable(true); action->setChecked(settings_.value(QStringLiteral("theme")).toString() == style); themeGroup->addAction(action);
    }
    theme->addSeparator(); auto *modeGroup = new QActionGroup(theme);
    for (const QString &mode : {QStringLiteral("light"), QStringLiteral("dark")}) {
        auto *action = theme->addAction(mode == QStringLiteral("light") ? QStringLiteral("亮色模式") : QStringLiteral("暗色模式"), this, [this, mode] { setColorMode(mode); }); action->setCheckable(true); action->setChecked(settings_.value(QStringLiteral("color_mode")).toString() == mode); modeGroup->addAction(action);
    }
    theme->addSeparator();
    auto *accent = theme->addMenu(QStringLiteral("强调色"));
    for (const QString &name : {QStringLiteral("blue"), QStringLiteral("teal"), QStringLiteral("green"), QStringLiteral("purple"), QStringLiteral("orange"), QStringLiteral("indigo"), QStringLiteral("mtblue")}) accent->addAction(name, this, [this, name] { settings_.insert(QStringLiteral("accent"), name); setTheme(settings_.value(QStringLiteral("theme")).toString()); });
    auto *motion = theme->addAction(QStringLiteral("减少动画")); motion->setCheckable(true); motion->setChecked(settings_.value(QStringLiteral("reduced_motion")).toBool()); connect(motion, &QAction::toggled, this, &Window::setReducedMotion);
    auto *sorting = popup.addMenu(QStringLiteral("排序"));
    auto applyOptions = [this] { for (Pane *pane : {left_, right_}) pane->setOptions(settings_.value(QStringLiteral("show_hidden")).toBool(), settings_.value(QStringLiteral("sort")).toString(QStringLiteral("name")), settings_.value(QStringLiteral("descending")).toBool()); };
    for (const QString &sort : {QStringLiteral("name"), QStringLiteral("size"), QStringLiteral("mtime")}) sorting->addAction(sort, this, [this, sort, applyOptions] { settings_.insert(QStringLiteral("sort"), sort); applyOptions(); });
    auto *descending = sorting->addAction(QStringLiteral("降序")); descending->setCheckable(true); descending->setChecked(settings_.value(QStringLiteral("descending")).toBool()); connect(descending, &QAction::toggled, this, [this, applyOptions](bool checked) { settings_.insert(QStringLiteral("descending"), checked); applyOptions(); });
    auto *hidden = popup.addAction(QStringLiteral("显示隐藏文件")); hidden->setCheckable(true); hidden->setChecked(settings_.value(QStringLiteral("show_hidden")).toBool());
    connect(hidden, &QAction::toggled, this, [this, applyOptions](bool checked) { settings_.insert(QStringLiteral("show_hidden"), checked); applyOptions(); });
    popup.addAction(QStringLiteral("收藏当前文件夹"), this, [this] {
        if (active_->location().source != Source::Local) return;
        QJsonArray favorites = settings_.value(QStringLiteral("favorites")).toArray(); const QString path = active_->location().path;
        if (!favorites.contains(path)) favorites.append(path);
        settings_.insert(QStringLiteral("favorites"), favorites); statusBar()->showMessage(QStringLiteral("已收藏，下次启动显示"), 5000);
    });
    popup.addAction(QStringLiteral("导入旧版设置 / 工作区"), this, [this] {
        const QString source = QFileDialog::getOpenFileName(this, QStringLiteral("选择旧版 settings.json；只读取，不修改旧文件"), QDir(qEnvironmentVariable("APPDATA", QDir::homePath())).filePath(QStringLiteral("WinMT/settings.json")), QStringLiteral("JSON (*.json)")); if (source.isEmpty()) return;
        const auto legacy = readSettings(source); if (legacy.isEmpty()) { report(QStringLiteral("配置为空、格式不正确或超过大小上限")); return; }
        if (QMessageBox::question(this, QStringLiteral("导入设置"), QStringLiteral("替换当前原生版设置与工作区？旧版配置文件不变；收藏列表重启后显示。")) != QMessageBox::Yes) return;
        settings_ = migrateLegacySettings(legacy); const auto workspace = settings_.value(QStringLiteral("workspace")).toObject();
        setReducedMotion(settings_.value(QStringLiteral("reduced_motion")).toBool());
        left_->restore(workspace.value(QStringLiteral("left")).toObject()); right_->restore(workspace.value(QStringLiteral("right")).toObject()); setActive(workspace.value(QStringLiteral("active")).toString() == QStringLiteral("right") ? right_ : left_); setTheme(settings_.value(QStringLiteral("theme")).toString()); for (Pane *pane : {left_, right_}) pane->setOptions(settings_.value(QStringLiteral("show_hidden")).toBool(), settings_.value(QStringLiteral("sort")).toString(), settings_.value(QStringLiteral("descending")).toBool()); QString error; if (!writeSettings(settingsPath(), settings_, &error)) report(error);
    });
    popup.addSeparator();
    popup.addAction(QStringLiteral("本地 / 远程 Payload 分区提取"), this, &Window::payload);
    popup.addAction(QStringLiteral("HFX USB + WiFi 多轨传输"), this, &Window::hfx);
    popup.addAction(QStringLiteral("关于 / 原生迁移说明"), this, [this] { showText(QStringLiteral("TwinFile C++ 原生版"), QStringLiteral("业务代码为 C++20 / Qt 6，不包含或调用 Python 后端。\n\n包含双栏、多标签、文件操作、原生 ADB 同步、HFX 多轨、本地和远程 Payload、ZIP64 编辑与嵌套回写、编码保留文本编辑、图片编辑、媒体与歌词、文本/目录比较、校验清单、APK 清单/工程编辑与签名。\n\n这是重新实现，不是逐像素复刻。复杂 Android drawable、增量 OTA 等仍有明确边界；手机、QQ 收件、在线歌词服务和安装升级需要实机验收。自动测试不等于全部旧版行为已人工确认。\n\n安装包与旧版并行安装，配置独立；不会自动删除旧工程或覆盖旧 3.1 安装包。详细说明见 MIGRATION.md 和 THIRD_PARTY.md。")); });
    popup.exec(QCursor::pos());
}

bool Window::eventFilter(QObject *object, QEvent *event) {
    if (object->objectName() == QStringLiteral("workspaceToolbar") && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest))
        adaptToolbar();
    if ((object == sidebar_ || object == deviceNavigation_ || object == centralWidget() || object->objectName() == QStringLiteral("navigationLocations")) && event->type() == QEvent::MouseButtonPress) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::NoModifier && !static_cast<QWidget *>(object)->childAt(mouse->position().toPoint())) active_->deselect();
    }
    return QMainWindow::eventFilter(object, event);
}

void Window::adaptToolbar() {
    auto *toolbar = findChild<QWidget *>(QStringLiteral("workspaceToolbar"));
    if (!toolbar) return;
    const bool compact = toolbar->width() < (taskProgress_ && !taskProgress_->isHidden() ? 900 : 570);
    for (auto *button : toolbar->findChildren<QPushButton *>(QString(), Qt::FindDirectChildrenOnly)) {
        const QString text = button->property("toolbarText").toString();
        if (text.isEmpty()) continue;
        button->setText(compact ? QString() : text);
        button->setFixedWidth(compact ? 32 : button->sizeHint().width());
    }
}

void Window::saveSettings() {
    const auto persisted = readSettings(settingsPath());
    if (persisted.contains(QStringLiteral("player_order"))) settings_.insert(QStringLiteral("player_order"), persisted.value(QStringLiteral("player_order")));
    settings_.insert(QStringLiteral("geometry"), QString::fromLatin1(saveGeometry().toHex()));
    settings_.insert(QStringLiteral("extension_visible"), extensionDock_->isVisible());
    settings_.insert(QStringLiteral("extension_width"), extensionDock_->width());
    settings_.insert(QStringLiteral("workspace"), QJsonObject{{QStringLiteral("left"), left_->workspace()}, {QStringLiteral("right"), right_->workspace()}, {QStringLiteral("active"), active_ == left_ ? QStringLiteral("left") : QStringLiteral("right")}});
    QString error; if (!writeSettings(settingsPath(), settings_, &error)) statusBar()->showMessage(QStringLiteral("保存设置失败：") + error);
}
void Window::closeEvent(QCloseEvent *event) {
    if (mcp_) { mcp_->stop(); if (mcp_->busy()) { statusBar()->showMessage(QStringLiteral("正在停止 MCP 操作，请稍后再次关闭")); event->ignore(); return; } }
    if (busy_) { if (cancellation_) cancellation_->store(true); statusBar()->showMessage(QStringLiteral("正在取消操作，请完成后再次关闭")); event->ignore(); return; }
    if (taskProgress_->active()) { taskProgress_->cancel(); statusBar()->showMessage(QStringLiteral("正在取消下载，请完成后再次关闭")); event->ignore(); return; }
    for (auto *dialog : findChildren<QDialog *>(QString(), Qt::FindDirectChildrenOnly)) { if (!dialog->isVisible()) continue; dialog->close(); if (dialog->isVisible()) { event->ignore(); return; } }
    closing_ = true; updates_->cancel(); saveSettings(); QMainWindow::closeEvent(event);
}

}
