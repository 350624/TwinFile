#pragma once

#include <QColor>
#include <QEasingCurve>
#include <QPalette>

namespace twinfile {

// Native adaptation of Miuix Console's design tokens. See licenses/MIUIX-CONSOLE.md.
struct MiuixTokens {
    QColor background, surface1, surface2, surface3, surface4;
    QColor primary, primaryHover, primaryActive, primaryDisabled, onPrimaryDisabled;
    QColor text, secondaryText, mutedText, disabledText, border, divider, hover, active, selection;
};

MiuixTokens miuixTokens(bool dark, const QColor &accent = {});
MiuixTokens miuixTokens(const QPalette &palette);
QPalette miuixPalette(bool dark, const QColor &accent = {});
QString miuixStyleSheet(bool dark, const QPalette &palette);
QString miuixFontFamily();
QEasingCurve miuixEase();
QEasingCurve miuixSpring();

}
