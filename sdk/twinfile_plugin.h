#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtPlugin>

class QWidget;

namespace twinfile::extensions {

inline constexpr int ApiVersion = 1;

struct Command {
    QString id;
    QString title;
};

class Host {
public:
    virtual ~Host() = default;
    virtual int apiVersion() const { return ApiVersion; }
    virtual QJsonObject selectionContext() const = 0;
    virtual void refreshActivePane() = 0;
    virtual void showStatusMessage(const QString &message) = 0;
};

class Plugin {
public:
    virtual ~Plugin() = default;
    virtual bool activate(Host *host, QString *error) = 0;
    virtual void deactivate() = 0;
    virtual QWidget *createPanel(QWidget *parent) = 0;
    virtual QList<Command> commands() const { return {}; }
    virtual void execute(const QString &commandId) {}
    virtual void configure(QWidget *parent) {}
};

}

#define TwinFilePlugin_iid "org.twinfile.Extension/1.0"
Q_DECLARE_INTERFACE(twinfile::extensions::Plugin, TwinFilePlugin_iid)
