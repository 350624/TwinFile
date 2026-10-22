#pragma once

#include <QDialog>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUrl>

class QNetworkReply;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace twinfile {

struct UpdateRelease {
    QString version;
    QString notes;
    QUrl page;
    bool newer = false;
};

class UpdateChecker : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Checking, Current, Available, Error };
    explicit UpdateChecker(const QString &currentVersion, QObject *parent = nullptr, const QUrl &testEndpoint = {});
    ~UpdateChecker() override;
    static bool parseRelease(const QByteArray &data, const QString &currentVersion, UpdateRelease *release, QString *error);
    void check();
    void cancel();
    void scheduleStartup(bool enabled);
    void setStartupEnabled(bool enabled) { startupEnabled_ = enabled; }
    State state() const { return state_; }
    QString currentVersion() const { return currentVersion_; }
    UpdateRelease release() const { return release_; }
    QString error() const { return error_; }
signals:
    void stateChanged();
private:
    void finish(const QString &error = {});
    void disposeReply();
    QString currentVersion_, error_;
    UpdateRelease release_;
    State state_ = State::Idle;
    QUrl endpoint_;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QTimer deadline_, startup_;
    QByteArray buffer_;
    bool startupEnabled_ = true;
    bool startupScheduled_ = false;
};

class UpdateDialog : public QDialog {
    Q_OBJECT
public:
    UpdateDialog(UpdateChecker *checker, bool startupEnabled, QWidget *parent = nullptr);
signals:
    void startupChanged(bool enabled);
private:
    void render();
    UpdateChecker *checker_;
    QLabel *status_;
    QPlainTextEdit *notes_;
    QPushButton *check_, *download_;
};

}
