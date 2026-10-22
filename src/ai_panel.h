#pragma once

#include "ai.h"
#include "agent.h"
#include "twinfile_plugin.h"
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
class QComboBox;
class QDialog;
class QFrame;
class QAction;

namespace twinfile {

class AiPanel : public QWidget {
    Q_OBJECT
public:
    AiPanel(AiSettings *settings, extensions::Host *host, QWidget *parent = nullptr);
    void updateSettingsLabel();
    void clearConversation();
    void revokeAgentAccess();
    bool busy() const { return client_.busy() || agent_.busy(); }
signals:
    void configureRequested();
protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    void send();
    void render();
    void setBusy(bool busy);
    void attachSelection();
    void attachText();
    void previewAttachment(const QString &title, const QString &text);
    void updateComposerHeight();
    void updateAgentScope();
    void clearAttachment();
    AiSettings *settings_;
    extensions::Host *host_;
    AiClient client_;
    Agent agent_;
    QComboBox *mode_ = nullptr;
    QComboBox *permission_ = nullptr;
    QLabel *agentRootLabel_ = nullptr;
    QLabel *privacy_ = nullptr;
    QString agentRoot_;
    QString authorizedAgentRoot_;
    QPointer<QDialog> approval_;
    QJsonArray history_;
    struct Turn { QString question; QString answer; QString status; bool expanded = false; };
    QList<Turn> turns_;
    QString pendingQuestion_;
    QString pendingPrompt_;
    QString attachment_;
    QString lastAnswer_;
    QTextBrowser *transcript_ = nullptr;
    QPlainTextEdit *input_ = nullptr;
    QLabel *service_ = nullptr;
    QLabel *heading_ = nullptr;
    QLabel *attachmentLabel_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *send_ = nullptr;
    QPushButton *configure_ = nullptr;
    QFrame *composer_ = nullptr;
    QWidget *attachmentRow_ = nullptr;
    QWidget *scopeRow_ = nullptr;
    QList<QAction *> idleActions_;
    QList<QWidget *> idleControls_;
    QTimer paintTimer_;
};

class AiExtension : public QObject, public extensions::Plugin {
    Q_OBJECT
    Q_INTERFACES(twinfile::extensions::Plugin)
public:
    explicit AiExtension(QString directory);
    bool activate(extensions::Host *host, QString *error) override;
    void deactivate() override;
    QWidget *createPanel(QWidget *parent) override;
    void configure(QWidget *parent) override;
private:
    QString directory_;
    QString loadError_;
    AiSettings settings_;
    extensions::Host *host_ = nullptr;
    QPointer<AiPanel> panel_;
};

}
