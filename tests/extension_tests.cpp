#include "ai.h"
#include "ai_panel.h"
#include "extensions.h"
#include "window.h"
#include "appearance.h"
#include "updates.h"
#include "task_progress.h"
#include "properties.h"

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFrame>
#include <QImage>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QTabWidget>
#include <QToolButton>
#include <QSplitter>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QTest>
#include <QTextBrowser>
#include <QTouchEvent>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <limits>

using namespace twinfile;

class FakeHost : public extensions::Host {
public:
    QJsonObject context{{QStringLiteral("selected"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("测试.txt")}, {QStringLiteral("path"), QStringLiteral("C:/private/测试.txt")}}}}};
    int refreshed = 0;
    QString status;
    QJsonObject selectionContext() const override { return context; }
    void refreshActivePane() override { ++refreshed; }
    void showStatusMessage(const QString &message) override { status = message; }
};

class MockApi : public QObject {
public:
    QTcpServer server;
    QList<QByteArray> requests;
    QList<QJsonObject> bodies;
    std::function<void(QTcpSocket *)> respond;
    MockApi() {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                socket->setParent(this);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    QByteArray buffer = socket->property("buffer").toByteArray() + socket->readAll();
                    socket->setProperty("buffer", buffer);
                    if (socket->property("responded").toBool()) return;
                    const qsizetype separator = buffer.indexOf("\r\n\r\n"); if (separator < 0) return;
                    qint64 length = 0;
                    for (const QByteArray &line : buffer.left(separator).split('\n')) if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
                    if (buffer.size() < separator + 4 + length) return;
                    socket->setProperty("responded", true); requests.append(buffer);
                    bodies.append(QJsonDocument::fromJson(buffer.mid(separator + 4, length)).object());
                    if (respond) respond(socket);
                });
            }
        });
        server.listen(QHostAddress::LocalHost, 0);
    }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort()); }
    static void response(QTcpSocket *socket, const QByteArray &body, const QByteArray &type = "application/json", int status = 200, const QByteArray &extra = {}) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n" + extra + "\r\n" + body);
        socket->disconnectFromHost();
    }
};

class FailingBuiltin : public QObject, public extensions::Plugin {
    Q_OBJECT
    Q_INTERFACES(twinfile::extensions::Plugin)
public:
    bool activate(extensions::Host *, QString *error) override { *error = QStringLiteral("fixture failure"); return false; }
    void deactivate() override {}
    QWidget *createPanel(QWidget *) override { return nullptr; }
};

class NavigationPhoneApi : public QObject {
public:
    QTcpServer server;
    int listings = 0;
    NavigationPhoneApi() {
        server.listen(QHostAddress::LocalHost);
        qputenv("TWINFILE_ADB_TEST_PORT", QByteArray::number(server.serverPort()));
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                socket->setParent(this);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray buffer = socket->property("buffer").toByteArray() + socket->readAll();
                    while (buffer.size() >= 4) {
                        if (socket->property("sync").toBool()) {
                            if (buffer.size() < 8) break;
                            const quint32 count = quint8(buffer[4]) | (quint32(quint8(buffer[5])) << 8) | (quint32(quint8(buffer[6])) << 16) | (quint32(quint8(buffer[7])) << 24);
                            if (count > 4096 || buffer.size() < 8 + count) break;
                            if (buffer.left(4) == "LIST") { ++listings; socket->write(QByteArray("DONE") + QByteArray(16, '\0')); }
                            buffer.remove(0, 8 + count);
                        } else {
                            bool valid = false; const int count = buffer.left(4).toInt(&valid, 16);
                            if (!valid || count > 4096 || buffer.size() < 4 + count) break;
                            const QByteArray service = buffer.mid(4, count); buffer.remove(0, 4 + count);
                            socket->write("OKAY");
                            if (service.endsWith(":features")) socket->write("0000");
                            if (service == "sync:") socket->setProperty("sync", true);
                        }
                    }
                    socket->setProperty("buffer", buffer);
                });
            }
        });
    }
    ~NavigationPhoneApi() override { qunsetenv("TWINFILE_ADB_TEST_PORT"); }
};

static AiSettings apiSettings(const MockApi &api) {
    AiSettings settings; settings.endpoint = api.url(); settings.model = QStringLiteral("mock-model"); settings.apiKey = QStringLiteral("test-secret-not-real"); return settings;
}

static QJsonArray question(const QString &text = QStringLiteral("你好")) {
    return {QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), text}}};
}

static QByteArray jsonAnswer(const QString &answer) {
    return QJsonDocument(QJsonObject{{QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), answer}}}, {QStringLiteral("finish_reason"), QStringLiteral("stop")}}}}}).toJson(QJsonDocument::Compact);
}

static QString nextPatchVersion(const QString &version) {
    QStringList components = version.split(u'.');
    if (components.size() != 3) return {};
    bool valid = false;
    const int patch = components.last().toInt(&valid);
    if (!valid || patch == std::numeric_limits<int>::max()) return {};
    components[2] = QString::number(patch + 1);
    return components.join(u'.');
}

static QByteArray updateReleasePayload(const QString &version, const QString &notes) {
    const QString tag = QStringLiteral("v") + version;
    const QString name = QStringLiteral("TwinFile-%1-Windows-x64-Setup.exe").arg(version);
    const QString page = QStringLiteral("https://github.com/350624/TwinFile/releases/tag/") + tag;
    const QString asset = QStringLiteral("https://github.com/350624/TwinFile/releases/download/%1/%2").arg(tag, name);
    return QJsonDocument(QJsonObject{{QStringLiteral("tag_name"), tag}, {QStringLiteral("html_url"), page},
        {QStringLiteral("draft"), false}, {QStringLiteral("prerelease"), false}, {QStringLiteral("body"), notes},
        {QStringLiteral("assets"), QJsonArray{QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("state"), QStringLiteral("uploaded")},
            {QStringLiteral("size"), 2048}, {QStringLiteral("browser_download_url"), asset}}}}}).toJson(QJsonDocument::Compact);
}

static QString examplePath() { return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("twinfile_example_plugin.dll")); }

class ExtensionTests : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void updateReleaseValidation_data() {
        QTest::addColumn<QByteArray>("payload"); QTest::addColumn<bool>("expected"); QTest::addColumn<QString>("version");
        auto makeRelease = [](const QString &version, const QString &tag, const QString &page, const QString &installerVersion, bool draft = false) {
            const QString name = QStringLiteral("TwinFile-%1-Windows-x64-Setup.exe").arg(installerVersion);
            return QJsonDocument(QJsonObject{{QStringLiteral("tag_name"), tag}, {QStringLiteral("html_url"), page}, {QStringLiteral("draft"), draft},
                {QStringLiteral("prerelease"), false}, {QStringLiteral("body"), QStringLiteral("更新说明")},
                {QStringLiteral("assets"), QJsonArray{QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("state"), QStringLiteral("uploaded")},
                    {QStringLiteral("size"), 1234}, {QStringLiteral("browser_download_url"), QStringLiteral("https://github.com/350624/TwinFile/releases/download/%1/%2").arg(tag, name)}}}}}).toJson();
        };
        QTest::newRow("new-release") << makeRelease(QStringLiteral("3.1.2"), QStringLiteral("v3.1.2"), QStringLiteral("https://github.com/350624/TwinFile/releases/tag/v3.1.2"), QStringLiteral("3.1.2")) << true << QStringLiteral("3.1.2");
        QTest::newRow("current") << makeRelease(QStringLiteral("3.1.1"), QStringLiteral("v3.1.1"), QStringLiteral("https://github.com/350624/TwinFile/releases/tag/v3.1.1"), QStringLiteral("3.1.1")) << true << QStringLiteral("3.1.1");
        QTest::newRow("draft") << makeRelease(QStringLiteral("3.1.2"), QStringLiteral("v3.1.2"), QStringLiteral("https://github.com/350624/TwinFile/releases/tag/v3.1.2"), QStringLiteral("3.1.2"), true) << false << QString();
        QTest::newRow("wrong-page") << makeRelease(QStringLiteral("3.1.2"), QStringLiteral("v3.1.2"), QStringLiteral("https://example.com/fake"), QStringLiteral("3.1.2")) << false << QString();
        QTest::newRow("missing-installer") << makeRelease(QStringLiteral("3.1.2"), QStringLiteral("v3.1.2"), QStringLiteral("https://github.com/350624/TwinFile/releases/tag/v3.1.2"), QStringLiteral("3.1.1")) << false << QString();
        QTest::newRow("invalid-json") << QByteArray("<html/>") << false << QString();
    }
    void updateReleaseValidation() {
        QFETCH(QByteArray, payload); QFETCH(bool, expected); QFETCH(QString, version);
        UpdateRelease release; QString error;
        QCOMPARE(UpdateChecker::parseRelease(payload, QStringLiteral("3.1.1"), &release, &error), expected);
        if (expected) { QCOMPARE(release.version, version); QCOMPARE(release.newer, version != QStringLiteral("3.1.1")); QCOMPARE(release.notes, QStringLiteral("更新说明")); }
        else QVERIFY(!error.isEmpty());
    }
    void updateCheckIsAsyncBoundedAndShowsReleaseNotes() {
        MockApi api;
        const QString currentVersion = QStringLiteral(TWINFILE_VERSION);
        const QString newerVersion = nextPatchVersion(currentVersion);
        QVERIFY(!newerVersion.isEmpty());
        api.respond = [newerVersion](QTcpSocket *socket) { MockApi::response(socket, updateReleasePayload(newerVersion, QStringLiteral("新增检查更新"))); };
        UpdateChecker checker(currentVersion, nullptr, QUrl(api.url())); QSignalSpy states(&checker, &UpdateChecker::stateChanged);
        UpdateDialog dialog(&checker, true); auto *startup = dialog.findChild<QCheckBox *>(QStringLiteral("updateOnStartup"));
        auto *check = dialog.findChild<QPushButton *>(QStringLiteral("updateCheck")); auto *notes = dialog.findChild<QPlainTextEdit *>(QStringLiteral("updateNotes"));
        QVERIFY(startup && check && notes); QVERIFY(startup->isChecked()); QVERIFY(api.requests.isEmpty());
        const QString screenshotRoot = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        if (!screenshotRoot.isEmpty()) { dialog.show(); QTest::qWait(100); QVERIFY(dialog.grab().save(QDir(screenshotRoot).filePath(QStringLiteral("native-update-settings.png")))); }
        startup->click(); QVERIFY(!startup->isChecked());
        check->click(); QCOMPARE(checker.state(), UpdateChecker::State::Checking); QTRY_COMPARE(api.requests.size(), 1);
        QTRY_COMPARE(checker.state(), UpdateChecker::State::Available); QCOMPARE(notes->toPlainText(), QStringLiteral("新增检查更新"));
        QVERIFY(!dialog.findChild<QPushButton *>(QStringLiteral("updateDownload"))->isHidden());
        QVERIFY(api.requests.first().startsWith("GET /v1 HTTP/1.1")); QVERIFY(api.requests.first().toLower().contains("user-agent: twinfile/" + currentVersion.toLatin1()));
        QVERIFY(!api.requests.first().contains("authorization:")); QVERIFY(states.size() >= 2);
    }
    void applicationChecksUpdatesAtStartupAndFromSettings() {
        const QString currentVersion = QStringLiteral(TWINFILE_VERSION);
        const QString newerVersion = nextPatchVersion(currentVersion);
        QVERIFY(!newerVersion.isEmpty());
        MockApi api;
        api.respond = [newerVersion](QTcpSocket *socket) { MockApi::response(socket, updateReleasePayload(newerVersion, QStringLiteral("启动更新测试"))); };
        QJsonObject state = readSettings(settingsPath());
        state.insert(QStringLiteral("check_updates_on_startup"), true);
        state.insert(QStringLiteral("extension_visible"), false);
        state.insert(QStringLiteral("workspace"), QJsonObject());
        state.insert(QStringLiteral("geometry"), QString());
        QVERIFY(writeSettings(settingsPath(), state));
        Window window(nullptr, QUrl(api.url()));
        window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1100, 650); window.show();
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), 1, 5000);
        auto *checker = window.findChild<UpdateChecker *>(QStringLiteral("updateChecker"));
        QVERIFY(checker); QTRY_COMPARE(checker->state(), UpdateChecker::State::Available);
        auto *dialog = window.findChild<UpdateDialog *>(QStringLiteral("updateSettings"));
        QVERIFY(dialog); QVERIFY(dialog->isVisible()); QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("updateStatus"))->text(), QStringLiteral("发现新版本 %1").arg(newerVersion));
        QCOMPARE(dialog->findChild<QPlainTextEdit *>(QStringLiteral("updateNotes"))->toPlainText(), QStringLiteral("启动更新测试"));
        const QString screenshotRoot = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        if (!screenshotRoot.isEmpty()) QVERIFY(dialog->grab().save(QDir(screenshotRoot).filePath(QStringLiteral("native-update-found.png"))));
        auto *startup = dialog->findChild<QCheckBox *>(QStringLiteral("updateOnStartup"));
        auto *check = dialog->findChild<QPushButton *>(QStringLiteral("updateCheck"));
        QVERIFY(startup && startup->isChecked() && check);
        startup->click(); QVERIFY(!startup->isChecked());
        QVERIFY(!readSettings(settingsPath()).value(QStringLiteral("check_updates_on_startup")).toBool(true));
        check->click(); QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), 2, 3000);
        QTRY_COMPARE(checker->state(), UpdateChecker::State::Available);
        bool openedFromMenu = false;
        QTimer::singleShot(0, &window, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            for (QAction *action : menu->actions()) {
                if (action->text() != QStringLiteral("设置") || !action->menu()) continue;
                for (QAction *settingsAction : action->menu()->actions()) {
                    if (settingsAction->text() != QStringLiteral("软件更新…")) continue;
                    settingsAction->trigger(); openedFromMenu = true; menu->close(); return;
                }
            }
        });
        auto *more = window.findChild<QPushButton *>(QStringLiteral("action_more")); QVERIFY(more); more->click(); QVERIFY(openedFromMenu);
        QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), 3, 3000); QTRY_COMPARE(checker->state(), UpdateChecker::State::Available);
        window.close();
    }
    void updateCheckFailureCancelAndStartupOnce() {
        MockApi api; int responseCount = 0; api.respond = [&responseCount](QTcpSocket *socket) { if (++responseCount == 1) MockApi::response(socket, "{}", "application/json", 429); };
        UpdateChecker disabled(QStringLiteral("3.1.2"), nullptr, QUrl(api.url()));
        disabled.scheduleStartup(false); QTest::qWait(2100); QVERIFY(api.requests.isEmpty());
        UpdateChecker checker(QStringLiteral("3.1.2"), nullptr, QUrl(api.url()));
        checker.scheduleStartup(true); QTRY_COMPARE_WITH_TIMEOUT(api.requests.size(), 1, 4000); QTRY_COMPARE(checker.state(), UpdateChecker::State::Error);
        QVERIFY(checker.error().contains(QStringLiteral("请求受限"))); checker.scheduleStartup(true); QTest::qWait(100); QCOMPARE(api.requests.size(), 1);
        checker.check(); QTRY_COMPARE(api.requests.size(), 2); checker.cancel(); QCOMPARE(checker.state(), UpdateChecker::State::Idle);
    }
    void initTestCase() {
        QVERIFY(settings_.isValid()); qputenv("TWINFILE_TEST_MODE", "1");
        qputenv("TWINFILE_SETTINGS", settings_.filePath(QStringLiteral("settings.json")).toUtf8());
        qputenv("TWINFILE_THUMBNAIL_CACHE", settings_.filePath(QStringLiteral("thumbnails")).toUtf8());
        qApp->setStyle(QStringLiteral("Fusion"));
    }
    void endpointValidation_data() {
        QTest::addColumn<QString>("input"); QTest::addColumn<QString>("expected");
        QTest::newRow("base") << QStringLiteral("https://api.example.com/v1/") << QStringLiteral("https://api.example.com/v1/chat/completions");
        QTest::newRow("exact") << QStringLiteral("https://api.example.com/chat/completions") << QStringLiteral("https://api.example.com/chat/completions");
        QTest::newRow("loopback") << QStringLiteral("http://127.0.0.1:1234/v1") << QStringLiteral("http://127.0.0.1:1234/v1/chat/completions");
        QTest::newRow("ipv6") << QStringLiteral("http://[::1]:1234/v1") << QStringLiteral("http://[::1]:1234/v1/chat/completions");
        QTest::newRow("insecure") << QStringLiteral("http://remote.example/v1") << QString();
        QTest::newRow("credentials") << QStringLiteral("https://user:secret@example.com/v1") << QString();
        QTest::newRow("query") << QStringLiteral("https://example.com/v1?api_key=secret") << QString();
        QTest::newRow("fragment") << QStringLiteral("https://example.com/v1#secret") << QString();
        QTest::newRow("file") << QStringLiteral("file:///C:/private.txt") << QString();
        QTest::newRow("empty") << QString() << QString();
    }
    void endpointValidation() {
        QFETCH(QString, input); QFETCH(QString, expected); QString error;
        QCOMPARE(AiSettings::requestUrl(input, &error).toString(), expected);
        QCOMPARE(error.isEmpty(), !expected.isEmpty());
    }
    void modelsEndpointAndExplicitGet() {
        QString error;
        QCOMPARE(AiSettings::modelsUrl(QStringLiteral("https://example.com/v1/chat/completions/"), &error).toString(), QStringLiteral("https://example.com/v1/models"));
        QCOMPARE(AiSettings::modelsUrl(QStringLiteral("https://api.deepseek.com"), &error).path(), QStringLiteral("/models"));
        QVERIFY(AiSettings::modelsUrl(QStringLiteral("http://example.com"), &error).isEmpty());
        MockApi api;
        api.respond = [](QTcpSocket *socket) { MockApi::response(socket, QStringLiteral("{\"data\":[{\"id\":\"z-model\"},{\"id\":\"a-model\"},{\"id\":\"a-model\"},{\"id\":\"中文模型\"},{\"id\":\"\"}]}").toUtf8()); };
        AiModelClient client; QSignalSpy received(&client, &AiModelClient::received); QSignalSpy failed(&client, &AiModelClient::failed);
        QTest::qWait(20); QVERIFY(api.requests.isEmpty());
        QVERIFY(client.fetch(api.url(), QStringLiteral("private-test-key"), &error)); QTRY_COMPARE(received.size(), 1); QVERIFY(failed.isEmpty()); QVERIFY(!client.busy());
        QCOMPARE(received.first().first().toStringList(), (QStringList{QStringLiteral("a-model"), QStringLiteral("z-model"), QStringLiteral("中文模型")}));
        QCOMPARE(api.requests.size(), 1); QVERIFY(api.requests.first().startsWith("GET /v1/models HTTP/1.1")); QVERIFY(api.requests.first().contains("Authorization: Bearer private-test-key")); QVERIFY(api.bodies.first().isEmpty());
        QVERIFY(!client.fetch(api.url(), QStringLiteral("key\r\ninjection"), &error)); QCOMPARE(api.requests.size(), 1);
    }
    void modelsFailures_data() {
        QTest::addColumn<int>("status"); QTest::addColumn<QByteArray>("body");
        QTest::newRow("auth") << 401 << QByteArray("secret-not-shown");
        QTest::newRow("unsupported") << 404 << QByteArray("not supported");
        QTest::newRow("redirect") << 302 << QByteArray();
        QTest::newRow("invalid-json") << 200 << QByteArray("<html>secret-not-shown</html>");
        QTest::newRow("empty") << 200 << QByteArray("{\"data\":[]}");
        QTest::newRow("wrong-schema") << 200 << QByteArray("{\"models\":[\"abc\"]}");
        QTest::newRow("provider-error") << 200 << QByteArray("{\"error\":\"secret-not-shown\",\"data\":[]}");
        QTest::newRow("oversize") << 200 << QByteArray(2 * 1024 * 1024 + 1, 'x');
        QJsonArray many; for (int index = 0; index < 4097; ++index) many.append(QJsonObject{{QStringLiteral("id"), QString::number(index)}});
        QTest::newRow("too-many") << 200 << QJsonDocument(QJsonObject{{QStringLiteral("data"), many}}).toJson();
    }
    void modelsFailures() {
        QFETCH(int, status); QFETCH(QByteArray, body); MockApi api;
        api.respond = [status, body](QTcpSocket *socket) { MockApi::response(socket, body, "application/json", status, "Location: http://127.0.0.1:1/steal\r\n"); };
        AiModelClient client; QSignalSpy received(&client, &AiModelClient::received); QSignalSpy failed(&client, &AiModelClient::failed); QString error;
        QVERIFY(client.fetch(api.url(), {}, &error)); QTRY_COMPARE(failed.size(), 1); QVERIFY(received.isEmpty()); QVERIFY(!client.busy()); QVERIFY(!failed.first().first().toString().contains(QStringLiteral("secret-not-shown"))); QCOMPARE(api.requests.size(), 1);
    }
    void modelsCancelAndRestart() {
        MockApi api; AiModelClient client; QSignalSpy received(&client, &AiModelClient::received); QSignalSpy failed(&client, &AiModelClient::failed); QString error;
        QVERIFY(client.fetch(api.url(), {}, &error)); QTRY_COMPARE(api.requests.size(), 1); QVERIFY(!client.fetch(api.url(), {}, &error)); client.cancel(); QVERIFY(!client.busy());
        api.respond = [](QTcpSocket *socket) { MockApi::response(socket, "{\"data\":[{\"id\":\"new-model\"}]}"); };
        QVERIFY(client.fetch(api.url(), {}, &error)); QTRY_COMPARE(received.size(), 1); QVERIFY(failed.isEmpty()); QCOMPARE(received.first().first().toStringList(), QStringList{QStringLiteral("new-model")});
    }
    void modelSettingsDialogFetchAndManualFallback() {
        MockApi api; QTemporaryDir directory; AiSettings saved; saved.endpoint = api.url(); saved.model = QStringLiteral("manual-model"); QString error; QVERIFY(saved.save(directory.path(), &error));
        AiExtension extension(directory.path()); bool verified = false; const QString output = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        api.respond = [](QTcpSocket *socket) { MockApi::response(socket, "{\"data\":[{\"id\":\"first-model\"},{\"id\":\"second-model\"}]}"); };
        QTimer::singleShot(0, &extension, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            QTimer::singleShot(8000, dialog, &QDialog::reject);
            auto *combo = dialog->findChild<QComboBox *>(QStringLiteral("aiModel")); auto *button = dialog->findChild<QPushButton *>(QStringLiteral("aiFetchModels")); auto *key = dialog->findChild<QLineEdit *>(QStringLiteral("aiKey"));
            if (!combo || !button || !key) { dialog->reject(); return; }
            QVERIFY(api.requests.isEmpty()); QCOMPARE(combo->currentText(), QStringLiteral("manual-model")); button->click();
            QTRY_COMPARE(combo->count(), 2); QCOMPARE(combo->currentText(), QStringLiteral("manual-model")); combo->setCurrentIndex(1); QCOMPARE(combo->currentText(), QStringLiteral("second-model"));
            if (!output.isEmpty()) QVERIFY(dialog->grab().save(QDir(output).filePath(QStringLiteral("native-ai-model-settings.png"))));
            combo->setEditText(QStringLiteral("custom-model")); key->setText(QStringLiteral("changed-key")); QCOMPARE(combo->currentText(), QStringLiteral("custom-model")); QCOMPARE(combo->count(), 0);
            button->click(); key->setText(QStringLiteral("new-key")); QTest::qWait(40); QCOMPARE(combo->count(), 0);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click(); verified = true;
        });
        extension.configure(nullptr); QVERIFY(verified); QCOMPARE(AiSettings::load(directory.path()).model, QStringLiteral("custom-model"));
    }
    void appearanceMigrationAndPalette_data() {
        QTest::addColumn<QString>("theme"); QTest::addColumn<QString>("mode");
        for (const QString &theme : themeIds()) for (const QString &mode : {QStringLiteral("light"), QStringLiteral("dark")}) QTest::newRow(qPrintable(theme + u'-' + mode)) << theme << mode;
    }
    void appearanceMigrationAndPalette() {
        QFETCH(QString, theme); QFETCH(QString, mode);
        QJsonObject legacy{{QStringLiteral("theme"), mode}}; normalizeAppearance(legacy); QCOMPARE(legacy.value(QStringLiteral("theme")).toString(), QStringLiteral("twinfile")); QCOMPARE(legacy.value(QStringLiteral("color_mode")).toString(), mode);
        QJsonObject state{{QStringLiteral("theme"), theme}, {QStringLiteral("color_mode"), mode}}; normalizeAppearance(state); QCOMPARE(state.value(QStringLiteral("theme")).toString(), theme);
        const auto palette = appearancePalette(theme, mode == QStringLiteral("dark"), {});
        QVERIFY(qAbs(palette.color(QPalette::Base).lightness() - palette.color(QPalette::Text).lightness()) > 145);
        QVERIFY(qAbs(palette.color(QPalette::Base).lightness() - palette.color(QPalette::PlaceholderText).lightness()) > 85);
        QCOMPARE(palette.color(QPalette::ToolTipText), palette.color(QPalette::Text)); QVERIFY(palette.color(QPalette::Disabled, QPalette::Text) != palette.color(QPalette::Text));
    }
    void appearanceRendersAllModesAndRestores() {
        const QString output = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT"); QTemporaryDir workspace;
        auto initial = readSettings(settingsPath()); initial.insert(QStringLiteral("extension_visible"), true); QVERIFY(writeSettings(settingsPath(), initial));
        QVERIFY(QDir().mkpath(workspace.filePath(QStringLiteral("项目文件"))));
        QFile fixture(workspace.filePath(QStringLiteral("README.md"))); QVERIFY(fixture.open(QIODevice::WriteOnly)); fixture.write("TwinFile workspace"); fixture.close();
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.setReducedMotion(true); window.show();
        for (Pane *pane : {window.leftPane(), window.rightPane()}) pane->navigate({Source::Local, workspace.path(), {}, {}});
        QTRY_COMPARE(window.leftPane()->fileModel()->rowCount(), 2); QTRY_COMPARE(window.rightPane()->fileModel()->rowCount(), 2);
        for (const QString &theme : themeIds()) for (const QString &mode : {QStringLiteral("light"), QStringLiteral("dark")}) {
            window.setTheme(theme); window.setColorMode(mode); QTest::qWait(30);
            QCOMPARE(qApp->property("macosStyle").toBool(), theme == QStringLiteral("macos"));
            QVERIFY(window.findChild<QWidget *>(QStringLiteral("workspaceHeader"))->isVisible());
            QCOMPARE(window.leftPane()->view()->sizeHintForRow(0), theme == QStringLiteral("miuix") ? 60 : 54);
            QCOMPARE(window.leftPane()->view()->palette().color(QPalette::Text), qApp->palette().color(QPalette::Text));
            if (!output.isEmpty()) { QVERIFY(QDir().mkpath(output)); QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("native-appearance-%1-%2.png").arg(theme, mode)))); }
        }
        bool checked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            QTimer::singleShot(8000, dialog, &QDialog::reject);
            auto *themes = dialog->findChild<QComboBox *>(QStringLiteral("appearanceTheme"));
            QVERIFY(themes); QCOMPARE(themes->count(), themeIds().size()); QVERIFY(themes->findData(QStringLiteral("dark")) < 0); QVERIFY(themes->findData(QStringLiteral("light")) < 0);
            QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("appearanceMode_light"))->isDefault());
            if (!output.isEmpty()) QVERIFY(dialog->grab().save(QDir(output).filePath(QStringLiteral("native-appearance-settings-macos.png"))));
            themes->setCurrentIndex(themes->findData(QStringLiteral("cream"))); dialog->findChild<QPushButton *>(QStringLiteral("appearanceMode_dark"))->click();
            QCOMPARE(qApp->palette().color(QPalette::Base), appearancePalette(QStringLiteral("cream"), true, {}).color(QPalette::Base));
            if (!output.isEmpty()) QVERIFY(dialog->grab().save(QDir(output).filePath(QStringLiteral("native-appearance-settings.png"))));
            checked = true; dialog->reject();
        });
        window.showAppearanceSettings(); QVERIFY(checked); window.close();
        const auto stored = readSettings(settingsPath()); QCOMPARE(stored.value(QStringLiteral("theme")).toString(), QStringLiteral("cream")); QCOMPARE(stored.value(QStringLiteral("color_mode")).toString(), QStringLiteral("dark"));
        Window restored; QCOMPARE(qApp->palette().color(QPalette::Base), appearancePalette(QStringLiteral("cream"), true, {}).color(QPalette::Base)); restored.close();
    }
    void mtOriginalLayoutAndActions_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("light") << QStringLiteral("light");
        QTest::newRow("dark") << QStringLiteral("dark");
    }
    void miuixLayoutNavigationAndMotion_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("light") << QStringLiteral("light");
        QTest::newRow("dark") << QStringLiteral("dark");
    }
    void miuixLayoutNavigationAndMotion() {
        QFETCH(QString, mode);
        QTemporaryDir root; QVERIFY(root.isValid());
        const QString source = root.filePath(QStringLiteral("项目资料")), destination = root.filePath(QStringLiteral("整理归档"));
        QVERIFY(QDir().mkpath(source + QStringLiteral("/图片素材"))); QVERIFY(QDir().mkpath(source + QStringLiteral("/版本备份")));
        QVERIFY(QDir().mkpath(destination + QStringLiteral("/已完成")));
        for (const QString &name : {QStringLiteral("项目说明.md"), QStringLiteral("工作计划.txt"), QStringLiteral("界面配置.json"), QStringLiteral("设计规范.pdf")}) {
            QFile file(QDir(source).filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("miuix workspace fixture");
        }
        auto state = readSettings(settingsPath()); state.insert(QStringLiteral("extension_visible"), false); state.insert(QStringLiteral("geometry"), QString());
        QVERIFY(writeSettings(settingsPath(), state));
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.setTheme(QStringLiteral("miuix")); window.setColorMode(mode);
        window.setReducedMotion(true); window.resize(1440, 900); window.show();
        window.leftPane()->navigate({Source::Local, source, {}, {}}); window.rightPane()->navigate({Source::Local, destination, {}, {}});
        QTRY_COMPARE(window.leftPane()->fileModel()->rowCount(), 6); QTRY_COMPARE(window.rightPane()->fileModel()->rowCount(), 1);
        window.setActive(window.leftPane());
        auto *selector = window.findChild<QWidget *>(QStringLiteral("miuixPaneSwitch"));
        auto *motion = selector->findChild<QVariantAnimation *>(QStringLiteral("miuixPaneAnimation"));
        auto *left = window.findChild<QToolButton *>(QStringLiteral("miuixLeftPane"));
        auto *right = window.findChild<QToolButton *>(QStringLiteral("miuixRightPane"));
        auto *toolbar = window.findChild<QWidget *>(QStringLiteral("workspaceToolbar"));
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("extensionDock"));
        QVERIFY(selector && motion && left && right && toolbar && dock); QVERIFY(selector->isVisible());
        QVERIFY(qApp->property("miuixStyle").toBool()); QVERIFY(!qApp->property("mtStyle").toBool());
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("workspaceHeading"))->isVisible());
        QVERIFY(toolbar->mapTo(&window, QPoint()).y() > window.leftPane()->mapTo(&window, window.leftPane()->rect().bottomLeft()).y());
        for (Pane *pane : {window.leftPane(), window.rightPane()}) {
            QVERIFY(pane->findChild<QWidget *>(QStringLiteral("miuixPaneHeading"))->isVisible());
            QVERIFY(!pane->findChild<QWidget *>(QStringLiteral("paneColumns"))->isVisible());
            QCOMPARE(pane->view()->sizeHintForRow(0), 60);
        }
        QCOMPARE(window.leftPane()->findChild<QLabel *>(QStringLiteral("miuixFolderDetails"))->text(), QStringLiteral("2 个文件夹 · 4 个文件"));
        window.leftPane()->view()->selectAll();
        QCOMPARE(window.leftPane()->findChild<QLabel *>(QStringLiteral("miuixFolderDetails"))->text(), QStringLiteral("已选择 6 项"));
        const QString evidence = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        auto screenshot = [&](const QString &scene) {
            if (!evidence.isEmpty()) QVERIFY(window.grab().save(QDir(evidence).filePath(QStringLiteral("native-miuix-%1-%2.png").arg(scene, mode))));
        };
        QTest::qWait(60); screenshot(QStringLiteral("desktop"));
        window.setReducedMotion(false);
        QTest::mouseClick(right, Qt::LeftButton); QCOMPARE(window.activePane(), window.rightPane()); QVERIFY(right->isChecked()); QVERIFY(!left->isChecked());
        QCOMPARE(window.findChild<QPushButton *>(QStringLiteral("action_copy"))->property("symbol").toString(), QStringLiteral("copy-back"));
        QCOMPARE(motion->state(), QAbstractAnimation::Running); motion->setCurrentTime(105);
        const qreal middle = selector->property("position").toReal(); QVERIFY(middle > 0 && middle < 1);
        screenshot(QStringLiteral("switch-moving"));
        QTest::mouseClick(left, Qt::LeftButton);
        QVERIFY(qAbs(selector->property("position").toReal() - middle) < 0.12);
        QTRY_COMPARE(motion->state(), QAbstractAnimation::Stopped); QCOMPARE(selector->property("position").toReal(), 0.0);
        QTest::mouseClick(right, Qt::LeftButton); QTest::qWait(25); window.setReducedMotion(true);
        QCOMPARE(motion->state(), QAbstractAnimation::Stopped); QCOMPARE(selector->property("position").toReal(), 1.0);
        QTest::mouseClick(left, Qt::LeftButton); QCOMPARE(selector->property("position").toReal(), 0.0);
        QCOMPARE(window.leftPane()->selected().size(), 6); QVERIFY(window.rightPane()->selected().isEmpty());
        QCOMPARE(QDir::fromNativeSeparators(window.findChild<QLabel *>(QStringLiteral("miuixLocation"))->toolTip()), source);
        auto *path = window.rightPane()->findChild<QLineEdit *>(QStringLiteral("panePath"));
        path->setText(source); QTest::keyClick(path, Qt::Key_Return);
        QTRY_COMPARE(window.rightPane()->location().path, source); QTRY_COMPARE(window.rightPane()->fileModel()->rowCount(), 6);
        auto *task = static_cast<TaskProgress *>(window.findChild<QWidget *>(QStringLiteral("taskProgress")));
        QVERIFY(task); task->begin(QStringLiteral("复制文件"), [] {}); task->updateProgress(512 * 1024, 2 * 1024 * 1024, QStringLiteral("项目资料"));
        dock->show(); window.resize(1100, 650); QTest::qWait(100); QCOMPARE(window.width(), 1100);
        for (Pane *pane : {window.leftPane(), window.rightPane()}) {
            QVERIFY(pane->width() >= 260); QVERIFY(!pane->view()->horizontalScrollBar()->isVisible());
            auto *address = pane->findChild<QLineEdit *>(QStringLiteral("panePath"));
            QVERIFY(pane->rect().contains(QRect(address->mapTo(pane, QPoint()), address->size())));
        }
        QVERIFY(toolbar->rect().contains(task->geometry()));
        auto *header = window.findChild<QWidget *>(QStringLiteral("workspaceHeader"));
        QVERIFY(header->rect().contains(QRect(selector->mapTo(header, QPoint()), selector->size())));
        screenshot(QStringLiteral("compact-assistant-progress"));
        task->finish(QStringLiteral("完成"), true);
        bool settingsChecked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            const auto close = qScopeGuard([dialog] { dialog->reject(); });
            auto *themes = dialog->findChild<QComboBox *>(QStringLiteral("appearanceTheme")); QVERIFY(themes);
            QCOMPARE(themes->currentData().toString(), QStringLiteral("miuix"));
            if (!evidence.isEmpty()) QVERIFY(dialog->grab().save(QDir(evidence).filePath(QStringLiteral("native-miuix-settings-%1.png").arg(mode))));
            settingsChecked = true;
        });
        window.showAppearanceSettings(); QVERIFY(settingsChecked);
        for (const QString &theme : {QStringLiteral("mt"), QStringLiteral("twinfile"), QStringLiteral("macos")}) {
            window.setTheme(theme); QTest::qWait(20);
            QVERIFY(!selector->isVisible()); QVERIFY(!qApp->property("miuixStyle").toBool());
            QCOMPARE(window.leftPane()->view()->sizeHintForRow(0), 54); QCOMPARE(window.leftPane()->selected().size(), 6);
            QVERIFY(toolbar->mapTo(&window, QPoint()).y() < window.leftPane()->mapTo(&window, QPoint()).y());
            window.setTheme(QStringLiteral("miuix")); QTest::qWait(20); QVERIFY(selector->isVisible());
            QCOMPARE(window.leftPane()->location().path, source); QCOMPARE(window.leftPane()->selected().size(), 6);
        }
        window.close(); QCOMPARE(readSettings(settingsPath()).value(QStringLiteral("theme")).toString(), QStringLiteral("miuix"));
        Window restored; QCOMPARE(qApp->property("miuixStyle").toBool(), true); QCOMPARE(restored.leftPane()->location().path, source); restored.close();
    }
    void paneAddressNavigation() {
        QTemporaryDir root; QVERIFY(root.isValid());
        const QString folder = root.filePath(QStringLiteral("nested"));
        QVERIFY(QDir().mkpath(folder));
        const QString file = root.filePath(QStringLiteral("notes.txt")), archive = root.filePath(QStringLiteral("fixture.zip"));
        { QFile output(file); QVERIFY(output.open(QIODevice::WriteOnly)); output.write("address fixture"); }
        QVERIFY(createArchive({folder, file}, archive).ok);
        Pane pane;
        QVERIFY(pane.navigatePath(root.path())); QCOMPARE(pane.location().source, Source::Local);
        QVERIFY(pane.navigatePath(QStringLiteral("nested"))); QCOMPARE(pane.location().path, folder);
        QVERIFY(pane.navigatePath(QStringLiteral(".."))); QCOMPARE(pane.location().path, root.path());
        const Location before = pane.location();
        QVERIFY(!pane.navigatePath(QStringLiteral("missing"))); QVERIFY(pane.location() == before);
        QVERIFY(!pane.navigatePath(file)); QVERIFY(pane.location() == before);
        QVERIFY(pane.navigatePath(archive)); QCOMPARE(pane.location().source, Source::Archive);
        QVERIFY(pane.navigatePath(archive + QStringLiteral("!/nested"))); QCOMPARE(pane.location().inner, QStringLiteral("nested"));
        QVERIFY(!pane.navigatePath(archive + QStringLiteral("!/../outside"))); QCOMPARE(pane.location().inner, QStringLiteral("nested"));
        QVERIFY(pane.navigatePath(QStringLiteral("此电脑"))); QCOMPARE(pane.location().source, Source::Drive);
        NavigationPhoneApi phone;
        pane.navigate({Source::Phone, QStringLiteral("address-phone"), QStringLiteral("/sdcard"), QStringLiteral("Phone fixture")});
        QVERIFY(pane.navigatePath(QStringLiteral("address-phone:/sdcard/Download"))); QCOMPARE(pane.location().inner, QStringLiteral("/sdcard/Download"));
        QVERIFY(pane.navigatePath(QStringLiteral("/sdcard/../storage"))); QCOMPARE(pane.location().inner, QStringLiteral("/storage"));
        QVERIFY(!pane.navigatePath(QStringLiteral("address-phone:relative"))); QCOMPARE(pane.location().inner, QStringLiteral("/storage"));
        QVERIFY(pane.navigatePath(root.path())); QCOMPARE(pane.location().source, Source::Local);
        QTRY_COMPARE(phone.listings, 3); QTest::qWait(20);
    }
    void mtOriginalLayoutAndActions() {
        QFETCH(QString, mode); QTemporaryDir root;
        const QString left = root.filePath(QStringLiteral("Download")), right = root.filePath(QStringLiteral("appshare"));
        QVERIFY(QDir().mkpath(left + QStringLiteral("/Android"))); QVERIFY(QDir().mkpath(right + QStringLiteral("/QQ")));
        { QFile file(left + QStringLiteral("/notes.txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("MT fixture"); }
        auto state = readSettings(settingsPath()); state.insert(QStringLiteral("extension_visible"), false); state.insert(QStringLiteral("geometry"), QString()); state.insert(QStringLiteral("favorites"), QJsonArray{right});
        QVERIFY(writeSettings(settingsPath(), state));
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.setReducedMotion(true); window.setTheme(QStringLiteral("mt")); window.setColorMode(mode); window.resize(1280, 820); window.show();
        auto *first = window.leftPane(), *second = window.rightPane();
        first->navigate({Source::Local, left, {}, {}}); second->navigate({Source::Local, right, {}, {}});
        QTRY_COMPARE(first->fileModel()->rowCount(), 2); QTRY_COMPARE(second->fileModel()->rowCount(), 1); window.setActive(first);
        QCOMPARE(themeName(QStringLiteral("mt")), QStringLiteral("MT 原版")); QVERIFY(qApp->property("mtStyle").toBool());
        auto *headerPath = window.findChild<QLineEdit *>(QStringLiteral("mtPath")); auto *statistics = window.findChild<QLabel *>(QStringLiteral("mtStatistics"));
        QVERIFY(headerPath);
        QCOMPARE(headerPath->text(), first->location().display()); QVERIFY(statistics->text().contains(QStringLiteral("文件夹: 1   文件: 1")));
        auto *bottom = window.findChild<QWidget *>(QStringLiteral("mtBottomBar")); QVERIFY(bottom->isVisible());
        auto *sidebar = window.findChild<QWidget *>(QStringLiteral("navigationSidebar")); QVERIFY(!sidebar->isVisible());
        QVERIFY(!first->findChild<QWidget *>(QStringLiteral("paneColumns"))->isVisible()); QCOMPARE(window.findChild<QSplitter *>(QStringLiteral("fileSplitter"))->handleWidth(), 3);
        for (Pane *pane : {first, second}) {
            QVERIFY(!pane->findChild<QWidget *>(QStringLiteral("paneChrome"))->isVisible());
            QVERIFY(!pane->findChild<QLineEdit *>(QStringLiteral("panePath"))->isVisible());
            QVERIFY(!pane->findChild<QTabBar *>(QStringLiteral("paneTabs"))->isVisible());
        }
        const QString output = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        auto capture = [&](const QString &name, QWidget *widget) {
            if (!output.isEmpty()) QVERIFY(widget->grab().save(QDir(output).filePath(QStringLiteral("native-mt-original-") + name + QChar('-') + mode + QStringLiteral(".png"))));
        };
        QTest::qWait(50); capture(QStringLiteral("workspace"), &window);
        QApplication::setActiveWindow(&window); headerPath->setFocus(); QTRY_VERIFY(headerPath->hasFocus());
        QTest::mouseClick(headerPath, Qt::LeftButton); QTest::qWait(10);
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("mtPlaces")));
        headerPath->setText(QStringLiteral("Android"));
        capture(QStringLiteral("path-edit"), &window);
        QTest::keyClick(headerPath, Qt::Key_Return); QCOMPARE(first->location().path, left + QStringLiteral("/Android")); QCOMPARE(second->location().path, right);
        window.findChild<QToolButton *>(QStringLiteral("mt_back"))->click(); QCOMPARE(first->location().path, left);
        headerPath->setFocus(); headerPath->setText(root.filePath(QStringLiteral("does-not-exist")));
        QTest::keyClick(headerPath, Qt::Key_Return); QCOMPARE(first->location().path, left); QVERIFY2(headerPath->toolTip().contains(QStringLiteral("路径无效")), qPrintable(headerPath->toolTip()));
        QTest::keyClick(headerPath, Qt::Key_Escape); QCOMPARE(headerPath->text(), first->location().display());
        QTest::keyClick(first->view(), Qt::Key_L, Qt::ControlModifier); QTRY_VERIFY(headerPath->hasFocus()); QCOMPARE(headerPath->selectedText(), headerPath->text());
        QTest::keyClick(headerPath, Qt::Key_Escape);
        headerPath->setFocus(); headerPath->setText(QStringLiteral("keep-editing"));
        first->refresh(); QTRY_COMPARE(first->fileModel()->rowCount(), 2); QCOMPARE(headerPath->text(), QStringLiteral("keep-editing"));
        QTest::keyClick(headerPath, Qt::Key_Escape);
        headerPath->setFocus(); headerPath->setText(QStringLiteral("unfinished"));
        window.setActive(second); QCOMPARE(headerPath->text(), second->location().display()); window.setActive(first);
        headerPath->setText(QDir::toNativeSeparators(right)); QTest::keyClick(headerPath, Qt::Key_Return); QCOMPARE(first->location().path, right);
        window.findChild<QToolButton *>(QStringLiteral("mt_back"))->click(); QTRY_COMPARE(first->fileModel()->rowCount(), 2);
        const QImage folderIcon = entryIcon(first->fileModel()->entry(0)).pixmap(96, 96).toImage();
        QCOMPARE(folderIcon.pixelColor(48, 48), QColor(Qt::white)); QCOMPARE(folderIcon.pixelColor(12, 48), QColor(QStringLiteral("#303030")));
        window.findChild<QPushButton *>(QStringLiteral("mtDrawer"))->click(); QTest::qWait(20); QVERIFY(sidebar->isVisible()); capture(QStringLiteral("drawer"), &window);
        auto *drawerShade = window.findChild<QPushButton *>(QStringLiteral("mtDrawerShade")); QVERIFY(drawerShade->isVisible());
        QTest::mouseClick(drawerShade, Qt::LeftButton, Qt::NoModifier, QPoint(drawerShade->width() - 10, 10)); QVERIFY(!sidebar->isVisible()); QVERIFY(!drawerShade->isVisible());
        first->view()->selectionModel()->select(first->fileModel()->index(1), QItemSelectionModel::ClearAndSelect);
        const QJsonObject firstTabs = first->workspace(); const bool historyBeforeSwap = first->canGoBack();
        window.setActive(second); QCOMPARE(headerPath->text(), second->location().display());
        window.findChild<QToolButton *>(QStringLiteral("mt_swap"))->click();
        QCOMPARE(window.leftPane(), second); QCOMPARE(window.rightPane(), first); QCOMPARE(window.activePane(), second);
        QCOMPARE(first->selected().size(), 1); QCOMPARE(first->selected().first().name, QStringLiteral("notes.txt")); QCOMPARE(first->workspace(), firstTabs); QCOMPARE(first->canGoBack(), historyBeforeSwap);
        QCOMPARE(window.findChild<QPushButton *>(QStringLiteral("action_copy"))->property("symbol").toString(), QStringLiteral("copy-forward"));
        window.findChild<QToolButton *>(QStringLiteral("mt_swap"))->click(); window.setActive(first);
        window.findChild<QToolButton *>(QStringLiteral("mt_up"))->click(); QCOMPARE(first->location().path, root.path());
        window.findChild<QToolButton *>(QStringLiteral("mt_back"))->click(); QCOMPARE(first->location().path, left);
        QVERIFY(window.findChild<QToolButton *>(QStringLiteral("mt_forward"))->isEnabled());
        first->findChild<QToolButton *>(QStringLiteral("mtParent"))->click(); QCOMPARE(first->location().path, root.path());
        window.findChild<QToolButton *>(QStringLiteral("mt_back"))->click(); QTRY_COMPARE(first->fileModel()->rowCount(), 2);
        for (bool folder : {false, true}) {
            const QString name = folder ? QStringLiteral("created-folder") : QStringLiteral("created.txt"); bool checked = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
                const auto closeOnFailure = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
                QCOMPARE(dialog->objectName(), QStringLiteral("mtCreate"));
                auto *input = dialog->findChild<QLineEdit *>(QStringLiteral("mtCreateName")); QVERIFY(input);
                auto *fileButton = dialog->findChild<QPushButton *>(QStringLiteral("mtCreateFile"));
                auto *folderButton = dialog->findChild<QPushButton *>(QStringLiteral("mtCreateFolder"));
                QVERIFY(!fileButton->isEnabled()); QVERIFY(!folderButton->isEnabled());
                input->setText(QStringLiteral("../bad")); QVERIFY(!fileButton->isEnabled()); QVERIFY(!folderButton->isEnabled());
                input->setText(name); QVERIFY(fileButton->isEnabled()); QVERIFY(folderButton->isEnabled());
                capture(QStringLiteral("create"), dialog); checked = true; (folder ? folderButton : fileButton)->click();
            });
            window.findChild<QToolButton *>(QStringLiteral("mt_add"))->click(); QVERIFY(checked);
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(QDir(left).filePath(name)), 5000);
            auto *task = static_cast<TaskProgress *>(window.findChild<QWidget *>(QStringLiteral("taskProgress"))); QTRY_VERIFY(!task->active());
            QCOMPARE(QFileInfo(QDir(left).filePath(name)).isDir(), folder);
        }
        bool menuChecked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); if (!menu) return;
            const auto close = qScopeGuard([menu] { menu->close(); }); QCOMPARE(menu->objectName(), QStringLiteral("mtMenu"));
            QStringList labels; for (auto *action : menu->actions()) { labels.append(action->text()); if (!action->isSeparator()) QVERIFY(!action->icon().isNull()); }
            QVERIFY(labels.contains(QStringLiteral("刷新"))); QVERIFY(labels.contains(QStringLiteral("交换窗口"))); QVERIFY(labels.contains(QStringLiteral("更多工具")));
            capture(QStringLiteral("menu"), menu); menuChecked = true;
        });
        window.findChild<QPushButton *>(QStringLiteral("mtOverflow"))->click(); QVERIFY(menuChecked);
        auto swipe = [](QWidget *widget, int dy, int dx = 0) {
            const QPointF from(widget->rect().center()), to = from + QPointF(dx, dy);
            QMouseEvent press(QEvent::MouseButtonPress, from, widget->mapToGlobal(from.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(widget, &press);
            QMouseEvent move(QEvent::MouseMove, to, widget->mapToGlobal(to.toPoint()), Qt::NoButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(widget, &move);
            QMouseEvent release(QEvent::MouseButtonRelease, to, widget->mapToGlobal(to.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier); QApplication::sendEvent(widget, &release);
        };
        swipe(bottom, -8); QVERIFY(!window.findChild<QWidget *>(QStringLiteral("mtPlaces")));
        swipe(bottom, -35, 100); QVERIFY(!window.findChild<QWidget *>(QStringLiteral("mtPlaces")));
        swipe(window.findChild<QToolButton *>(QStringLiteral("mt_add")), -90); QTest::qWait(20);
        QVERIFY(!QApplication::activeModalWidget());
        QPointer<QWidget> sheet = window.findChild<QWidget *>(QStringLiteral("mtPlaces")); QVERIFY(sheet); QVERIFY(sheet->isVisible());
        QVERIFY(window.centralWidget()->rect().contains(sheet->geometry())); auto *tabs = sheet->findChild<QTabWidget *>(QStringLiteral("mtPlacesTabs")); QCOMPARE(tabs->count(), 2);
        capture(QStringLiteral("recent"), &window);
        tabs->setCurrentIndex(1); auto *bookmarks = qobject_cast<QListWidget *>(tabs->currentWidget()); QVERIFY(bookmarks); QCOMPARE(bookmarks->count(), 1);
        auto *item = bookmarks->item(0); QTest::mouseClick(bookmarks->viewport(), Qt::LeftButton, Qt::NoModifier, bookmarks->visualItemRect(item).center());
        QTRY_VERIFY(sheet.isNull()); QCOMPARE(first->location().path, right); QCOMPARE(second->location().path, right);
        swipe(window.findChild<QToolButton *>(QStringLiteral("mt_swap")), -90); QCOMPARE(window.leftPane(), first); QCOMPARE(window.rightPane(), second);
        auto *placesShade = window.findChild<QPushButton *>(QStringLiteral("mtPlacesShade")); QVERIFY(placesShade);
        QPointer<QWidget> dismissSheet = window.findChild<QWidget *>(QStringLiteral("mtPlaces"));
        QTest::mouseClick(placesShade, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10)); QTRY_VERIFY(dismissSheet.isNull());
        swipe(bottom, -90); QPointer<QWidget> downSheet = window.findChild<QWidget *>(QStringLiteral("mtPlaces")); QVERIFY(downSheet);
        swipe(downSheet->findChild<QToolButton *>(QStringLiteral("mtPlacesClose")), 90); QTRY_VERIFY(downSheet.isNull());
        QWheelEvent wheel(QPointF(bottom->rect().center()), bottom->mapToGlobal(bottom->rect().center()), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(bottom, &wheel); QPointer<QWidget> wheelSheet = window.findChild<QWidget *>(QStringLiteral("mtPlaces")); QVERIFY(wheelSheet);
        QApplication::setActiveWindow(&window);
        QTest::keyClick(wheelSheet->findChild<QTabWidget *>(QStringLiteral("mtPlacesTabs")), Qt::Key_Escape); QTRY_VERIFY(wheelSheet.isNull());
        auto *device = QTest::createTouchDevice();
        // Hidden test windows bypass native touch routing; deliver the widget events directly.
        auto touch = [device](QWidget *widget, QPoint from, QPoint to) {
            auto send = [&](QEvent::Type type, QEventPoint::State state, const QPoint &position) {
                QEventPoint point(0, state, position, widget->mapToGlobal(position)); QtGuiTest::setEventPointPosition(point, position);
                QTouchEvent event(type, device, Qt::NoModifier, {point}); QApplication::sendEvent(widget, &event);
            };
            send(QEvent::TouchBegin, QEventPoint::Pressed, from);
            send(QEvent::TouchUpdate, QEventPoint::Updated, to);
            send(QEvent::TouchEnd, QEventPoint::Released, to);
        };
        touch(bottom, bottom->rect().center(), bottom->rect().center() - QPoint(0, 90));
        QPointer<QWidget> touchSheet = window.findChild<QWidget *>(QStringLiteral("mtPlaces")); QVERIFY(touchSheet);
        auto *touchTabs = touchSheet->findChild<QTabWidget *>(QStringLiteral("mtPlacesTabs"))->tabBar();
        const QPoint bookmarkTab = touchTabs->tabRect(1).center();
        touch(touchTabs, bookmarkTab, bookmarkTab); QCOMPARE(touchTabs->currentIndex(), 1);
        swipe(touchTabs, 90); QTRY_VERIFY(touchSheet.isNull());
        touch(bottom, bottom->rect().center(), bottom->rect().center() - QPoint(0, 90));
        touchSheet = window.findChild<QWidget *>(QStringLiteral("mtPlaces")); QVERIFY(touchSheet);
        window.resize(1200, 780); QTRY_VERIFY(touchSheet.isNull());
        const int tabsBefore = first->workspace().value(QStringLiteral("tabs")).toArray().size();
        first->newTab(); QCOMPARE(first->workspace().value(QStringLiteral("tabs")).toArray().size(), tabsBefore + 1); first->closeTab();
        window.findChild<QPushButton *>(QStringLiteral("assistantToggle"))->click(); window.findChild<QPushButton *>(QStringLiteral("mtDrawer"))->click();
        window.resize(1100, 650); QTest::qWait(50); QCOMPARE(window.size(), QSize(1100, 650)); QVERIFY(sidebar->isVisible());
        QVERIFY(window.centralWidget()->rect().contains(sidebar->geometry()));
        for (Pane *pane : {first, second}) { QVERIFY(pane->width() >= 260); QVERIFY(pane->rect().contains(QRect(pane->view()->mapTo(pane, QPoint()), pane->view()->size()))); }
        capture(QStringLiteral("compact-drawer-assistant"), &window);
        window.setReducedMotion(false); swipe(bottom, -90);
        QPointer<QWidget> compactSheet = window.findChild<QWidget *>(QStringLiteral("mtPlaces")); QVERIFY(compactSheet);
        QTest::qWait(280); QCOMPARE(compactSheet->width(), window.centralWidget()->width());
        QVERIFY(window.centralWidget()->rect().contains(compactSheet->geometry())); capture(QStringLiteral("compact-recent-assistant"), &window);
        compactSheet->findChild<QToolButton *>(QStringLiteral("mtPlacesClose"))->click(); QTRY_VERIFY(compactSheet.isNull()); window.setReducedMotion(true);
        bool appearanceChecked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            const auto close = qScopeGuard([dialog] { dialog->reject(); });
            auto *themes = dialog->findChild<QComboBox *>(QStringLiteral("appearanceTheme")); QVERIFY(themes);
            QCOMPARE(themes->currentText(), QStringLiteral("MT 原版")); capture(QStringLiteral("settings"), dialog); appearanceChecked = true;
        });
        window.showAppearanceSettings(); QVERIFY(appearanceChecked);
        window.setTheme(QStringLiteral("twinfile")); QVERIFY(!qApp->property("mtStyle").toBool()); QVERIFY(!bottom->isVisible()); QVERIFY(sidebar->isVisible());
        QVERIFY(first->findChild<QWidget *>(QStringLiteral("paneColumns"))->isVisible()); QVERIFY(!first->findChild<QToolButton *>(QStringLiteral("mtParent"))->isVisible());
        QVERIFY(first->findChild<QWidget *>(QStringLiteral("paneChrome"))->isVisible());
        QVERIFY(first->findChild<QLineEdit *>(QStringLiteral("panePath"))->isVisible());
        const QImage normalIcon = entryIcon(first->fileModel()->entry(0)).pixmap(96, 96).toImage(); QVERIFY(normalIcon != folderIcon);
        window.setTheme(QStringLiteral("mt")); window.close();
        Window restored; restored.setAttribute(Qt::WA_DontShowOnScreen); restored.show(); QVERIFY(qApp->property("mtStyle").toBool()); QVERIFY(restored.findChild<QWidget *>(QStringLiteral("mtBottomBar"))->isVisible());
        restored.setTheme(QStringLiteral("twinfile")); restored.close();
    }
    void mtMotionTransitions_data() { mtOriginalLayoutAndActions_data(); }
    void mtMotionTransitions() {
        QFETCH(QString, mode);
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.setTheme(QStringLiteral("mt")); window.setColorMode(mode);
        window.setReducedMotion(false); window.resize(1280, 820); window.show(); QTest::qWait(30);
        auto capture = [&](const QString &name) {
            const QString output = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
            if (!output.isEmpty()) QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("native-mt-motion-") + name + QChar('-') + mode + QStringLiteral(".png"))));
        };
        auto *drawerButton = window.findChild<QPushButton *>(QStringLiteral("mtDrawer"));
        auto *sidebar = window.findChild<QWidget *>(QStringLiteral("navigationSidebar"));
        auto *drawerShade = window.findChild<QPushButton *>(QStringLiteral("mtDrawerShade"));
        auto *drawerAnimation = drawerShade->findChild<QVariantAnimation *>(QStringLiteral("mtOverlayAnimation"));
        QVERIFY(drawerAnimation); drawerButton->click(); QCOMPARE(drawerAnimation->state(), QAbstractAnimation::Running);
        drawerAnimation->pause(); drawerAnimation->setCurrentTime(80);
        QVERIFY(sidebar->x() > -sidebar->width()); QVERIFY(sidebar->x() < 0);
        QVERIFY(drawerShade->property("mtMotionProgress").toReal() > 0); QVERIFY(drawerShade->property("mtMotionProgress").toReal() < 1);
        capture(QStringLiteral("drawer-opening")); drawerAnimation->resume(); QTRY_COMPARE(sidebar->x(), 0);
        drawerShade->click(); QVERIFY(sidebar->isVisible()); QVERIFY(drawerShade->property("mtClosing").toBool());
        drawerAnimation->pause(); drawerAnimation->setCurrentTime(100); QVERIFY(sidebar->x() < 0); capture(QStringLiteral("drawer-closing"));
        const int partial = sidebar->x(); drawerButton->click(); QVERIFY(!drawerShade->property("mtClosing").toBool());
        QCOMPARE(sidebar->x(), partial); QTRY_COMPARE(sidebar->x(), 0);
        QApplication::setActiveWindow(&window); QTest::keyClick(sidebar, Qt::Key_Escape);
        QTRY_VERIFY(!sidebar->isVisible()); QVERIFY(!drawerShade->isVisible());
        auto *bottom = window.findChild<QWidget *>(QStringLiteral("mtBottomBar"));
        auto openPlaces = [&] {
            QWheelEvent wheel(QPointF(bottom->rect().center()), bottom->mapToGlobal(bottom->rect().center()), QPoint(), QPoint(0, 120),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(bottom, &wheel);
        };
        openPlaces(); QPointer<QWidget> sheet = window.findChild<QWidget *>(QStringLiteral("mtPlaces")); QVERIFY(sheet);
        auto *shade = sheet->parentWidget(); auto *animation = shade->findChild<QVariantAnimation *>(QStringLiteral("mtOverlayAnimation"));
        QVERIFY(animation); QCOMPARE(animation->state(), QAbstractAnimation::Running);
        animation->pause(); animation->setCurrentTime(80); const int intermediate = sheet->y();
        QVERIFY(intermediate < window.centralWidget()->height()); QVERIFY(intermediate > window.centralWidget()->height() - sheet->height());
        capture(QStringLiteral("sheet-opening")); animation->resume(); QTRY_COMPARE(animation->state(), QAbstractAnimation::Stopped);
        const int expanded = sheet->y(); sheet->findChild<QToolButton *>(QStringLiteral("mtPlacesClose"))->click();
        QVERIFY(sheet); QVERIFY(shade->property("mtClosing").toBool()); QVERIFY(!sheet->isEnabled());
        animation->pause(); animation->setCurrentTime(100); QVERIFY(sheet->y() > expanded); QVERIFY(sheet->y() < window.centralWidget()->height());
        capture(QStringLiteral("sheet-closing")); shade->close(); QCOMPARE(animation->state(), QAbstractAnimation::Paused);
        animation->resume(); QTRY_VERIFY(sheet.isNull());
        openPlaces(); sheet = window.findChild<QWidget *>(QStringLiteral("mtPlaces")); QVERIFY(sheet);
        window.setReducedMotion(true); QCOMPARE(sheet->y(), window.centralWidget()->height() - sheet->height());
        sheet->findChild<QToolButton *>(QStringLiteral("mtPlacesClose"))->click(); QVERIFY(!sheet->isVisible()); QTRY_VERIFY(sheet.isNull());
        window.setReducedMotion(false); openPlaces(); sheet = window.findChild<QWidget *>(QStringLiteral("mtPlaces")); QVERIFY(sheet);
        sheet->parentWidget()->close(); window.setReducedMotion(true); QTRY_VERIFY(sheet.isNull());
        drawerButton->click(); QCOMPARE(sidebar->x(), 0); drawerShade->click(); QVERIFY(!sidebar->isVisible());
        window.setReducedMotion(false); drawerButton->click(); window.resize(1100, 650); QTest::qWait(280);
        QVERIFY(window.centralWidget()->rect().contains(sidebar->geometry()));
        window.setTheme(QStringLiteral("twinfile")); QVERIFY(sidebar->isVisible()); QVERIFY(!drawerShade->isVisible());
        window.setTheme(QStringLiteral("mt")); window.setReducedMotion(false); openPlaces();
        sheet = window.findChild<QWidget *>(QStringLiteral("mtPlaces")); QVERIFY(sheet);
        window.resize(1200, 700); QTRY_VERIFY(sheet.isNull());
        bool menuChecked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget()); if (!popup) return;
            const auto cleanup = qScopeGuard([popup] { popup->close(); });
            auto *enter = popup->findChild<QVariantAnimation *>(QStringLiteral("mtMenuAnimation")); QVERIFY(enter);
            QCOMPARE(enter->state(), QAbstractAnimation::Running); enter->pause(); enter->setCurrentTime(60);
            QVERIFY(popup->windowOpacity() > 0); QVERIFY(popup->windowOpacity() < 1); enter->resume();
            QTest::qWait(200); QCOMPARE(popup->windowOpacity(), 1.0);
            QMenu *tools = nullptr; for (auto *action : popup->actions()) if (action->text() == QStringLiteral("更多工具")) tools = action->menu();
            QVERIFY(tools); tools->popup(popup->mapToGlobal(QPoint(popup->width(), 0)));
            auto *submenuMotion = tools->findChild<QVariantAnimation *>(QStringLiteral("mtMenuAnimation")); QVERIFY(submenuMotion);
            QCOMPARE(submenuMotion->state(), QAbstractAnimation::Running); submenuMotion->setCurrentTime(submenuMotion->duration());
            QCOMPARE(tools->windowOpacity(), 1.0); tools->close(); menuChecked = true;
        });
        window.findChild<QPushButton *>(QStringLiteral("mtOverflow"))->click(); QVERIFY(menuChecked);
        QPointer<QWidget> fade = window.findChild<QWidget *>(QStringLiteral("mtMenuFade")); QVERIFY(fade);
        QVERIFY(fade->testAttribute(Qt::WA_TransparentForMouseEvents)); QVERIFY(!QApplication::activePopupWidget()); QTRY_VERIFY(fade.isNull());
        bool reducedMenuChecked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget()); if (!popup) return;
            const auto cleanup = qScopeGuard([popup] { popup->close(); });
            window.setReducedMotion(true);
            auto *enter = popup->findChild<QVariantAnimation *>(QStringLiteral("mtMenuAnimation")); QVERIFY(enter);
            QCOMPARE(enter->state(), QAbstractAnimation::Stopped); QCOMPARE(popup->windowOpacity(), 1.0); reducedMenuChecked = true;
        });
        window.findChild<QPushButton *>(QStringLiteral("mtOverflow"))->click(); QVERIFY(reducedMenuChecked);
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("mtMenuFade")));
        for (bool reduced : {false, true}) {
            window.setReducedMotion(reduced); bool checked = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
                const auto cleanup = qScopeGuard([dialog] { dialog->reject(); });
                auto *enter = dialog->findChild<QVariantAnimation *>(QStringLiteral("mtDialogAnimation")); QVERIFY(enter);
                QVERIFY(window.findChild<QWidget *>(QStringLiteral("mtDialogShade")));
                if (reduced) QCOMPARE(enter->state(), QAbstractAnimation::Stopped);
                else {
                    QCOMPARE(enter->state(), QAbstractAnimation::Running); enter->pause(); enter->setCurrentTime(60);
                    QVERIFY(dialog->windowOpacity() > 0); QVERIFY(dialog->windowOpacity() < 1);
                    enter->resume(); QTest::qWait(200); QVERIFY(dialog->windowOpacity() > 0.98);
                }
                dialog->reject();
                if (!reduced) {
                    QVERIFY(dialog->isVisible()); QVERIFY(dialog->property("mtClosing").toBool());
                    QCOMPARE(enter->state(), QAbstractAnimation::Running); window.setReducedMotion(true); QVERIFY(!dialog->isVisible());
                } else QVERIFY(!dialog->isVisible());
                checked = true;
            });
            window.findChild<QToolButton *>(QStringLiteral("mt_add"))->click(); QVERIFY(checked);
            QVERIFY(!window.findChild<QWidget *>(QStringLiteral("mtDialogShade"))); QVERIFY(!QApplication::activeModalWidget());
        }
        window.setTheme(QStringLiteral("twinfile")); window.close();
    }
    void keysUseDpapiAndCanBeForgotten() {
        QTemporaryDir root; AiSettings settings; settings.endpoint = QStringLiteral("https://example.com/v1"); settings.model = QStringLiteral("example"); settings.apiKey = QStringLiteral("private-key-中文-12345");
        QString error; QVERIFY2(settings.save(root.path(), &error), qPrintable(error));
        QVERIFY(AiSettings::load(root.path()).apiKey.isEmpty());
        settings.rememberKey = true; QVERIFY2(settings.save(root.path(), &error), qPrintable(error));
        QCOMPARE(AiSettings::load(root.path(), &error).apiKey, settings.apiKey); QVERIFY(error.isEmpty());
        QFile file(root.filePath(QStringLiteral("ai.json"))); QVERIFY(file.open(QIODevice::ReadOnly)); const QByteArray bytes = file.readAll(); file.close();
        QVERIFY(!bytes.contains(settings.apiKey.toUtf8())); QVERIFY(bytes.contains("protected_key"));
        settings.rememberKey = false; QVERIFY(settings.save(root.path(), &error)); QVERIFY(AiSettings::load(root.path()).apiKey.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(!file.readAll().contains("protected_key")); file.close();
        QVERIFY(writeSettings(file.fileName(), {{QStringLiteral("remember_key"), true}, {QStringLiteral("protected_key"), QStringLiteral("ZmFrZQ==")}}));
        QVERIFY(AiSettings::load(root.path(), &error).apiKey.isEmpty()); QVERIFY(!error.isEmpty());
    }
    void propertiesRenderAndSpaceOpensProperties_data() {
        QTest::addColumn<QString>("theme"); QTest::addColumn<QString>("mode");
        for (const QString &theme : themeIds()) for (const QString &mode : {QStringLiteral("dark"), QStringLiteral("light")})
            QTest::newRow(qPrintable(theme + QChar('-') + mode)) << theme << mode;
    }
    void propertiesRenderAndSpaceOpensProperties() {
        QFETCH(QString, theme); QFETCH(QString, mode); QTemporaryDir root;
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("folder/nested"))));
        { QFile file(root.filePath(QStringLiteral("folder/nested/data.bin"))); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(QByteArray(4096, 'x')), 4096); }
        { QFile file(root.filePath(QStringLiteral("note.txt"))); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("hello"), 5); }
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.setReducedMotion(true); window.setTheme(theme); window.setColorMode(mode); window.show();
        window.leftPane()->navigate({Source::Local, root.path(), {}, {}}); QTRY_COMPARE(window.leftPane()->fileModel()->rowCount(), 2);
        window.setActive(window.leftPane()); auto *view = window.leftPane()->view(); view->setCurrentIndex(window.leftPane()->fileModel()->index(0));
        QTest::keyClick(view, Qt::Key_Space);
        QPointer<QDialog> dialog = window.findChild<QDialog *>(QStringLiteral("fileProperties")); QVERIFY(dialog); QCOMPARE(dialog->windowTitle(), QStringLiteral("属性"));
        QTRY_VERIFY(dialog->property("statisticsFinished").toBool());
        QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("propertySize"))->text(), QStringLiteral("4.0 KB（4096 字节）"));
        QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("propertyContents"))->text(), QStringLiteral("1 个文件，1 个子文件夹"));
        QVERIFY(!dialog->findChild<QPlainTextEdit *>()); QVERIFY(!dialog->findChild<QLineEdit *>()); QVERIFY(!QApplication::activeModalWidget());
        dialog->resize(480, 320); QTest::qWait(20);
        for (auto *label : dialog->findChildren<QLabel *>()) QVERIFY(dialog->rect().contains(label->mapTo(dialog, label->rect().topLeft())));
        const QString evidence = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        if (!evidence.isEmpty()) QVERIFY(dialog->grab().save(QDir(evidence).filePath(QStringLiteral("native-properties-") + theme + QChar('-') + mode + QStringLiteral(".png"))));
        dialog->close(); QTRY_VERIFY(dialog.isNull());
        view->setCurrentIndex(window.leftPane()->fileModel()->index(1)); QTest::keyClick(view, Qt::Key_Space);
        dialog = window.findChild<QDialog *>(QStringLiteral("fileProperties")); QVERIFY(dialog); QTRY_VERIFY(dialog->property("statisticsFinished").toBool());
        QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("propertySize"))->text(), QStringLiteral("5 B（5 字节）"));
        QVERIFY(!dialog->findChild<QPlainTextEdit *>()); dialog->close(); QTRY_VERIFY(dialog.isNull());
        view->selectionModel()->clearSelection(); QTest::keyClick(view, Qt::Key_Space);
        dialog = window.findChild<QDialog *>(QStringLiteral("fileProperties")); QVERIFY(dialog); QTRY_VERIFY(dialog->property("statisticsFinished").toBool());
        QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("propertySize"))->text(), QStringLiteral("4.0 KB（4101 字节）")); dialog->close(); QTRY_VERIFY(dialog.isNull());
        Entry missing; missing.path = root.filePath(QStringLiteral("missing"));
        dialog = new PropertiesDialog({missing}, &window); dialog->show(); QTRY_VERIFY(dialog->property("statisticsFinished").toBool());
        QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("propertySize"))->text(), QStringLiteral("未完成")); QVERIFY(!dialog->findChild<QLabel *>(QStringLiteral("propertyStatus"))->text().isEmpty());
        dialog->close(); QTRY_VERIFY(dialog.isNull());
        Entry folder; folder.path = root.path(); folder.directory = true;
        dialog = new PropertiesDialog({folder}, &window); dialog->show(); dialog->close(); QTRY_VERIFY(dialog.isNull());
        window.close();
    }
    void contextMenusMatchSelectedEntryTypes() {
        QTemporaryDir root; QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("folder.apk")))); QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("folder.zip"))));
        for (const QString &name : {QStringLiteral("file.apk"), QStringLiteral("file.zip"), QStringLiteral("note.txt")}) { QFile file(root.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture"); }
        { QFile file(root.filePath(QStringLiteral("folder.apk/apktool.yml"))); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture"); }
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.setReducedMotion(true); window.show();
        auto *pane = window.leftPane(); pane->navigate({Source::Local, root.path(), {}, {}}); QTRY_COMPARE(pane->fileModel()->rowCount(), 5); window.setActive(pane);
        auto select = [&](const QStringList &names) {
            pane->view()->selectionModel()->clear();
            const auto entries = pane->fileModel()->entries();
            for (qsizetype row = 0; row < entries.size(); ++row) if (names.contains(entries[row].name)) pane->view()->selectionModel()->select(pane->fileModel()->index(int(row)), QItemSelectionModel::Select);
        };
        QStringList actions;
        auto inspectMenu = [&] {
            actions.clear();
            QTimer::singleShot(0, &window, [&] {
                auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget()); if (!popup) return;
                const auto close = qScopeGuard([popup] { popup->close(); });
                const QString evidence = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
                if (!evidence.isEmpty() && pane->selected().size() == 1 && pane->selected().first().name == QStringLiteral("folder.apk"))
                    QVERIFY(popup->grab().save(QDir(evidence).filePath(QStringLiteral("native-context-folder.png"))));
                for (auto *action : popup->actions()) { actions.append(action->text()); if (action->text() == QStringLiteral("属性（空格）")) { action->trigger(); } }
            });
            pane->menuRequested(window.mapToGlobal(QPoint(280, 150)));
        };
        for (const QString &name : {QStringLiteral("folder.apk"), QStringLiteral("folder.zip"), QStringLiteral("note.txt")}) {
            select({name}); inspectMenu(); QVERIFY(!actions.isEmpty()); QVERIFY(!actions.contains(QStringLiteral("APK 工具"))); QVERIFY(!actions.contains(QStringLiteral("解压 ZIP/APK")));
            QCOMPARE(actions.count(QStringLiteral("属性（空格）")), 1); QVERIFY(!actions.contains(QStringLiteral("属性"))); QVERIFY(!actions.contains(QStringLiteral("快速预览（空格）")));
            auto *dialog = window.findChild<QDialog *>(QStringLiteral("fileProperties")); QVERIFY(dialog); dialog->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        select({QStringLiteral("file.zip")}); inspectMenu(); QVERIFY(actions.contains(QStringLiteral("解压 ZIP/APK"))); QVERIFY(!actions.contains(QStringLiteral("APK 工具")));
        window.findChild<QDialog *>(QStringLiteral("fileProperties"))->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        select({QStringLiteral("file.apk")}); inspectMenu(); QVERIFY(actions.contains(QStringLiteral("解压 ZIP/APK"))); QVERIFY(actions.contains(QStringLiteral("APK 工具")));
        window.findChild<QDialog *>(QStringLiteral("fileProperties"))->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        select({QStringLiteral("file.apk"), QStringLiteral("folder.apk")}); inspectMenu(); QVERIFY(!actions.contains(QStringLiteral("APK 工具"))); QVERIFY(!actions.contains(QStringLiteral("解压 ZIP/APK")));
        window.findChild<QDialog *>(QStringLiteral("fileProperties"))->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        bool inspected = false, projectFound = false;
        QTimer::singleShot(0, &window, [&] {
            auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget()); if (!popup) return;
            const auto close = qScopeGuard([popup] { popup->close(); });
            inspected = true; for (auto *action : popup->actions()) { QVERIFY(!action->text().contains(QStringLiteral("导入旧版"))); if (action->text() == QStringLiteral("APK 工程")) projectFound = true; }
        });
        window.findChild<QPushButton *>(QStringLiteral("action_more"))->click(); QVERIFY(inspected); QVERIFY(!projectFound);
        pane->navigate({Source::Local, root.filePath(QStringLiteral("folder.apk")), {}, {}}); QTRY_COMPARE(pane->fileModel()->rowCount(), 1);
        QTimer::singleShot(0, &window, [&] {
            auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget()); if (!popup) return;
            const auto close = qScopeGuard([popup] { popup->close(); });
            for (auto *action : popup->actions()) if (action->text() == QStringLiteral("APK 工程")) { projectFound = true; QVERIFY(action->menu()); QCOMPARE(action->menu()->actions().size(), 2); }
        });
        window.findChild<QPushButton *>(QStringLiteral("action_more"))->click(); QVERIFY(projectFound); window.close();
    }
    void toolbarSelectionAndDirection_data() {
        QTest::addColumn<QString>("theme"); QTest::addColumn<QString>("mode");
        for (const QString &theme : themeIds()) for (const QString &mode : {QStringLiteral("dark"), QStringLiteral("light")})
            QTest::newRow(qPrintable(theme + QChar('-') + mode)) << theme << mode;
    }
    void toolbarSelectionAndDirection() {
        QFETCH(QString, theme); QFETCH(QString, mode); QTemporaryDir root;
        const QString left = root.filePath(QStringLiteral("left")), right = root.filePath(QStringLiteral("right"));
        QVERIFY(QDir().mkpath(left + QStringLiteral("/folder"))); QVERIFY(QDir().mkpath(right + QStringLiteral("/folder")));
        for (const QString &name : {QStringLiteral("a.txt"), QStringLiteral("b.txt")}) { QFile file(QDir(left).filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("left"); }
        for (const QString &name : {QStringLiteral("c.txt"), QStringLiteral("d.txt"), QStringLiteral("e.txt")}) { QFile file(QDir(right).filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("right"); }
        auto settings = readSettings(settingsPath()); settings.insert(QStringLiteral("extension_visible"), false); settings.insert(QStringLiteral("geometry"), QString()); QVERIFY(writeSettings(settingsPath(), settings));
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.setReducedMotion(true); window.setTheme(theme); window.setColorMode(mode); window.resize(1520, 900); window.show();
        window.leftPane()->navigate({Source::Local, left, {}, {}}); window.rightPane()->navigate({Source::Local, right, {}, {}});
        QTRY_COMPARE(window.leftPane()->fileModel()->rowCount(), 3); QTRY_COMPARE(window.rightPane()->fileModel()->rowCount(), 4);
        QApplication::setActiveWindow(&window);
        auto *copy = window.findChild<QPushButton *>(QStringLiteral("action_copy")), *move = window.findChild<QPushButton *>(QStringLiteral("action_move"));
        auto *all = window.findChild<QPushButton *>(QStringLiteral("action_select-all")), *none = window.findChild<QPushButton *>(QStringLiteral("action_select-none"));
        QVERIFY(copy && move && all && none);
        for (Pane *pane : {window.leftPane(), window.rightPane()}) pane->view()->selectionModel()->select(pane->fileModel()->index(0), QItemSelectionModel::ClearAndSelect);
        auto *leftPath = window.leftPane()->findChild<QLineEdit *>(QStringLiteral("panePath"));
        auto *rightPath = window.rightPane()->findChild<QLineEdit *>(QStringLiteral("panePath"));
        QTest::mouseClick(leftPath, Qt::LeftButton); QCOMPARE(window.activePane(), window.leftPane());
        QCOMPARE(copy->property("symbol").toString(), QStringLiteral("copy-forward")); QCOMPARE(move->property("symbol").toString(), QStringLiteral("move-forward"));
        const QImage forward = copy->icon().pixmap(56, 36).toImage();
        QTest::mouseClick(all, Qt::LeftButton); QCOMPARE(window.activePane(), window.leftPane()); QCOMPARE(window.leftPane()->selected().size(), 3); QCOMPARE(window.rightPane()->selected().size(), 1);
        QVERIFY(window.leftPane()->view()->hasFocus());
        const QString evidence = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        QTest::qWait(30);
        if (!evidence.isEmpty()) QVERIFY(window.grab().save(QDir(evidence).filePath(QStringLiteral("native-toolbar-right-") + theme + QChar('-') + mode + QStringLiteral(".png"))));
        QTest::mouseClick(rightPath, Qt::LeftButton); QCOMPARE(window.activePane(), window.rightPane());
        QCOMPARE(copy->property("symbol").toString(), QStringLiteral("copy-back")); QCOMPARE(move->property("symbol").toString(), QStringLiteral("move-back"));
        QVERIFY(copy->icon().pixmap(56, 36).toImage() != forward);
        QVERIFY(copy->toolTip().contains(QStringLiteral("右栏所选项目到左栏"))); QVERIFY(move->toolTip().contains(QStringLiteral("右栏所选项目到左栏")));
        QTest::mouseClick(all, Qt::LeftButton); QCOMPARE(window.rightPane()->selected().size(), 4); QCOMPARE(window.leftPane()->selected().size(), 3);
        QTest::mouseClick(none, Qt::LeftButton); QVERIFY(window.rightPane()->selected().isEmpty()); QVERIFY(!window.rightPane()->view()->currentIndex().isValid()); QCOMPARE(window.leftPane()->selected().size(), 3);
        window.rightPane()->search(QStringLiteral("c.txt")); QTRY_COMPARE(window.rightPane()->fileModel()->rowCount(), 1);
        QTest::mouseClick(all, Qt::LeftButton); QCOMPARE(window.rightPane()->selected().size(), 1); QCOMPARE(window.rightPane()->selected().first().name, QStringLiteral("c.txt")); QCOMPARE(window.leftPane()->selected().size(), 3);
        window.rightPane()->search(QStringLiteral("no-matching-files")); QTRY_COMPARE(window.rightPane()->fileModel()->rowCount(), 0);
        QTest::mouseClick(all, Qt::LeftButton); QTest::mouseClick(none, Qt::LeftButton); QVERIFY(window.rightPane()->selected().isEmpty()); QCOMPARE(window.leftPane()->selected().size(), 3);
        window.rightPane()->refresh(); QTRY_COMPARE(window.rightPane()->fileModel()->rowCount(), 4);
        window.setColorMode(mode == QStringLiteral("dark") ? QStringLiteral("light") : QStringLiteral("dark")); window.setColorMode(mode);
        QCOMPARE(copy->property("symbol").toString(), QStringLiteral("copy-back")); QCOMPARE(copy->iconSize(), QSize(28, 18)); QCOMPARE(move->iconSize(), QSize(18, 18));
        QTest::qWait(30);
        if (!evidence.isEmpty()) QVERIFY(window.grab().save(QDir(evidence).filePath(QStringLiteral("native-toolbar-left-") + theme + QChar('-') + mode + QStringLiteral(".png"))));
        auto *toolbar = window.findChild<QWidget *>(QStringLiteral("workspaceToolbar")); QVERIFY(toolbar);
        auto *taskWidget = window.findChild<QWidget *>(QStringLiteral("taskProgress")); QVERIFY(taskWidget); auto *task = static_cast<TaskProgress *>(taskWidget);
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("extensionDock")); QVERIFY(dock); dock->show();
        task->begin(QStringLiteral("复制文件"), [] {}); task->updateProgress(1024, 4096, QStringLiteral("复制文件"));
        window.resize(1100, 650); QTest::qWait(60); QCOMPARE(window.width(), 1100);
        const auto buttons = toolbar->findChildren<QPushButton *>(QString(), Qt::FindDirectChildrenOnly);
        for (auto *button : buttons) {
            QVERIFY(button->isVisible()); QVERIFY(toolbar->rect().contains(button->geometry())); QVERIFY(!button->icon().isNull());
            for (auto *other : buttons) if (other != button) QVERIFY(!button->geometry().intersects(other->geometry()));
            QVERIFY(!button->geometry().intersects(task->geometry()));
        }
        QVERIFY(toolbar->rect().contains(task->geometry())); QCOMPARE(task->height(), 44); QVERIFY(copy->text().isEmpty()); QCOMPARE(copy->iconSize(), QSize(28, 18));
        QCOMPARE(copy->property("symbol").toString(), QStringLiteral("copy-back"));
        if (!evidence.isEmpty()) QVERIFY(window.grab().save(QDir(evidence).filePath(QStringLiteral("native-toolbar-compact-") + theme + QChar('-') + mode + QStringLiteral(".png"))));
        QVERIFY(!QApplication::activeModalWidget()); task->finish(QStringLiteral("完成"), true); window.close();
    }
    void toolbarTransfersFollowActivePane() {
        QTemporaryDir root; const QString left = root.filePath(QStringLiteral("left")), right = root.filePath(QStringLiteral("right"));
        QVERIFY(QDir().mkpath(left)); QVERIFY(QDir().mkpath(right));
        for (const auto &directory : {left, right}) for (const QString &command : {QStringLiteral("copy"), QStringLiteral("move")}) {
            const QString name = QFileInfo(directory).fileName() + QChar('-') + command + QStringLiteral(".txt");
            QFile file(QDir(directory).filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(name.toUtf8());
        }
        auto settings = readSettings(settingsPath()); settings.insert(QStringLiteral("extension_visible"), false); QVERIFY(writeSettings(settingsPath(), settings));
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.setReducedMotion(true); window.show();
        window.leftPane()->navigate({Source::Local, left, {}, {}}); window.rightPane()->navigate({Source::Local, right, {}, {}});
        QTRY_COMPARE(window.leftPane()->fileModel()->rowCount(), 2); QTRY_COMPARE(window.rightPane()->fileModel()->rowCount(), 2);
        auto *taskWidget = window.findChild<QWidget *>(QStringLiteral("taskProgress")); QVERIFY(taskWidget); auto *task = static_cast<TaskProgress *>(taskWidget);
        for (const QString &command : {QStringLiteral("copy"), QStringLiteral("move")}) for (Pane *pane : {window.leftPane(), window.rightPane()}) {
            const bool toRight = pane == window.leftPane(); const QString directory = toRight ? left : right, target = toRight ? right : left;
            const QString name = (toRight ? QStringLiteral("left-") : QStringLiteral("right-")) + command + QStringLiteral(".txt");
            int row = -1; const auto entries = pane->fileModel()->entries();
            for (qsizetype index = 0; index < entries.size(); ++index) if (entries[index].name == name) row = int(index);
            QVERIFY(row >= 0);
            QTest::mouseClick(pane->view()->viewport(), Qt::LeftButton, Qt::NoModifier, pane->view()->visualRect(pane->fileModel()->index(row)).center());
            QCOMPARE(window.activePane(), pane); QCOMPARE(pane->selected().size(), 1); QCOMPARE(pane->selected().first().name, name);
            auto *button = window.findChild<QPushButton *>(QStringLiteral("action_") + command); QVERIFY(button);
            QTest::mouseClick(button, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(!task->active(), 5000); QVERIFY2(QFileInfo::exists(QDir(target).filePath(name)), qPrintable(task->toolTip()));
            QCOMPARE(QFileInfo::exists(QDir(directory).filePath(name)), command == QStringLiteral("copy"));
            QFile result(QDir(target).filePath(name)); QVERIFY(result.open(QIODevice::ReadOnly)); QCOMPARE(result.readAll(), name.toUtf8());
            QVERIFY(!QApplication::activeModalWidget());
        }
        window.close();
    }
    void workspaceRedesignLayoutAndNavigation() {
        QTemporaryDir workspace; QVERIFY(workspace.isValid());
        const QString source = workspace.filePath(QStringLiteral("工作文件"));
        const QString destination = workspace.filePath(QStringLiteral("整理归档"));
        QVERIFY(QDir().mkpath(source)); QVERIFY(QDir().mkpath(destination));
        for (const QString &name : {QStringLiteral("产品设计"), QStringLiteral("图片素材"), QStringLiteral("应用安装包"), QStringLiteral("项目备份")}) QVERIFY(QDir().mkpath(QDir(source).filePath(name)));
        for (const QString &name : {QStringLiteral("项目说明.md"), QStringLiteral("设计规范.pdf"), QStringLiteral("工作计划.txt"), QStringLiteral("界面配置.json"), QStringLiteral("资源归档.zip"), QStringLiteral("更新日志.md")}) {
            QFile file(QDir(source).filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("TwinFile UI regression fixture\n");
        }
        for (const QString &name : {QStringLiteral("已完成"), QStringLiteral("共享资料"), QStringLiteral("历史版本")}) QVERIFY(QDir().mkpath(QDir(destination).filePath(name)));
        QFile archive(QDir(destination).filePath(QStringLiteral("归档索引.md"))); QVERIFY(archive.open(QIODevice::WriteOnly)); archive.write("Archived workspace\n"); archive.close();
        QImage preview(240, 160, QImage::Format_RGB32); preview.fill(QColor(QStringLiteral("#6e8ab5"))); QVERIFY(preview.save(QDir(source).filePath(QStringLiteral("封面预览.png"))));
        auto state = readSettings(settingsPath()); state.insert(QStringLiteral("extension_visible"), false); state.insert(QStringLiteral("geometry"), QString()); state.insert(QStringLiteral("favorites"), QJsonArray{source, destination}); QVERIFY(writeSettings(settingsPath(), state));
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.setReducedMotion(true); window.setTheme(QStringLiteral("twinfile")); window.setColorMode(QStringLiteral("dark")); window.resize(1440, 900); window.show();
        window.leftPane()->navigate({Source::Local, source, {}, {}}); window.rightPane()->navigate({Source::Local, destination, {}, {}});
        QTRY_COMPARE(window.leftPane()->fileModel()->rowCount(), 11); QTRY_COMPARE(window.rightPane()->fileModel()->rowCount(), 4); window.setActive(window.leftPane());
        auto *toolbar = window.findChild<QWidget *>(QStringLiteral("workspaceToolbar")); auto *header = window.findChild<QWidget *>(QStringLiteral("workspaceHeader")); auto *dock = window.findChild<QDockWidget *>(QStringLiteral("extensionDock")); auto *assistant = window.findChild<QPushButton *>(QStringLiteral("assistantToggle"));
        QVERIFY(toolbar); QVERIFY(header); QVERIFY(dock); QVERIFY(assistant); QVERIFY(header->isVisible()); QVERIFY(!dock->isVisible()); QVERIFY(!assistant->isChecked());
        QVERIFY(toolbar->mapTo(&window, QPoint()).y() < window.leftPane()->mapTo(&window, QPoint()).y());
        QPushButton *sourceNavigation = nullptr; QPushButton *destinationNavigation = nullptr;
        for (auto *button : window.findChildren<QPushButton *>()) {
            if (button->property("navigationPath").toString() == source) sourceNavigation = button;
            if (button->property("navigationPath").toString() == destination) destinationNavigation = button;
        }
        QVERIFY(sourceNavigation); QVERIFY(destinationNavigation); QVERIFY(sourceNavigation->isChecked()); QVERIFY(!destinationNavigation->isChecked());
        window.setActive(window.rightPane()); QVERIFY(destinationNavigation->isChecked()); QVERIFY(!sourceNavigation->isChecked());
        QVERIFY(window.leftPane()->findChild<QLabel *>(QStringLiteral("paneIndicator"))->palette().color(QPalette::WindowText) != window.rightPane()->findChild<QLabel *>(QStringLiteral("paneIndicator"))->palette().color(QPalette::WindowText));
        sourceNavigation->click(); QTRY_COMPARE(window.rightPane()->location().path, source); QVERIFY(sourceNavigation->isChecked());
        window.rightPane()->navigate({Source::Local, destination, {}, {}}); window.setActive(window.leftPane());
        auto checkLayout = [&] {
            QList<QPushButton *> buttons = toolbar->findChildren<QPushButton *>();
            for (auto *button : buttons) {
                QVERIFY(button->isVisible()); QVERIFY(!button->icon().isNull()); QVERIFY(!button->accessibleName().isEmpty()); QVERIFY(toolbar->rect().contains(button->geometry()));
                for (auto *other : buttons) if (other != button) QVERIFY(!button->geometry().intersects(other->geometry()));
            }
            for (Pane *pane : {window.leftPane(), window.rightPane()}) {
                QVERIFY(pane->width() >= 260); QVERIFY(pane->view()->width() > 210); QCOMPARE(pane->view()->sizeHintForRow(0), 54);
                QVERIFY(!pane->view()->horizontalScrollBar()->isVisible()); QVERIFY(pane->view()->visualRect(pane->fileModel()->index(0)).width() <= pane->view()->viewport()->width());
                auto *path = pane->findChild<QLineEdit *>(QStringLiteral("panePath")); QVERIFY(path); QVERIFY(pane->rect().contains(QRect(path->mapTo(pane, QPoint()), path->size())));
            }
            QVERIFY(window.rightPane()->geometry().left() - window.leftPane()->geometry().right() >= 10);
        };
        const QString output = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        QTest::qWait(100); checkLayout(); window.leftPane()->view()->selectionModel()->select(window.leftPane()->fileModel()->index(5), QItemSelectionModel::ClearAndSelect);
        if (!output.isEmpty()) QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("native-workspace-redesign-dark.png"))));
        auto *locations = window.findChild<QWidget *>(QStringLiteral("navigationLocations")); QVERIFY(locations); QTest::mouseClick(locations, Qt::LeftButton, Qt::NoModifier, QPoint(80, locations->height() - 6)); QVERIFY(window.leftPane()->selected().isEmpty());
        assistant->click(); QTRY_VERIFY(dock->isVisible()); QVERIFY(assistant->isChecked()); QTest::qWait(40); checkLayout();
        if (!output.isEmpty()) QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("native-workspace-redesign-assistant.png"))));
        window.resize(1100, 720); QTest::qWait(80); QCOMPARE(window.width(), 1100); checkLayout();
        if (!output.isEmpty()) QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("native-workspace-redesign-compact.png"))));
        dock->hide(); QVERIFY(!assistant->isChecked()); window.resize(1440, 900); window.setColorMode(QStringLiteral("light")); QTest::qWait(50); checkLayout();
        if (!output.isEmpty()) QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("native-workspace-redesign-light.png"))));
        bool searchChecked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            QTimer::singleShot(8000, dialog, &QDialog::reject); dialog->setTextValue(QStringLiteral(".md")); searchChecked = true; dialog->accept();
        });
        window.findChild<QPushButton *>(QStringLiteral("workspaceSearch"))->click(); QVERIFY(searchChecked); QTRY_COMPARE(window.leftPane()->fileModel()->rowCount(), 2); QCOMPARE(window.rightPane()->fileModel()->rowCount(), 4);
        bool sortChecked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); if (!menu) return;
            QTimer::singleShot(8000, menu, &QMenu::close);
            for (auto *action : menu->actions()) if (action->text() == QStringLiteral("大小")) { action->trigger(); sortChecked = true; break; }
            menu->close();
        });
        window.findChild<QPushButton *>(QStringLiteral("action_sort"))->click(); QVERIFY(sortChecked); window.close();
    }
    void workspaceNavigationScrollsWithoutGrowingWindow() {
        QTemporaryDir workspace; QJsonArray favorites;
        for (int index = 0; index < 40; ++index) {
            const QString path = workspace.filePath(QStringLiteral("收藏文件夹_很长的名称_%1").arg(index)); QVERIFY(QDir().mkpath(path)); favorites.append(path);
        }
        auto state = readSettings(settingsPath()); state.insert(QStringLiteral("favorites"), favorites); state.insert(QStringLiteral("extension_visible"), true); state.insert(QStringLiteral("geometry"), QString()); QVERIFY(writeSettings(settingsPath(), state));
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1100, 650); window.show(); QTest::qWait(50);
        QCOMPARE(window.size(), QSize(1100, 650)); auto *scroll = window.findChild<QScrollArea *>(QStringLiteral("navigationScroll")); QVERIFY(scroll); QVERIFY(scroll->verticalScrollBar()->maximum() > 0); QVERIFY(!scroll->horizontalScrollBar()->isVisible());
        QPushButton *last = nullptr;
        for (auto *button : scroll->findChildren<QPushButton *>()) if (button->property("navigationPath").toString() == favorites.last().toString()) last = button;
        QVERIFY(last); QVERIFY(last->text().contains(QChar(0x2026))); QCOMPARE(last->accessibleName(), QFileInfo(favorites.last().toString()).fileName()); scroll->ensureWidgetVisible(last); QTest::qWait(10);
        QVERIFY(scroll->viewport()->rect().contains(QRect(last->mapTo(scroll->viewport(), QPoint()), last->size()))); last->click(); QCOMPARE(window.activePane()->location().path, favorites.last().toString()); QVERIFY(last->isChecked()); window.close();
    }
    void storageLocationsAndTestModeIsolation() {
        const auto locations = storageLocations();
        QVERIFY(!locations.isEmpty()); QSet<QString> paths;
        for (const auto &entry : locations) {
            QCOMPARE(entry.source, Source::Drive); QVERIFY(entry.directory); QVERIFY(QDir::isAbsolutePath(entry.path));
            QVERIFY(entry.state == QStringLiteral("drive") || entry.state == QStringLiteral("usb"));
            QVERIFY(!entry.path.startsWith(QStringLiteral("//"))); QVERIFY(!paths.contains(entry.path)); paths.insert(entry.path);
        }
        StorageMonitor monitor; QSignalSpy changed(&monitor, &StorageMonitor::changed); monitor.start(); monitor.detect(); QTest::qWait(40); QCOMPARE(changed.size(), 0);
        auto restore = qScopeGuard([] { qputenv("TWINFILE_TEST_MODE", "1"); });
        qunsetenv("TWINFILE_TEST_MODE"); monitor.detect(); monitor.detect(); QTRY_COMPARE(changed.size(), 1);
    }
    void deviceAndDriveShortcuts_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("dark") << QStringLiteral("dark");
        QTest::newRow("light") << QStringLiteral("light");
    }
    void deviceAndDriveShortcuts() {
        QFETCH(QString, mode);
        NavigationPhoneApi phoneApi; QVERIFY(phoneApi.server.isListening());
        QTemporaryDir workspace; QVERIFY(workspace.isValid());
        auto makeDrive = [&](const QString &name, const QString &path, const QString &kind) {
            Entry entry; entry.name = name; entry.path = workspace.filePath(path); entry.state = kind; entry.source = Source::Drive; entry.directory = true; QDir().mkpath(entry.path); return entry;
        };
        const Entry system = makeDrive(QStringLiteral("本地磁盘 (C:)"), QStringLiteral("system"), QStringLiteral("drive"));
        const Entry data = makeDrive(QStringLiteral("工作资料 (D:)"), QStringLiteral("data"), QStringLiteral("drive"));
        const Entry usb = makeDrive(QStringLiteral("U盘 (E:)"), QStringLiteral("usb"), QStringLiteral("usb"));
        QVERIFY(QDir().mkpath(QDir(data.path).filePath(QStringLiteral("项目"))));
        auto state = readSettings(settingsPath()); state.insert(QStringLiteral("workspace"), QJsonObject()); state.insert(QStringLiteral("favorites"), QJsonArray()); state.insert(QStringLiteral("geometry"), QString()); state.insert(QStringLiteral("extension_visible"), false); QVERIFY(writeSettings(settingsPath(), state));
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.setReducedMotion(true); window.setTheme(QStringLiteral("twinfile")); window.setColorMode(mode); window.resize(1240, 820); window.show();
        auto *phones = window.findChild<DeviceMonitor *>(); auto *drives = window.findChild<StorageMonitor *>(); auto *section = window.findChild<QWidget *>(QStringLiteral("navigationDevices")); auto *scroll = window.findChild<QScrollArea *>(QStringLiteral("navigationScroll"));
        QVERIFY(phones); QVERIFY(drives); QVERIFY(section); QVERIFY(scroll);
        auto *detect = section->findChild<QPushButton *>(QStringLiteral("navigationPhoneDetect")); QVERIFY(detect); detect->click();
        drives->changed({system, data, usb});
        const auto parsed = parseDevices(QStringLiteral("phone-online device model:Pixel_9\nphone-locked unauthorized model:备用手机\n")); QCOMPARE(parsed.size(), 2); phones->changed(parsed);
        auto find = [&](const char *property, const QString &value) -> QPushButton * {
            for (auto *button : section->findChildren<QPushButton *>()) if (button->property(property).toString() == value) return button;
            return nullptr;
        };
        auto *disk = find("navigationDrive", data.path); auto *removable = find("navigationDrive", usb.path); auto *phone = find("navigationDevice", QStringLiteral("phone-online")); auto *locked = find("navigationDevice", QStringLiteral("phone-locked"));
        QVERIFY(disk); QVERIFY(removable); QVERIFY(phone); QVERIFY(locked); QCOMPARE(removable->property("symbol").toString(), QStringLiteral("usb")); QVERIFY(!phone->icon().isNull());
        window.leftPane()->navigate({Source::Local, system.path, {}, {}}); window.rightPane()->navigate({Source::Local, usb.path, {}, {}}); window.setActive(window.rightPane());
        disk->click(); QCOMPARE(window.rightPane()->location().path, data.path); QCOMPARE(window.leftPane()->location().path, system.path); QVERIFY(disk->isChecked());
        window.rightPane()->navigate({Source::Local, QDir(data.path).filePath(QStringLiteral("项目")), {}, {}}); QVERIFY(disk->isChecked());
        const QString sibling = data.path + QStringLiteral("-other"); QVERIFY(QDir().mkpath(sibling)); window.rightPane()->navigate({Source::Local, sibling, {}, {}}); QVERIFY(!disk->isChecked());
        locked->click(); QCOMPARE(window.rightPane()->location().source, Source::Local); QVERIFY(!locked->isChecked());
        phone->click(); QCOMPARE(window.rightPane()->location().source, Source::Phone); QCOMPARE(window.rightPane()->location().path, QStringLiteral("phone-online")); QCOMPARE(window.rightPane()->location().inner, QStringLiteral("/sdcard")); QVERIFY(phone->isChecked()); QCOMPARE(window.leftPane()->location().path, system.path);
        QTRY_COMPARE(phoneApi.listings, 1); QTest::qWait(50);
        window.setActive(window.leftPane()); QVERIFY(!phone->isChecked()); window.setActive(window.rightPane()); QVERIFY(phone->isChecked());
        scroll->verticalScrollBar()->setValue(30); const int offset = scroll->verticalScrollBar()->value();
        drives->changed({system, data, usb}); phones->changed(parsed); QCOMPARE(find("navigationDrive", data.path), disk); QCOMPARE(find("navigationDevice", QStringLiteral("phone-online")), phone); QCOMPARE(scroll->verticalScrollBar()->value(), offset);
        scroll->ensureWidgetVisible(removable); QTest::qWait(30);
        for (auto *button : section->findChildren<QPushButton *>()) { QVERIFY(section->rect().contains(button->geometry())); QVERIFY(!button->accessibleName().isEmpty()); }
        QVERIFY(!scroll->horizontalScrollBar()->isVisible());
        const QString output = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        if (!output.isEmpty()) { QDir().mkpath(output); QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("native-device-shortcuts-%1.png").arg(mode)))); }
        Entry renamed = data; renamed.name = QStringLiteral("新卷标 (D:)"); drives->changed({system, renamed}); QVERIFY(!find("navigationDrive", usb.path)); QCOMPARE(find("navigationDrive", data.path)->accessibleName(), renamed.name);
        const Location before = window.rightPane()->location(); phones->changed({}); QVERIFY(section->findChild<QPushButton *>(QStringLiteral("navigationPhoneDetect"))); QVERIFY(!find("navigationDevice", QStringLiteral("phone-online"))); QVERIFY(window.rightPane()->location() == before);
        window.setActive(window.leftPane()); find("navigationDrive", data.path)->click(); QCOMPARE(window.leftPane()->location().path, data.path); QVERIFY(window.rightPane()->location() == before);
        window.rightPane()->navigate({Source::Local, usb.path, {}, {}}); window.resize(1100, 650); QTest::qWait(30); QCOMPARE(window.size(), QSize(1100, 650)); QVERIFY(!scroll->horizontalScrollBar()->isVisible()); window.close();
    }
    void jsonResponseAndExplicitPayload() {
        MockApi api; QVERIFY(api.server.isListening());
        api.respond = [](QTcpSocket *socket) { MockApi::response(socket, jsonAnswer(QStringLiteral("回答正常"))); };
        AiClient client; QSignalSpy completed(&client, &AiClient::completed); QSignalSpy failed(&client, &AiClient::failed); QString error;
        QVERIFY(client.send(apiSettings(api), question(), &error)); QTRY_COMPARE(completed.size(), 1); QVERIFY(failed.isEmpty()); QVERIFY(!client.busy());
        QCOMPARE(completed.first().first().toString(), QStringLiteral("回答正常")); QCOMPARE(api.requests.size(), 1);
        QVERIFY(api.requests.first().startsWith("POST /v1/chat/completions HTTP/1.1"));
        QVERIFY(api.requests.first().contains("Authorization: Bearer test-secret-not-real"));
        const auto body = api.bodies.first(); QCOMPARE(body.value(QStringLiteral("model")).toString(), QStringLiteral("mock-model"));
        QVERIFY(body.value(QStringLiteral("stream")).toBool()); QCOMPARE(body.value(QStringLiteral("messages")).toArray().size(), 2);
        QVERIFY(!body.contains(QStringLiteral("tools"))); QVERIFY(!QJsonDocument(body).toJson().contains("C:/private"));
    }
    void streamingHandlesSplitUtf8AndCrlf() {
        MockApi api;
        const QByteArray body = QByteArray(": heartbeat\r\n\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"") + QStringLiteral("你好").toUtf8() + "\"}}]}\r\n\r\ndata: {\"choices\":[{\"delta\":{\"content\":\" world\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
        api.respond = [body](QTcpSocket *socket) {
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n");
            const qsizetype split = body.indexOf(QStringLiteral("你").toUtf8()) + 1;
            socket->write(body.left(split));
            QTimer::singleShot(30, socket, [socket, body, split] { socket->write(body.mid(split)); socket->disconnectFromHost(); });
        };
        AiClient client; QSignalSpy completed(&client, &AiClient::completed); QSignalSpy changed(&client, &AiClient::textChanged); QString error;
        QVERIFY(client.send(apiSettings(api), question(), &error)); QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(completed.first().first().toString(), QStringLiteral("你好 world")); QVERIFY(changed.size() >= 2);
    }
    void streamDoneDoesNotWaitForSocketClose() {
        MockApi api;
        api.respond = [](QTcpSocket *socket) {
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: keep-alive\r\n\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\ndata: [DONE]\n\n");
        };
        AiClient client; QSignalSpy completed(&client, &AiClient::completed); QString error;
        QVERIFY(client.send(apiSettings(api), question(), &error)); QTRY_COMPARE(completed.size(), 1); QVERIFY(!client.busy()); QCOMPARE(completed.first().first().toString(), QStringLiteral("done"));
    }
    void invalidAndIncompleteResponses_data() {
        QTest::addColumn<QByteArray>("body"); QTest::addColumn<QByteArray>("type");
        QTest::newRow("html") << QByteArray("<html>not json</html>") << QByteArray("text/html");
        QTest::newRow("empty") << QByteArray("{\"choices\":[]}") << QByteArray("application/json");
        QTest::newRow("bad-sse") << QByteArray("data: not-json\n\n") << QByteArray("text/event-stream");
        QTest::newRow("provider-error") << QByteArray("data: {\"error\":{\"message\":\"secret\"}}\n\n") << QByteArray("text/event-stream");
        QTest::newRow("truncated") << QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n") << QByteArray("text/event-stream");
        QTest::newRow("length") << QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"partial\"},\"finish_reason\":\"length\"}]}\n\n") << QByteArray("text/event-stream");
        QTest::newRow("oversize") << QByteArray(2 * 1024 * 1024 + 1, 'x') << QByteArray("application/json");
    }
    void invalidAndIncompleteResponses() {
        QFETCH(QByteArray, body); QFETCH(QByteArray, type); MockApi api;
        api.respond = [body, type](QTcpSocket *socket) { MockApi::response(socket, body, type); };
        AiClient client; QSignalSpy failed(&client, &AiClient::failed); QSignalSpy completed(&client, &AiClient::completed); QString error;
        QVERIFY(client.send(apiSettings(api), question(), &error)); QTRY_COMPARE(failed.size(), 1);
        QVERIFY(completed.isEmpty()); QVERIFY(!client.busy()); QVERIFY(!failed.first().first().toString().contains(QStringLiteral("secret")));
    }
    void noRedirectsOrRawProviderErrorLeaks() {
        MockApi target; MockApi api;
        api.respond = [&target](QTcpSocket *socket) { MockApi::response(socket, "test-secret-not-real", "text/plain", 307, "Location: " + target.url().toUtf8() + "\r\n"); };
        AiClient client; QSignalSpy failed(&client, &AiClient::failed); QString error;
        QVERIFY(client.send(apiSettings(api), question(), &error)); QTRY_COMPARE(failed.size(), 1); QTest::qWait(50); QVERIFY(target.requests.isEmpty());
        QVERIFY(!failed.first().first().toString().contains(QStringLiteral("test-secret")));
        api.respond = [](QTcpSocket *socket) { MockApi::response(socket, "test-secret-not-real", "text/plain", 401); };
        QVERIFY(client.send(apiSettings(api), question(), &error)); QTRY_COMPARE(failed.size(), 2); QVERIFY(failed.last().first().toString().contains(QStringLiteral("401")));
        QVERIFY(!failed.last().first().toString().contains(QStringLiteral("test-secret")));
    }
    void cancellationAndValidationDoNotIssueExtraRequests() {
        MockApi api; AiClient client; QSignalSpy failed(&client, &AiClient::failed); QSignalSpy completed(&client, &AiClient::completed); QString error;
        auto settings = apiSettings(api); settings.apiKey += QStringLiteral("\r\nInjected: yes"); QVERIFY(!client.send(settings, question(), &error));
        QVERIFY(!client.send(apiSettings(api), question(QString(128 * 1024 + 1, u'a')), &error)); QVERIFY(api.requests.isEmpty());
        QVERIFY(client.send(apiSettings(api), question(), &error)); QVERIFY(!client.send(apiSettings(api), question(), &error)); QTRY_COMPARE(api.requests.size(), 1);
        client.cancel(); QCOMPARE(failed.size(), 1); QVERIFY(completed.isEmpty()); QVERIFY(!client.busy()); client.cancel(); QCOMPARE(failed.size(), 1);
    }
    void nativePluginTrustLifecycleAndCommands() {
        QTemporaryDir root; QWidget owner; FakeHost host; ExtensionManager manager(root.path(), &host, &owner); QString error;
        const auto info = ExtensionManager::inspect(examplePath(), &error); QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(info.id, QStringLiteral("example.selection"));
        QVERIFY(!manager.install(examplePath(), false, info.hash, &error)); QVERIFY(manager.installed().isEmpty());
        QVERIFY(!manager.install(examplePath(), true, QString(64, u'0'), &error)); QVERIFY(manager.installed().isEmpty());
        QVERIFY2(manager.install(examplePath(), true, info.hash, &error), qPrintable(error)); QVERIFY(!manager.installed().first().enabled); QVERIFY(!manager.panel(info.id));
        QVERIFY2(manager.setEnabled(info.id, true, &error), qPrintable(error)); QVERIFY(manager.panel(info.id)); QCOMPARE(manager.commands(info.id).size(), 2);
        QVERIFY(manager.execute(info.id, QStringLiteral("pane.refresh"), &error)); QCOMPARE(host.refreshed, 1);
        QVERIFY(manager.execute(info.id, QStringLiteral("selection.summary"), &error)); QVERIFY(host.status.contains(QStringLiteral("测试.txt")));
        QVERIFY(!manager.execute(info.id, QStringLiteral("does.not.exist"), &error));
        QPointer<QWidget> panel = manager.panel(info.id); QVERIFY(manager.setEnabled(info.id, false, &error)); QVERIFY(panel.isNull()); QVERIFY(manager.commands(info.id).isEmpty());
        QVERIFY(manager.setEnabled(info.id, true, &error)); QVERIFY(manager.remove(info.id, &error)); QVERIFY(manager.installed().isEmpty());
        QVERIFY(readSettings(root.filePath(QStringLiteral("index.json"))).value(QStringLiteral("plugins")).toArray().isEmpty());
    }
    void pluginStateRestoresAndTamperingIsBlocked() {
        QTemporaryDir root; QWidget owner; FakeHost host; QString error; const auto info = ExtensionManager::inspect(examplePath(), &error);
        {
            ExtensionManager manager(root.path(), &host, &owner); QVERIFY(manager.install(examplePath(), true, info.hash, &error));
        }
        {
            ExtensionManager manager(root.path(), &host, &owner); manager.restore(); QCOMPARE(manager.installed().size(), 1); QVERIFY(!manager.installed().first().enabled);
        }
        QFile file(root.filePath(info.file)); QVERIFY(file.open(QIODevice::Append)); QVERIFY(file.write("changed") > 0); file.close();
        auto state = readSettings(root.filePath(QStringLiteral("index.json"))); auto array = state.value(QStringLiteral("plugins")).toArray(); auto entry = array[0].toObject(); entry.insert(QStringLiteral("enabled"), true); array[0] = entry;
        QVERIFY(writeSettings(root.filePath(QStringLiteral("index.json")), {{QStringLiteral("plugins"), array}}));
        ExtensionManager manager(root.path(), &host, &owner); manager.restore(); QCOMPARE(manager.installed().size(), 1); QVERIFY(!manager.installed().first().enabled); QVERIFY(!manager.installed().first().error.isEmpty());
        QVERIFY(!manager.setEnabled(info.id, true, &error)); QVERIFY(!manager.panel(info.id));
    }
    void enabledPluginRestoresAndBuiltinDisablePersists() {
        QTemporaryDir root; QWidget owner; FakeHost host; QString error; const auto info = ExtensionManager::inspect(examplePath(), &error);
        {
            ExtensionManager manager(root.path(), &host, &owner); QVERIFY(manager.install(examplePath(), true, info.hash, &error));
            QVERIFY(manager.setEnabled(info.id, true, &error)); QVERIFY(manager.panel(info.id));
        }
        {
            ExtensionManager manager(root.path(), &host, &owner); manager.restore();
            QVERIFY(manager.installed().first().enabled); QVERIFY(manager.panel(info.id)); QVERIFY(manager.execute(info.id, QStringLiteral("pane.refresh"), &error)); QCOMPARE(host.refreshed, 1);
        }
        QTemporaryDir builtinRoot;
        {
            ExtensionSidebar sidebar(builtinRoot.path()); sidebar.manager()->restore(); QVERIFY(sidebar.manager()->panel(QStringLiteral("twinfile.ai"))); QVERIFY(sidebar.manager()->setEnabled(QStringLiteral("twinfile.ai"), false, &error));
        }
        ExtensionSidebar sidebar(builtinRoot.path()); sidebar.manager()->restore(); QVERIFY(!sidebar.manager()->panel(QStringLiteral("twinfile.ai"))); QVERIFY(!sidebar.manager()->installed().first().enabled);
    }
    void incompatibleAndUnregisteredPluginsNeverLoad() {
        QTemporaryDir root; QWidget owner; FakeHost host; QString error;
        const QString incompatible = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("twinfile_incompatible_plugin.dll"));
        QVERIFY(ExtensionManager::inspect(incompatible, &error).id.isEmpty()); QVERIFY(!error.isEmpty());
        QVERIFY(QFile::copy(examplePath(), root.filePath(QStringLiteral("unregistered.dll"))));
        const QJsonArray malicious{QJsonObject{{QStringLiteral("id"), QStringLiteral("example.selection")}, {QStringLiteral("file"), QStringLiteral("../unregistered.dll")}, {QStringLiteral("sha256"), QString(64, u'a')}, {QStringLiteral("enabled"), true}}};
        QVERIFY(writeSettings(root.filePath(QStringLiteral("index.json")), {{QStringLiteral("plugins"), malicious}}));
        ExtensionManager manager(root.path(), &host, &owner); manager.restore(); QVERIFY(manager.installed().isEmpty());
        ExtensionInfo builtin; builtin.id = QStringLiteral("builtin.failing"); builtin.name = QStringLiteral("Failing"); manager.addBuiltin(new FailingBuiltin, builtin);
        QVERIFY(!manager.setEnabled(QStringLiteral("builtin.failing"), true, &error)); QVERIFY(error.contains(QStringLiteral("fixture failure"))); QVERIFY(!manager.panel(QStringLiteral("builtin.failing")));
    }
    void panelOnlySendsExplicitInputAndCancelsCleanly() {
        MockApi api; FakeHost host; auto settings = apiSettings(api); AiPanel panel(&settings, &host);
        auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("aiInput")); auto *send = panel.findChild<QPushButton *>(QStringLiteral("aiSend")); QVERIFY(input); QVERIFY(send);
        QVERIFY(api.requests.isEmpty()); input->setPlainText(QStringLiteral("仅发送此问题")); send->click(); QTRY_COMPARE(api.requests.size(), 1); QVERIFY(panel.busy());
        QVERIFY(!QJsonDocument(api.bodies.first()).toJson().contains("private"));
        send->click(); QVERIFY(!panel.busy()); QCOMPARE(input->toPlainText(), QStringLiteral("仅发送此问题"));
        api.respond = [](QTcpSocket *socket) { MockApi::response(socket, jsonAnswer(QStringLiteral("<img src=\"file:///C:/private\"> 不应被当作 HTML"))); };
        send->click(); QTRY_VERIFY(!panel.busy()); QCOMPARE(api.requests.size(), 2);
        QCOMPARE(api.bodies.last().value(QStringLiteral("messages")).toArray().size(), 2);
        auto *transcript = panel.findChild<QTextBrowser *>(QStringLiteral("aiTranscript")); QVERIFY(transcript->toPlainText().contains(QStringLiteral("<img src=")));
        panel.clearConversation(); QVERIFY(!transcript->toPlainText().contains(QStringLiteral("不应被当作 HTML")));
    }
    void attachmentsRequirePreviewAndExcludePrivatePaths() {
        MockApi api; api.respond = [](QTcpSocket *socket) { MockApi::response(socket, jsonAnswer(QStringLiteral("已收到"))); };
        FakeHost host; auto settings = apiSettings(api); AiPanel panel(&settings, &host);
        auto *attach = panel.findChild<QAction *>(QStringLiteral("aiAttachSelection"));
        QVERIFY(attach);
        bool previewed = false;
        QTimer::singleShot(0, &panel, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto *preview = dialog->findChild<QPlainTextEdit *>();
            previewed = preview && preview->toPlainText().contains(QStringLiteral("测试.txt")) && !preview->toPlainText().contains(QStringLiteral("C:/private"));
            dialog->accept();
        });
        attach->trigger(); QVERIFY(previewed); QVERIFY(api.requests.isEmpty());
        auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("aiInput")); auto *send = panel.findChild<QPushButton *>(QStringLiteral("aiSend"));
        input->setPlainText(QStringLiteral("说明这些文件")); send->click(); QTRY_VERIFY(!panel.busy()); QCOMPARE(api.requests.size(), 1);
        const QByteArray first = QJsonDocument(api.bodies.first()).toJson(); QVERIFY(first.contains(QStringLiteral("测试.txt").toUtf8())); QVERIFY(!first.contains("private"));
        host.context = {{QStringLiteral("selected"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("未附加的秘密.txt")}}}}};
        input->setPlainText(QStringLiteral("继续说明")); send->click(); QTRY_VERIFY(!panel.busy()); QCOMPARE(api.requests.size(), 2);
        const QByteArray second = QJsonDocument(api.bodies.last()).toJson(); QVERIFY(second.contains(QStringLiteral("测试.txt").toUtf8())); QVERIFY(!second.contains(QStringLiteral("未附加的秘密").toUtf8()));
        QCOMPARE(api.bodies.last().value(QStringLiteral("messages")).toArray().size(), 4);
    }
    void sidebarRendersAndDoesNotStealFileShortcuts() {
        const QString output = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        QTemporaryDir workspace; QVERIFY(QDir().mkpath(workspace.filePath(QStringLiteral("示例文件"))));
        QFile fixture(workspace.filePath(QStringLiteral("示例文件/项目说明.txt"))); QVERIFY(fixture.open(QIODevice::WriteOnly)); fixture.write("TwinFile example"); fixture.close();
        for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("mt"), QStringLiteral("cream"), QStringLiteral("bataUI01")}) {
            auto state = readSettings(settingsPath()); state.insert(QStringLiteral("extension_visible"), true); QVERIFY(writeSettings(settingsPath(), state));
            Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.setTheme(theme); window.show(); QTest::qWait(80);
            auto *dock = window.findChild<QDockWidget *>(QStringLiteral("extensionDock")); QVERIFY(dock); QVERIFY(dock->isVisible());
            window.leftPane()->navigate({Source::Local, workspace.filePath(QStringLiteral("示例文件")), {}, {}}); window.rightPane()->navigate({Source::Local, workspace.filePath(QStringLiteral("示例文件")), {}, {}});
            QTRY_COMPARE(window.leftPane()->fileModel()->rowCount(), 1); QTRY_COMPARE(window.rightPane()->fileModel()->rowCount(), 1);
            auto *sidebar = window.findChild<ExtensionSidebar *>(); QVERIFY(sidebar); auto *input = sidebar->findChild<QPlainTextEdit *>(QStringLiteral("aiInput")); QVERIFY(input);
            QCOMPARE(sidebar->findChild<QTabWidget *>(QStringLiteral("extensionTabs"))->currentWidget(), sidebar->manager()->panel(QStringLiteral("twinfile.ai")));
            const Location before = window.activePane()->location(); input->setFocus(); input->setPlainText(QStringLiteral("abc")); input->moveCursor(QTextCursor::End); QTest::keyClick(input, Qt::Key_Backspace);
            QCOMPARE(input->toPlainText(), QStringLiteral("ab")); QCOMPARE(window.activePane()->location(), before);
            input->clear();
            QVERIFY(dock->width() >= 310); QVERIFY(dock->width() <= 700);
            if (!output.isEmpty()) {
                window.leftPane()->view()->stopTransition(); window.rightPane()->view()->stopTransition();
                QVERIFY(QDir().mkpath(output)); QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("native-ai-%1.png").arg(theme))));
                sidebar->showExtensions(); QTest::qWait(30); QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("native-extensions-%1.png").arg(theme))));
            }
            QString error; QVERIFY(sidebar->manager()->setEnabled(QStringLiteral("twinfile.ai"), false, &error)); QVERIFY(!sidebar->findChild<AiPanel *>());
            QVERIFY(sidebar->manager()->setEnabled(QStringLiteral("twinfile.ai"), true, &error)); QVERIFY(sidebar->findChild<AiPanel *>());
            auto *close = sidebar->findChild<QPushButton *>(QStringLiteral("extensionClose")); QVERIFY(close); QVERIFY(close->isVisible()); QVERIFY(sidebar->rect().contains(QRect(close->mapTo(sidebar, QPoint(0, 0)), close->size()))); close->click(); QVERIFY(!dock->isVisible()); dock->show(); QVERIFY(dock->isVisible()); window.close();
        }
    }
    void codexComposerRendersAllModes_data() {
        QTest::addColumn<QString>("theme"); QTest::addColumn<bool>("dark");
        for (const auto &theme : themeIds()) for (bool dark : {false, true}) QTest::newRow(qPrintable(theme + (dark ? QStringLiteral("-dark") : QStringLiteral("-light")))) << theme << dark;
    }
    void codexComposerRendersAllModes() {
        QFETCH(QString, theme); QFETCH(bool, dark); FakeHost host; AiSettings settings; settings.endpoint = QStringLiteral("https://example.com/v1"); settings.model = QStringLiteral("a-very-long-user-model-name-for-narrow-sidebar");
        const QPalette previousPalette = qApp->palette(); const QString previousSheet = qApp->styleSheet();
        const auto restoreAppearance = qScopeGuard([&] { qApp->setPalette(previousPalette); qApp->setStyleSheet(previousSheet); });
        const QPalette palette = appearancePalette(theme, dark, {}); qApp->setPalette(palette); qApp->setStyleSheet(appearanceStyleSheet(theme, dark, palette));
        AiPanel panel(&settings, &host); panel.setAttribute(Qt::WA_DontShowOnScreen); panel.resize(420, 660); panel.show(); QTest::qWait(20);
        auto *composer = panel.findChild<QFrame *>(QStringLiteral("aiComposer")); auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("aiInput")); auto *mode = panel.findChild<QComboBox *>(QStringLiteral("aiMode")); auto *send = panel.findChild<QPushButton *>(QStringLiteral("aiSend")); auto *transcript = panel.findChild<QTextBrowser *>(QStringLiteral("aiTranscript")); auto *scopeRow = panel.findChild<QWidget *>(QStringLiteral("aiScopeRow"));
        QVERIFY(composer); QVERIFY(input); QVERIFY(mode); QVERIFY(send); QVERIFY(transcript); QVERIFY(scopeRow);
        QCOMPARE(input->parentWidget(), composer); QCOMPARE(mode->parentWidget(), composer); QCOMPARE(send->parentWidget(), composer); QVERIFY(send->text().isEmpty()); QVERIFY(!send->icon().isNull()); QVERIFY(!send->accessibleName().isEmpty());
        QVERIFY(transcript->toPlainText().trimmed().isEmpty()); QVERIFY(!scopeRow->isVisible()); QVERIFY(!panel.findChild<QWidget *>(QStringLiteral("aiAttachmentRow"))->isVisible());
        QVERIFY(input->height() <= 80); QVERIFY(transcript->height() > panel.height() / 2); QVERIFY(composer->geometry().bottom() < panel.height());
        const QString output = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        if (!output.isEmpty()) QVERIFY(panel.grab().save(QDir(output).filePath(QStringLiteral("native-codex-%1-%2.png").arg(theme, dark ? QStringLiteral("dark") : QStringLiteral("light")))));
        mode->setCurrentIndex(1); QVERIFY(scopeRow->isVisible()); QVERIFY(input->placeholderText().size() < 15);
        if (!output.isEmpty() && theme == QStringLiteral("macos")) QVERIFY(panel.grab().save(QDir(output).filePath(QStringLiteral("native-codex-agent-%1.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light")))));
        panel.resize(310, 540); input->setPlainText(QStringLiteral("多行输入\n第二行\n第三行\n第四行\n第五行\n第六行\n第七行")); QTest::qWait(20);
        QVERIFY(input->height() > 80); QVERIFY(input->height() <= 144); QVERIFY(panel.rect().contains(composer->geometry())); QVERIFY(composer->rect().contains(send->geometry())); QVERIFY(mode->geometry().right() < send->geometry().left());
        input->clear(); QTRY_VERIFY(input->height() <= 80); QVERIFY(panel.findChild<QMenu *>(QStringLiteral("aiAttachmentMenu"))); QVERIFY(panel.findChild<QMenu *>(QStringLiteral("aiMoreMenu"))); QVERIFY(panel.findChild<QLabel *>(QStringLiteral("aiPrivacy"))->text().size() < 25);
    }
    void codexComposerMenusKeyboardAndStop() {
        MockApi api; FakeHost host; auto settings = apiSettings(api); AiPanel panel(&settings, &host); panel.setAttribute(Qt::WA_DontShowOnScreen); panel.resize(370, 620); panel.show();
        auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("aiInput")); auto *send = panel.findChild<QPushButton *>(QStringLiteral("aiSend")); auto *fresh = panel.findChild<QPushButton *>(QStringLiteral("aiNewConversation")); auto *attach = panel.findChild<QAction *>(QStringLiteral("aiAttachSelection")); auto *copy = panel.findChild<QAction *>(QStringLiteral("aiCopy"));
        QVERIFY(input); QVERIFY(send); QVERIFY(fresh); QVERIFY(attach); QVERIFY(copy);
        QApplication::setActiveWindow(&panel); input->setFocus(); QTRY_VERIFY(input->hasFocus()); input->setPlainText(QStringLiteral("测试输入")); QTest::keyClick(input, Qt::Key_Return); QVERIFY(api.requests.isEmpty()); QVERIFY(input->toPlainText().contains(u'\n'));
        QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier); QTRY_COMPARE(api.requests.size(), 1); QVERIFY(panel.busy()); QVERIFY(!fresh->isEnabled()); QVERIFY(!attach->isEnabled()); QVERIFY(!copy->isEnabled()); QCOMPARE(send->toolTip(), QStringLiteral("停止"));
        send->click(); QVERIFY(!panel.busy()); QVERIFY(fresh->isEnabled()); QVERIFY(attach->isEnabled()); QVERIFY(!input->toPlainText().isEmpty()); fresh->click(); QVERIFY(input->toPlainText().isEmpty()); QVERIFY(panel.findChild<QTextBrowser *>(QStringLiteral("aiTranscript"))->toPlainText().trimmed().isEmpty());
        api.respond = [](QTcpSocket *socket) { MockApi::response(socket, jsonAnswer(QStringLiteral("测试回答 <b>保持文字</b>"))); };
        input->setPlainText(QStringLiteral("简短回答")); send->click(); QTRY_VERIFY(!panel.busy()); copy->trigger(); QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("测试回答 <b>保持文字</b>"));
        QVERIFY(panel.findChild<QTextBrowser *>(QStringLiteral("aiTranscript"))->toPlainText().contains(QStringLiteral("<b>保持文字</b>")));
    }
};

QTEST_MAIN(ExtensionTests)
#include "extension_tests.moc"
