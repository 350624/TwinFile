#pragma once

#include "core.h"
#include <QJsonObject>

namespace twinfile {
QJsonObject runAgentCommand(const QString &root, const QJsonObject &arguments, const Cancellation &cancel);
}
