#include "inspection.h"
#include "editor.h"
#include <QAbstractTableModel>
#include <QCheckBox>
#include <QColor>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace twinfile {
namespace {
class DirectoryModel : public QAbstractTableModel {
public:
    QList<DirectoryRow> rows;
    using QAbstractTableModel::QAbstractTableModel;
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(rows.size()); }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 4; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override { return orientation == Qt::Horizontal && role == Qt::DisplayRole ? QStringList{QStringLiteral("相对路径"), QStringLiteral("状态"), QStringLiteral("左侧"), QStringLiteral("右侧")}.value(section) : QVariant(); }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() >= rows.size()) return {};
        const auto &row = rows[index.row()];
        if (role == Qt::DisplayRole) return index.column() == 0 ? row.name : index.column() == 1 ? (row.status.isEmpty() ? QStringLiteral("一致") : row.status) : index.column() == 2 ? row.left : row.right;
        if (role == Qt::ToolTipRole) return row.left + QChar('\n') + row.right;
        if (role == Qt::ForegroundRole && !row.status.isEmpty()) return QColor(QStringLiteral("#d97706"));
        return {};
    }
};
class DirectoryFilter : public QSortFilterProxyModel {
public:
    QString directory, text;
    bool different = false, recursive = false;
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void update() { invalidateFilter(); }
protected:
    bool filterAcceptsRow(int index, const QModelIndex &) const override {
        const auto *model = static_cast<DirectoryModel *>(sourceModel());
        const auto &row = model->rows[index];
        if (!row.name.startsWith(directory)) return false;
        if (!recursive && row.name.mid(directory.size()).contains(QChar('/'))) return false;
        if (!text.isEmpty() && !row.name.contains(text, Qt::CaseInsensitive)) return false;
        if (different && row.status.isEmpty() && !row.directory) return false;
        return true;
    }
};
}
DirectoryComparisonDialog::DirectoryComparisonDialog(DirectoryComparison comparison, std::shared_ptr<QTemporaryDir> temporary, QWidget *parent) : QDialog(parent) {
    setAttribute(Qt::WA_DeleteOnClose); setWindowTitle(QStringLiteral("目录比较 · 双击目录进入，双击两侧文本查看差异")); resize(1150, 760);
    auto *layout = new QVBoxLayout(this); auto *status = new QLabel(comparison.summary); status->setWordWrap(true); layout->addWidget(status);
    auto *tools = new QHBoxLayout; auto *up = new QPushButton(QStringLiteral("上一级")); auto *path = new QLabel(QStringLiteral("/")); auto *find = new QLineEdit; find->setPlaceholderText(QStringLiteral("筛选相对路径")); auto *different = new QCheckBox(QStringLiteral("仅差异")); auto *recursive = new QCheckBox(QStringLiteral("包含所有子目录")); auto *save = new QPushButton(QStringLiteral("导出差异"));
    tools->addWidget(up); tools->addWidget(path); tools->addWidget(find, 1); tools->addWidget(different); tools->addWidget(recursive); tools->addWidget(save); layout->addLayout(tools);
    auto *model = new DirectoryModel(this); model->rows = std::move(comparison.rows); auto *filter = new DirectoryFilter(this); filter->setSourceModel(model); auto *table = new QTableView; table->setModel(filter); table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setWordWrap(false); table->verticalHeader()->hide(); table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive); table->setColumnWidth(0, 330); table->setColumnWidth(1, 110); table->setColumnWidth(2, 280); table->horizontalHeader()->setStretchLastSection(true); layout->addWidget(table);
    connect(find, &QLineEdit::textChanged, this, [filter](const QString &value) { filter->text = value; filter->update(); });
    connect(different, &QCheckBox::toggled, this, [filter](bool value) { filter->different = value; filter->update(); });
    connect(recursive, &QCheckBox::toggled, this, [filter](bool value) { filter->recursive = value; filter->update(); });
    connect(up, &QPushButton::clicked, this, [filter, path] { QString value = filter->directory; if (value.endsWith(QChar('/'))) value.chop(1); const int slash = value.lastIndexOf(QChar('/')); filter->directory = slash < 0 ? QString() : value.left(slash + 1); path->setText(QChar('/') + filter->directory); filter->update(); });
    connect(save, &QPushButton::clicked, this, [this, output = comparison.output] { const QString destination = QFileDialog::getSaveFileName(this, QStringLiteral("导出目录差异"), {}, QStringLiteral("文本 (*.txt)")); if (destination.isEmpty()) return; QSaveFile file(destination); const QByteArray bytes = output.toUtf8(); if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) QMessageBox::warning(this, QStringLiteral("保存失败"), file.errorString()); });
    connect(table, &QTableView::doubleClicked, this, [this, temporary, model, filter, path, table, status](const QModelIndex &index) {
        const auto &row = model->rows[filter->mapToSource(index).row()];
        if (row.directory) { filter->directory = row.name + QChar('/'); path->setText(QChar('/') + filter->directory); filter->update(); return; }
        if (row.left.isEmpty() || row.right.isEmpty() || !isText(row.name) || QFileInfo(row.left).isSymLink() || QFileInfo(row.right).isSymLink()) { status->setText(QStringLiteral("请选择两侧均存在的普通文本文件；不会打开或跟随链接。")); return; }
        table->setEnabled(false); auto *watcher = new QFutureWatcher<Result>(this);
        connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, table, status] { const auto result = watcher->result(); watcher->deleteLater(); table->setEnabled(true); if (!result.ok) { status->setText(result.error); return; } (new DifferenceDialog(result.paths[0], result.paths[1], this))->show(); });
        watcher->setFuture(QtConcurrent::run([row, temporary] {
            QStringList texts;
            for (const QString &source : {row.left, row.right}) { QFile input(source); if (!input.open(QIODevice::ReadOnly) || input.size() > 16 * 1024 * 1024) return Result{false, QStringLiteral("文本不可读或超过 16 MiB"), {}, {}}; TextDocument document; const QByteArray bytes = input.readAll(); if (!decodeText(bytes, {}, &document).ok && !decodeText(bytes, QStringLiteral("GB18030"), &document).ok) return Result{false, QStringLiteral("无法无损解码文本"), {}, {}}; texts.append(document.text); }
            return Result{true, {}, {}, texts};
        }));
    });
}
}
