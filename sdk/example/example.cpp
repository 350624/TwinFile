#include "example.h"

#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

bool SelectionExample::activate(twinfile::extensions::Host *host, QString *error) {
    if (!host || host->apiVersion() != twinfile::extensions::ApiVersion) {
        if (error) *error = QStringLiteral("宿主 API 不兼容");
        return false;
    }
    host_ = host;
    return true;
}

void SelectionExample::deactivate() { label_ = nullptr; host_ = nullptr; }

QWidget *SelectionExample::createPanel(QWidget *parent) {
    auto *panel = new QWidget(parent); panel->setObjectName(QStringLiteral("selectionExample"));
    auto *layout = new QVBoxLayout(panel);
    auto *heading = new QLabel(QStringLiteral("选择摘要 · 示例扩展")); layout->addWidget(heading);
    label_ = new QLabel(QStringLiteral("点击按钮读取当前文件栏的选择。\n此插件不读取文件内容，也不联网。")); label_->setTextFormat(Qt::PlainText); label_->setWordWrap(true); layout->addWidget(label_);
    auto *inspect = new QPushButton(QStringLiteral("读取选择")); layout->addWidget(inspect);
    connect(inspect, &QPushButton::clicked, this, &SelectionExample::updateSelection);
    auto *refresh = new QPushButton(QStringLiteral("刷新活动文件栏")); layout->addWidget(refresh);
    connect(refresh, &QPushButton::clicked, this, [this] { if (host_) host_->refreshActivePane(); });
    layout->addStretch(); return panel;
}

QList<twinfile::extensions::Command> SelectionExample::commands() const {
    return {{QStringLiteral("selection.summary"), QStringLiteral("显示选择摘要")}, {QStringLiteral("pane.refresh"), QStringLiteral("刷新活动栏")}};
}

void SelectionExample::execute(const QString &commandId) {
    if (!host_) return;
    if (commandId == QStringLiteral("selection.summary")) updateSelection();
    else if (commandId == QStringLiteral("pane.refresh")) host_->refreshActivePane();
}

void SelectionExample::updateSelection() {
    if (!host_) return;
    const auto selected = host_->selectionContext().value(QStringLiteral("selected")).toArray();
    QStringList names;
    for (const auto &value : selected) names.append(value.toObject().value(QStringLiteral("name")).toString());
    const QString summary = QStringLiteral("已选 %1 项\n%2").arg(selected.size()).arg(names.join(u'\n'));
    if (label_) label_->setText(summary);
    host_->showStatusMessage(summary);
}
