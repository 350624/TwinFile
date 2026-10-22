#pragma once

#include "devices.h"
#include "pane.h"
#include "hfx.h"
#include "operations.h"
#include <QMainWindow>
#include <functional>

class QDockWidget;

namespace twinfile {

class ExtensionSidebar;

class Window : public QMainWindow {
    Q_OBJECT
public:
    explicit Window(QWidget *parent = nullptr);
    Pane *leftPane() const { return left_; }
    Pane *rightPane() const { return right_; }
    Pane *activePane() const { return active_; }
    void setActive(Pane *pane);
    void refreshActive();
    void setTheme(const QString &theme);
    void setColorMode(const QString &mode);
    void showAppearanceSettings();
    void setReducedMotion(bool enabled);
protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    void buildSidebar(QWidget *sidebar);
    void buildActions();
    void searchFiles();
    void sortMenu(QWidget *anchor);
    void updateNavigation();
    void updateDeviceNavigation();
    void buildExtensions();
    void menu(const QPoint &position);
    void moreMenu();
    void runJob(const QString &title, std::function<Result(const Cancellation &)> function,
                std::function<void(const Result &)> completed = {}, std::function<void()> settled = {}, bool writeBack = false);
    void transfer(bool move, bool clipboard = false);
    void paste();
    void remove();
    void rename(bool batch);
    void create(bool directory);
    void compress();
    void extract();
    void open(const Entry &entry);
    void preview();
    void properties();
    void edit(const QString &path);
    void image(const QString &path, const QList<Entry> &neighbors = {});
    void media(const QString &path, const QList<Entry> &neighbors = {}, const Entry &current = {});
    void hashes();
    void verifyHash();
    void checksumList(bool verify);
    void compare(bool directories);
    void apkDecompile();
    void apkBuild();
    void apkInstall();
    void apkEdit();
    void apkProjectEdit();
    void payload();
    void hfx();
    void showText(const QString &title, const QString &text);
    void saveSettings();
    void report(const QString &message);
    Pane *otherPane() const;
    Pane *left_;
    Pane *right_;
    Pane *active_ = nullptr;
    QWidget *sidebar_;
    DeviceMonitor *monitor_ = nullptr;
    StorageMonitor *storageMonitor_ = nullptr;
    QWidget *deviceNavigation_ = nullptr;
    QList<Entry> navigationPhones_, navigationDrives_;
    QStringList navigationSignature_;
    QJsonObject settings_;
    QList<Entry> clipboard_;
    bool cut_ = false;
    bool busy_ = false;
    Cancellation cancellation_;
    std::shared_ptr<HfxSession> hfxSession_;
    ArchiveMounts mounts_;
    ExtensionSidebar *extensions_ = nullptr;
    QDockWidget *extensionDock_ = nullptr;
};

}
