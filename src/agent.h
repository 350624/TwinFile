#pragma once

#include "ai.h"
#include "core.h"
#include <QFutureWatcher>

namespace twinfile {

enum class AgentPermission { ConfirmChanges, FullAccess };

class AgentTools {
public:
    static QJsonArray definitions(AgentPermission permission = AgentPermission::ConfirmChanges);
    static bool validRoot(const QString &root, QString *error);
    static QString approval(const QString &root, const QString &name, const QJsonObject &arguments, QString *error);
    static QJsonObject execute(const QString &root, const QString &name, const QJsonObject &arguments,
                               bool approved, const Cancellation &cancel, AgentPermission permission = AgentPermission::ConfirmChanges);
};

class Agent : public QObject {
    Q_OBJECT
public:
    explicit Agent(QObject *parent = nullptr);
    ~Agent() override;
    bool busy() const { return active_ || workerPending_ || finishPending_; }
    bool start(const AiSettings &settings, const QString &root, const QString &question, QString *error,
               AgentPermission permission = AgentPermission::ConfirmChanges);
    void cancel();
    void resolveApproval(bool approved);
signals:
    void activity(const QString &text);
    void approvalRequested(const QString &description);
    void changedFiles();
    void completed(const QString &text);
    void failed(const QString &message);
private:
    void request();
    void receive();
    void nextTool();
    void executeTool(bool approved);
    void toolResult(const QJsonObject &result);
    void finish(const QString &message, bool success);
    void deliverFinish();
    void disposeReply();
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QTimer deadline_;
    QTimer requestDeadline_;
    QFutureWatcher<QJsonObject> worker_;
    Cancellation cancel_;
    AiSettings settings_;
    QString root_;
    AgentPermission permission_ = AgentPermission::ConfirmChanges;
    QJsonArray messages_;
    QJsonArray calls_;
    QByteArray buffer_;
    int callIndex_ = 0;
    int callCount_ = 0;
    quint64 generation_ = 0;
    bool active_ = false;
    bool awaitingApproval_ = false;
    bool workerPending_ = false;
    bool finishPending_ = false;
    bool finishSuccess_ = false;
    QString finishMessage_;
};

}
