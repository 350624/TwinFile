#include "editor.h"
#include <QApplication>
#include <QAbstractTableModel>
#include <QComboBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTextLayout>
#include <QTableView>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <vector>

namespace twinfile {
TextComparison alignedDifference(const QString &left, const QString &right, int ignore, const Cancellation &cancel) {
    auto lines = [](QString text) { text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QChar('\r'), QChar('\n')); return text.split(QChar('\n')); };
    const auto before = lines(left), after = lines(right);
    if (before.size() > 50000 || after.size() > 50000) return {{}, QStringLiteral("每侧最多比较 50000 行"), false};
    auto key = [ignore](QString value) { if (ignore == 1) return value.trimmed(); if (ignore >= 2) value.remove(QRegularExpression(QStringLiteral("\\s+"))); return value; };
    QList<int> beforeMap, afterMap;
    QStringList first, second;
    for (int index = 0; index < before.size(); ++index) { const QString value = key(before[index]); if (ignore != 3 || !value.isEmpty()) { beforeMap.append(index); first.append(value); } }
    for (int index = 0; index < after.size(); ++index) { const QString value = key(after[index]); if (ignore != 3 || !value.isEmpty()) { afterMap.append(index); second.append(value); } }
    TextComparison result;
    int consumedLeft = 0, consumedRight = 0;
    auto append = [&](int leftIndex, int rightIndex, int kind) {
        const int leftLine = leftIndex < 0 ? -1 : beforeMap[leftIndex], rightLine = rightIndex < 0 ? -1 : afterMap[rightIndex];
        if (ignore == 3) {
            while (consumedLeft < (leftLine < 0 ? consumedLeft : leftLine) && key(before[consumedLeft]).isEmpty()) { result.rows.append({consumedLeft + 1, 0, 0, before[consumedLeft]}); ++consumedLeft; }
            while (consumedRight < (rightLine < 0 ? consumedRight : rightLine) && key(after[consumedRight]).isEmpty()) { result.rows.append({0, consumedRight + 1, 0, after[consumedRight]}); ++consumedRight; }
        }
        result.rows.append({leftLine + 1, rightLine + 1, kind, leftLine >= 0 ? before[leftLine] : after[rightLine]});
        if (leftLine >= 0) consumedLeft = leftLine + 1;
        if (rightLine >= 0) consumedRight = rightLine + 1;
    };
    int prefix = 0;
    while (prefix < first.size() && prefix < second.size() && first[prefix] == second[prefix]) { append(prefix, prefix, 0); ++prefix; }
    int suffix = 0;
    while (suffix < first.size() - prefix && suffix < second.size() - prefix && first[first.size() - suffix - 1] == second[second.size() - suffix - 1]) ++suffix;
    const int leftCount = int(first.size()) - prefix - suffix, rightCount = int(second.size()) - prefix - suffix;
    result.coarse = qint64(leftCount + 1) * (rightCount + 1) > 16000000;
    if (result.coarse) {
        for (int index = 0; index < leftCount; ++index) append(prefix + index, -1, -1);
        for (int index = 0; index < rightCount; ++index) append(-1, prefix + index, 1);
    } else {
        const qsizetype columns = rightCount + 1;
        std::vector<quint32> lengths(size_t(leftCount + 1) * size_t(columns), 0);
        for (int leftIndex = leftCount - 1; leftIndex >= 0; --leftIndex) {
            if (cancel && cancel->load()) return {{}, QStringLiteral("已取消"), false};
            for (int rightIndex = rightCount - 1; rightIndex >= 0; --rightIndex) lengths[size_t(leftIndex * columns + rightIndex)] = first[prefix + leftIndex] == second[prefix + rightIndex] ? 1 + lengths[size_t((leftIndex + 1) * columns + rightIndex + 1)] : qMax(lengths[size_t((leftIndex + 1) * columns + rightIndex)], lengths[size_t(leftIndex * columns + rightIndex + 1)]);
        }
        int leftIndex = 0, rightIndex = 0;
        while (leftIndex < leftCount || rightIndex < rightCount) {
            if (first.value(prefix + leftIndex) == second.value(prefix + rightIndex) && leftIndex < leftCount && rightIndex < rightCount) { append(prefix + leftIndex++, prefix + rightIndex++, 0); continue; }
            if (leftIndex < leftCount && (rightIndex == rightCount || lengths[size_t((leftIndex + 1) * columns + rightIndex)] >= lengths[size_t(leftIndex * columns + rightIndex + 1)])) append(prefix + leftIndex++, -1, -1);
            else append(-1, prefix + rightIndex++, 1);
        }
    }
    for (int index = suffix; index > 0; --index) append(int(first.size()) - index, int(second.size()) - index, 0);
    if (ignore == 3) {
        while (consumedLeft < before.size()) { result.rows.append({consumedLeft + 1, 0, 0, before[consumedLeft]}); ++consumedLeft; }
        while (consumedRight < after.size()) { result.rows.append({0, consumedRight + 1, 0, after[consumedRight]}); ++consumedRight; }
    }
    for (qsizetype start = 0; start < result.rows.size();) {
        if (cancel && cancel->load()) return {{}, QStringLiteral("已取消"), false};
        if (result.rows[start].kind != -1) { ++start; continue; }
        qsizetype middle = start;
        while (middle < result.rows.size() && result.rows[middle].kind == -1) ++middle;
        qsizetype finish = middle;
        while (finish < result.rows.size() && result.rows[finish].kind == 1) ++finish;
        const qsizetype pairs = qMin(middle - start, finish - middle);
        for (qsizetype index = 0; index < pairs; ++index) {
            auto &before = result.rows[start + index]; auto &after = result.rows[middle + index];
            qsizetype prefix = 0, suffix = 0;
            while (prefix < before.text.size() && prefix < after.text.size() && before.text[prefix] == after.text[prefix]) ++prefix;
            if (prefix > 0 && prefix < before.text.size() && before.text[prefix].isLowSurrogate()) --prefix;
            while (suffix < before.text.size() - prefix && suffix < after.text.size() - prefix && before.text[before.text.size() - suffix - 1] == after.text[after.text.size() - suffix - 1]) ++suffix;
            if (suffix > 0 && suffix < before.text.size() && before.text[before.text.size() - suffix].isLowSurrogate()) --suffix;
            before.changedStart = after.changedStart = int(prefix);
            before.changedLength = int(before.text.size() - prefix - suffix); after.changedLength = int(after.text.size() - prefix - suffix);
        }
        start = finish;
    }
    return result;
}
namespace {
class DifferenceModel : public QAbstractTableModel {
public:
    using QAbstractTableModel::QAbstractTableModel;
    QList<DiffRow> rows;
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(rows.size()); }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 3; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override { if (orientation == Qt::Horizontal && role == Qt::DisplayRole) return QStringList{QStringLiteral("左行号"), QStringLiteral("右行号"), QStringLiteral("统一差异视图")}.value(section); return {}; }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() >= rows.size()) return {};
        const auto &row = rows[index.row()];
        if (role == Qt::DisplayRole) return index.column() == 0 ? (row.left ? QVariant(row.left) : QVariant()) : index.column() == 1 ? (row.right ? QVariant(row.right) : QVariant()) : QVariant((row.kind < 0 ? QStringLiteral("− ") : row.kind > 0 ? QStringLiteral("+ ") : QStringLiteral("  ")) + row.text);
        if (role == Qt::BackgroundRole && row.kind) return QColor(row.kind < 0 ? QStringLiteral("#57383c") : QStringLiteral("#285142"));
        if (role == Qt::ForegroundRole && row.kind) return QColor(Qt::white);
        if (role == Qt::FontRole && index.column() == 2) return QFont(QStringLiteral("Consolas"), 10);
        if (role == Qt::UserRole && index.column() == 2 && row.changedStart >= 0) return QVariantList{row.changedStart + 2, row.changedLength};
        return {};
    }
    void replace(QList<DiffRow> replacement) { beginResetModel(); rows = std::move(replacement); endResetModel(); }
};
class DifferenceDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto span = index.data(Qt::UserRole).toList();
        if (span.size() != 2 || span[1].toInt() <= 0) { QStyledItemDelegate::paint(painter, option, index); return; }
        QStyleOptionViewItem styled(option); initStyleOption(&styled, index); const QString text = styled.text; styled.text.clear();
        const QStyle *style = styled.widget ? styled.widget->style() : QApplication::style(); style->drawControl(QStyle::CE_ItemViewItem, &styled, painter, styled.widget);
        const QRect area = style->subElementRect(QStyle::SE_ItemViewItemText, &styled, styled.widget); QTextLayout layout(text, styled.font);
        QTextLayout::FormatRange changed; changed.start = span[0].toInt(); changed.length = span[1].toInt(); changed.format.setBackground(QColor(QStringLiteral("#b45309"))); changed.format.setForeground(Qt::white); layout.setFormats({changed}); layout.beginLayout(); auto line = layout.createLine(); line.setLineWidth(qMax(area.width(), 1)); layout.endLayout();
        painter->save(); painter->setClipRect(area); painter->setPen(styled.state & QStyle::State_Selected ? styled.palette.highlightedText().color() : index.data(Qt::ForegroundRole).canConvert<QColor>() ? index.data(Qt::ForegroundRole).value<QColor>() : styled.palette.text().color()); layout.draw(painter, QPointF(area.left(), area.top() + (area.height() - line.height()) / 2)); painter->restore();
    }
};
}
DifferenceDialog::DifferenceDialog(const QString &left, const QString &right, QWidget *parent) : QDialog(parent) {
    setAttribute(Qt::WA_DeleteOnClose); setWindowTitle(QStringLiteral("文本比较")); resize(1100, 760);
    auto *layout = new QVBoxLayout(this); auto *controls = new QHBoxLayout;
    auto *ignore = new QComboBox; ignore->addItems({QStringLiteral("不忽略"), QStringLiteral("忽略首尾空格"), QStringLiteral("忽略所有空格"), QStringLiteral("忽略空格和空行")}); controls->addWidget(ignore);
    auto *previous = new QPushButton(QStringLiteral("上一处差异")), *next = new QPushButton(QStringLiteral("下一处差异")); controls->addWidget(previous); controls->addWidget(next); controls->addStretch(); layout->addLayout(controls);
    auto *status = new QLabel; layout->addWidget(status); auto *table = new QTableView; auto *model = new DifferenceModel(table); table->setModel(model); table->setItemDelegateForColumn(2, new DifferenceDelegate(table)); table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setWordWrap(false); table->verticalHeader()->hide(); table->verticalHeader()->setDefaultSectionSize(24); table->horizontalHeader()->setStretchLastSection(true); layout->addWidget(table, 1);
    auto generation = std::make_shared<quint64>(0);
    auto update = [this, left, right, status, model, table, generation](int mode) {
        if (cancel_) cancel_->store(true);
        cancel_ = std::make_shared<std::atomic_bool>(false); const auto cancel = cancel_; const quint64 ticket = ++*generation; status->setText(QStringLiteral("正在计算…")); auto *watcher = new QFutureWatcher<TextComparison>(this);
        connect(watcher, &QFutureWatcher<TextComparison>::finished, this, [watcher, status, model, table, generation, ticket] {
            const auto result = watcher->result(); watcher->deleteLater(); if (ticket != *generation) return;
            if (!result.error.isEmpty()) { status->setText(result.error); return; }
            int added = 0, removed = 0; for (const auto &row : result.rows) { added += row.kind > 0; removed += row.kind < 0; }
            status->setText(QStringLiteral("新增 %1 行 · 删除 %2 行%3").arg(added).arg(removed).arg(result.coarse ? QStringLiteral(" · 大差异块采用有界粗粒度对齐") : QString())); model->replace(result.rows); table->resizeColumnToContents(0); table->resizeColumnToContents(1);
        });
        watcher->setFuture(QtConcurrent::run([left, right, mode, cancel] { return alignedDifference(left, right, mode, cancel); }));
    };
    connect(ignore, &QComboBox::currentIndexChanged, this, update);
    auto navigate = [model, table](int step) { const int count = int(model->rows.size()); if (!count) return; const int current = table->currentIndex().row(); for (int distance = 1; distance <= count; ++distance) { const int row = (qMax(0, current) + step * distance + count) % count; if (model->rows[row].kind && (row == 0 || model->rows[row - 1].kind == 0)) { table->selectRow(row); table->scrollTo(model->index(row, 2), QAbstractItemView::PositionAtCenter); return; } } };
    connect(previous, &QPushButton::clicked, this, [navigate] { navigate(-1); }); connect(next, &QPushButton::clicked, this, [navigate] { navigate(1); }); update(0);
}
DifferenceDialog::~DifferenceDialog() { if (cancel_) cancel_->store(true); }
}
