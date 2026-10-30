#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTemporaryFile>
#include <QTimer>
#include <memory>

class QNetworkReply;
class QCloseEvent;

namespace twinfile {

class TaskProgress;
class DirectDownload : public QObject {
    Q_OBJECT
public:
    explicit DirectDownload(QObject *parent = nullptr);
    ~DirectDownload() override;
    bool start(const QUrl &url, const QString &destination, int connections, QString *error);
    void cancel();
    bool busy() const { return active_; }
    static QList<QPair<qint64, qint64>> ranges(qint64 size, int connections);
signals:
    void progress(qint64 received, qint64 total, double bytesPerSecond, int connections);
    void status(const QString &message);
    void completed(const QString &path);
    void failed(const QString &message);
private:
    friend class DownloadTestAccess;
    struct Part;
    void probe();
    void probeData();
    void probeFinished();
    void requestPart(const std::shared_ptr<Part> &part);
    void dispatchParts();
    void retryPart(const std::shared_ptr<Part> &part, const QString &reason, bool redistribute = false);
    void watchTransfers();
    void partData(const std::shared_ptr<Part> &part);
    void partFinished(const std::shared_ptr<Part> &part);
    void single();
    void singleData();
    void singleFinished();
    bool write(qint64 offset, const QByteArray &bytes);
    void finish(const QString &error = {});
    void cleanup();
    void notify();
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    std::unique_ptr<QTemporaryFile> output_;
    QTimer progressTimer_;
    QTimer watchdog_;
    QTimer deadline_;
    QElapsedTimer elapsed_;
    QList<std::shared_ptr<Part>> parts_;
    QList<QNetworkAccessManager *> networks_;
    QUrl url_;
    QString destination_;
    QByteArray validator_, probeBytes_;
    QByteArray etag_, modified_;
    qint64 total_ = -1, received_ = 0;
    qint64 lastProgressBytes_ = 0, lastProgressAt_ = 0;
    qint64 lastDataAt_ = 0;
    quint64 generation_ = 0;
    int requested_ = 16, connections_ = 1;
    int stallTimeoutMs_ = 15000, slowWindowMs_ = 30000, handshakeTimeoutMs_ = 30000;
    bool active_ = false, singleResponse_ = false, singleValidated_ = false;
};

class DownloadDialog : public QDialog {
    Q_OBJECT
public:
    explicit DownloadDialog(const QString &directory, QWidget *parent = nullptr, const QUrl &initialUrl = {}, const QString &initialFileName = {}, bool startImmediately = false);
    void setTaskProgress(TaskProgress *progress);
signals:
    void destinationSelected(const QString &path);
protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;
private:
    DirectDownload download_;
    TaskProgress *taskProgress_ = nullptr;
};

}
