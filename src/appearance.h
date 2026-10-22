#pragma once

#include <QJsonObject>
#include <QIcon>
#include <QPalette>
#include <QStringList>

namespace twinfile {

QStringList themeIds();
QString themeName(const QString &theme);
QIcon sidebarSymbol(const QString &label);
QIcon actionSymbol(const QString &name, const QColor &color);
void normalizeAppearance(QJsonObject &settings);
QPalette appearancePalette(const QString &theme, bool dark, const QString &accent);
QString appearanceStyleSheet(const QString &theme, bool dark, const QPalette &palette);

}
