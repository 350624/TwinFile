#pragma once
#include "core.h"
#include <QImage>
#include <QProcessEnvironment>
namespace twinfile {
struct BatchPlan { QString program, command, directory; QProcessEnvironment environment; };
BatchPlan batchPlan(const QString &path);
Result launchBatch(const QString &path);
QImage shellThumbnail(const QString &path, const Cancellation &cancel = {});
}
