#include "miuix_theme.h"

namespace twinfile {

MiuixTokens miuixTokens(bool dark, const QColor &accent) {
    MiuixTokens t;
    t.background = QColor(dark ? "#161616" : "#ffffff");
    t.surface1 = QColor(dark ? "#0c0c0c" : "#f7f7f7");
    t.surface2 = QColor(dark ? "#1c1c1c" : "#ffffff");
    t.surface3 = QColor(dark ? "#242424" : "#e8e8e8");
    t.surface4 = QColor(dark ? "#2c2c2c" : "#e8e8e8");
    t.primary = accent.isValid() ? accent : QColor(dark ? "#277af7" : "#3482ff");
    t.primaryHover = accent.isValid() ? accent.lighter(112) : QColor(dark ? "#338fe4" : "#5d9bff");
    t.primaryActive = accent.isValid() ? accent.darker(112) : QColor(dark ? "#0073dd" : "#277af7");
    t.primaryDisabled = QColor(dark ? "#253e64" : "#c2d9ff");
    t.onPrimaryDisabled = QColor(dark ? "#677993" : "#f3f8ff");
    t.text = QColor(dark ? "#f2f2f2" : "#000000");
    t.secondaryText = QColor(dark ? "#999999" : "#666666");
    t.mutedText = QColor(dark ? "#666666" : "#999999");
    t.disabledText = QColor(dark ? "#666666" : "#b2b2b2");
    t.border = QColor(dark ? "#484848" : "#d9d9d9");
    t.divider = QColor(dark ? "#404040" : "#e0e0e0");
    t.hover = QColor(dark ? Qt::white : Qt::black); t.hover.setAlphaF(0.04);
    t.active = QColor(dark ? Qt::white : Qt::black); t.active.setAlphaF(0.07);
    t.selection = t.primary; t.selection.setAlphaF(dark ? 0.15 : 0.12);
    return t;
}

MiuixTokens miuixTokens(const QPalette &palette) {
    const bool dark = palette.color(QPalette::Window).lightness() < 128;
    const QColor primary = palette.color(QPalette::Highlight);
    return miuixTokens(dark, primary == miuixTokens(dark).primary ? QColor() : primary);
}

QPalette miuixPalette(bool dark, const QColor &accent) {
    const auto t = miuixTokens(dark, accent);
    QPalette p;
    p.setColor(QPalette::Window, t.background);
    p.setColor(QPalette::Base, t.surface2);
    p.setColor(QPalette::AlternateBase, t.surface3);
    p.setColor(QPalette::Button, t.surface3);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::ToolTipText}) p.setColor(role, t.text);
    p.setColor(QPalette::ToolTipBase, t.surface4);
    p.setColor(QPalette::Mid, t.border);
    p.setColor(QPalette::Dark, t.divider);
    p.setColor(QPalette::Light, t.surface1);
    p.setColor(QPalette::Shadow, QColor(dark ? "#000000" : "#c2c2c2"));
    p.setColor(QPalette::PlaceholderText, t.secondaryText);
    p.setColor(QPalette::Highlight, t.primary);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, t.primary);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::PlaceholderText})
        p.setColor(QPalette::Disabled, role, t.disabledText);
    p.setColor(QPalette::Disabled, QPalette::Highlight, t.primaryDisabled);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, t.onPrimaryDisabled);
    return p;
}

QEasingCurve miuixEase() {
    QEasingCurve curve(QEasingCurve::BezierSpline);
    curve.addCubicBezierSegment({0.16, 1}, {0.3, 1}, {1, 1});
    return curve;
}

QEasingCurve miuixSpring() {
    QEasingCurve curve(QEasingCurve::BezierSpline);
    curve.addCubicBezierSegment({0.34, 1.56}, {0.64, 1}, {1, 1});
    return curve;
}

QString miuixStyleSheet(bool dark, const QPalette &palette) {
    const auto t = miuixTokens(palette);
    QString sheet = QStringLiteral(R"(
QWidget { font-family: "MiSans", "Mi Sans", "Microsoft YaHei UI"; font-size: 13px; letter-spacing: 0; }
QMainWindow, QDialog, QWidget#workspace, QWidget#workspaceContent { background: %1; }
QWidget#workspaceHeader { background: %2; border: none; border-bottom: 1px solid %6; min-height: 52px; max-height: 52px; }
QLabel#miuixTitle { font-size: 16px; font-weight: 600; background: transparent; }
QLabel#miuixLocation { font-size: 11px; color: %7; background: transparent; }
QWidget#miuixPaneSwitch { background: transparent; border: none; }
QWidget#miuixPaneSwitch QToolButton { background: transparent; border: none; padding: 0 14px; font-size: 12px; color: %7; }
QWidget#miuixPaneSwitch QToolButton:checked { background: transparent; color: white; font-weight: 600; }
QPushButton#workspaceSearch { background: %4; border: 1px solid %6; border-radius: 12px; min-height: 34px; padding: 0 12px; min-width: 0; color: %7; }
QPushButton#workspaceSearch:hover { background: %4; border-color: %8; }
QPushButton#appearanceButton { background: transparent; border: none; border-radius: 12px; padding: 0; }
QPushButton#appearanceButton:hover { background: %4; }
QWidget#navigationSidebar, QScrollArea#navigationScroll, QWidget#navigationLocations { background: %2; border: none; }
QWidget#navigationSidebar { border-right: 1px solid %6; }
QLabel#brandTitle { font-size: 16px; font-weight: 600; letter-spacing: 0; }
QLabel#navigationSection { font-size: 11px; font-weight: 600; color: %7; padding: 12px 10px 4px; }
QWidget#navigationSidebar QPushButton[navigationItem="true"] { background: transparent; border: none; border-radius: 12px; padding: 8px 10px; min-height: 20px; text-align: left; color: %7; }
QWidget#navigationSidebar QPushButton[navigationItem="true"]:hover { background: %4; color: palette(text); }
QWidget#navigationSidebar QPushButton[navigationItem="true"]:checked { background: transparent; color: %8; font-weight: 600; }
QFrame#filePane, QFrame#filePane[active="true"] { background: transparent; border: 1px solid transparent; border-radius: 0; }
QWidget#miuixPaneHeading { background: transparent; }
QLabel#miuixFolderTitle { font-size: 20px; font-weight: 600; }
QLabel#miuixFolderDetails { font-size: 12px; color: %7; }
QLabel#paneIndicator { color: %8; padding-left: 3px; }
QTabBar#paneTabs { background: transparent; }
QTabBar#paneTabs::tab { background: transparent; padding: 8px 14px; margin: 0 2px; border: none; border-radius: 12px; font-size: 12px; min-width: 54px; }
QTabBar#paneTabs::tab:hover:!selected { background: %4; }
QTabBar#paneTabs::tab:selected { background: transparent; color: %8; font-weight: 600; }
QLineEdit#panePath, QLineEdit#panePath:hover, QLineEdit#panePath:focus { background: transparent; border: 1px solid transparent; border-radius: 0; padding: 8px 12px; font-size: 12px; }
QLabel#paneFooter { padding: 7px 8px; border: none; font-size: 11px; color: %7; }
QWidget#workspaceToolbar { background: transparent; border: none; min-height: 52px; max-height: 52px; }
QWidget#workspaceToolbar QPushButton { border: none; border-radius: 12px; min-height: 20px; padding: 8px 14px; font-weight: 600; }
QWidget#workspaceToolbar QPushButton[compactToolbar="true"] { padding: 8px; }
QWidget#workspaceToolbar QPushButton[primaryAction="true"] { background: %8; color: white; }
QSplitter#fileSplitter::handle { background: transparent; width: 12px; }
QWidget#extensionSidebar { background: %3; border-left: 1px solid %6; }
QTabWidget#extensionTabs::pane { background: %3; border: none; }
QTabBar#extensionTabBar::tab { padding: 10px 12px; margin: 0 2px; border: none; border-radius: 0; background: transparent; }
QTabBar#extensionTabBar::tab:selected { background: transparent; color: %8; }
QWidget#aiPanel, QWidget#aiScopeRow, QTextEdit#aiTranscript { background: %3; }
QFrame#aiComposer, QFrame#aiComposer[focused="true"] { background: transparent; border: 1px solid transparent; border-radius: 0; }
QDialog QPushButton, QDialog QToolButton { border: 1px solid transparent; border-radius: 12px; padding: 8px 18px; min-height: 20px; background: %4; font-weight: 600; }
QDialog QPushButton:hover, QDialog QToolButton:hover { background: %5; }
QDialog QPushButton:default, QDialog QPushButton:checked, QDialog QToolButton:checked { background: %8; color: white; }
QDialog QLineEdit, QDialog QComboBox, QDialog QSpinBox, QDialog QDoubleSpinBox { border: 1px solid %6; border-radius: 12px; padding: 8px 12px; min-height: 22px; background: %4; }
QDialog QLineEdit:focus, QDialog QComboBox:focus, QDialog QSpinBox:focus { border-color: %8; background: %3; }
QComboBox QAbstractItemView { background: %5; border: 1px solid %6; padding: 4px; }
QGroupBox { border: 1px solid %6; border-radius: 16px; padding: 12px; }
QMenu { background: transparent; border: 1px solid transparent; padding: 6px; border-radius: 0; }
QMenu::item { padding: 8px 24px; border-radius: 8px; }
QMenu::item:selected { background: %4; color: palette(text); }
QMenu::separator { background: %6; height: 1px; margin: 4px 6px; }
QProgressBar { min-height: 6px; max-height: 6px; background: %4; border: none; border-radius: 3px; }
QProgressBar::chunk { background: %8; border-radius: 3px; }
QScrollBar:vertical { width: 6px; background: transparent; }
QScrollBar::handle:vertical { background: palette(mid); border-radius: 3px; min-height: 32px; }
QScrollBar:horizontal { height: 6px; background: transparent; }
QStatusBar { background: %1; color: %7; font-size: 11px; border-top: 1px solid %6; }
)").arg(t.background.name(), t.surface1.name(), t.surface2.name(), t.surface3.name(),
         t.surface4.name(), t.divider.name(), t.secondaryText.name(), t.primary.name());
    sheet += QStringLiteral("QDialog QPushButton:disabled, QDialog QToolButton:disabled { color: %1; background: %2; }\n")
        .arg(t.disabledText.name(), t.surface2.name());
    Q_UNUSED(dark);
    return sheet;
}

}
