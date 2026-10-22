#pragma once

#include "core.h"
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QFutureWatcher>

namespace twinfile {

QList<Entry> parseDevices(const QString &text);
QList<Entry> storageLocations();

class StorageMonitor : public QObject {
    Q_OBJECT
public:
    explicit StorageMonitor(QObject *parent = nullptr);
    void start();
public slots:
    void detect();
signals:
    void changed(const QList<twinfile::Entry> &locations);
private:
    QTimer poll_;
    QFutureWatcher<QList<Entry>> worker_;
};

class DeviceMonitor : public QObject {
    Q_OBJECT
public:
    explicit DeviceMonitor(QObject *parent = nullptr);
    ~DeviceMonitor() override;
    QList<Entry> entries() const { return devices_; }
    void start();
public slots:
    void detect();
signals:
    void changed(const QList<twinfile::Entry> &devices);
    void status(const QString &message);
private:
    void completed();
    void scheduleRetry();
    QProcess process_;
    QTimer poll_;
    QTimer retry_;
    QTimer timeout_;
    QList<Entry> devices_;
    int attempts_ = 0;
    bool warming_ = false;
};

}
