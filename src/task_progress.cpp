#include "task_progress.h"
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace twinfile {
namespace {
QString bytes(qint64 value) {
    double number = double(qMax<qint64>(0, value));
    const QStringList units{QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB")};
    int unit = 0;
    while (number >= 1024 && unit < units.size() - 1) { number /= 1024; ++unit; }
    return QString::number(number, 'f', unit ? 1 : 0) + QChar(' ') + units[unit];
}
}

TaskProgress::TaskProgress(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("taskProgress"));
    setMinimumWidth(170); setMaximumWidth(440);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); setFixedHeight(44);
    auto *layout = new QHBoxLayout(this); layout->setContentsMargins(8, 0, 4, 0); layout->setSpacing(6);
    auto *column = new QVBoxLayout; column->setContentsMargins(0, 0, 0, 0); column->setSpacing(2);
    label_ = new QLabel; label_->setObjectName(QStringLiteral("taskStage"));
    detail_ = new QLabel; detail_->setObjectName(QStringLiteral("taskSpeed"));
    for (auto *label : {label_, detail_}) { label->setMinimumWidth(0); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); }
    bar_ = new QProgressBar; bar_->setObjectName(QStringLiteral("jobProgress")); bar_->setFixedHeight(6); bar_->setTextVisible(false);
    column->addWidget(label_); column->addWidget(bar_); column->addWidget(detail_); layout->addLayout(column, 1);
    cancel_ = new QToolButton; cancel_->setObjectName(QStringLiteral("taskCancel"));
    cancel_->setToolTip(QStringLiteral("取消当前任务")); cancel_->setAccessibleName(cancel_->toolTip());
    cancel_->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton)); cancel_->setFixedSize(24, 24);
    layout->addWidget(cancel_); connect(cancel_, &QToolButton::clicked, this, &TaskProgress::cancel);
    timer_ = new QTimer(this); timer_->setInterval(250);
    connect(timer_, &QTimer::timeout, this, [this] {
        const qint64 now = elapsed_.elapsed();
        if (now - sampleAt_ >= 500) {
            speed_ = double(qMax<qint64>(0, done_ - sampleBytes_)) * 1000 / qMax<qint64>(1, now - sampleAt_);
            sampleBytes_ = done_; sampleAt_ = now; refresh();
        }
    });
    hide();
}

void TaskProgress::begin(const QString &title, std::function<void()> cancel) {
    title_ = stage_ = title; cancelAction_ = std::move(cancel); active_ = true;
    done_ = total_ = sampleBytes_ = sampleAt_ = 0; speed_ = 0; elapsed_.start();
    bar_->setRange(0, 0); cancel_->show(); cancel_->setEnabled(true); timer_->start(); refresh(); show();
}

void TaskProgress::updateProgress(qint64 done, qint64 total, const QString &stage, double speed) {
    if (!active_) return;
    if (done < done_) { sampleBytes_ = done; sampleAt_ = elapsed_.elapsed(); speed_ = 0; }
    done_ = done; total_ = total;
    if (cancel_->isEnabled()) stage_ = stage.isEmpty() ? title_ : stage;
    if (speed >= 0) { speed_ = speed; sampleBytes_ = done; sampleAt_ = elapsed_.elapsed(); }
    bar_->setRange(0, total > 0 ? 1000 : 0);
    if (total > 0) bar_->setValue(int(qBound(0.0, double(done) / double(total), 1.0) * 1000));
    refresh();
}

void TaskProgress::finish(const QString &message, bool success) {
    timer_->stop(); active_ = false; cancelAction_ = {};
    stage_ = message; detailText_ = title_; cancel_->hide();
    bar_->setRange(0, 1000); bar_->setValue(success ? 1000 : 0); refresh();
}

void TaskProgress::cancel() {
    if (!active_ || !cancelAction_ || !cancel_->isEnabled()) return;
    cancel_->setEnabled(false); stage_ = QStringLiteral("正在取消…"); refresh();
    const auto action = cancelAction_; action();
}

void TaskProgress::refresh() {
    if (active_) {
        const QString amount = total_ > 0
            ? QStringLiteral("%1% · %2 / %3").arg(int(qBound(0.0, double(done_) / double(total_), 1.0) * 100)).arg(bytes(done_), bytes(total_))
            : bytes(done_);
        detailText_ = bytes(qint64(speed_)) + QStringLiteral("/s · ") + amount;
    }
    label_->setText(label_->fontMetrics().elidedText(stage_.simplified(), Qt::ElideMiddle, label_->width()));
    detail_->setText(detail_->fontMetrics().elidedText(detailText_, Qt::ElideRight, detail_->width()));
    setToolTip(stage_ + QChar('\n') + detailText_);
}
void TaskProgress::resizeEvent(QResizeEvent *event) { QWidget::resizeEvent(event); refresh(); }
void TaskProgress::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        cancel_->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton));
}

}
