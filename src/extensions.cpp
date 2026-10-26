#include "extensions.h"
#include "motion.h"
#include "ai_panel.h"
#include "core.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLibrary>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPluginLoader>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTabWidget>
#include <QTabBar>
#include <QUrl>
#include <QVBoxLayout>

namespace twinfile {

struct ExtensionManager::Record {
    ExtensionInfo info;
    std::unique_ptr<QPluginLoader> loader;
    QPointer<QObject> object;
    extensions::Plugin *plugin = nullptr;
    QPointer<QWidget> panel;
};

static QString fileHash(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

ExtensionManager::ExtensionManager(QString directory, extensions::Host *host, QWidget *panelParent, QObject *parent)
    : QObject(parent), directory_(std::move(directory)), host_(host), panelParent_(panelParent) {}

ExtensionManager::~ExtensionManager() {
    for (const auto &record : records_) stop(record);
}

ExtensionInfo ExtensionManager::inspect(const QString &file, QString *error) {
    if (error) error->clear();
    ExtensionInfo info;
    const QFileInfo source(file);
    if (!source.isFile() || source.isSymLink() || source.suffix().compare(QStringLiteral("dll"), Qt::CaseInsensitive) || source.size() > 64 * 1024 * 1024) {
        if (error) *error = QStringLiteral("请选择不超过 64 MiB 的原生插件 DLL 文件");
        return {};
    }
    QPluginLoader probe(source.absoluteFilePath());
    const auto metadata = probe.metaData();
    const auto manifest = metadata.value(QStringLiteral("MetaData")).toObject();
    info.id = manifest.value(QStringLiteral("Id")).toString();
    info.name = manifest.value(QStringLiteral("Name")).toString();
    info.version = manifest.value(QStringLiteral("Version")).toString();
    info.description = manifest.value(QStringLiteral("Description")).toString();
    const QRegularExpression identifier(QStringLiteral("^[a-z0-9]+(?:[.-][a-z0-9]+)+$"));
    if (metadata.value(QStringLiteral("IID")).toString() != QStringLiteral(TwinFilePlugin_iid) || manifest.value(QStringLiteral("ApiVersion")).toInt() != extensions::ApiVersion ||
        info.id.size() > 100 || !identifier.match(info.id).hasMatch() || info.id.startsWith(QStringLiteral("twinfile.")) ||
        info.name.isEmpty() || info.name.size() > 80 || info.version.isEmpty() || info.version.size() > 32 || info.description.size() > 2000) {
        if (error) *error = QStringLiteral("插件清单无效、接口版本不兼容，或使用了保留 ID（需要 TwinFile Extension API 1）");
        return {};
    }
    info.hash = fileHash(source.absoluteFilePath());
    if (info.hash.isEmpty()) { if (error) *error = QStringLiteral("无法读取插件内容"); return {}; }
    info.file = info.id + u'-' + info.hash + QStringLiteral(".dll");
    return info;
}

void ExtensionManager::addBuiltin(QObject *object, const ExtensionInfo &info) {
    auto record = std::make_shared<Record>(); record->info = info; record->info.builtin = true; record->info.enabled = false;
    object->setParent(this); record->object = object; record->plugin = qobject_cast<extensions::Plugin *>(object);
    records_.insert(info.id, record);
}

bool ExtensionManager::persist(QString *error) {
    QJsonArray plugins;
    for (const auto &record : records_) {
        plugins.append(QJsonObject{{QStringLiteral("id"), record->info.id}, {QStringLiteral("file"), record->info.file},
                                  {QStringLiteral("sha256"), record->info.hash}, {QStringLiteral("enabled"), record->info.enabled}});
    }
    return writeSettings(QDir(directory_).filePath(QStringLiteral("index.json")), {{QStringLiteral("plugins"), plugins}}, error);
}

void ExtensionManager::restore() {
    if (restored_) return;
    restored_ = true;
    const auto plugins = readSettings(QDir(directory_).filePath(QStringLiteral("index.json"))).value(QStringLiteral("plugins")).toArray();
    QMap<QString, bool> enabled;
    for (const auto &value : plugins) {
        if (records_.size() >= 65) break;
        const auto entry = value.toObject();
        const QString id = entry.value(QStringLiteral("id")).toString();
        if (records_.contains(id)) {
            if (records_[id]->info.builtin) enabled.insert(id, entry.value(QStringLiteral("enabled")).toBool());
            continue;
        }
        const QString filename = entry.value(QStringLiteral("file")).toString();
        const QString digest = entry.value(QStringLiteral("sha256")).toString();
        const QRegularExpression safeHash(QStringLiteral("^[a-f0-9]{64}$"));
        const QRegularExpression safeId(QStringLiteral("^[a-z0-9]+(?:[.-][a-z0-9]+)+$"));
        if (id.size() > 100 || !safeId.match(id).hasMatch() || id.startsWith(QStringLiteral("twinfile.")) || !safeHash.match(digest).hasMatch() || filename != id + u'-' + digest + QStringLiteral(".dll")) continue;
        QString error;
        auto info = inspect(QDir(directory_).filePath(filename), &error);
        if (error.isEmpty() && (info.hash != digest || info.id != id)) error = QStringLiteral("插件内容或标识已变化，需要重新安装并信任");
        if (!error.isEmpty()) { info.id = id; info.name = id; info.hash = digest; info.file = filename; info.error = error; }
        auto record = std::make_shared<Record>(); record->info = info;
        records_.insert(id, record);
        enabled.insert(id, entry.value(QStringLiteral("enabled")).toBool());
    }
    for (const auto &record : records_) {
        if (!enabled.value(record->info.id, record->info.builtin) || !record->info.error.isEmpty()) continue;
        QString error;
        if (!start(record, &error)) record->info.error = error;
    }
    emit changed();
}

bool ExtensionManager::start(const std::shared_ptr<Record> &record, QString *error) {
    if (error) error->clear();
    auto fail = [&](const QString &message) { if (error) *error = message; record->info.error = message; return false; };
    if (!record->info.builtin) {
        const QString path = QDir(directory_).filePath(record->info.file);
        QString checkError;
        const auto current = inspect(path, &checkError);
        if (!checkError.isEmpty() || current.hash != record->info.hash || current.id != record->info.id) return fail(QStringLiteral("插件校验失败，请重新安装并确认信任"));
        if (!record->loader) record->loader = std::make_unique<QPluginLoader>(path);
        if (!record->object) record->object = record->loader->instance();
        record->plugin = qobject_cast<extensions::Plugin *>(record->object.data());
    }
    if (!record->plugin) return fail(QStringLiteral("插件无法加载，请确认 Qt 6.10.3 / MinGW 13.1 / x64 编译环境与依赖一致"));
    try {
        QString activationError;
        if (!record->plugin->activate(host_, &activationError)) {
            record->plugin->deactivate();
            return fail(QStringLiteral("插件启动失败：") + activationError.left(500));
        }
        record->panel = record->plugin->createPanel(panelParent_);
        if (record->panel && record->panel->parentWidget() != panelParent_) record->panel->setParent(panelParent_);
        record->info.enabled = true; record->info.error.clear();
        return true;
    } catch (...) {
        stop(record);
        return fail(QStringLiteral("插件启动时抛出异常，已停用"));
    }
}

void ExtensionManager::stop(const std::shared_ptr<Record> &record) {
    delete record->panel.data(); record->panel = nullptr;
    if (record->plugin) { try { record->plugin->deactivate(); } catch (...) {} }
    record->info.enabled = false;
}

QList<ExtensionInfo> ExtensionManager::installed() const {
    QList<ExtensionInfo> result;
    for (const auto &record : records_) result.append(record->info);
    return result;
}

QWidget *ExtensionManager::panel(const QString &id) const { const auto record = records_.value(id); return record ? record->panel.data() : nullptr; }

bool ExtensionManager::install(const QString &file, bool trusted, const QString &approvedHash, QString *error) {
    if (error) error->clear();
    if (!trusted) { if (error) *error = QStringLiteral("未获用户信任，不加载或安装原生插件"); return false; }
    auto info = inspect(file, error); if (info.id.isEmpty()) return false;
    if (approvedHash != info.hash) { if (error) *error = QStringLiteral("插件与用户确认的指纹不一致，请重新检查并确认信任"); return false; }
    if (records_.contains(info.id)) { if (error) *error = QStringLiteral("相同 ID 已安装，请先卸载旧插件并重启后再安装新版"); return false; }
    if (records_.size() >= 65) { if (error) *error = QStringLiteral("最多安装 64 个外部插件"); return false; }
    if (!QDir().mkpath(directory_)) { if (error) *error = QStringLiteral("无法创建插件目录"); return false; }
    QFile source(file);
    if (!source.open(QIODevice::ReadOnly) || source.size() > 64 * 1024 * 1024) { if (error) *error = QStringLiteral("无法读取插件"); return false; }
    const QByteArray bytes = source.readAll();
    if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) != info.hash) { if (error) *error = QStringLiteral("插件在检查后发生变化，请重新选择"); return false; }
    QSaveFile destination(QDir(directory_).filePath(info.file));
    if (!destination.open(QIODevice::WriteOnly) || destination.write(bytes) != bytes.size() || !destination.commit()) { if (error) *error = QStringLiteral("无法保存插件副本"); return false; }
    auto record = std::make_shared<Record>(); record->info = info; records_.insert(info.id, record);
    if (!persist(error)) { records_.remove(info.id); return false; }
    emit changed();
    return true;
}

bool ExtensionManager::setEnabled(const QString &id, bool enabled, QString *error) {
    if (error) error->clear();
    const auto record = records_.value(id);
    if (!record) { if (error) *error = QStringLiteral("插件不存在"); return false; }
    if (record->info.enabled == enabled) return true;
    if (enabled && !start(record, error)) { emit changed(); return false; }
    if (!enabled) stop(record);
    if (!persist(error)) {
        if (enabled) stop(record);
        emit changed(); return false;
    }
    emit changed(); return true;
}

bool ExtensionManager::remove(const QString &id, QString *error) {
    if (error) error->clear();
    const auto record = records_.value(id);
    if (!record || record->info.builtin) { if (error) *error = QStringLiteral("内置扩展只能停用，不能卸载"); return false; }
    stop(record); records_.remove(id);
    if (!persist(error)) { records_.insert(id, record); emit changed(); return false; }
    emit changed(); return true;
}

QList<extensions::Command> ExtensionManager::commands(const QString &id) const {
    const auto record = records_.value(id);
    if (!record || !record->info.enabled) return {};
    try { return record->plugin->commands().mid(0, 100); } catch (...) { return {}; }
}

bool ExtensionManager::execute(const QString &id, const QString &command, QString *error) {
    if (error) error->clear();
    const auto record = records_.value(id);
    for (const auto &item : commands(id)) {
        if (item.id != command) continue;
        try { record->plugin->execute(command); return true; } catch (...) { break; }
    }
    if (error) *error = QStringLiteral("命令不可用或插件执行异常");
    return false;
}

bool ExtensionManager::configure(const QString &id, QWidget *parent, QString *error) {
    if (error) error->clear();
    const auto record = records_.value(id);
    if (record && record->info.enabled) {
        try { record->plugin->configure(parent); return true; } catch (...) {}
    }
    if (error) *error = QStringLiteral("请先启用插件，或检查插件设置接口");
    return false;
}

ExtensionSidebar::ExtensionSidebar(const QString &directory, QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("extensionSidebar")); setMinimumWidth(310);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    tabs_ = new MotionTabWidget; tabs_->setObjectName(QStringLiteral("extensionTabs")); tabs_->tabBar()->setObjectName(QStringLiteral("extensionTabBar")); tabs_->setDocumentMode(true); tabs_->tabBar()->setDrawBase(false); tabs_->setUsesScrollButtons(true); layout->addWidget(tabs_);
    auto *corner = new QWidget; corner->setFixedHeight(50); auto *cornerLayout = new QHBoxLayout(corner); cornerLayout->setContentsMargins(4, 0, 8, 0); cornerLayout->setAlignment(Qt::AlignVCenter);
    auto *close = new QPushButton(QStringLiteral("×")); close->setObjectName(QStringLiteral("extensionClose")); close->setFixedSize(28, 28); close->setToolTip(QStringLiteral("收起侧栏")); close->setAccessibleName(QStringLiteral("收起侧栏")); close->setCursor(Qt::PointingHandCursor); cornerLayout->addWidget(close); tabs_->setCornerWidget(corner);
    connect(close, &QPushButton::clicked, this, &ExtensionSidebar::closeRequested);
    management_ = new QWidget(tabs_);
    auto *manage = new QVBoxLayout(management_); manage->setContentsMargins(8, 8, 8, 8);
    auto *heading = new QLabel(QStringLiteral("扩展 · 本地插件")); QFont font = heading->font(); font.setBold(true); font.setPointSize(14); heading->setFont(font); manage->addWidget(heading);
    auto *description = new QLabel(QStringLiteral("按需安装 C++ 插件，扩展侧栏与命令。\nAI 接口由你配置，不绑定任何平台。")); description->setWordWrap(true); description->setObjectName(QStringLiteral("subtle")); manage->addWidget(description);
    auto *install = new QPushButton(QStringLiteral("＋ 安装本地插件…")); connect(install, &QPushButton::clicked, this, &ExtensionSidebar::importPlugin); manage->addWidget(install);
    list_ = new QListWidget; list_->setObjectName(QStringLiteral("extensionList")); list_->setWordWrap(true); list_->setTextElideMode(Qt::ElideNone); manage->addWidget(list_, 1);
    auto *row = new QHBoxLayout;
    auto add = [&](const QString &text, auto callback) { auto *button = new QPushButton(text); row->addWidget(button); connect(button, &QPushButton::clicked, this, callback); };
    add(QStringLiteral("启用 / 停用"), [this] {
        const QString id = selectedId();
        for (const auto &info : manager_->installed()) if (info.id == id) { QString error; if (!manager_->setEnabled(id, !info.enabled, &error)) showError(error); break; }
    });
    add(QStringLiteral("设置"), [this] { QString error; if (!manager_->configure(selectedId(), this, &error)) showError(error); });
    add(QStringLiteral("卸载"), [this] {
        const QString id = selectedId(); if (id.isEmpty()) return;
        if (QMessageBox::question(this, QStringLiteral("卸载插件"), QStringLiteral("取消此插件的信任并停用？已加载 DLL 可能保留到退出，磁盘副本不会自动执行。")) != QMessageBox::Yes) return;
        QString error; if (!manager_->remove(id, &error)) showError(error);
    });
    manage->addLayout(row);
    auto *command = new QPushButton(QStringLiteral("执行所选插件命令…")); manage->addWidget(command);
    connect(command, &QPushButton::clicked, this, [this, command] {
        QMenu menu(this); const QString id = selectedId();
        for (const auto &item : manager_->commands(id)) menu.addAction(item.title.left(120), this, [this, id, item] { QString error; if (!manager_->execute(id, item.id, &error)) showError(error); });
        if (menu.isEmpty()) { showStatusMessage(QStringLiteral("该插件没有可用命令，或尚未启用")); return; }
        menu.exec(command->mapToGlobal(QPoint(0, command->height())));
    });
    auto *warning = new QLabel(QStringLiteral("仅信任可靠来源。原生 DLL 与主程序具有相同权限，不是沙箱；也不兼容 VS Code 扩展。")); warning->setWordWrap(true); warning->setObjectName(QStringLiteral("subtle")); manage->addWidget(warning);
    auto *docs = new QPushButton(QStringLiteral("打开插件开发文档")); manage->addWidget(docs);
    connect(docs, &QPushButton::clicked, this, [this] {
        const QString installed = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("PLUGIN_SDK.md"));
        if (!QFileInfo::exists(installed)) { showError(QStringLiteral("请查看源码工程中的 PLUGIN_SDK.md 与 sdk/example")); return; }
        QDesktopServices::openUrl(QUrl::fromLocalFile(installed));
    });
    manager_ = new ExtensionManager(directory, this, tabs_, this);
    auto *ai = new AiExtension(QFileInfo(directory).absolutePath());
    manager_->addBuiltin(ai, {QStringLiteral("twinfile.ai"), QStringLiteral("AI 助手与 Agent"), QStringLiteral("1.1.0"), QStringLiteral("自接 API · 流式聊天 · 内置文件 Agent · 授权目录 · 修改逐项确认"), {}, {}, {}, true, false});
    connect(manager_, &ExtensionManager::changed, this, &ExtensionSidebar::rebuild);
}

QJsonObject ExtensionSidebar::selectionContext() const { return contextProvider ? contextProvider() : QJsonObject(); }
void ExtensionSidebar::refreshActivePane() { if (refresh) refresh(); }
void ExtensionSidebar::showStatusMessage(const QString &message) { if (notify) notify(message.left(1000)); }
void ExtensionSidebar::showExtensions() { tabs_->setCurrentWidget(management_); }
QString ExtensionSidebar::selectedId() const { return list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString() : QString(); }
void ExtensionSidebar::showError(const QString &message) { QMessageBox::warning(this, QStringLiteral("扩展"), message); }

void ExtensionSidebar::rebuild() {
    const QString selected = selectedId();
    QPointer<QWidget> previous = tabs_->currentWidget();
    while (tabs_->count()) tabs_->removeTab(0);
    list_->clear();
    for (const auto &info : manager_->installed()) {
        const QString state = !info.error.isEmpty() ? QStringLiteral("错误：") + info.error : info.enabled ? QStringLiteral("已启用") : QStringLiteral("已停用");
        auto *item = new QListWidgetItem(QStringLiteral("%1  %2\n%3 · %4\n%5").arg(info.name, info.version, info.builtin ? QStringLiteral("内置") : QStringLiteral("本地"), state, info.description), list_);
        item->setData(Qt::UserRole, info.id); item->setToolTip(info.id + u'\n' + info.hash); item->setSizeHint(QSize(200, 110));
        if (info.id == selected) list_->setCurrentItem(item);
        if (auto *panel = manager_->panel(info.id)) tabs_->addTab(panel, info.id == QStringLiteral("twinfile.ai") ? QStringLiteral("对话") : info.name);
    }
    tabs_->addTab(management_, QStringLiteral("扩展"));
    if (previous && tabs_->indexOf(previous) >= 0) tabs_->setCurrentWidget(previous);
    if (!list_->currentItem() && list_->count()) list_->setCurrentRow(0);
}

void ExtensionSidebar::importPlugin() {
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择 TwinFile 原生扩展"), {}, QStringLiteral("TwinFile 插件 (*.dll)"));
    if (file.isEmpty()) return;
    QString error; const auto info = ExtensionManager::inspect(file, &error);
    if (!error.isEmpty()) { showError(error); return; }
    const QString prompt = QStringLiteral("%1  %2\nID：%3\n\n%4\n\nSHA256：%5\n\n这是本机原生代码，与主程序拥有相同权限，可访问文件和网络。只有信任开发者和来源时才安装。安装后默认为停用，需手动启用。继续？").arg(info.name, info.version, info.id, info.description, info.hash);
    QMessageBox confirmation(QMessageBox::Warning, QStringLiteral("信任并安装插件？"), prompt, QMessageBox::Yes | QMessageBox::No, this);
    confirmation.setTextFormat(Qt::PlainText); confirmation.setDefaultButton(QMessageBox::No);
    if (confirmation.exec() != QMessageBox::Yes) return;
    if (!manager_->install(file, true, info.hash, &error)) showError(error);
    else showExtensions();
}

}
