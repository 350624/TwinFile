#pragma once

#include "twinfile_plugin.h"
#include <QObject>
#include <QPointer>

class QLabel;

class SelectionExample : public QObject, public twinfile::extensions::Plugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID TwinFilePlugin_iid FILE "example.json")
    Q_INTERFACES(twinfile::extensions::Plugin)
public:
    bool activate(twinfile::extensions::Host *host, QString *error) override;
    void deactivate() override;
    QWidget *createPanel(QWidget *parent) override;
    QList<twinfile::extensions::Command> commands() const override;
    void execute(const QString &commandId) override;
private:
    void updateSelection();
    twinfile::extensions::Host *host_ = nullptr;
    QPointer<QLabel> label_;
};
