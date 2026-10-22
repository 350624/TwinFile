#include "ai.h"
#include "ai_panel.h"
#include "extensions.h"
#include "window.h"
#include "appearance.h"
#include "updates.h"

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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QTest>
#include <QTextBrowser>
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
            QCOMPARE(window.leftPane()->view()->sizeHintForRow(0), 54);
            QCOMPARE(window.leftPane()->view()->palette().color(QPalette::Text), qApp->palette().color(QPalette::Text));
            if (!output.isEmpty()) { QVERIFY(QDir().mkpath(output)); QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("native-appearance-%1-%2.png").arg(theme, mode)))); }
        }
        bool checked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            QTimer::singleShot(8000, dialog, &QDialog::reject);
            auto *themes = dialog->findChild<QComboBox *>(QStringLiteral("appearanceTheme"));
            QVERIFY(themes); QCOMPARE(themes->count(), 5); QVERIFY(themes->findData(QStringLiteral("dark")) < 0); QVERIFY(themes->findData(QStringLiteral("light")) < 0);
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
