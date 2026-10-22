#include "devices.h"
#include <QRegularExpression>
#include <QDir>
#include <QtConcurrent>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace twinfile {

QList<Entry> storageLocations() {
    QList<Entry> entries;
#ifdef Q_OS_WIN
    DWORD previousMode = 0;
    const bool modeChanged = SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previousMode);
    const DWORD drives = GetLogicalDrives();
    for (int index = 0; index < 26; ++index) {
        if (!(drives & (DWORD(1) << index))) continue;
        const QString root = QString(QChar('A' + index)) + QStringLiteral(":\\");
        const UINT type = GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        wchar_t label[MAX_PATH + 1] = {};
        const bool ready = GetVolumeInformationW(reinterpret_cast<LPCWSTR>(root.utf16()), label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0);
        if (type == DRIVE_REMOVABLE && !ready) continue;
        Entry entry;
        entry.source = Source::Drive; entry.directory = true; entry.path = QDir::fromNativeSeparators(root);
        entry.state = type == DRIVE_REMOVABLE ? QStringLiteral("usb") : QStringLiteral("drive");
        const QString volume = QString::fromWCharArray(label).trimmed();
        entry.name = (type == DRIVE_REMOVABLE ? QStringLiteral("U盘") : volume.isEmpty() ? QStringLiteral("本地磁盘") : volume) + QStringLiteral(" (%1)").arg(root.left(2));
        entry.detail = type == DRIVE_REMOVABLE && !volume.isEmpty() ? volume : QString();
        entries.append(entry);
    }
    if (modeChanged) SetThreadErrorMode(previousMode, nullptr);
#endif
    return entries;
}

StorageMonitor::StorageMonitor(QObject *parent) : QObject(parent) {
    poll_.setInterval(3000);
    connect(&poll_, &QTimer::timeout, this, &StorageMonitor::detect);
    connect(&worker_, &QFutureWatcher<QList<Entry>>::finished, this, [this] { emit changed(worker_.result()); });
}

void StorageMonitor::start() {
    if (qEnvironmentVariableIsSet("TWINFILE_TEST_MODE")) return;
    poll_.start(); detect();
}

void StorageMonitor::detect() {
    if (qEnvironmentVariableIsSet("TWINFILE_TEST_MODE") || worker_.isRunning()) return;
    worker_.setFuture(QtConcurrent::run(storageLocations));
}

QList<Entry> parseDevices(const QString &text) {
    QList<Entry> entries;
    for (const QString &line : text.split(QChar('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QStringLiteral("List of devices")) || trimmed.startsWith(QChar('*'))) continue;
        const QStringList fields = trimmed.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (fields.size() < 2 || !QStringList{QStringLiteral("device"), QStringLiteral("offline"), QStringLiteral("unauthorized"), QStringLiteral("recovery"), QStringLiteral("sideload")}.contains(fields[1])) continue;
        Entry entry;
        entry.path = fields[0]; entry.serial = fields[0]; entry.name = fields[0];
        entry.state = fields[1]; entry.directory = true; entry.source = Source::Device;
        for (const QString &field : fields) {
            if (field.startsWith(QStringLiteral("model:"))) { entry.name = field.mid(6); entry.name.replace(QChar('_'), QChar(' ')); }
        }
        entry.detail = entry.state == QStringLiteral("device") ? QStringLiteral("已连接 · Android")
            : entry.state == QStringLiteral("unauthorized") ? QStringLiteral("未授权：请在手机上允许 USB 调试")
            : entry.state == QStringLiteral("offline") ? QStringLiteral("离线：请重新连接数据线") : entry.state;
        entries.append(entry);
    }
    return entries;
}

DeviceMonitor::DeviceMonitor(QObject *parent) : QObject(parent) {
    process_.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) { arguments->flags |= CREATE_NO_WINDOW; });
#endif
    poll_.setInterval(5000);
    retry_.setSingleShot(true);
    timeout_.setSingleShot(true);
    connect(&poll_, &QTimer::timeout, this, &DeviceMonitor::detect);
    connect(&retry_, &QTimer::timeout, this, &DeviceMonitor::detect);
    connect(&timeout_, &QTimer::timeout, &process_, &QProcess::kill);
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &DeviceMonitor::completed);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { timeout_.stop(); emit status(QStringLiteral("ADB 启动失败：") + process_.errorString()); scheduleRetry(); }
    });
}

DeviceMonitor::~DeviceMonitor() {
    poll_.stop(); retry_.stop(); timeout_.stop();
    process_.disconnect(this);
    if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(3000); }
}

void DeviceMonitor::start() {
    if (qEnvironmentVariableIsSet("TWINFILE_TEST_MODE")) return;
    poll_.start();
    const QString adb = toolPath(QStringLiteral("adb/adb.exe"));
    if (adb.isEmpty()) { emit status(QStringLiteral("未找到 ADB，手机检测不可用")); return; }
    warming_ = true;
    process_.start(adb, {QStringLiteral("start-server")});
    timeout_.start(20000);
}

void DeviceMonitor::detect() {
    if (qEnvironmentVariableIsSet("TWINFILE_TEST_MODE") || process_.state() != QProcess::NotRunning) return;
    const QString adb = toolPath(QStringLiteral("adb/adb.exe"));
    if (adb.isEmpty()) return;
    warming_ = false;
    process_.start(adb, {QStringLiteral("devices"), QStringLiteral("-l")});
    timeout_.start(15000);
}

void DeviceMonitor::completed() {
    timeout_.stop();
    const QString output = QString::fromUtf8(process_.readAll());
    if (warming_) { warming_ = false; QTimer::singleShot(0, this, &DeviceMonitor::detect); return; }
    if (process_.exitStatus() != QProcess::NormalExit || process_.exitCode() != 0) { emit status(QStringLiteral("检测手机失败：") + output.left(200)); scheduleRetry(); return; }
    devices_ = parseDevices(output);
    emit changed(devices_);
    if (devices_.isEmpty()) scheduleRetry();
    else { retry_.stop(); attempts_ = 3; emit status(QStringLiteral("检测到 %1 台手机").arg(devices_.size())); }
}

void DeviceMonitor::scheduleRetry() {
    const int delays[] = {1000, 2000, 4000};
    if (attempts_ < 3 && !retry_.isActive()) retry_.start(delays[attempts_++]);
}

}
