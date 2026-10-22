#include "appearance.h"

#include <QColor>
#include <QMap>
#include <QPainter>
#include <QPainterPath>

namespace twinfile {

QIcon actionSymbol(const QString &name, const QColor &color) {
    QPixmap pixmap(40, 40); pixmap.setDevicePixelRatio(2); pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (name == QStringLiteral("back") || name == QStringLiteral("forward")) {
        if (name == QStringLiteral("forward")) { painter.translate(20, 0); painter.scale(-1, 1); }
        painter.drawLine(QPointF(16, 10), QPointF(4, 10)); painter.drawLine(QPointF(4, 10), QPointF(9, 5)); painter.drawLine(QPointF(4, 10), QPointF(9, 15));
    } else if (name == QStringLiteral("up")) {
        painter.drawLine(QPointF(10, 16), QPointF(10, 4)); painter.drawLine(QPointF(5, 9), QPointF(10, 4)); painter.drawLine(QPointF(10, 4), QPointF(15, 9));
    } else if (name == QStringLiteral("add")) {
        painter.drawLine(QPointF(4, 10), QPointF(16, 10)); painter.drawLine(QPointF(10, 4), QPointF(10, 16));
    } else if (name == QStringLiteral("search")) {
        painter.drawEllipse(QRectF(3, 3, 10, 10)); painter.drawLine(QPointF(12, 12), QPointF(17, 17));
    } else if (name == QStringLiteral("copy")) {
        painter.drawRoundedRect(QRectF(7, 7, 10, 10), 2, 2); QPainterPath shape; shape.moveTo(12, 4); shape.lineTo(12, 3); shape.lineTo(3, 3); shape.lineTo(3, 12); shape.lineTo(4, 12); painter.drawPath(shape);
    } else if (name == QStringLiteral("move")) {
        painter.drawRoundedRect(QRectF(2, 4, 8, 12), 2, 2); painter.drawLine(QPointF(7, 10), QPointF(18, 10)); painter.drawLine(QPointF(14, 6), QPointF(18, 10)); painter.drawLine(QPointF(18, 10), QPointF(14, 14));
    } else if (name == QStringLiteral("delete")) {
        painter.drawLine(QPointF(3, 5), QPointF(17, 5)); painter.drawLine(QPointF(7, 2), QPointF(13, 2)); QPainterPath bin; bin.moveTo(5, 5); bin.lineTo(6, 17); bin.lineTo(14, 17); bin.lineTo(15, 5); painter.drawPath(bin); painter.drawLine(QPointF(9, 8), QPointF(9, 14)); painter.drawLine(QPointF(12, 8), QPointF(12, 14));
    } else if (name == QStringLiteral("sort")) {
        painter.drawLine(QPointF(3, 5), QPointF(12, 5)); painter.drawLine(QPointF(3, 10), QPointF(10, 10)); painter.drawLine(QPointF(3, 15), QPointF(7, 15)); painter.drawLine(QPointF(15, 4), QPointF(15, 16)); painter.drawLine(QPointF(12, 13), QPointF(15, 16)); painter.drawLine(QPointF(15, 16), QPointF(18, 13));
    } else if (name == QStringLiteral("assistant")) {
        painter.drawRoundedRect(QRectF(2, 3, 16, 14), 4, 4); painter.drawLine(QPointF(12, 3), QPointF(12, 17)); painter.drawLine(QPointF(5, 8), QPointF(9, 8)); painter.drawLine(QPointF(5, 12), QPointF(8, 12));
    } else if (name == QStringLiteral("phone")) {
        painter.drawRoundedRect(QRectF(5, 1.5, 10, 17), 2, 2); painter.drawLine(QPointF(8, 4), QPointF(12, 4)); painter.drawPoint(QPointF(10, 16));
    } else if (name == QStringLiteral("drive")) {
        painter.drawRoundedRect(QRectF(2, 4, 16, 13), 2, 2); painter.drawLine(QPointF(2, 12), QPointF(18, 12)); painter.drawPoint(QPointF(14, 14.5));
    } else if (name == QStringLiteral("usb")) {
        painter.drawRoundedRect(QRectF(5, 7, 10, 11), 2, 2); painter.drawRect(QRectF(7, 2, 6, 5)); painter.drawLine(QPointF(9, 3.5), QPointF(9, 5)); painter.drawLine(QPointF(11, 3.5), QPointF(11, 5));
    } else if (name == QStringLiteral("theme")) {
        painter.drawEllipse(QRectF(3, 3, 14, 14)); painter.setBrush(color); painter.drawPie(QRectF(3, 3, 14, 14), 90 * 16, 180 * 16);
    } else if (name == QStringLiteral("more")) {
        painter.setPen(Qt::NoPen); painter.setBrush(color); for (int position : {4, 10, 16}) painter.drawEllipse(QPointF(position, 10), 1.3, 1.3);
    } else {
        QPainterPath folder; folder.moveTo(2, 6); folder.lineTo(2, 16); folder.quadTo(2, 18, 4, 18); folder.lineTo(16, 18); folder.quadTo(18, 18, 18, 16); folder.lineTo(18, 6); folder.lineTo(10, 6); folder.lineTo(8, 3); folder.lineTo(4, 3); folder.quadTo(2, 3, 2, 6); painter.drawPath(folder);
    }
    return QIcon(pixmap);
}

QStringList themeIds() {
    return {QStringLiteral("twinfile"), QStringLiteral("mt"), QStringLiteral("cream"), QStringLiteral("bataUI01"), QStringLiteral("macos")};
}

QString themeName(const QString &theme) {
    static const QMap<QString, QString> names{{QStringLiteral("twinfile"), QStringLiteral("TwinFile")},
        {QStringLiteral("mt"), QStringLiteral("MT · 简洁")}, {QStringLiteral("cream"), QStringLiteral("Cream · 奶油")},
        {QStringLiteral("bataUI01"), QStringLiteral("bataUI01 · 清透蓝")}, {QStringLiteral("macos"), QStringLiteral("macOS · Finder 风格")}};
    return names.value(theme, QStringLiteral("TwinFile"));
}

QIcon sidebarSymbol(const QString &label) {
    QPixmap pixmap(40, 40); pixmap.setDevicePixelRatio(2); pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#2996ed")), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (label == QStringLiteral("此电脑") || label == QStringLiteral("桌面")) {
        painter.drawRoundedRect(QRectF(2, 3, 16, 11), 2, 2); painter.drawLine(QPointF(10, 14), QPointF(10, 18)); painter.drawLine(QPointF(6, 18), QPointF(14, 18));
    } else if (label == QStringLiteral("下载")) {
        painter.drawLine(QPointF(10, 2), QPointF(10, 13)); painter.drawLine(QPointF(6, 9), QPointF(10, 13)); painter.drawLine(QPointF(10, 13), QPointF(14, 9));
        QPainterPath tray; tray.moveTo(3, 13); tray.lineTo(3, 18); tray.lineTo(17, 18); tray.lineTo(17, 13); painter.drawPath(tray);
    } else if (label == QStringLiteral("文档")) {
        painter.drawRoundedRect(QRectF(4, 2, 12, 16), 2, 2); painter.drawLine(QPointF(7, 7), QPointF(13, 7)); painter.drawLine(QPointF(7, 11), QPointF(13, 11)); painter.drawLine(QPointF(7, 15), QPointF(11, 15));
    } else if (label == QStringLiteral("图片")) {
        painter.drawRoundedRect(QRectF(2, 3, 16, 14), 2, 2); painter.drawEllipse(QPointF(7, 7), 1, 1);
        QPainterPath mountain; mountain.moveTo(3, 15); mountain.lineTo(8, 10); mountain.lineTo(11, 13); mountain.lineTo(14, 9); mountain.lineTo(18, 14); painter.drawPath(mountain);
    } else if (label == QStringLiteral("音乐")) {
        QPainterPath note; note.moveTo(7, 15); note.lineTo(7, 5); note.lineTo(16, 3); note.lineTo(16, 13); painter.drawPath(note); painter.setBrush(QColor(QStringLiteral("#2996ed"))); painter.drawEllipse(QPointF(5, 16), 2.5, 1.8); painter.drawEllipse(QPointF(14, 14), 2.5, 1.8);
    } else if (label == QStringLiteral("视频")) {
        painter.drawRoundedRect(QRectF(2, 3, 16, 14), 3, 3); QPainterPath play; play.moveTo(8, 7); play.lineTo(13, 10); play.lineTo(8, 13); play.closeSubpath(); painter.drawPath(play);
    } else {
        QPainterPath folder; folder.moveTo(2, 6); folder.lineTo(2, 17); folder.lineTo(18, 17); folder.lineTo(18, 6); folder.lineTo(10, 6); folder.lineTo(8, 3); folder.lineTo(2, 3); folder.closeSubpath(); painter.drawPath(folder);
    }
    return QIcon(pixmap);
}

void normalizeAppearance(QJsonObject &settings) {
    QString theme = settings.value(QStringLiteral("theme")).toString(QStringLiteral("dark"));
    QString mode = settings.value(QStringLiteral("color_mode")).toString();
    if (theme == QStringLiteral("dark") || theme == QStringLiteral("light")) {
        mode = theme;
        theme = QStringLiteral("twinfile");
    }
    if (mode != QStringLiteral("dark") && mode != QStringLiteral("light")) mode = theme == QStringLiteral("twinfile") ? QStringLiteral("dark") : QStringLiteral("light");
    if (!themeIds().contains(theme)) theme = QStringLiteral("twinfile");
    settings.insert(QStringLiteral("theme"), theme);
    settings.insert(QStringLiteral("color_mode"), mode);
}

QPalette appearancePalette(const QString &theme, bool dark, const QString &accent) {
    static const QMap<QString, QStringList> lightColors{
        {QStringLiteral("twinfile"), {QStringLiteral("#f3f4f7"), QStringLiteral("#ffffff"), QStringLiteral("#edf0f6"), QStringLiteral("#252936"), QStringLiteral("#e2e5ec"), QStringLiteral("#697386")}},
        {QStringLiteral("mt"), {QStringLiteral("#eeeeee"), QStringLiteral("#fafafa"), QStringLiteral("#e5e5e5"), QStringLiteral("#242424"), QStringLiteral("#cccccc"), QStringLiteral("#626262")}},
        {QStringLiteral("cream"), {QStringLiteral("#f3eee5"), QStringLiteral("#fffdf8"), QStringLiteral("#eee4d5"), QStringLiteral("#352e27"), QStringLiteral("#d7cbb9"), QStringLiteral("#74634f")}},
        {QStringLiteral("bataUI01"), {QStringLiteral("#e9eff7"), QStringLiteral("#fcfdff"), QStringLiteral("#e0e9f5"), QStringLiteral("#23344b"), QStringLiteral("#bfcee1"), QStringLiteral("#526985")}},
        {QStringLiteral("macos"), {QStringLiteral("#ececec"), QStringLiteral("#ffffff"), QStringLiteral("#e8edf4"), QStringLiteral("#242424"), QStringLiteral("#cdcdcf"), QStringLiteral("#636368")}}};
    static const QMap<QString, QStringList> darkColors{
        {QStringLiteral("twinfile"), {QStringLiteral("#17191f"), QStringLiteral("#20232b"), QStringLiteral("#2b303c"), QStringLiteral("#e9ecf4"), QStringLiteral("#333845"), QStringLiteral("#a0a8b8")}},
        {QStringLiteral("mt"), {QStringLiteral("#222222"), QStringLiteral("#2b2b2b"), QStringLiteral("#393939"), QStringLiteral("#efefef"), QStringLiteral("#505050"), QStringLiteral("#b0b0b0")}},
        {QStringLiteral("cream"), {QStringLiteral("#29251f"), QStringLiteral("#332e27"), QStringLiteral("#474035"), QStringLiteral("#f2e9db"), QStringLiteral("#5c5245"), QStringLiteral("#c4b59e")}},
        {QStringLiteral("bataUI01"), {QStringLiteral("#182333"), QStringLiteral("#233146"), QStringLiteral("#31445f"), QStringLiteral("#e4eefb"), QStringLiteral("#455c7c"), QStringLiteral("#a5bad5")}},
        {QStringLiteral("macos"), {QStringLiteral("#262628"), QStringLiteral("#1e1e20"), QStringLiteral("#343438"), QStringLiteral("#f0f0f2"), QStringLiteral("#49494d"), QStringLiteral("#b0b0b8")}}};
    const auto colors = (dark ? darkColors : lightColors).value(theme, (dark ? darkColors : lightColors).value(QStringLiteral("twinfile")));
    static const QMap<QString, QString> accents{{QStringLiteral("blue"), QStringLiteral("#0277bd")}, {QStringLiteral("teal"), QStringLiteral("#00695c")}, {QStringLiteral("green"), QStringLiteral("#1b5e20")}, {QStringLiteral("purple"), QStringLiteral("#4527a0")}, {QStringLiteral("orange"), QStringLiteral("#b94300")}, {QStringLiteral("indigo"), QStringLiteral("#4f46e5")}, {QStringLiteral("mtblue"), QStringLiteral("#1565c0")}};
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(colors[0]));
    palette.setColor(QPalette::Base, QColor(colors[1]));
    palette.setColor(QPalette::AlternateBase, QColor(colors[2]));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::ToolTipText}) palette.setColor(role, QColor(colors[3]));
    palette.setColor(QPalette::Button, QColor(colors[1]));
    palette.setColor(QPalette::Mid, QColor(colors[4]));
    palette.setColor(QPalette::PlaceholderText, QColor(colors[5]));
    palette.setColor(QPalette::ToolTipBase, QColor(colors[1]));
    palette.setColor(QPalette::Highlight, QColor(accents.value(accent, theme == QStringLiteral("macos") ? QStringLiteral("#0069d9") : QStringLiteral("#4f46e5"))));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, dark ? QColor(QStringLiteral("#8cbfff")) : QColor(QStringLiteral("#005db5")));
    palette.setColor(QPalette::Light, QColor(colors[2]));
    palette.setColor(QPalette::Dark, QColor(colors[4]));
    palette.setColor(QPalette::Shadow, dark ? QColor(QStringLiteral("#121214")) : QColor(QStringLiteral("#c2c2c6")));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::PlaceholderText}) palette.setColor(QPalette::Disabled, role, QColor(colors[5]));
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(colors[2]));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(colors[5]));
    return palette;
}

QString appearanceStyleSheet(const QString &theme, bool dark, const QPalette &palette) {
    const bool mac = theme == QStringLiteral("macos");
    const int radius = mac ? 9 : theme == QStringLiteral("mt") ? 3 : 6;
    const QString border = palette.color(QPalette::Mid).name();
    const QString accent = palette.color(QPalette::Highlight).name();
    const QString muted = palette.color(QPalette::PlaceholderText).name();
    QString sheet = QStringLiteral(R"(
QWidget { font-family: "Microsoft YaHei UI"; font-size: 13px; color: palette(window-text); }
QMainWindow, QDialog { background: palette(window); }
QFrame#filePane { background: palette(base); border: 1px solid %1; border-radius: %4px; }
QFrame#filePane[active="true"] { border: 1px solid %2; }
QListView, QTreeView, QTableView, QTextEdit, QPlainTextEdit { background: palette(base); color: palette(text); selection-background-color: %2; selection-color: white; border: 1px solid %1; border-radius: %4px; }
QListView#fileView { border: none; border-radius: 0; }
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: palette(base); color: palette(text); border: 1px solid %1; border-radius: %4px; padding: 6px; selection-background-color: %2; selection-color: white; }
QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus, QTextEdit:focus { border: 1px solid %2; }
QComboBox { padding-right: 25px; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox::down-arrow { image: url(:/appearance-chevron.svg); width: 12px; height: 12px; }
QComboBox QAbstractItemView { background: palette(base); color: palette(text); selection-background-color: %2; selection-color: white; }
QPushButton, QToolButton { background: palette(button); color: palette(button-text); border: 1px solid %1; border-radius: %4px; padding: 6px 9px; }
QPushButton:hover, QToolButton:hover { background: palette(alternate-base); border-color: %3; }
QPushButton:pressed, QToolButton:pressed { background: palette(mid); }
QPushButton:checked, QPushButton:default, QToolButton:checked { background: %2; color: white; border-color: %2; }
QPushButton:disabled, QToolButton:disabled { color: %3; background: palette(window); border-color: %1; }
QPushButton:flat { background: transparent; text-align: left; border: 1px solid transparent; }
QPushButton:flat:hover { background: palette(alternate-base); }
QTabWidget::pane { border: 1px solid %1; border-radius: %4px; background: palette(base); }
QTabBar::tab { padding: 8px 12px; margin: 2px; background: palette(window); color: %3; border: 1px solid transparent; border-radius: %4px; }
QTabBar::tab:selected { background: palette(base); color: palette(text); border-color: %1; }
QTabBar::tab:hover:!selected { background: palette(alternate-base); color: palette(text); }
QTabBar::close-button { image: url(:/appearance-close.svg); width: 14px; height: 14px; border-radius: 3px; }
QTabBar::close-button:hover { background: palette(mid); }
QLabel#appearanceHeading { font-size: 18px; font-weight: 600; }
QLabel#subtle, QLabel#paneFooter { color: %3; }
QLabel#paneFooter { padding: 3px 9px; border-top: 1px solid %1; }
QWidget#deselectStrip { background: palette(base); border: none; }
QMenu { background: palette(base); color: palette(text); border: 1px solid %1; padding: 5px; }
QMenu::item { padding: 7px 28px; border-radius: 4px; }
QMenu::item:selected { background: %2; color: white; }
QMenu::separator { height: 1px; background: %1; margin: 5px 8px; }
QToolTip { background: palette(tool-tip-base); color: palette(tool-tip-text); border: 1px solid %1; padding: 6px; }
QGroupBox { border: 1px solid %1; border-radius: %4px; margin-top: 12px; padding: 12px; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; }
QCheckBox, QRadioButton { spacing: 7px; }
QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid %1; border-radius: 4px; background: palette(base); }
QCheckBox::indicator:checked { background: %2; border-color: %2; image: url(:/appearance-check.svg); }
QSplitter::handle { background: palette(window); width: 6px; }
QSplitter::handle:hover { background: palette(mid); }
QDockWidget { titlebar-close-icon: url(:/appearance-close.svg); }
QDockWidget::title { background: palette(window); padding: 10px; border-bottom: 1px solid %1; }
QHeaderView::section { background: palette(window); color: palette(window-text); padding: 7px; border: none; border-bottom: 1px solid %1; }
QStatusBar { background: palette(window); color: %3; }
QStatusBar::item { border: none; }
QScrollBar:vertical { background: transparent; width: 12px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 2px; }
QScrollBar::handle { background: %1; border-radius: 4px; min-height: 28px; min-width: 28px; }
QScrollBar::handle:hover { background: %3; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QProgressBar { border: 1px solid %1; border-radius: 5px; background: palette(base); text-align: center; }
QProgressBar::chunk { background: %2; border-radius: 4px; }
)").arg(border, accent, muted).arg(radius);
    if (mac) sheet += QStringLiteral(R"(
QWidget { font-family: "Segoe UI", "Microsoft YaHei UI"; }
QWidget#navigationSidebar { background: %1; border: 1px solid %2; border-radius: 12px; }
QWidget#navigationSidebar QLabel { background: transparent; border: none; }
QWidget#navigationSidebar QPushButton { padding: 7px 12px; border-radius: 7px; font-size: 14px; }
QLabel#workspaceHeading { font-size: 19px; font-weight: 600; padding: 7px 12px; }
QLabel#workspaceDetail { color: %3; padding-right: 12px; }
QFrame#filePane { border-radius: 12px; }
QTabBar#paneTabs::tab { padding: 9px 15px; border-radius: 7px; }
QTabBar#paneTabs::tab:selected { font-weight: 600; }
QLineEdit#panePath { background: palette(window); padding: 7px 10px; border-radius: 8px; }
QFrame#filePane QToolButton { padding: 5px 7px; border: none; background: transparent; border-radius: 6px; font-size: 16px; }
QFrame#filePane QToolButton:hover { background: palette(alternate-base); }
QPushButton { padding: 7px 12px; border-radius: 8px; }
QDialog { background: palette(window); }
QPlainTextEdit#aiInput { padding: 8px; border-radius: 11px; }
QTextEdit#aiTranscript { padding: 8px; border-radius: 11px; }
QMenu::item { padding: 6px 28px; border-radius: 5px; }
QScrollBar:vertical { width: 10px; }
QScrollBar:horizontal { height: 10px; }
)").arg(dark ? QStringLiteral("#303033") : QStringLiteral("#e1e4e9"), border, muted);
    sheet += QStringLiteral(R"(
QTabWidget#extensionTabs::pane { background: palette(window); border: none; border-radius: 0; }
QTabBar#extensionTabBar::tab { background: transparent; padding: 10px 12px; margin: 0 3px 2px 3px; border: none; border-bottom: 2px solid transparent; border-radius: 0; font-size: 12px; }
QTabBar#extensionTabBar::tab:selected { background: transparent; color: palette(text); border-bottom: 2px solid palette(text); }
QTabBar#extensionTabBar::tab:hover:!selected { background: palette(alternate-base); }
QWidget#aiPanel, QWidget#aiScopeRow, QWidget#aiAttachmentRow { background: palette(window); }
QWidget#aiAttachmentRow { background: transparent; }
QLabel#aiHeading { font-size: 14px; font-weight: 600; }
QLabel#aiService, QLabel#aiAgentRoot, QLabel#aiStatus, QLabel#aiPrivacy { color: %1; font-size: 11px; background: transparent; }
QLabel#aiAttachment { color: palette(text); font-size: 12px; background: transparent; }
QTextEdit#aiTranscript, QTextEdit#aiTranscript:focus { background: palette(window); border: none; border-radius: 0; padding: 0; font-size: 13px; }
QFrame#aiComposer { background: palette(base); border: 1px solid %2; border-radius: 18px; }
QFrame#aiComposer[focused="true"] { border-color: %1; }
QPlainTextEdit#aiInput, QPlainTextEdit#aiInput:focus { background: transparent; border: none; border-radius: 0; padding: 4px; font-size: 13px; selection-background-color: palette(highlight); }
QPushButton[aiIcon="true"], QPushButton#extensionClose { background: transparent; border: none; border-radius: 8px; padding: 0; }
QPushButton[aiIcon="true"]:hover, QPushButton#extensionClose:hover { background: palette(alternate-base); }
QPushButton[aiIcon="true"]:pressed, QPushButton#extensionClose:pressed { background: palette(mid); }
QPushButton[aiIcon="true"]:disabled { background: transparent; border: none; }
QPushButton[aiIcon="true"]::menu-indicator { image: none; width: 0; }
QPushButton#aiSend { background: palette(window-text); border: none; border-radius: 16px; padding: 0; }
QPushButton#aiSend:hover { background: %1; }
QComboBox#aiMode, QComboBox#aiMode:focus { background: transparent; border: none; border-radius: 6px; padding: 4px 22px 4px 4px; font-size: 12px; }
QComboBox#aiMode:hover { background: palette(alternate-base); }
QComboBox#aiPermission { background: transparent; border: none; padding: 4px 22px 4px 4px; font-size: 11px; }
QComboBox#aiPermission:hover { background: palette(alternate-base); }
QPushButton#aiAgentScope { background: transparent; border: none; border-radius: 6px; padding: 4px 6px; font-size: 11px; }
QPushButton#aiAgentScope:hover { background: palette(alternate-base); }
QTextEdit#aiTranscript QScrollBar:vertical { width: 6px; }
QPlainTextEdit#aiInput QScrollBar:vertical { width: 5px; }
)").arg(muted, border);
    sheet += QStringLiteral(R"(
QWidget { font-family: "Segoe UI", "Microsoft YaHei UI"; font-size: 13px; }
QWidget#workspace, QWidget#workspaceContent { background: palette(window); }
QWidget#workspaceHeader { background: palette(window); }
QLabel#workspaceHeading { font-size: 23px; font-weight: 600; padding: 0; }
QLabel#workspaceDetail { color: %1; font-size: 12px; padding: 6px 0 0 2px; }
QPushButton#workspaceSearch { background: palette(base); border: 1px solid %2; border-radius: 9px; padding: 8px 14px; color: %1; text-align: left; min-width: 156px; }
QPushButton#appearanceButton { background: transparent; border: none; border-radius: 8px; padding: 0; }
QPushButton#appearanceButton:hover { background: palette(alternate-base); }
QWidget#navigationSidebar { background: palette(window); border: none; border-radius: 0; }
QScrollArea#navigationScroll, QWidget#navigationLocations { background: palette(window); border: none; }
QWidget#navigationSidebar QLabel { background: transparent; border: none; }
QLabel#brandTitle { font-size: 17px; font-weight: 600; letter-spacing: 0.4px; }
QLabel#navigationSection { color: %1; font-size: 11px; padding: 0 10px 8px 10px; }
QWidget#navigationSidebar QPushButton[navigationItem="true"] { text-align: left; background: transparent; border: none; border-radius: 8px; padding: 7px 10px; font-size: 13px; }
QWidget#navigationSidebar QPushButton[navigationItem="true"]:hover { background: palette(alternate-base); }
QWidget#navigationSidebar QPushButton[navigationItem="true"]:checked { background: palette(alternate-base); color: palette(text); font-weight: 600; }
QWidget#workspaceToolbar { background: transparent; min-height: 38px; }
QWidget#workspaceToolbar QPushButton { background: transparent; border: none; border-radius: 8px; padding: 8px 10px; }
QWidget#workspaceToolbar QPushButton:hover { background: palette(alternate-base); }
QWidget#workspaceToolbar QPushButton:pressed { background: palette(mid); }
QWidget#workspaceToolbar QPushButton[primaryAction="true"] { background: palette(alternate-base); color: palette(text); padding: 8px 13px; font-weight: 600; }
QWidget#workspaceToolbar QPushButton#assistantToggle:checked { background: palette(alternate-base); color: palette(text); }
QSplitter#fileSplitter::handle { background: palette(window); width: 12px; }
QSplitter#fileSplitter { qproperty-handleWidth: 12; }
QSplitter#fileSplitter::handle:hover { background: palette(alternate-base); border-radius: 5px; }
QFrame#filePane, QFrame#filePane[active="true"] { background: palette(base); border: 1px solid %2; border-radius: 12px; }
QLabel#paneIndicator { color: %2; font-size: 9px; padding: 0 3px; }
QFrame#filePane[active="true"] QLabel#paneIndicator { color: %3; }
QTabBar#paneTabs { background: transparent; }
QTabBar#paneTabs::tab { background: transparent; border: none; border-radius: 7px; margin: 0 3px 0 0; padding: 7px 10px; color: %1; min-width: 40px; max-width: 185px; }
QTabBar#paneTabs::tab:selected { background: palette(alternate-base); color: palette(text); font-weight: 600; }
QTabBar#paneTabs::tab:hover:!selected { background: palette(window); }
QFrame#filePane QToolButton { background: transparent; border: none; border-radius: 7px; padding: 0; }
QFrame#filePane QToolButton:hover { background: palette(alternate-base); }
QLineEdit#panePath, QLineEdit#panePath:focus { background: palette(window); border: 1px solid transparent; border-radius: 7px; padding: 6px 8px; font-size: 12px; }
QLineEdit#panePath:focus { border-color: %3; }
QWidget#paneColumns { background: transparent; border-bottom: 1px solid %2; }
QLabel#paneColumnLabel, QLabel#paneSourceLabel { color: %1; font-size: 11px; background: transparent; }
QLabel#paneFooter { color: %1; font-size: 11px; padding: 5px 10px; border-top: 1px solid %2; }
QListView#fileView { background: palette(base); border: none; outline: none; }
QWidget#deselectStrip { background: palette(base); }
QStatusBar { background: palette(window); color: %1; font-size: 11px; padding: 3px 12px; }
QLabel#workspaceShortcuts { color: %1; font-size: 10px; padding-right: 12px; }
QMainWindow::separator { background: palette(window); width: 12px; }
QMainWindow::separator:hover { background: palette(alternate-base); }
QWidget#extensionSidebar { background: palette(window); border-left: 1px solid %2; }
QTabWidget#extensionTabs::pane { background: palette(window); border: none; }
QTabBar#extensionTabBar::tab { margin: 10px 0 8px 10px; padding: 7px 12px; border: none; border-radius: 8px; color: %1; background: transparent; }
QTabBar#extensionTabBar::tab:selected { background: palette(alternate-base); color: palette(text); border: none; }
QFrame#aiComposer { background: palette(base); border: 1px solid %2; border-radius: 16px; }
QLabel#aiHeading { font-size: 14px; font-weight: 600; }
QLabel#aiPrivacy { padding-top: 3px; font-size: 10px; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 3px 0; }
QScrollBar:horizontal { background: transparent; height: 8px; margin: 0 3px; }
QScrollBar::handle { background: %2; border-radius: 3px; min-height: 30px; min-width: 30px; }
QScrollBar::handle:hover { background: %1; }
QDialog { background: palette(window); }
QPushButton { border-radius: 8px; padding: 7px 12px; }
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { border-radius: 8px; }
QMenu { padding: 7px; }
QMenu::item { padding: 8px 28px; border-radius: 5px; }
QHeaderView::section { padding: 10px; color: %1; }
)").arg(muted, border, accent);
    return sheet;
}

}
