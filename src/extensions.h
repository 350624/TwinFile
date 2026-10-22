#pragma once

#include "twinfile_plugin.h"
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QWidget>
#include <functional>
#include <memory>

class QListWidget;
class QTabWidget;

namespace twinfile {

struct ExtensionInfo {
    QString id;
    QString name;
    QString version;
    QString description;
    QString hash;
    QString file;
    QString error;
    bool builtin = false;
    bool enabled = false;
};

class ExtensionManager : public QObject {
    Q_OBJECT
public:
    ExtensionManager(QString directory, extensions::Host *host, QWidget *panelParent, QObject *parent = nullptr);
    ~ExtensionManager() override;
    static ExtensionInfo inspect(const QString &file, QString *error);
    void addBuiltin(QObject *object, const ExtensionInfo &info);
    void restore();
    QList<ExtensionInfo> installed() const;
    QWidget *panel(const QString &id) const;
    bool install(const QString &file, bool trusted, const QString &approvedHash, QString *error);
    bool setEnabled(const QString &id, bool enabled, QString *error);
    bool remove(const QString &id, QString *error);
    QList<extensions::Command> commands(const QString &id) const;
    bool execute(const QString &id, const QString &command, QString *error);
    bool configure(const QString &id, QWidget *parent, QString *error);
signals:
    void changed();
private:
    struct Record;
    bool start(const std::shared_ptr<Record> &record, QString *error);
    void stop(const std::shared_ptr<Record> &record);
    bool persist(QString *error);
    QString directory_;
    extensions::Host *host_;
    QWidget *panelParent_;
    QMap<QString, std::shared_ptr<Record>> records_;
    bool restored_ = false;
};

class ExtensionSidebar : public QWidget, public extensions::Host {
    Q_OBJECT
public:
    ExtensionSidebar(const QString &directory, QWidget *parent = nullptr);
    QJsonObject selectionContext() const override;
    void refreshActivePane() override;
    void showStatusMessage(const QString &message) override;
    ExtensionManager *manager() const { return manager_; }
    std::function<QJsonObject()> contextProvider;
    std::function<void()> refresh;
    std::function<void(const QString &)> notify;
    void showExtensions();
signals:
    void closeRequested();
private:
    void rebuild();
    void importPlugin();
    QString selectedId() const;
    void showError(const QString &message);
    ExtensionManager *manager_;
    QTabWidget *tabs_;
    QWidget *management_;
    QListWidget *list_;
};

}
