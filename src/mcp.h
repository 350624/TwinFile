#pragma once

#include "core.h"
#include <QFutureWatcher>
#include <QJsonArray>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>

class QWidget;

namespace twinfile {

struct McpRule { QString path; QString access; };
struct McpSettings {
    QString home;
    quint16 port = 8787;
    bool lan = false;
    QByteArray tokenHash;
    QList<McpRule> rules;
    QString phoneSerial, phoneRoot;
    bool phoneWrite = false;
    QJsonObject json() const;
    static McpSettings fromJson(const QJsonObject &object);
    static QString newToken();
};

class McpTools {
public:
    static bool validate(const McpSettings &settings, QString *error);
    static QString resolve(const McpSettings &settings, const QString &path, bool write, bool missing, QString *error);
    static QJsonArray definitions(const McpSettings &settings);
    static QJsonObject execute(const McpSettings &settings, const QString &name, const QJsonObject &arguments, const Cancellation &cancel);
};

class McpServer : public QObject {
    Q_OBJECT
public:
    explicit McpServer(QObject *parent = nullptr);
    ~McpServer() override;
    bool start(const McpSettings &settings, QString *error);
    void stop();
    bool running() const { return listener_.isListening(); }
    bool busy() const { return worker_.isRunning() || !activeTool_.isEmpty(); }
    QString endpoint() const;
    QStringList history() const { return history_; }
signals:
    void stateChanged();
    void filesChanged();
private:
    void accept();
    void receive(QTcpSocket *socket);
    void dispatch(QTcpSocket *socket, const QJsonObject &message);
    void respond(QTcpSocket *socket, int status, const QJsonObject &body = {});
    void rpcError(QTcpSocket *socket, const QJsonValue &id, int code, const QString &message);
    QTcpServer listener_;
    McpSettings settings_;
    QFutureWatcher<QJsonObject> worker_;
    QPointer<QTcpSocket> activeSocket_;
    QJsonValue activeId_;
    QString activeTool_;
    Cancellation cancel_;
    QStringList history_;
};

void showMcpDialog(McpServer *server, McpSettings settings, QWidget *parent, const std::function<void(const McpSettings &)> &save);

}
