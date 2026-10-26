#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QWidget>
#include <functional>

class QLabel;
class QProgressBar;
class QToolButton;
class QTimer;

namespace twinfile {

class TaskProgress : public QWidget {
public:
    explicit TaskProgress(QWidget *parent = nullptr);
    bool active() const { return active_; }
    void begin(const QString &title, std::function<void()> cancel);
    void updateProgress(qint64 done, qint64 total, const QString &stage, double speed = -1);
    void updateNetworkProgress(qint64 done, qint64 total);
    void finish(const QString &message, bool success);
    void cancel();
protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    void refresh();
    QLabel *label_, *detail_;
    QProgressBar *bar_;
    QToolButton *cancel_;
    QTimer *timer_;
    QElapsedTimer elapsed_;
    QString title_, stage_, detailText_;
    qint64 done_ = 0, total_ = 0, sampleBytes_ = 0, sampleAt_ = 0;
    qint64 networkDone_ = 0, networkTotal_ = 0;
    QList<QPair<qint64, qint64>> samples_;
    bool networkMode_ = false;
    double speed_ = 0;
    bool active_ = false;
    std::function<void()> cancelAction_;
};

}
