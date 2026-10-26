#include "pane.h"
#include "shell.h"
#include "android.h"
#include "thumbnails.h"
#include "appearance.h"
#include "motion.h"

#include <QEvent>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QImageReader>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>

namespace twinfile {

class RefreshButton : public QToolButton {
public:
    explicit RefreshButton(QWidget *parent = nullptr) : QToolButton(parent) {
        setObjectName(QStringLiteral("refreshCurrentPane")); setAccessibleName(QStringLiteral("刷新当前栏"));
        setToolTip(QStringLiteral("刷新当前栏 F2 / Ctrl+R")); setAutoRaise(true); setFixedSize(30, 30);
        rotation_.setObjectName(QStringLiteral("refreshRotation"));
        rotation_.setStartValue(0.0); rotation_.setEndValue(360.0); rotation_.setDuration(850); rotation_.setLoopCount(-1);
        connect(&rotation_, &QVariantAnimation::valueChanged, this, [this] { update(); });
    }
    void setBusy(bool busy) { busy_ = busy; updateMotion(); }
    void setAnimationsEnabled(bool enabled) { animationsEnabled_ = enabled; updateMotion(); }
protected:
    void paintEvent(QPaintEvent *event) override {
        QToolButton::paintEvent(event);
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); painter.translate(rect().center());
        if (rotation_.state() == QAbstractAnimation::Running) painter.rotate(rotation_.currentValue().toReal());
        painter.setPen(QPen(palette().buttonText().color(), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawArc(QRectF(-6, -6, 12, 12), 0, 285 * 16);
        QPainterPath arrow; arrow.moveTo(3, -3); arrow.lineTo(6, 0); arrow.lineTo(9, -3); painter.drawPath(arrow);
    }
    void hideEvent(QHideEvent *event) override { rotation_.stop(); QToolButton::hideEvent(event); }
    void showEvent(QShowEvent *event) override { QToolButton::showEvent(event); updateMotion(); }
private:
    void updateMotion() {
        if (busy_ && animationsEnabled_ && isVisible()) { if (rotation_.state() != QAbstractAnimation::Running) rotation_.start(); }
        else rotation_.stop();
        update();
    }
    QVariantAnimation rotation_{this};
    bool busy_ = false;
    bool animationsEnabled_ = true;
};

class ParentButton : public QToolButton {
public:
    using QToolButton::QToolButton;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        if (underMouse() || hasFocus()) painter.fillRect(rect(), palette().alternateBase());
        icon().paint(&painter, QRect(8, (height() - 40) / 2, 40, 40));
        painter.setPen(palette().text().color()); painter.setFont(font());
        painter.drawText(QRect(60, 0, width() - 68, height()), Qt::AlignLeft | Qt::AlignVCenter, text());
    }
};

static FileView::Transition navigationTransition(const Location &from, const Location &to, FileView::Transition fallback) {
    if (from == to) return FileView::Transition::Refresh;
    if (to.source == Source::Drive) return FileView::Transition::LeaveFolder;
    if (from.source == Source::Drive) return FileView::Transition::EnterFolder;
    if (from.source != to.source) return fallback;
    if (from.source != Source::Local && from.path != to.path) return fallback;
    QString source = QDir::cleanPath(QDir::fromNativeSeparators(from.source == Source::Local ? from.path : from.inner));
    QString destination = QDir::cleanPath(QDir::fromNativeSeparators(to.source == Source::Local ? to.path : to.inner));
    if (source == QStringLiteral(".")) source.clear();
    if (destination == QStringLiteral(".")) destination.clear();
    if (from.source == Source::Archive) {
        if (!source.startsWith(QChar('/'))) source.prepend(QChar('/'));
        if (!destination.startsWith(QChar('/'))) destination.prepend(QChar('/'));
    }
    if (!source.endsWith(QChar('/'))) source += QChar('/');
    if (!destination.endsWith(QChar('/'))) destination += QChar('/');
    const Qt::CaseSensitivity sensitivity = from.source == Source::Local ? Qt::CaseInsensitive : Qt::CaseSensitive;
    if (source.startsWith(destination, sensitivity)) return FileView::Transition::LeaveFolder;
    if (destination.startsWith(source, sensitivity)) return FileView::Transition::EnterFolder;
    return fallback;
}

Pane::Pane(QWidget *parent) : QFrame(parent) {
    setObjectName(QStringLiteral("filePane"));
    setMinimumWidth(260);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(10, 10, 10, 0); layout->setSpacing(6);
    auto *miuixHeading = new QWidget; miuixHeading->setObjectName(QStringLiteral("miuixPaneHeading"));
    auto *miuixLayout = new QVBoxLayout(miuixHeading); miuixLayout->setContentsMargins(6, 6, 6, 8); miuixLayout->setSpacing(3);
    for (const QString &name : {QStringLiteral("miuixFolderTitle"), QStringLiteral("miuixFolderDetails")}) {
        auto *label = new QLabel; label->setObjectName(name); label->setMinimumWidth(0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); miuixLayout->addWidget(label);
    }
    layout->addWidget(miuixHeading); miuixHeading->hide(); miuixHeading->installEventFilter(this);
    auto *chrome = new QWidget; chrome->setObjectName(QStringLiteral("paneChrome"));
    auto *chromeLayout = new QVBoxLayout(chrome); chromeLayout->setContentsMargins(0, 0, 0, 0); chromeLayout->setSpacing(6);
    auto *tabLayout = new QHBoxLayout; tabLayout->setSpacing(4);
    auto *indicator = new QLabel(QStringLiteral("●")); indicator->setObjectName(QStringLiteral("paneIndicator")); indicator->setToolTip(QStringLiteral("点击此栏后，操作作用于此栏")); tabLayout->addWidget(indicator);
    tabs_ = new MotionTabBar; tabs_->setObjectName(QStringLiteral("paneTabs")); tabs_->setDrawBase(false); tabs_->setExpanding(false); tabs_->setTabsClosable(true); tabs_->setUsesScrollButtons(true); tabs_->setElideMode(Qt::ElideMiddle); tabs_->addTab(QStringLiteral("此电脑"));
    auto *add = new QToolButton; add->setProperty("symbol", QStringLiteral("add")); add->setIcon(actionSymbol(QStringLiteral("add"), palette().color(QPalette::Text))); add->setFixedSize(28, 28); add->setToolTip(QStringLiteral("新标签 Ctrl+T")); add->setAccessibleName(QStringLiteral("新建标签页"));
    tabLayout->addWidget(tabs_, 1); tabLayout->addWidget(add); chromeLayout->addLayout(tabLayout);
    auto *navigation = new QHBoxLayout; navigation->setSpacing(2);
    auto addNavigation = [&](const QString &text, const QString &tip, auto handler) {
        auto *button = new QToolButton; button->setProperty("symbol", text); button->setIcon(actionSymbol(text, palette().color(QPalette::Text))); button->setToolTip(tip); button->setAccessibleName(tip); button->setFixedSize(28, 30); button->setAutoRaise(true);
        connect(button, &QToolButton::clicked, this, handler); navigation->addWidget(button);
    };
    addNavigation(QStringLiteral("back"), QStringLiteral("后退 Alt+Left"), &Pane::goBack);
    addNavigation(QStringLiteral("forward"), QStringLiteral("前进 Alt+Right"), &Pane::goForward);
    addNavigation(QStringLiteral("up"), QStringLiteral("上一级 Backspace"), &Pane::goUp);
    refresh_ = new RefreshButton; navigation->addWidget(refresh_); connect(refresh_, &QToolButton::clicked, this, [this] { emit activated(this); refresh(); });
    path_ = new QLineEdit(QStringLiteral("此电脑")); path_->setObjectName(QStringLiteral("panePath")); path_->setClearButtonEnabled(true); path_->setMinimumWidth(70); path_->setAccessibleName(QStringLiteral("当前路径，可输入地址跳转"));
    navigation->addWidget(path_, 1); chromeLayout->addLayout(navigation); layout->addWidget(chrome);
    auto *columns = new QWidget; columns->setObjectName(QStringLiteral("paneColumns")); auto *columnLayout = new QHBoxLayout(columns); columnLayout->setContentsMargins(12, 4, 12, 4);
    auto *nameColumn = new QLabel(QStringLiteral("名称")); nameColumn->setObjectName(QStringLiteral("paneColumnLabel")); columnLayout->addWidget(nameColumn); columnLayout->addStretch();
    auto *sourceColumn = new QLabel(QStringLiteral("位置")); sourceColumn->setObjectName(QStringLiteral("paneSourceLabel")); columnLayout->addWidget(sourceColumn); layout->addWidget(columns);
    auto *parentEntry = new ParentButton; parentEntry->setObjectName(QStringLiteral("mtParent")); parentEntry->setText(QStringLiteral(".."));
    parentEntry->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); parentEntry->setIconSize(QSize(40, 40)); parentEntry->setFixedHeight(50);
    parentEntry->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); parentEntry->setToolTip(QStringLiteral("上一级")); parentEntry->setAccessibleName(parentEntry->toolTip());
    connect(parentEntry, &QToolButton::clicked, this, &Pane::goUp); layout->addWidget(parentEntry); parentEntry->hide();
    model_ = new FileModel(this); view_ = new FileView; view_->setModel(model_); layout->addWidget(view_, 1);
    blank_ = new QWidget; blank_->setFixedHeight(16); blank_->setObjectName(QStringLiteral("deselectStrip")); blank_->installEventFilter(this); layout->addWidget(blank_);
    footer_ = new QLabel; footer_->setObjectName(QStringLiteral("paneFooter")); footer_->setMinimumHeight(24); layout->addWidget(footer_);
    for (QWidget *widget : {static_cast<QWidget *>(path_), static_cast<QWidget *>(tabs_), static_cast<QWidget *>(footer_)}) widget->installEventFilter(this);
    states_.append(Tab{{}, {Location()}, 0, 0, {}, {}});
    connect(add, &QToolButton::clicked, this, &Pane::newTab);
    connect(tabs_, &QTabBar::tabCloseRequested, this, [this](int index) { tabs_->setCurrentIndex(index); closeTab(); });
    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (switching_ || index < 0 || index >= states_.size()) return;
        saveView(); tab_ = index; refresh(false); emit activated(this);
    });
    connect(view_, &FileView::activatedPane, this, [this] { emit activated(this); });
    connect(view_, &FileView::previewRequested, this, &Pane::previewRequested);
    connect(view_, &QListView::activated, this, [this](const QModelIndex &index) { if (!view_->hasFocus()) return; if (index.isValid()) openIndex(index); });
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &Pane::updateFooter);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view_, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        emit activated(this);
        const QModelIndex index = view_->indexAt(position);
        if (index.isValid() && !view_->selectionModel()->isSelected(index)) view_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
        emit menuRequested(view_->viewport()->mapToGlobal(position));
    });
    connect(path_, &QLineEdit::returnPressed, this, [this] {
        if (!navigatePath(path_->text())) path_->setToolTip(QStringLiteral("目录不存在或路径无效"));
    });
    thumbnailTimer_.setSingleShot(true); thumbnailTimer_.setInterval(16);
    connect(&thumbnailTimer_, &QTimer::timeout, this, &Pane::loadThumbnails);
    connect(view_->verticalScrollBar(), &QScrollBar::valueChanged, this, &Pane::scheduleThumbnails);
    view_->viewport()->installEventFilter(this);
    installEventFilter(this);
    for (QToolButton *button : findChildren<QToolButton *>()) button->installEventFilter(this);
    refresh(false);
}

Pane::~Pane() { if (searchCancel_) searchCancel_->store(true); if (thumbnailCancel_) thumbnailCancel_->store(true); }
Location Pane::location() const { return states_[tab_].location; }

QList<Entry> Pane::selected() const {
    QList<Entry> entries;
    for (const QModelIndex &index : view_->selectionModel()->selectedRows()) entries.append(model_->entry(index.row()));
    return entries;
}

void Pane::saveView() {
    if (tab_ < 0 || tab_ >= states_.size()) return;
    Tab &state = states_[tab_]; state.selection.clear();
    for (const Entry &entry : selected()) state.selection.insert(entry.key());
    state.current = view_->currentIndex().isValid() ? model_->entry(view_->currentIndex().row()).key() : QString();
    state.scroll = view_->verticalScrollBar()->value();
}

void Pane::apply(QList<Entry> entries, bool restoreView) {
    std::stable_sort(entries.begin(), entries.end(), [this](const Entry &first, const Entry &second) {
        if (first.source == Source::Device && second.source != Source::Device) return true;
        if (first.source != Source::Device && second.source == Source::Device) return false;
        if (first.directory != second.directory) return first.directory;
        int order = sort_ == QStringLiteral("size") && first.size != second.size ? (first.size < second.size ? -1 : 1) : sort_ == QStringLiteral("mtime") && first.modified != second.modified ? (first.modified < second.modified ? -1 : 1) : QString::compare(first.name, second.name, Qt::CaseInsensitive);
        return descending_ ? order > 0 : order < 0;
    });
    model_->replace(std::move(entries));
    if (restoreView) {
        const QSignalBlocker blocker(view_->selectionModel());
        const Tab &state = states_[tab_];
        for (int row = 0; row < model_->rowCount(); ++row) {
            const QString key = model_->entry(row).key();
            if (state.selection.contains(key)) view_->selectionModel()->select(model_->index(row), QItemSelectionModel::Select);
            if (state.current == key) view_->selectionModel()->setCurrentIndex(model_->index(row), QItemSelectionModel::NoUpdate);
        }
        view_->verticalScrollBar()->setValue(state.scroll);
    }
    updateFooter(); scheduleThumbnails();
}

void Pane::refresh(bool preserve) { reload(preserve, FileView::Transition::Refresh); }

void Pane::reload(bool preserve, FileView::Transition transition) {
    view_->stopTransition();
    if (!view_->isVisible() || !view_->animationsEnabled()) transition = FileView::Transition::None;
    refresh_->setBusy(true);
    if (searchCancel_) searchCancel_->store(true);
    if (thumbnailCancel_) thumbnailCancel_->store(true);
    thumbnailCancel_ = std::make_shared<std::atomic_bool>(false);
    if (preserve) saveView();
    ++generation_; thumbnailsRequested_.clear(); thumbnailsFailed_.clear(); thumbnailAttempts_.clear(); error_.clear();
    const quint64 generation = generation_;
    const Location current = location();
    path_->setText(current.display());
    findChild<QToolButton *>(QStringLiteral("mtParent"))->setVisible(property("mtStyle").toBool() && current.source != Source::Drive);
    path_->setToolTip(current.display());
    findChild<QLabel *>(QStringLiteral("paneSourceLabel"))->setText(current.source == Source::Local ? QStringLiteral("本地文件") : current.source == Source::Phone ? QStringLiteral("Android 设备") : current.source == Source::Archive ? QStringLiteral("压缩包") : QStringLiteral("驱动器与设备"));
    emit locationChanged();
    const QString label = current.source == Source::Drive ? QStringLiteral("此电脑")
        : current.source == Source::Phone ? (current.label.isEmpty() ? current.path : current.label)
        : current.source == Source::Archive ? QFileInfo(current.path).fileName() : QFileInfo(current.path).fileName();
    tabs_->setTabText(tab_, label.isEmpty() ? current.path : label); tabs_->setTabToolTip(tab_, current.display());
    footer_->setText(QStringLiteral("正在读取…"));
    if (!preserve) apply({}, false);
    const bool hidden = hidden_, descending = descending_; const QString sort = sort_;
    auto *watcher = new QFutureWatcher<Listing>(this);
    connect(watcher, &QFutureWatcher<Listing>::finished, this, [this, watcher, generation, current, transition] {
        const Listing result = watcher->result(); watcher->deleteLater();
        if (generation != generation_ || !(current == location())) return;
        refresh_->setBusy(false);
        error_ = result.error;
        QList<Entry> entries = result.entries;
        if (current.source == Source::Drive) entries = devices_ + entries;
        apply(entries, true);
        view_->startTransition(transition);
    });
    watcher->setFuture(QtConcurrent::run([current, hidden, sort, descending] {
        if (current.source == Source::Archive) return listArchive(current.path, current.inner);
        if (current.source == Source::Phone) return listPhone(current.path, current.inner, hidden);
        return listLocal(current.source == Source::Drive ? QString() : current.path, hidden, sort, descending);
    }));
}

void Pane::setDevices(const QList<Entry> &devices) {
    devices_ = devices;
    if (location().source != Source::Drive) return;
    saveView(); QList<Entry> entries = devices;
    for (const Entry &entry : model_->entries()) if (entry.source != Source::Device) entries.append(entry);
    apply(entries, true);
}

void Pane::setOptions(bool hidden, QString sort, bool descending) {
    hidden_ = hidden; sort_ = std::move(sort); descending_ = descending; refresh();
}

void Pane::setAnimationsEnabled(bool enabled) { view_->setAnimationsEnabled(enabled); refresh_->setAnimationsEnabled(enabled); }

void Pane::setMtStyle(bool enabled) {
    setProperty("mtStyle", enabled);
    layout()->setContentsMargins(enabled ? 0 : 10, enabled ? 0 : 10, enabled ? 0 : 10, 0);
    layout()->setSpacing(enabled ? 0 : 6);
    findChild<QWidget *>(QStringLiteral("paneChrome"))->setVisible(!enabled);
    findChild<QWidget *>(QStringLiteral("paneColumns"))->setVisible(!enabled);
    auto *parentEntry = findChild<QToolButton *>(QStringLiteral("mtParent"));
    Entry folder; folder.directory = true; parentEntry->setIcon(entryIcon(folder));
    parentEntry->setVisible(enabled && location().source != Source::Drive);
    blank_->setVisible(!enabled);
    view_->stopTransition(); view_->doItemsLayout(); view_->viewport()->update();
}

void Pane::setMiuixStyle(bool enabled) {
    setProperty("miuixStyle", enabled);
    findChild<QWidget *>(QStringLiteral("miuixPaneHeading"))->setVisible(enabled);
    if (enabled) {
        layout()->setContentsMargins(12, 10, 12, 0);
        layout()->setSpacing(8);
        findChild<QWidget *>(QStringLiteral("paneColumns"))->hide();
        blank_->hide();
    } else {
        layout()->setContentsMargins(10, 10, 10, 0);
        layout()->setSpacing(6);
        const bool mt = property("mtStyle").toBool();
        findChild<QWidget *>(QStringLiteral("paneColumns"))->setVisible(!mt);
        blank_->setVisible(!mt);
    }
    updateMiuixHeading();
    view_->doItemsLayout(); view_->viewport()->update();
}

void Pane::updateMiuixHeading() {
    if (!property("miuixStyle").toBool()) return;
    const Location current = location();
    const QString name = current.source == Source::Drive ? QStringLiteral("此电脑")
        : current.source == Source::Phone ? current.inner.section('/', -1)
        : current.source == Source::Archive && !current.inner.isEmpty() ? current.inner.section('/', -1)
        : QFileInfo(current.path).fileName();
    auto *title = findChild<QLabel *>(QStringLiteral("miuixFolderTitle"));
    const QString text = name.isEmpty() ? current.display() : name;
    title->setText(title->fontMetrics().elidedText(text, Qt::ElideMiddle, qMax(30, width() - 34)));
    title->setToolTip(current.display());
    int directories = 0;
    for (const Entry &entry : model_->entries()) if (entry.directory) ++directories;
    const int selection = view_->selectionModel()->selectedRows().size();
    const QString details = selection ? QStringLiteral("已选择 %1 项").arg(selection)
        : QStringLiteral("%1 个文件夹 · %2 个文件").arg(directories).arg(model_->rowCount() - directories);
    findChild<QLabel *>(QStringLiteral("miuixFolderDetails"))->setText(details);
}

bool Pane::canGoBack() const { return states_[tab_].historyIndex > 0; }
bool Pane::canGoForward() const { return states_[tab_].historyIndex + 1 < states_[tab_].history.size(); }

bool Pane::navigatePath(const QString &text) {
    QString path = QDir::fromNativeSeparators(text.trimmed());
    if (path.contains(QChar(0))) return false;
    if (path.isEmpty() || path == QStringLiteral("此电脑")) { navigate({}); return true; }
    const Location current = location();
    const QString phonePrefix = current.path + QChar(':');
    if (current.source == Source::Phone && (path.startsWith(phonePrefix) || (path.startsWith(QChar('/')) && !path.startsWith(QStringLiteral("//"))))) {
        const QString inner = QDir::cleanPath(path.startsWith(phonePrefix) ? path.mid(phonePrefix.size()) : path);
        if (!inner.startsWith(QChar('/'))) return false;
        navigate({Source::Phone, current.path, inner, current.label}); return true;
    }
    const qsizetype separator = path.indexOf(QStringLiteral("!/"));
    if (separator >= 0) {
        const QFileInfo archive(path.left(separator));
        const QString inner = path.mid(separator + 2);
        if (!archive.isFile() || !isArchive(archive.fileName()) || (!inner.isEmpty() && safeArchiveName(inner).isEmpty())) return false;
        navigate({Source::Archive, archive.absoluteFilePath(), inner, {}}); return true;
    }
    if (QDir::isRelativePath(path) && current.source == Source::Local) path = QDir(current.path).filePath(path);
    const QFileInfo info(path);
    if (info.isDir()) { navigate({Source::Local, info.absoluteFilePath(), {}, {}}); return true; }
    if (info.isFile() && isArchive(info.fileName())) { navigate({Source::Archive, info.absoluteFilePath(), {}, {}}); return true; }
    return false;
}

void Pane::navigate(const Location &destination) {
    navigateTo(destination, navigationTransition(location(), destination, FileView::Transition::EnterFolder));
}

void Pane::navigateTo(const Location &destination, FileView::Transition transition) {
    emit activated(this); saveView();
    Tab &state = states_[tab_];
    state.history = state.history.mid(0, state.historyIndex + 1);
    state.history.append(destination); state.historyIndex = int(state.history.size() - 1);
    state.location = destination; state.selection.clear(); state.current.clear(); state.scroll = 0;
    reload(false, transition);
}

void Pane::newTab() {
    if (states_.size() >= 32) return;
    saveView(); const Location current = location();
    states_.append(Tab{current, {current}, 0, 0, {}, {}});
    switching_ = true; tabs_->addTab(QStringLiteral("新标签")); switching_ = false;
    tabs_->setCurrentIndex(int(states_.size() - 1));
}

void Pane::closeTab() {
    if (states_.size() == 1) return;
    switching_ = true; states_.removeAt(tab_); tabs_->removeTab(tab_);
    tab_ = qMin(tab_, int(states_.size() - 1)); tabs_->setCurrentIndex(tab_); switching_ = false;
    refresh(false);
}
void Pane::cycleTab(int direction) { tabs_->setCurrentIndex((tab_ + direction + int(states_.size())) % int(states_.size())); }
void Pane::goBack() {
    Tab &state = states_[tab_]; if (state.historyIndex <= 0) return;
    const Location destination = state.history[state.historyIndex - 1];
    const auto transition = navigationTransition(state.location, destination, FileView::Transition::LeaveFolder);
    --state.historyIndex; state.location = destination; state.selection.clear(); state.current.clear(); state.scroll = 0; reload(false, transition);
}
void Pane::goForward() {
    Tab &state = states_[tab_]; if (state.historyIndex + 1 >= state.history.size()) return;
    const Location destination = state.history[state.historyIndex + 1];
    const auto transition = navigationTransition(state.location, destination, FileView::Transition::EnterFolder);
    ++state.historyIndex; state.location = destination; state.selection.clear(); state.current.clear(); state.scroll = 0; reload(false, transition);
}
void Pane::goUp() {
    Location current = location();
    if (current.source == Source::Drive) return;
    if (current.source == Source::Archive) {
        if (current.inner.isEmpty()) navigateTo(archiveParents_.value(current.path, {Source::Local, QFileInfo(current.path).absolutePath(), {}, {}}), FileView::Transition::LeaveFolder);
        else { current.inner = current.inner.section(QChar('/'), 0, -2); navigateTo(current, FileView::Transition::LeaveFolder); }
    } else if (current.source == Source::Phone) {
        if (current.inner == QStringLiteral("/") || current.inner == QStringLiteral("/sdcard")) navigateTo({}, FileView::Transition::LeaveFolder);
        else { current.inner = QFileInfo(current.inner).path(); navigateTo(current, FileView::Transition::LeaveFolder); }
    } else {
        const QString parent = QFileInfo(current.path).absolutePath();
        navigateTo(parent == current.path ? Location() : Location{Source::Local, parent, {}, {}}, FileView::Transition::LeaveFolder);
    }
}

void Pane::openIndex(const QModelIndex &index) {
    if (!index.isValid()) return;
    const Entry entry = model_->entry(index.row());
    if (entry.source == Source::Device) {
        if (entry.state != QStringLiteral("device")) { error_ = entry.detail; updateFooter(); return; }
        navigate({Source::Phone, entry.serial, QStringLiteral("/sdcard"), entry.name});
    } else if (entry.directory) {
        if (entry.source == Source::Archive) navigate({Source::Archive, entry.path, entry.inner, {}});
        else if (entry.source == Source::Phone) navigate({Source::Phone, entry.serial, entry.path, location().label});
        else navigate({Source::Local, entry.path, {}, {}});
    } else if (entry.source == Source::Local && isArchive(entry.name)) navigate({Source::Archive, entry.path, {}, {}});
    else emit openRequested(entry);
}

void Pane::focusPath() { path_->setFocus(); path_->selectAll(); }
void Pane::deselect() { view_->deselect(); emit activated(this); }
void Pane::updateFooter() {
    updateMiuixHeading();
    const int count = view_->selectionModel()->selectedRows().size();
    footer_->setText(error_.isEmpty() ? QStringLiteral("共 %1 项%2").arg(model_->rowCount()).arg(count ? QStringLiteral(" · 已选 %1").arg(count) : QString()) : error_);
}

bool Pane::eventFilter(QObject *object, QEvent *event) {
    if (object == this && event->type() == QEvent::Resize) updateMiuixHeading();
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusIn) emit activated(this);
    if (object == blank_ && event->type() == QEvent::MouseButtonPress) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::NoModifier) { deselect(); return true; }
    }
    if (view_ && object == view_->viewport() && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) scheduleThumbnails();
    return QFrame::eventFilter(object, event);
}

void Pane::scheduleThumbnails() { if (!thumbnailTimer_.isActive()) thumbnailTimer_.start(); }
void Pane::loadThumbnails() {
    if (!view_->isVisible()) return;
    int first = view_->indexAt(QPoint(4, 4)).row(); if (first < 0) first = 0;
    const int rowHeight = qMax(1, view_->sizeHintForRow(first));
    const int limit = qMin(model_->rowCount(), first + view_->viewport()->height() / rowHeight + 2);
    for (int row = first; row < limit; ++row) {
        const Entry entry = model_->entry(row);
        const QString key = entry.key();
        if (!supportsThumbnail(entry) || thumbnailsRequested_.contains(key) || thumbnailsFailed_.contains(key) || !model_->thumbnail(row).isNull()) continue;
        const QImage cached = cachedThumbnail(entry);
        if (!cached.isNull()) { model_->setThumbnail(key, cached); continue; }
        if (thumbnailJobs_ >= 4) continue;
        thumbnailsRequested_.insert(key); ++thumbnailAttempts_[key]; ++thumbnailJobs_;
        const quint64 generation = generation_; const auto cancel = thumbnailCancel_;
        auto *watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, generation, entry, key, cancel] {
            const QImage image = watcher->result(); watcher->deleteLater(); --thumbnailJobs_;
            if (generation == generation_) {
                thumbnailsRequested_.remove(key);
                if (!image.isNull()) model_->setThumbnail(key, image);
                else if (!cancel || !cancel->load()) {
                    thumbnailsFailed_.insert(key);
                    if (thumbnailAttempts_.value(key) < 2) QTimer::singleShot(1500, this, [this, generation, key] { if (generation == generation_) { thumbnailsFailed_.remove(key); scheduleThumbnails(); } });
                }
            }
            scheduleThumbnails();
        });
        watcher->setFuture(QtConcurrent::run(thumbnailPool(), [entry, cancel] { return loadThumbnail(entry, cancel); }));
    }
}

void Pane::search(const QString &pattern) {
    if (location().source == Source::Drive || pattern.isEmpty()) return;
    view_->stopTransition(); refresh_->setBusy(false);
    if (searchCancel_) searchCancel_->store(true);
    if (thumbnailCancel_) thumbnailCancel_->store(true);
    thumbnailCancel_ = std::make_shared<std::atomic_bool>(false);
    thumbnailsRequested_.clear(); thumbnailsFailed_.clear(); thumbnailAttempts_.clear();
    searchCancel_ = std::make_shared<std::atomic_bool>(false);
    const auto cancel = searchCancel_; const Location source = location(); const bool hidden = hidden_;
    const quint64 generation = ++generation_;
    footer_->setText(QStringLiteral("正在搜索…"));
    auto *watcher = new QFutureWatcher<Listing>(this);
    connect(watcher, &QFutureWatcher<Listing>::finished, this, [this, watcher, generation] {
        const Listing result = watcher->result(); watcher->deleteLater();
        if (generation != generation_) return;
        error_ = result.error; apply(result.entries, false);
    });
    watcher->setFuture(QtConcurrent::run([source, pattern, hidden, cancel] { return searchLocation(source, pattern, hidden, cancel); }));
}

QJsonObject Pane::workspace() const {
    QJsonArray tabs;
    for (const Tab &state : states_) tabs.append(state.location.json());
    return {{QStringLiteral("tabs"), tabs}, {QStringLiteral("current"), tab_}};
}

void Pane::restore(const QJsonObject &workspace) {
    const QJsonArray saved = workspace.value(QStringLiteral("tabs")).toArray();
    if (saved.isEmpty()) return;
    switching_ = true;
    while (tabs_->count()) tabs_->removeTab(0);
    states_.clear();
    for (const QJsonValue &value : saved) {
        if (states_.size() >= 32) break;
        const Location location = Location::fromJson(value.toObject());
        states_.append(Tab{location, {location}, 0, 0, {}, {}}); tabs_->addTab(QStringLiteral("标签"));
    }
    tab_ = qBound(0, workspace.value(QStringLiteral("current")).toInt(), int(states_.size() - 1));
    tabs_->setCurrentIndex(tab_); switching_ = false; refresh(false);
}

}
