#include "mcp.h"
#include "appearance.h"
#include "android.h"
#include "dex.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <windows.h>

using namespace twinfile;

static bool fixture(const QString &path, const QByteArray &bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
static QByteArray contents(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
static QString digest(const QString &path) { return QString::fromLatin1(QCryptographicHash::hash(contents(path), QCryptographicHash::Sha256).toHex()); }
static McpSettings settingsFor(const QString &home) { McpSettings settings; settings.home = home; settings.tokenHash = QCryptographicHash::hash("test-token", QCryptographicHash::Sha256); return settings; }
static QJsonObject rpc(const QString &method, const QJsonObject &params = {}, int id = 1) { return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}; }

class McpTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir configuration_;
    QByteArray previousSettings_;
    QNetworkAccessManager network_;
    QNetworkReply *post(McpServer &server, const QJsonObject &message, const QByteArray &token = "test-token", const QByteArray &origin = {}, const QByteArray &version = "2025-06-18") {
        QNetworkRequest request{QUrl(server.endpoint())};
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("Authorization", "Bearer " + token);
        request.setRawHeader("MCP-Protocol-Version", version);
        if (!origin.isEmpty()) request.setRawHeader("Origin", origin);
        return network_.post(request, QJsonDocument(message).toJson(QJsonDocument::Compact));
    }
    bool start(McpServer &server, McpSettings settings) {
        QTcpServer port; if (!port.listen(QHostAddress::LocalHost, 0)) return false;
        settings.port = port.serverPort(); port.close(); QString error; return server.start(settings, &error);
    }
private slots:
    void initTestCase() {
        QVERIFY(configuration_.isValid());
        previousSettings_ = qgetenv("TWINFILE_SETTINGS");
        qputenv("TWINFILE_SETTINGS", configuration_.filePath("settings.json").toUtf8());
        qApp->setStyle("Fusion"); qputenv("TWINFILE_TEST_MODE", "1");
    }
    void cleanupTestCase() {
        if (previousSettings_.isEmpty()) qunsetenv("TWINFILE_SETTINGS"); else qputenv("TWINFILE_SETTINGS", previousSettings_);
    }
    void settingsAndTools() {
        QTemporaryDir root; auto settings = settingsFor(root.path()); QString error;
        QVERIFY(McpTools::validate(settings, &error));
        QCOMPARE(McpSettings::fromJson(settings.json()).json(), settings.json());
        const auto token = McpSettings::newToken(); QCOMPARE(token.size(), 64); QVERIFY(token != McpSettings::newToken());
        QVERIFY(!settings.json().contains("token"));
        QCOMPARE(McpTools::definitions(settings).size(), 23);
        settings.phoneSerial = "test-device"; settings.phoneRoot = "/storage/emulated/0/Download";
        QVERIFY(McpTools::validate(settings, &error)); QCOMPARE(McpTools::definitions(settings).size(), 25);
        settings.phoneWrite = true; QCOMPARE(McpTools::definitions(settings).size(), 26);
        QVERIFY(!McpTools::execute(settings, "run_command", {{"command", "whoami"}}, {}).value("ok").toBool());
        settings.phoneRoot = "/"; QVERIFY(!McpTools::validate(settings, &error));
        settings = settingsFor(root.path()); settings.tokenHash.clear(); QVERIFY(!McpTools::validate(settings, &error));
    }
    void pathPermissionsAndLinks() {
        QTemporaryDir root, outside; auto settings = settingsFor(root.path()); QString error;
        QVERIFY(fixture(root.filePath("a.txt"), "abc")); QVERIFY(fixture(outside.filePath("b.txt"), "def"));
        QVERIFY(!McpTools::resolve(settings, "a.txt", false, false, &error).isEmpty());
        for (const auto &path : QStringList{"../outside", "a.txt:ads", "NUL", "//server/share", "missing/child", outside.filePath("b.txt")}) QVERIFY2(McpTools::resolve(settings, path, false, false, &error).isEmpty(), qPrintable(path));
        settings.rules = {{outside.path(), "read"}};
        QVERIFY(!McpTools::resolve(settings, outside.filePath("b.txt"), false, false, &error).isEmpty());
        QVERIFY(McpTools::resolve(settings, outside.filePath("new.txt"), true, true, &error).isEmpty());
        settings.rules.prepend({outside.path(), "deny"}); QVERIFY(McpTools::resolve(settings, outside.filePath("b.txt"), false, false, &error).isEmpty());
        settings.rules = {{outside.path(), "write"}}; QVERIFY(!McpTools::resolve(settings, outside.filePath("new.txt"), true, true, &error).isEmpty());
        const QString link = QDir::toNativeSeparators(root.filePath("hard.txt")), source = QDir::toNativeSeparators(root.filePath("a.txt"));
        QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(link.utf16()), reinterpret_cast<LPCWSTR>(source.utf16()), nullptr));
        QVERIFY(McpTools::resolve(settings, "hard.txt", false, false, &error).isEmpty());
        QVERIFY(McpTools::resolve(settings, "a.txt", true, false, &error).isEmpty());
        QVERIFY(QFile::remove(link));
        const QString junction = root.filePath("junction");
        QVERIFY(runProcess("cmd.exe", {"/c", "mklink", "/J", QDir::toNativeSeparators(junction), QDir::toNativeSeparators(outside.path())}).ok);
        QVERIFY(McpTools::resolve(settings, "junction/b.txt", false, false, &error).isEmpty());
        QVERIFY(QDir().rmdir(junction));
    }
    void filesRoundTripAndConflicts() {
        QTemporaryDir root; const auto settings = settingsFor(root.path());
        auto execute = [&](const QString &name, const QJsonObject &args) { return McpTools::execute(settings, name, args, {}); };
        QJsonObject write{{"path", "a.txt"}, {"content", "hello world"}, {"encoding", "utf8"}, {"expected_sha256", "new"}};
        QVERIFY(execute("write_file", write).value("ok").toBool());
        QVERIFY(!execute("write_file", write).value("ok").toBool());
        const auto read = execute("read_file", {{"path", "a.txt"}, {"offset", "0"}, {"encoding", "utf8"}}); QCOMPARE(read.value("content").toString(), QString("hello world"));
        QVERIFY(execute("patch_file", {{"path", "a.txt"}, {"offset", "6"}, {"content", "UVQ="}, {"expected_sha256", digest(root.filePath("a.txt"))}}).value("ok").toBool());
        QCOMPARE(contents(root.filePath("a.txt")), QByteArray("hello QTrld"));
        write.insert("expected_sha256", digest(root.filePath("a.txt"))); write.insert("content", "updated");
        QVERIFY(execute("write_file", write).value("ok").toBool()); QCOMPARE(contents(root.filePath("a.txt")), QByteArray("updated"));
        QVERIFY(!execute("write_file", write).value("ok").toBool());
        QVERIFY(execute("copy_file", {{"path", "a.txt"}, {"destination", "b.txt"}}).value("ok").toBool());
        QVERIFY(!execute("copy_file", {{"path", "a.txt"}, {"destination", "b.txt"}}).value("ok").toBool());
        QVERIFY(execute("move_file", {{"path", "b.txt"}, {"destination", "c.txt"}, {"expected_sha256", digest(root.filePath("b.txt"))}}).value("ok").toBool());
        QVERIFY(!execute("delete_file", {{"path", "c.txt"}, {"expected_sha256", "bad"}}).value("ok").toBool());
        QVERIFY(execute("delete_file", {{"path", "c.txt"}, {"expected_sha256", digest(root.filePath("c.txt"))}}).value("ok").toBool());
        QVERIFY(execute("create_directory", {{"path", "folder"}}).value("ok").toBool());
        QVERIFY(!execute("delete_file", {{"path", "folder"}, {"expected_sha256", "new"}}).value("ok").toBool());
        QVERIFY(!execute("read_file", {{"path", "a.txt"}, {"offset", "0"}, {"encoding", "utf8"}, {"extra", "bad"}}).value("ok").toBool());
        const auto list = execute("list_directory", {{"path", "."}, {"offset", "0"}}); QCOMPARE(list.value("entries").toArray().size(), 2);
        const auto search = execute("search_files", {{"path", "."}, {"query", "updated"}, {"mode", "content"}, {"min_size", "0"}, {"max_size", "100"}, {"after", ""}}); QCOMPARE(search.value("entries").toArray().size(), 1);
        auto cancel = std::make_shared<std::atomic_bool>(true);
        QVERIFY(!McpTools::execute(settings, "write_file", write, cancel).value("ok").toBool()); QCOMPARE(contents(root.filePath("a.txt")), QByteArray("updated"));
    }
    void zipRoundTrip() {
        QTemporaryDir root; const auto settings = settingsFor(root.path()); QVERIFY(fixture(root.filePath("a.txt"), "zip text"));
        auto execute = [&](const QString &name, const QJsonObject &args) { return McpTools::execute(settings, name, args, {}); };
        QVERIFY(execute("zip_create", {{"path", "test.zip"}, {"files", "[\"a.txt\"]"}}).value("ok").toBool());
        QCOMPARE(execute("zip_list", {{"path", "test.zip"}, {"offset", "0"}}).value("total").toInt(), 1);
        QVERIFY(execute("zip_check", {{"path", "test.zip"}}).value("ok").toBool());
        QCOMPARE(execute("zip_read", {{"path", "test.zip"}, {"entry", "a.txt"}, {"encoding", "utf8"}}).value("content").toString(), QString("zip text"));
        QVERIFY(!execute("zip_read", {{"path", "test.zip"}, {"entry", "missing"}, {"encoding", "utf8"}}).value("ok").toBool());
        const auto renamed = execute("zip_update", {{"path", "test.zip"}, {"operation", "rename"}, {"entry", "a.txt"}, {"value", "renamed.txt"}, {"expected_sha256", digest(root.filePath("test.zip"))}});
        QVERIFY2(renamed.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(renamed).toJson())));
        QVERIFY(execute("zip_extract", {{"path", "test.zip"}, {"entry", "renamed.txt"}, {"destination", "extracted.txt"}}).value("ok").toBool()); QCOMPARE(contents(root.filePath("extracted.txt")), QByteArray("zip text"));
        QVERIFY(!execute("zip_extract", {{"path", "test.zip"}, {"entry", "renamed.txt"}, {"destination", "a.txt"}}).value("ok").toBool());
        QVERIFY(!execute("dex_decode", {{"path", "a.txt"}, {"destination", "smali"}}).value("ok").toBool()); QVERIFY(!QFileInfo::exists(root.filePath("smali")));
    }
    void zipUpdateRejectsExpandedLimitWithoutReplacingOriginal() {
        QTemporaryDir root; const auto settings = settingsFor(root.path());
        const QString source = root.filePath("large.bin"), archive = root.filePath("bounded.zip");
        {
            QFile output(source); QVERIFY(output.open(QIODevice::WriteOnly));
            const QByteArray block(65536, '\0');
            for (int index = 0; index < 1040; ++index) QCOMPARE(output.write(block), qint64(block.size()));
        }
        auto execute = [&](const QString &name, const QJsonObject &arguments) { return McpTools::execute(settings, name, arguments, {}); };
        QVERIFY(execute("zip_create", {{"path", "bounded.zip"}, {"files", "[\"large.bin\"]"}}).value("ok").toBool());
        const QByteArray original = contents(archive);
        const auto result = execute("zip_update", {{"path", "bounded.zip"}, {"operation", "put"}, {"entry", "second.bin"}, {"value", "large.bin"}, {"expected_sha256", digest(archive)}});
        QVERIFY(!result.value("ok").toBool()); QVERIFY(result.value("error").toString().contains("128 MiB"));
        QCOMPARE(contents(archive), original);
        QVERIFY(execute("zip_check", {{"path", "bounded.zip"}}).value("ok").toBool());
    }
    void httpLifecycleAndTools() {
        QTemporaryDir root; McpServer server; QVERIFY(start(server, settingsFor(root.path())));
        auto *reply = post(server, rpc("initialize", {{"protocolVersion", "2025-06-18"}, {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}}));
        QSignalSpy initialized(reply, &QNetworkReply::finished); QVERIFY(initialized.wait()); QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        auto result = QJsonDocument::fromJson(reply->readAll()).object().value("result").toObject(); QCOMPARE(result.value("protocolVersion").toString(), QString("2025-06-18"));
        reply = post(server, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}); QSignalSpy notified(reply, &QNetworkReply::finished); QVERIFY(notified.wait()); QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 202);
        reply = post(server, rpc("tools/list")); QSignalSpy listed(reply, &QNetworkReply::finished); QVERIFY(listed.wait()); QCOMPARE(QJsonDocument::fromJson(reply->readAll()).object().value("result").toObject().value("tools").toArray().size(), 23);
        reply = post(server, rpc("tools/call", {{"name", "write_file"}, {"arguments", QJsonObject{{"path", "via-http.txt"}, {"content", "hello MCP"}, {"encoding", "utf8"}, {"expected_sha256", "new"}}}}));
        QSignalSpy called(reply, &QNetworkReply::finished); QVERIFY(called.wait()); result = QJsonDocument::fromJson(reply->readAll()).object().value("result").toObject(); QVERIFY(!result.value("isError").toBool()); QCOMPARE(contents(root.filePath("via-http.txt")), QByteArray("hello MCP"));
        QTRY_VERIFY(!server.busy()); QCOMPARE(server.history().size(), 1); server.stop(); QVERIFY(!server.running()); QVERIFY(start(server, settingsFor(root.path()))); server.stop();
    }
    void apkManifestAndDexRoundTrip() {
        QTemporaryDir root; const auto settings = settingsFor(root.path());
        QVERIFY(QFile::copy(toolPath("hfx/HybridFileXfer-phone.apk"), root.filePath("source.apk")));
        auto execute = [&](const QString &name, const QJsonObject &arguments) {
            const auto result = McpTools::execute(settings, name, arguments, {});
            if (!result.value("ok").toBool()) qWarning().noquote() << QJsonDocument(result).toJson(QJsonDocument::Compact);
            return result;
        };
        const QByteArray original = contents(root.filePath("source.apk"));
        QVERIFY(execute("apk_info", {{"path", "source.apk"}}).value("ok").toBool());
        QVERIFY(execute("apk_read_xml", {{"path", "source.apk"}, {"entry", "AndroidManifest.xml"}}).value("ok").toBool());
        QVERIFY(execute("apk_edit_manifest", {{"path", "source.apk"}, {"destination", "edited.apk"}, {"fields", "{\"versionName\":\"MCP-test\"}"}}).value("ok").toBool());
        QVERIFY(execute("apk_info", {{"path", "edited.apk"}}).value("output").toString().contains("MCP-test"));
        QVERIFY(execute("apk_sign", {{"path", "source.apk"}, {"destination", "signed.apk"}}).value("ok").toBool());
        QVERIFY(runProcess(toolPath("runtime/bin/java.exe"), {"-jar", toolPath("apksigner.jar"), "verify", root.filePath("signed.apk")}, 30000).ok);
        QVERIFY(execute("zip_extract", {{"path", "source.apk"}, {"entry", "classes.dex"}, {"destination", "classes.dex"}}).value("ok").toBool());
        QVERIFY(execute("dex_decode", {{"path", "classes.dex"}, {"destination", "smali"}}).value("ok").toBool());
        const auto classes = QDir(root.filePath("smali")).entryList({"*.smali"}, QDir::Files); QVERIFY(classes.size() > 10);
        QVERIFY(execute("dex_build", {{"path", "classes.dex"}, {"workspace", "smali"}, {"destination", "updated.dex"}}).value("ok").toBool());
        QVERIFY(validateDex(root.filePath("updated.dex")).ok);
        QVERIFY(execute("dex_decode", {{"path", "updated.dex"}, {"destination", "checked"}}).value("ok").toBool());
        QCOMPARE(QDir(root.filePath("checked")).entryList({"*.smali"}, QDir::Files).size(), classes.size());
        QCOMPARE(contents(root.filePath("source.apk")), original);
        QVERIFY(!execute("apk_sign", {{"path", "source.apk"}, {"destination", "signed.apk"}}).value("ok").toBool());
    }
    void readOnlyInputsCannotBeModified() {
        QTemporaryDir root, outside; auto settings = settingsFor(root.path()); settings.rules = {{outside.path(), "read"}};
        const QString source = outside.filePath("private.txt"); QVERIFY(fixture(source, "private data"));
        auto execute = [&](const QString &name, const QJsonObject &arguments) { return McpTools::execute(settings, name, arguments, {}); };
        QVERIFY(execute("copy_file", {{"path", source}, {"destination", "copy.txt"}}).value("ok").toBool());
        QVERIFY(!execute("copy_file", {{"path", "copy.txt"}, {"destination", outside.filePath("new.txt")}}).value("ok").toBool());
        QVERIFY(!execute("write_file", {{"path", source}, {"content", "modified"}, {"encoding", "utf8"}, {"expected_sha256", digest(source)}}).value("ok").toBool());
        QVERIFY(!execute("move_file", {{"path", source}, {"destination", "moved.txt"}, {"expected_sha256", digest(source)}}).value("ok").toBool());
        QVERIFY(!execute("delete_file", {{"path", source}, {"expected_sha256", digest(source)}}).value("ok").toBool());
        QCOMPARE(contents(source), QByteArray("private data"));
        QVERIFY(execute("zip_create", {{"path", "archive.zip"}, {"files", QString::fromUtf8(QJsonDocument(QJsonArray{source}).toJson(QJsonDocument::Compact))}}).value("ok").toBool());
        QVERIFY(!execute("zip_update", {{"path", "archive.zip"}, {"operation", "rename"}, {"entry", "private.txt"}, {"value", "../escaped.txt"}, {"expected_sha256", digest(root.filePath("archive.zip"))}}).value("ok").toBool());
        QVERIFY(!execute("zip_extract", {{"path", "archive.zip"}, {"entry", "private.txt"}, {"destination", outside.filePath("extracted.txt")}}).value("ok").toBool());
    }
    void httpRejectsUntrustedRequests_data() {
        QTest::addColumn<QByteArray>("token"); QTest::addColumn<QByteArray>("origin"); QTest::addColumn<QByteArray>("version"); QTest::addColumn<int>("status");
        QTest::newRow("wrong-token") << QByteArray("wrong") << QByteArray() << QByteArray("2025-06-18") << 401;
        QTest::newRow("evil-origin") << QByteArray("test-token") << QByteArray("https://evil.example") << QByteArray("2025-06-18") << 403;
        QTest::newRow("unsupported-version") << QByteArray("test-token") << QByteArray() << QByteArray("bad") << 400;
    }
    void stopDuringToolBlocksRestartUntilCleanup() {
        QTemporaryDir root; McpServer server; const auto settings = settingsFor(root.path()); QVERIFY(start(server, settings));
        bool interrupted = false, restartRejected = false;
        QSignalSpy changes(&server, &McpServer::filesChanged);
        connect(&server, &McpServer::stateChanged, this, [&] {
            if (!server.busy() || interrupted) return;
            interrupted = true; server.stop(); QString error; restartRejected = !server.start(settings, &error);
        });
        auto *reply = post(server, rpc("tools/call", {{"name", "create_directory"}, {"arguments", QJsonObject{{"path", "cancelled"}}}}));
        QSignalSpy finished(reply, &QNetworkReply::finished); QVERIFY(finished.wait());
        QVERIFY(interrupted); QVERIFY(restartRejected); QVERIFY(!server.running());
        QTRY_VERIFY(!server.busy()); QCOMPARE(changes.size(), 1); QCOMPARE(server.history().size(), 1);
        QVERIFY(start(server, settings));
        reply = post(server, rpc("tools/list")); QSignalSpy listed(reply, &QNetworkReply::finished); QVERIFY(listed.wait());
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200); server.stop();
    }
    void httpRejectsUntrustedRequests() {
        QFETCH(QByteArray, token); QFETCH(QByteArray, origin); QFETCH(QByteArray, version); QFETCH(int, status);
        QTemporaryDir root; McpServer server; QVERIFY(start(server, settingsFor(root.path())));
        auto *reply = post(server, rpc("tools/list"), token, origin, version); QSignalSpy finished(reply, &QNetworkReply::finished); QVERIFY(finished.wait()); QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), status);
    }
    void httpFramingAndMethods() {
        QTemporaryDir root; McpServer server; QVERIFY(start(server, settingsFor(root.path())));
        const QUrl url(server.endpoint());
        auto request = [&](const QByteArray &headers, const QByteArray &body) {
            QTcpSocket socket; socket.connectToHost(url.host(), url.port()); if (!socket.waitForConnected(1000)) return QByteArray();
            socket.write(headers + "\r\n\r\n" + body); socket.flush();
            QSignalSpy disconnected(&socket, &QTcpSocket::disconnected); if (!disconnected.wait(3000)) return QByteArray(); return socket.readAll();
        };
        const QByteArray base = "\r\nHost: 127.0.0.1:" + QByteArray::number(url.port()) + "\r\nAuthorization: Bearer test-token";
        QVERIFY(request("GET /mcp HTTP/1.1" + base, {}).startsWith("HTTP/1.1 405"));
        QVERIFY(request("POST /mcp HTTP/1.1" + base + "\r\nContent-Type: application/json\r\nContent-Length: 2", "{}").startsWith("HTTP/1.1 406"));
        QVERIFY(request("POST /mcp HTTP/1.1" + base + "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\nContent-Length: 9999999", {}).startsWith("HTTP/1.1 413"));
        QVERIFY(request("GET /mcp HTTP/1.1\r\nHost: evil.example\r\nAuthorization: Bearer test-token", {}).startsWith("HTTP/1.1 403"));
        auto *reply = post(server, rpc("tools/call", {{"name", "run_command"}, {"arguments", QJsonObject{}}})); QSignalSpy finished(reply, &QNetworkReply::finished); QVERIFY(finished.wait()); QCOMPARE(QJsonDocument::fromJson(reply->readAll()).object().value("error").toObject().value("code").toInt(), -32602);
    }
    void settingsDialogRendersAndStarts_data() {
        QTest::addColumn<QString>("theme"); QTest::addColumn<bool>("dark");
        for (const auto &theme : themeIds()) for (const bool dark : {false, true}) {
            const QByteArray name = (theme + (dark ? "-dark" : "-light")).toUtf8();
            QTest::newRow(name.constData()) << theme << dark;
        }
    }
    void settingsDialogRendersAndStarts() {
        QFETCH(QString, theme); QFETCH(bool, dark);
        const auto previousPalette = qApp->palette(); const auto previousSheet = qApp->styleSheet();
        const auto restoreAppearance = qScopeGuard([&] { qApp->setPalette(previousPalette); qApp->setStyleSheet(previousSheet); });
        const auto palette = appearancePalette(theme, dark, {});
        qApp->setPalette(palette); qApp->setStyleSheet(appearanceStyleSheet(theme, dark, palette));
        QTemporaryDir root; McpServer server; auto settings = settingsFor(root.path());
        QTcpServer available; QVERIFY(available.listen(QHostAddress::LocalHost, 0)); settings.port = available.serverPort(); available.close();
        bool visited = false, saved = false, started = false;
        QTimer timer; timer.setInterval(30);
        connect(&timer, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            if (auto *warning = qobject_cast<QMessageBox *>(dialog)) { warning->button(QMessageBox::Yes)->click(); return; }
            if (dialog->objectName() != "mcpDialog") return;
            if (!visited) {
                visited = true; const QString screenshot = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
                if (!screenshot.isEmpty()) dialog->grab().save(QDir(screenshot).filePath("native-mcp-settings-" + theme + (dark ? "-dark.png" : "-light.png")));
                QTimer::singleShot(0, dialog->findChild<QPushButton *>("mcpStart"), &QPushButton::click);
            } else if (server.running()) {
                started = true; QVERIFY(dialog->findChild<QPushButton *>("mcpCopyConfig")->isEnabled());
                dialog->findChild<QPushButton *>("mcpStop")->click(); dialog->reject();
            }
        });
        timer.start(); QTimer::singleShot(8000, &server, [&] { if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        showMcpDialog(&server, settings, nullptr, [&](const McpSettings &) { saved = true; }); timer.stop();
        QVERIFY(visited); QVERIFY(saved); QVERIFY(started); QVERIFY(!server.running());
    }
};
QTEST_MAIN(McpTests)
#include "mcp_tests.moc"
