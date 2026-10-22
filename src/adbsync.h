#pragma once
#include "core.h"
#include <QIODevice>

namespace twinfile {
Listing adbSyncList(const QString &serial, const QString &path, bool hidden, const Cancellation &cancel = {});
Result adbSyncPush(const QString &serial, const QString &local, const QString &remote, const Cancellation &cancel = {});
Result adbSyncPull(const QString &serial, const QString &remote, const QString &local, const Cancellation &cancel = {});
Result adbSyncRead(const QString &serial, const QString &remote, QIODevice *output, const Cancellation &cancel = {});
}
