#pragma once

#include "core.h"
#include "zip.h"
#include <QDomDocument>
#include <QImage>

namespace twinfile {

using AndroidResources = QHash<QString, QString>;
QDomDocument androidXml(const QByteArray &bytes, QString *error, int nodeLimit = 2048, int depthLimit = 16);
AndroidResources androidResources(const QString &dump);
AndroidResources androidResources(const QByteArray &table, const Cancellation &cancel = {}, QString *error = nullptr);
QImage androidBitmap(const QByteArray &bytes, int size = 128);
QImage androidDrawable(ZipReader &archive, const AndroidResources &resources, const QString &name, int size = 128);
QImage apkIcon(const QString &path, const Cancellation &cancel = {});
QString aaptPath();
Result buildApk(const QString &source, const QString &destination, bool sign, const Cancellation &cancel = {}, const QMap<QString, QString> &fields = {}, const QString &icon = {}, bool preserveAdaptive = true);
QString repairResourceXml(const QString &text, bool manifest);
Result prepareApkProject(const QString &source, const QString &destination, const QMap<QString, QString> &fields = {}, const QString &icon = {}, const Cancellation &cancel = {}, bool preserveAdaptive = true);
Result restoreApkManifest(const QString &project, const QString &apk, const Cancellation &cancel = {});
QByteArray patchAndroidManifest(const QByteArray &bytes, const QMap<QString, QString> &fields, QString *error);
Result quickEditApk(const QString &source, const QString &destination, const QMap<QString, QString> &fields,
                    const QMap<QString, QString> &icons = {}, const Cancellation &cancel = {});
Result signApk(const QString &source, const QString &destination, const Cancellation &cancel = {});

}
