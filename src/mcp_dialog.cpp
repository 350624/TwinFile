#include "mcp.h"
#include "motion.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace twinfile {

void showMcpDialog(McpServer *server, McpSettings settings, QWidget *parent, const std::function<void(const McpSettings &)> &save) {
    QDialog dialog(parent); dialog.setObjectName("mcpDialog"); dialog.setWindowTitle(QStringLiteral("MCP 服务")); dialog.resize(660, 540);
    auto *layout = new QVBoxLayout(&dialog);
    auto *status = new QLabel; status->setObjectName("mcpStatus"); layout->addWidget(status);
    auto *tabs = new MotionTabWidget; layout->addWidget(tabs, 1);
    auto *general = new QWidget; auto *form = new QFormLayout(general);
    auto *homeRow = new QWidget; auto *homeLayout = new QHBoxLayout(homeRow); homeLayout->setContentsMargins(0, 0, 0, 0);
    auto *home = new QLineEdit(settings.home); home->setObjectName("mcpHome"); auto *browse = new QPushButton(QStringLiteral("选择…")); homeLayout->addWidget(home, 1); homeLayout->addWidget(browse);
    form->addRow(QStringLiteral("Home 目录"), homeRow);
    auto *port = new QSpinBox; port->setRange(1, 65535); port->setValue(settings.port); port->setObjectName("mcpPort"); form->addRow(QStringLiteral("端口"), port);
    auto *lan = new QCheckBox(QStringLiteral("允许局域网连接（HTTP 不加密）")); lan->setChecked(settings.lan); lan->setObjectName("mcpLan"); form->addRow(lan);
    auto *endpoint = new QLabel; endpoint->setObjectName("mcpEndpoint"); endpoint->setTextInteractionFlags(Qt::TextSelectableByMouse); endpoint->setWordWrap(true); form->addRow(QStringLiteral("地址"), endpoint);
    auto *copy = new QPushButton(QStringLiteral("复制连接配置")); copy->setObjectName("mcpCopyConfig"); form->addRow(copy);
    auto *tip = new QLabel(QStringLiteral("先配置目录与令牌，再启动。模型由外部 AI 工具提供；关闭 TwinFile 即停止服务。")); tip->setWordWrap(true); form->addRow(tip);
    tabs->addTab(general, QStringLiteral("通用"));
    auto *files = new QWidget; auto *fileLayout = new QVBoxLayout(files);
    auto *permissionTip = new QLabel(QStringLiteral("Home 始终可读写。其他目录从上到下首条匹配生效，默认拒绝；写权限包含覆盖、移动和删除，不再逐项询问。")); permissionTip->setWordWrap(true); fileLayout->addWidget(permissionTip);
    auto *rules = new QTableWidget(0, 2); rules->setObjectName("mcpRules"); rules->setHorizontalHeaderLabels({QStringLiteral("目录"), QStringLiteral("权限")}); rules->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch); rules->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents); fileLayout->addWidget(rules, 1);
    auto addRule = [&](const McpRule &rule) {
        const int row = rules->rowCount(); rules->insertRow(row); rules->setItem(row, 0, new QTableWidgetItem(rule.path));
        auto *access = new QComboBox; access->addItem(QStringLiteral("只读"), "read"); access->addItem(QStringLiteral("读写"), "write"); access->addItem(QStringLiteral("拒绝"), "deny"); access->setCurrentIndex(qMax(0, access->findData(rule.access))); rules->setCellWidget(row, 1, access);
    };
    for (const auto &rule : settings.rules) addRule(rule);
    auto *ruleButtons = new QHBoxLayout; auto *add = new QPushButton(QStringLiteral("添加目录")); auto *remove = new QPushButton(QStringLiteral("移除")); auto *up = new QPushButton(QStringLiteral("上移")); auto *down = new QPushButton(QStringLiteral("下移"));
    for (auto *button : {add, remove, up, down}) ruleButtons->addWidget(button);
    fileLayout->addLayout(ruleButtons); tabs->addTab(files, QStringLiteral("文件权限"));
    auto *apk = new QWidget; auto *apkLayout = new QVBoxLayout(apk);
    auto *apkTip = new QLabel(QStringLiteral("APK：应用与权限、XML/布局、ZIP 条目、DEX ↔ Smali、清单修改和测试签名。\n\n工作区放在授权目录内，用文件工具编辑，再回编 DEX、替换包内条目并签名。原签名无法保留，请先备份。\n\n暂不支持 Java/so 反编译、资源工程回编、自动安装或自定义签名设置。工作区不自动清理。")); apkTip->setWordWrap(true); apkLayout->addWidget(apkTip); apkLayout->addStretch(); tabs->addTab(apk, "APK / ZIP");
    auto *phone = new QWidget; auto *phoneForm = new QFormLayout(phone);
    auto *serial = new QLineEdit(settings.phoneSerial); serial->setObjectName("mcpPhoneSerial"); serial->setPlaceholderText(QStringLiteral("留空禁用，可在设备属性或 adb devices 查看")); phoneForm->addRow(QStringLiteral("手机序列号"), serial);
    auto *remote = new QLineEdit(settings.phoneRoot); remote->setObjectName("mcpPhoneRoot"); remote->setPlaceholderText("/storage/emulated/0/Download"); phoneForm->addRow(QStringLiteral("手机授权目录"), remote);
    auto *phoneWrite = new QCheckBox(QStringLiteral("允许新建手机文件（不覆盖）")); phoneWrite->setChecked(settings.phoneWrite); phoneForm->addRow(phoneWrite);
    auto *phoneTip = new QLabel(QStringLiteral("需手机授权 USB 调试。提供列目录、读取和可选新建文件；不开放任意 shell、安装、覆盖或删除手机文件。")); phoneTip->setWordWrap(true); phoneForm->addRow(phoneTip); tabs->addTab(phone, QStringLiteral("手机"));
    auto *auth = new QWidget; auto *authForm = new QFormLayout(auth);
    auto *token = new QLineEdit; token->setReadOnly(true); token->setObjectName("mcpToken"); token->setEchoMode(QLineEdit::Password); token->setPlaceholderText(settings.tokenHash.isEmpty() ? QStringLiteral("尚未创建") : QStringLiteral("已保存令牌摘要，旧令牌不回显"));
    auto *generate = new QPushButton(QStringLiteral("创建 / 更换令牌")); generate->setObjectName("mcpGenerateToken"); auto *copyToken = new QPushButton(QStringLiteral("复制新令牌")); copyToken->setEnabled(false);
    authForm->addRow(QStringLiteral("Bearer 令牌"), token); authForm->addRow(generate); authForm->addRow(copyToken);
    auto *authTip = new QLabel(QStringLiteral("所有连接必须认证。新令牌只在此窗口保留，配置仅保存 SHA256 摘要；更换后旧令牌失效。不要把令牌发给模型或提交到仓库。")); authTip->setWordWrap(true); authForm->addRow(authTip); tabs->addTab(auth, QStringLiteral("认证"));
    auto *history = new QPlainTextEdit; history->setReadOnly(true); history->setObjectName("mcpHistory"); tabs->addTab(history, QStringLiteral("记录"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); auto *start = buttons->addButton(QStringLiteral("启动"), QDialogButtonBox::ActionRole); start->setObjectName("mcpStart"); auto *stop = buttons->addButton(QStringLiteral("停止"), QDialogButtonBox::ActionRole); stop->setObjectName("mcpStop"); auto *apply = buttons->addButton(QStringLiteral("保存设置"), QDialogButtonBox::ActionRole); apply->setObjectName("mcpSave"); layout->addWidget(buttons);
    auto collect = [&] {
        McpSettings value = settings; value.home = QDir::fromNativeSeparators(home->text().trimmed()); value.port = quint16(port->value()); value.lan = lan->isChecked(); value.phoneSerial = serial->text().trimmed(); value.phoneRoot = remote->text().trimmed(); value.phoneWrite = phoneWrite->isChecked(); value.rules.clear();
        for (int row = 0; row < rules->rowCount(); ++row) value.rules.append({QDir::fromNativeSeparators(rules->item(row, 0)->text().trimmed()), static_cast<QComboBox *>(rules->cellWidget(row, 1))->currentData().toString()});
        return value;
    };
    auto update = [&] {
        const bool locked = server->running() || server->busy();
        for (auto *page : {files, phone, auth}) page->setEnabled(!locked);
        for (QWidget *control : QList<QWidget *>{home, browse, port, lan}) control->setEnabled(!locked);
        copy->setEnabled(server->running()); start->setEnabled(!locked); apply->setEnabled(!locked); stop->setEnabled(server->running() || server->busy());
        status->setText(server->running() ? server->busy() ? QStringLiteral("运行中 · 正在处理工具请求") : QStringLiteral("运行中 · 等待连接") : server->busy() ? QStringLiteral("正在停止当前操作…") : QStringLiteral("已停止"));
        QString address = server->endpoint();
        if (server->running() && settings.lan) for (const auto &entry : QNetworkInterface::allAddresses()) if (entry.protocol() == QAbstractSocket::IPv4Protocol && !entry.isLoopback()) address += QStringLiteral("\nhttp://%1:%2/mcp").arg(entry.toString()).arg(port->value());
        endpoint->setText(address.isEmpty() ? QStringLiteral("服务未启动") : address);
        history->setPlainText(QStringLiteral("仅记录本次运行的工具名称与状态，不保存文件内容或令牌。\n") + server->history().join(u'\n'));
    };
    QObject::connect(server, &McpServer::stateChanged, &dialog, update);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&] { const QString directory = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("选择 Home"), home->text()); if (!directory.isEmpty()) home->setText(directory); });
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] { if (rules->rowCount() >= 32) return; const QString directory = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("授权目录")); if (!directory.isEmpty()) addRule({directory, "read"}); });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] { if (rules->currentRow() >= 0) rules->removeRow(rules->currentRow()); });
    auto move = [&](int delta) {
        const int row = rules->currentRow(), next = row + delta;
        if (row < 0 || next < 0 || next >= rules->rowCount()) return;
        const auto values = collect().rules;
        const auto first = values.at(row), second = values.at(next);
        rules->item(row, 0)->setText(second.path); static_cast<QComboBox *>(rules->cellWidget(row, 1))->setCurrentIndex(static_cast<QComboBox *>(rules->cellWidget(row, 1))->findData(second.access));
        rules->item(next, 0)->setText(first.path); static_cast<QComboBox *>(rules->cellWidget(next, 1))->setCurrentIndex(static_cast<QComboBox *>(rules->cellWidget(next, 1))->findData(first.access)); rules->setCurrentCell(next, 0);
    };
    QObject::connect(up, &QPushButton::clicked, &dialog, [&] { move(-1); }); QObject::connect(down, &QPushButton::clicked, &dialog, [&] { move(1); });
    QObject::connect(generate, &QPushButton::clicked, &dialog, [&] {
        if (!settings.tokenHash.isEmpty() && QMessageBox::question(&dialog, QStringLiteral("更换令牌"), QStringLiteral("保存设置后，旧令牌将失效。继续？"), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        token->setText(McpSettings::newToken()); settings.tokenHash = QCryptographicHash::hash(token->text().toUtf8(), QCryptographicHash::Sha256); copyToken->setEnabled(true);
    });
    QObject::connect(copyToken, &QPushButton::clicked, &dialog, [&] { qApp->clipboard()->setText(token->text()); });
    QObject::connect(copy, &QPushButton::clicked, &dialog, [&] {
        const QJsonObject config{{"mcpServers", QJsonObject{{"TwinFile", QJsonObject{{"url", server->endpoint()}, {"headers", QJsonObject{{"Authorization", "Bearer <填写访问令牌>"}}}}}}}};
        qApp->clipboard()->setText(QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Indented)));
    });
    auto persist = [&] { const auto value = collect(); QString error; if (!McpTools::validate(value, &error)) { status->setText(error); return false; } settings = value; save(settings); return true; };
    QObject::connect(apply, &QPushButton::clicked, &dialog, [&] { if (persist()) status->setText(QStringLiteral("设置已保存 · 服务未启动")); });
    QObject::connect(start, &QPushButton::clicked, &dialog, [&] {
        if (!persist()) return;
        const QString warning = settings.lan ? QStringLiteral("局域网客户端持令牌即可访问授权目录。HTTP 不加密，请仅在可信网络使用。\n\n") : QString();
        if (QMessageBox::warning(&dialog, QStringLiteral("启动 MCP 服务"), warning + QStringLiteral("AI 可读取授权文件，并在可写目录中直接修改、覆盖和删除，不再逐项询问。文件内容可能发往外部 AI 服务。请确认目录范围并先备份。"), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        QString error; if (!server->start(settings, &error)) status->setText(error); else update();
    });
    QObject::connect(stop, &QPushButton::clicked, server, &McpServer::stop);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    update(); dialog.exec();
}

}
