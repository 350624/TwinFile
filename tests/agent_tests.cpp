#include "agent.h"
#include "ai_panel.h"
#include "appearance.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QtConcurrent>
#include <windows.h>

using namespace twinfile;

class AgentApi : public QObject {
public:
    QTcpServer server;
    QList<QJsonObject> requests;
    std::function<void(QTcpSocket *)> respond;
    AgentApi() {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                socket->setParent(this);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    const QByteArray bytes = socket->property("buffer").toByteArray() + socket->readAll(); socket->setProperty("buffer", bytes);
                    if (socket->property("done").toBool()) return;
                    const qsizetype separator = bytes.indexOf("\r\n\r\n"); if (separator < 0) return;
                    qint64 length = 0;
                    for (const auto &line : bytes.left(separator).split('\n')) if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
                    if (bytes.size() < separator + 4 + length) return;
                    socket->setProperty("done", true); requests.append(QJsonDocument::fromJson(bytes.mid(separator + 4, length)).object());
                    if (respond) respond(socket);
                });
            }
        });
        server.listen(QHostAddress::LocalHost, 0);
    }
    AiSettings settings() const { AiSettings settings; settings.endpoint = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort()); settings.model = QStringLiteral("test-tool-model"); return settings; }
    static void reply(QTcpSocket *socket, const QByteArray &body, int status = 200) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body); socket->disconnectFromHost();
    }
};

static QJsonObject call(const QString &name, const QJsonObject &arguments, const QString &id = QStringLiteral("call_1")) {
    return {{QStringLiteral("id"), id}, {QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("arguments"), QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))}}}};
}

static QByteArray answer(const QJsonArray &calls = {}, const QString &text = QStringLiteral("任务完成"), const QString &reason = {}) {
    return QJsonDocument(QJsonObject{{QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("finish_reason"), reason.isEmpty() ? calls.isEmpty() ? QStringLiteral("stop") : QStringLiteral("tool_calls") : reason}, {QStringLiteral("message"), QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), text}, {QStringLiteral("tool_calls"), calls}}}}}}}).toJson(QJsonDocument::Compact);
}

static bool writeFixture(const QString &path, const QByteArray &bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
static QJsonObject args(const QString &path) { return {{QStringLiteral("path"), path}}; }
static QJsonObject commandArgs(const QString &command, const QString &directory = {}, int seconds = 10) {
    return {{QStringLiteral("command"), command}, {QStringLiteral("working_directory"), directory}, {QStringLiteral("timeout_seconds"), seconds}};
}
static QString psLiteral(QString value) { return u'\'' + value.replace(u'\'', QStringLiteral("''")) + u'\''; }

class AgentHost : public extensions::Host {
public:
    int refreshed = 0;
    QJsonObject selectionContext() const override { return {{QStringLiteral("private"), QStringLiteral("DO_NOT_SEND")}}; }
    void refreshActivePane() override { ++refreshed; }
    void showStatusMessage(const QString &) override {}
};

class AgentTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qApp->setStyle(QStringLiteral("Fusion")); qApp->setAttribute(Qt::AA_DontUseNativeDialogs); qputenv("TWINFILE_TEST_MODE", "1"); }
    void toolsReadSearchAndHash() {
        QTemporaryDir root; QVERIFY(root.isValid());
        QVERIFY(writeFixture(root.filePath(QStringLiteral("说明.txt")), QStringLiteral("你好 Agent").toUtf8()));
        QVERIFY(QDir(root.path()).mkdir(QStringLiteral("folder")));
        QVERIFY(writeFixture(root.filePath(QStringLiteral("folder/notes.md")), "abc"));
        auto result = AgentTools::execute(root.path(), QStringLiteral("list_directory"), args(QStringLiteral(".")), false, {});
        QVERIFY(result.value(QStringLiteral("ok")).toBool()); QCOMPARE(result.value(QStringLiteral("entries")).toArray().size(), 2);
        result = AgentTools::execute(root.path(), QStringLiteral("read_text"), args(QStringLiteral("说明.txt")), false, {});
        QCOMPARE(result.value(QStringLiteral("untrusted_text")).toString(), QStringLiteral("你好 Agent"));
        result = AgentTools::execute(root.path(), QStringLiteral("file_info"), args(QStringLiteral("folder")), false, {}); QVERIFY(result.value(QStringLiteral("directory")).toBool());
        result = AgentTools::execute(root.path(), QStringLiteral("sha256"), args(QStringLiteral("folder/notes.md")), false, {});
        QCOMPARE(result.value(QStringLiteral("sha256")).toString(), QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        auto search = args(QStringLiteral(".")); search.insert(QStringLiteral("query"), QStringLiteral("NOTE"));
        result = AgentTools::execute(root.path(), QStringLiteral("search_names"), search, false, {}); QCOMPARE(result.value(QStringLiteral("entries")).toArray().size(), 1);
        auto cancelled = std::make_shared<std::atomic_bool>(true); QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("read_text"), args(QStringLiteral("说明.txt")), false, cancelled).value(QStringLiteral("ok")).toBool());
    }
    void forbiddenPaths_data() {
        QTest::addColumn<QString>("path");
        for (const QString &path : {QStringLiteral("../secret.txt"), QStringLiteral("folder/../../secret.txt"), QStringLiteral("C:/secret.txt"), QStringLiteral("\\\\server\\secret"), QStringLiteral("file.txt:stream"), QStringLiteral("CON.txt"), QStringLiteral("file. "), QStringLiteral("missing/file.txt"), QStringLiteral("C:secret.txt"), QStringLiteral("/secret.txt")}) QTest::newRow(qPrintable(path)) << path;
    }
    void forbiddenPaths() {
        QFETCH(QString, path); QTemporaryDir root;
        auto result = AgentTools::execute(root.path(), QStringLiteral("read_text"), args(path), false, {}); QVERIFY(!result.value(QStringLiteral("ok")).toBool());
        auto create = args(path); create.insert(QStringLiteral("content"), QStringLiteral("test"));
        result = AgentTools::execute(root.path(), QStringLiteral("create_text"), create, true, {}); QVERIFY(!result.value(QStringLiteral("ok")).toBool());
    }
    void toolsRejectLinksAndHardLinks() {
        QTemporaryDir root, outside; const QString secret = outside.filePath(QStringLiteral("private.txt")); QVERIFY(writeFixture(secret, "private"));
        const QString hard = root.filePath(QStringLiteral("hard.txt"));
        QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(hard).utf16()), reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(secret).utf16()), nullptr));
        QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("read_text"), args(QStringLiteral("hard.txt")), false, {}).value(QStringLiteral("ok")).toBool());
        QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("sha256"), args(QStringLiteral("hard.txt")), false, {}).value(QStringLiteral("ok")).toBool());
        const QString junction = root.filePath(QStringLiteral("linked"));
        const auto created = runProcess(QStringLiteral("C:/Windows/System32/cmd.exe"), {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"), QDir::toNativeSeparators(junction), QDir::toNativeSeparators(outside.path())});
        QVERIFY2(created.ok, qPrintable(created.error));
        QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("read_text"), args(QStringLiteral("linked/private.txt")), false, {}).value(QStringLiteral("ok")).toBool());
        QString error; QVERIFY(!AgentTools::validRoot(junction, &error));
        QVERIFY(QDir(outside.path()).mkdir(QStringLiteral("child"))); QVERIFY(!AgentTools::validRoot(QDir(junction).filePath(QStringLiteral("child")), &error));
        QVERIFY(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(junction).utf16())));
        QVERIFY(QFile::exists(secret));
    }
    void toolsBoundedAndValidated() {
        QTemporaryDir root;
        QVERIFY(writeFixture(root.filePath(QStringLiteral("big.txt")), QByteArray(65537, 'a')));
        QVERIFY(writeFixture(root.filePath(QStringLiteral("binary.txt")), QByteArray("abc\0def", 7)));
        QVERIFY(writeFixture(root.filePath(QStringLiteral("invalid.txt")), QByteArray(1, char(0xff))));
        for (const QString &path : {QStringLiteral("big.txt"), QStringLiteral("binary.txt"), QStringLiteral("invalid.txt")}) QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("read_text"), args(path), false, {}).value(QStringLiteral("ok")).toBool());
        QFile big(root.filePath(QStringLiteral("huge.bin"))); QVERIFY(big.open(QIODevice::WriteOnly)); QVERIFY(big.resize(64 * 1024 * 1024 + 1)); big.close();
        QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("sha256"), args(QStringLiteral("huge.bin")), false, {}).value(QStringLiteral("ok")).toBool());
        QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("run_command"), args(QStringLiteral(".")), true, {}).value(QStringLiteral("ok")).toBool());
        auto extra = args(QStringLiteral(".")); extra.insert(QStringLiteral("command"), QStringLiteral("ignored")); QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("list_directory"), extra, false, {}).value(QStringLiteral("ok")).toBool());
        for (int index = 0; index < 210; ++index) QVERIFY(writeFixture(root.filePath(QStringLiteral("item%1.txt").arg(index)), "x"));
        const auto result = AgentTools::execute(root.path(), QStringLiteral("list_directory"), args(QStringLiteral(".")), false, {}); QCOMPARE(result.value(QStringLiteral("entries")).toArray().size(), 200); QVERIFY(result.value(QStringLiteral("truncated")).toBool());
    }
    void mutationsRequireApprovalAndNeverOverwrite() {
        QTemporaryDir root; QString error;
        auto create = args(QStringLiteral("new.txt")); create.insert(QStringLiteral("content"), QStringLiteral("完整内容"));
        QVERIFY(AgentTools::approval(root.path(), QStringLiteral("create_text"), create, &error).contains(QStringLiteral("完整内容")));
        QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("create_text"), create, false, {}).value(QStringLiteral("ok")).toBool()); QVERIFY(!QFile::exists(root.filePath(QStringLiteral("new.txt"))));
        QVERIFY(AgentTools::execute(root.path(), QStringLiteral("create_text"), create, true, {}).value(QStringLiteral("ok")).toBool());
        create.insert(QStringLiteral("content"), QStringLiteral("覆盖")); QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("create_text"), create, true, {}).value(QStringLiteral("ok")).toBool());
        create.insert(QStringLiteral("path"), QStringLiteral("script.ps1")); QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("create_text"), create, true, {}).value(QStringLiteral("ok")).toBool());
        auto rename = args(QStringLiteral("new.txt")); rename.insert(QStringLiteral("new_name"), QStringLiteral("renamed.txt"));
        QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("rename_entry"), rename, false, {}).value(QStringLiteral("ok")).toBool());
        QVERIFY(AgentTools::execute(root.path(), QStringLiteral("rename_entry"), rename, true, {}).value(QStringLiteral("ok")).toBool()); QVERIFY(!QFile::exists(root.filePath(QStringLiteral("new.txt"))));
        rename.insert(QStringLiteral("path"), QStringLiteral("renamed.txt")); rename.insert(QStringLiteral("new_name"), QStringLiteral("../outside.txt")); QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("rename_entry"), rename, true, {}).value(QStringLiteral("ok")).toBool());
        QVERIFY(AgentTools::execute(root.path(), QStringLiteral("create_folder"), args(QStringLiteral("folder")), true, {}).value(QStringLiteral("ok")).toBool());
        rename.insert(QStringLiteral("path"), QStringLiteral("folder")); rename.insert(QStringLiteral("new_name"), QStringLiteral("other")); QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("rename_entry"), rename, true, {}).value(QStringLiteral("ok")).toBool());
        QVERIFY(writeFixture(root.filePath(QStringLiteral("existing.txt")), "old")); rename.insert(QStringLiteral("path"), QStringLiteral("renamed.txt")); rename.insert(QStringLiteral("new_name"), QStringLiteral("existing.txt")); QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("rename_entry"), rename, true, {}).value(QStringLiteral("ok")).toBool());
        QCOMPARE(AgentTools::execute(root.path(), QStringLiteral("read_text"), args(QStringLiteral("renamed.txt")), false, {}).value(QStringLiteral("untrusted_text")).toString(), QStringLiteral("完整内容"));
    }
    void protocolRoundTripAndIndependentTasks() {
        QTemporaryDir root; QVERIFY(writeFixture(root.filePath(QStringLiteral("note.txt")), "untrusted-reference"));
        AgentApi api; Agent agent; QString error; QSignalSpy complete(&agent, &Agent::completed), failed(&agent, &Agent::failed);
        api.respond = [&](QTcpSocket *socket) {
            if (api.requests.size() == 1) AgentApi::reply(socket, answer({call(QStringLiteral("read_text"), args(QStringLiteral("note.txt")))}));
            else AgentApi::reply(socket, answer());
        };
        QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("读取指定文件"), &error)); QVERIFY(!agent.start(api.settings(), root.path(), QStringLiteral("重复"), &error));
        QTRY_COMPARE(complete.size(), 1); QVERIFY(failed.isEmpty()); QCOMPARE(api.requests.size(), 2); QVERIFY(!agent.busy());
        const auto first = api.requests.first(); QVERIFY(!first.value(QStringLiteral("stream")).toBool(true)); QCOMPARE(first.value(QStringLiteral("tools")).toArray().size(), 8);
        QVERIFY(!QJsonDocument(first).toJson().contains("untrusted-reference")); QVERIFY(!QJsonDocument(first).toJson().contains(root.path().toUtf8()));
        const auto messages = api.requests.last().value(QStringLiteral("messages")).toArray(); QCOMPARE(messages.size(), 4);
        QCOMPARE(messages.at(2).toObject().value(QStringLiteral("tool_calls")).toArray().size(), 1);
        QCOMPARE(messages.last().toObject().value(QStringLiteral("role")).toString(), QStringLiteral("tool")); QCOMPARE(messages.last().toObject().value(QStringLiteral("tool_call_id")).toString(), QStringLiteral("call_1"));
        QVERIFY(messages.last().toObject().value(QStringLiteral("content")).toString().contains(QStringLiteral("untrusted-reference")));
        QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("独立新任务"), &error)); QTRY_COMPARE(complete.size(), 2); QCOMPARE(api.requests.last().value(QStringLiteral("messages")).toArray().size(), 2);
    }
    void fullAccessRequiresHostPermission() {
        QTemporaryDir root;
        QCOMPARE(AgentTools::definitions().size(), 8);
        const auto tools = AgentTools::definitions(AgentPermission::FullAccess); QCOMPARE(tools.size(), 9);
        const auto command = tools.last().toObject().value(QStringLiteral("function")).toObject(); QCOMPARE(command.value(QStringLiteral("name")).toString(), QStringLiteral("run_command"));
        QCOMPARE(command.value(QStringLiteral("parameters")).toObject().value(QStringLiteral("properties")).toObject().value(QStringLiteral("timeout_seconds")).toObject().value(QStringLiteral("type")).toString(), QStringLiteral("integer"));
        const auto arguments = commandArgs(QStringLiteral("Set-Content -LiteralPath 'forbidden.txt' -Value 'no'"));
        QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("run_command"), arguments, true, {}).value(QStringLiteral("ok")).toBool());
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("forbidden.txt"))));
        auto escalating = arguments; escalating.insert(QStringLiteral("permission"), QStringLiteral("FullAccess"));
        QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("run_command"), escalating, false, {}, AgentPermission::FullAccess).value(QStringLiteral("ok")).toBool());
        auto create = args(QStringLiteral("authorized.txt")); create.insert(QStringLiteral("content"), QStringLiteral("yes"));
        QVERIFY(AgentTools::execute(root.path(), QStringLiteral("create_text"), create, false, {}, AgentPermission::FullAccess).value(QStringLiteral("ok")).toBool());
    }
    void fullAccessCommandReadsWritesOutsideRootAndDeletes() {
        QTemporaryDir root, outside; const QString target = outside.filePath(QStringLiteral("测试 'file.txt"));
        QVERIFY(writeFixture(target, "old"));
        const QString script = QStringLiteral("[IO.File]::WriteAllText(%1, '新内容'); [IO.File]::ReadAllText(%1)").arg(psLiteral(target));
        const auto result = AgentTools::execute(root.path(), QStringLiteral("run_command"), commandArgs(script, outside.path()), false, {}, AgentPermission::FullAccess);
        QVERIFY2(result.value(QStringLiteral("ok")).toBool(), qPrintable(QJsonDocument(result).toJson()));
        QVERIFY(result.value(QStringLiteral("untrusted_output")).toString().contains(QStringLiteral("新内容"))); QCOMPARE(result.value(QStringLiteral("exit_code")).toInt(), 0);
        QFile contents(target); QVERIFY(contents.open(QIODevice::ReadOnly)); QCOMPARE(contents.readAll(), QStringLiteral("新内容").toUtf8()); contents.close();
        const auto removed = AgentTools::execute(root.path(), QStringLiteral("run_command"), commandArgs(QStringLiteral("Remove-Item -LiteralPath %1").arg(psLiteral(target))), false, {}, AgentPermission::FullAccess);
        QVERIFY2(removed.value(QStringLiteral("ok")).toBool(), qPrintable(QJsonDocument(removed).toJson())); QVERIFY(!QFile::exists(target));
    }
    void fullAccessCommandErrorsAndBounds() {
        QTemporaryDir root;
        auto execute = [&](const QJsonObject &arguments) { return AgentTools::execute(root.path(), QStringLiteral("run_command"), arguments, false, {}, AgentPermission::FullAccess); };
        const auto failed = execute(commandArgs(QStringLiteral("[Console]::Error.WriteLine('stderr-test'); exit 7")));
        QVERIFY(!failed.value(QStringLiteral("ok")).toBool()); QCOMPARE(failed.value(QStringLiteral("exit_code")).toInt(), 7); QVERIFY(failed.value(QStringLiteral("untrusted_output")).toString().contains(QStringLiteral("stderr-test")));
        const auto overflow = execute(commandArgs(QStringLiteral("[Console]::Write(('x' * 70000))")));
        QVERIFY(!overflow.value(QStringLiteral("ok")).toBool()); QCOMPARE(overflow.value(QStringLiteral("untrusted_output")).toString().size(), 65536); QVERIFY(overflow.value(QStringLiteral("error")).toString().contains(QStringLiteral("64 KiB")));
        for (int seconds : {0, 121}) QVERIFY(!execute(commandArgs(QStringLiteral("'no'"), {}, seconds)).value(QStringLiteral("ok")).toBool());
        auto fractional = commandArgs(QStringLiteral("'no'")); fractional.insert(QStringLiteral("timeout_seconds"), 1.5); QVERIFY(!execute(fractional).value(QStringLiteral("ok")).toBool());
        QVERIFY(!execute(commandArgs(QString(8193, u'x'))).value(QStringLiteral("ok")).toBool());
        QVERIFY(!execute(commandArgs(QStringLiteral("'no'"), root.filePath(QStringLiteral("missing")))).value(QStringLiteral("ok")).toBool());
        auto cancelled = std::make_shared<std::atomic_bool>(true);
        QVERIFY(!AgentTools::execute(root.path(), QStringLiteral("run_command"), commandArgs(QStringLiteral("'no'")), false, cancelled, AgentPermission::FullAccess).value(QStringLiteral("ok")).toBool());
        const auto timeout = execute(commandArgs(QStringLiteral("Start-Sleep -Seconds 10"), {}, 1)); QVERIFY(!timeout.value(QStringLiteral("ok")).toBool()); QVERIFY(timeout.value(QStringLiteral("error")).toString().contains(QStringLiteral("超时")));
    }
    void fullAccessPreservesNativeExitCode() {
        QTemporaryDir root;
        const auto result = AgentTools::execute(root.path(), QStringLiteral("run_command"), commandArgs(QStringLiteral("& \"$env:SystemRoot\\System32\\cmd.exe\" /d /c exit 7; 'following output'")), false, {}, AgentPermission::FullAccess);
        QVERIFY(!result.value(QStringLiteral("ok")).toBool());
        QCOMPARE(result.value(QStringLiteral("exit_code")).toInt(), 7);
        QVERIFY(result.value(QStringLiteral("untrusted_output")).toString().contains(QStringLiteral("following output")));
        const auto success = AgentTools::execute(root.path(), QStringLiteral("run_command"), commandArgs(QStringLiteral("& \"$env:SystemRoot\\System32\\cmd.exe\" /d /c exit 0; 'ok'")), false, {}, AgentPermission::FullAccess);
        QVERIFY(success.value(QStringLiteral("ok")).toBool()); QCOMPARE(success.value(QStringLiteral("exit_code")).toInt(), 0);
        const auto exception = AgentTools::execute(root.path(), QStringLiteral("run_command"), commandArgs(QStringLiteral("throw 'test-error'")), false, {}, AgentPermission::FullAccess);
        QVERIFY(!exception.value(QStringLiteral("ok")).toBool()); QVERIFY(exception.value(QStringLiteral("exit_code")).toInt() != 0);
    }
    void cancellationWaitsForCommandCleanup() {
        QTemporaryDir root; AgentApi api; Agent agent; QString error;
        const QString pidFile = root.filePath(QStringLiteral("command.pid"));
        api.respond = [&](QTcpSocket *socket) { AgentApi::reply(socket, answer({call(QStringLiteral("run_command"), commandArgs(QStringLiteral("[IO.File]::WriteAllText(%1, [string]$PID); Start-Sleep -Seconds 30").arg(psLiteral(pidFile)), {}, 60))})); };
        QSignalSpy failed(&agent, &Agent::failed), completed(&agent, &Agent::completed);
        const auto cleanup = qScopeGuard([&] { agent.cancel(); });
        QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("wait"), &error, AgentPermission::FullAccess));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(pidFile).size() > 0, 10000);
        QFile file(pidFile); QVERIFY(file.open(QIODevice::ReadOnly)); const DWORD processId = file.readAll().toUInt(); file.close(); QVERIFY(processId);
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId); QVERIFY(process); const auto processGuard = qScopeGuard([&] { CloseHandle(process); });
        agent.cancel(); QVERIFY(agent.busy()); QVERIFY(failed.isEmpty());
        QVERIFY(!agent.start(api.settings(), root.path(), QStringLiteral("too soon"), &error));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000); QVERIFY(!agent.busy());
        QCOMPARE(WaitForSingleObject(process, 0), DWORD(WAIT_OBJECT_0)); QVERIFY(completed.isEmpty()); QCOMPARE(api.requests.size(), 1);
        api.respond = [&](QTcpSocket *socket) { AgentApi::reply(socket, answer()); };
        QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("new task"), &error)); QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(api.requests.last().value(QStringLiteral("tools")).toArray().size(), 8);
    }
    void fullAccessCancellationKillsChildProcess() {
        QTemporaryDir root; const QString pidFile = root.filePath(QStringLiteral("child.pid"));
        const QString script = QStringLiteral("$child = Start-Process -FilePath ($PSHOME + '\\powershell.exe') -ArgumentList '-NoProfile -NonInteractive -Command Start-Sleep -Seconds 30' -WindowStyle Hidden -PassThru; [IO.File]::WriteAllText(%1, [string]$child.Id); Start-Sleep -Seconds 30").arg(psLiteral(pidFile));
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        auto future = QtConcurrent::run([&] { return AgentTools::execute(root.path(), QStringLiteral("run_command"), commandArgs(script, {}, 60), false, cancellation, AgentPermission::FullAccess); });
        const auto cleanup = qScopeGuard([&] { cancellation->store(true); future.waitForFinished(); });
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(pidFile).size() > 0, 10000);
        QFile file(pidFile); QVERIFY(file.open(QIODevice::ReadOnly)); const DWORD processId = file.readAll().toUInt(); QVERIFY(processId); file.close();
        HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, processId); QVERIFY(child); const auto childGuard = qScopeGuard([&] { CloseHandle(child); });
        cancellation->store(true); QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        QVERIFY(!future.result().value(QStringLiteral("ok")).toBool()); QCOMPARE(WaitForSingleObject(child, 0), DWORD(WAIT_OBJECT_0));
    }
    void fullAccessProtocolWithoutApprovalsAndResets() {
        QTemporaryDir root; AgentApi api; Agent agent; QString error;
        QSignalSpy complete(&agent, &Agent::completed), approvals(&agent, &Agent::approvalRequested), changed(&agent, &Agent::changedFiles);
        api.respond = [&](QTcpSocket *socket) {
            if (api.requests.size() == 1) AgentApi::reply(socket, answer({call(QStringLiteral("run_command"), commandArgs(QStringLiteral("'tool-output'")))}));
            else AgentApi::reply(socket, answer());
        };
        QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("run"), &error, AgentPermission::FullAccess)); QTRY_COMPARE_WITH_TIMEOUT(complete.size(), 1, 10000);
        QVERIFY(approvals.isEmpty()); QCOMPARE(changed.size(), 1); QCOMPARE(api.requests.first().value(QStringLiteral("tools")).toArray().size(), 9);
        QVERIFY(api.requests.last().value(QStringLiteral("messages")).toArray().last().toObject().value(QStringLiteral("content")).toString().contains(QStringLiteral("tool-output")));
        QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("default"), &error)); QTRY_COMPARE(complete.size(), 2); QCOMPARE(api.requests.last().value(QStringLiteral("tools")).toArray().size(), 8);
    }
    void fullAccessPanelConsentAndReset() {
        AgentApi api; auto settings = api.settings(); AgentHost host;
        const QPalette previousPalette = qApp->palette(); const QString previousSheet = qApp->styleSheet();
        const auto restore = qScopeGuard([&] { qApp->setPalette(previousPalette); qApp->setStyleSheet(previousSheet); });
        const auto palette = appearancePalette(QStringLiteral("macos"), true, QStringLiteral("blue")); qApp->setPalette(palette); qApp->setStyleSheet(appearanceStyleSheet(QStringLiteral("macos"), true, palette));
        AiPanel panel(&settings, &host); panel.setAttribute(Qt::WA_DontShowOnScreen); panel.resize(350, 750); panel.show();
        auto *mode = panel.findChild<QComboBox *>(QStringLiteral("aiMode")); auto *permission = panel.findChild<QComboBox *>(QStringLiteral("aiPermission"));
        QVERIFY(mode); QVERIFY(permission); QCOMPARE(permission->currentIndex(), 0); mode->setCurrentIndex(1);
        int decision = QMessageBox::Cancel; int notices = 0; QTimer dialogs;
        connect(&dialogs, &QTimer::timeout, &panel, [&] { if (auto *warning = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) { ++notices; warning->done(decision); } });
        dialogs.start(10); QTimer::singleShot(8000, &dialogs, [] { if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        permission->setCurrentIndex(1); QCOMPARE(permission->currentIndex(), 0); QCOMPARE(notices, 1); QVERIFY(api.requests.isEmpty());
        decision = QMessageBox::Yes; permission->setCurrentIndex(1); QCOMPARE(permission->currentIndex(), 1); QCOMPARE(notices, 2);
        QVERIFY(panel.findChild<QLabel *>(QStringLiteral("aiPrivacy"))->text().contains(QStringLiteral("完全权限")));
        const QString output = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        if (!output.isEmpty()) { QTest::qWait(30); QVERIFY(panel.grab().save(QDir(output).filePath(QStringLiteral("native-agent-full-access.png")))); }
        auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("aiInput")); input->setPlainText(QStringLiteral("等待")); panel.findChild<QPushButton *>(QStringLiteral("aiSend"))->click();
        QTRY_COMPARE(api.requests.size(), 1); QVERIFY(!permission->isEnabled()); QCOMPARE(api.requests.first().value(QStringLiteral("tools")).toArray().size(), 9);
        panel.findChild<QPushButton *>(QStringLiteral("aiSend"))->click(); QTRY_VERIFY(!panel.busy()); QVERIFY(permission->isEnabled());
        panel.clearConversation(); QCOMPARE(permission->currentIndex(), 0); QCOMPARE(panel.findChild<QLabel *>(QStringLiteral("aiAgentRoot"))->text(), QStringLiteral("尚未授权"));
        input->setPlainText(QStringLiteral("不得自动授权主目录")); panel.findChild<QPushButton *>(QStringLiteral("aiSend"))->click(); QVERIFY(!panel.busy()); QCOMPARE(api.requests.size(), 1);
        permission->setCurrentIndex(1); permission->setCurrentIndex(0); QCOMPARE(panel.findChild<QLabel *>(QStringLiteral("aiAgentRoot"))->text(), QStringLiteral("尚未授权"));
        permission->setCurrentIndex(1); QCOMPARE(permission->currentIndex(), 1); mode->setCurrentIndex(0); QCOMPARE(permission->currentIndex(), 0);
        mode->setCurrentIndex(1); permission->setCurrentIndex(1); panel.revokeAgentAccess(); QCOMPARE(permission->currentIndex(), 0); QCOMPARE(panel.findChild<QLabel *>(QStringLiteral("aiAgentRoot"))->text(), QStringLiteral("尚未授权")); dialogs.stop();
    }
    void confirmationApproveDenyAndCancel() {
        for (int decision = 0; decision < 3; ++decision) {
            QTemporaryDir root; AgentApi api; Agent agent; QString error; QSignalSpy approvals(&agent, &Agent::approvalRequested), complete(&agent, &Agent::completed), failed(&agent, &Agent::failed), changed(&agent, &Agent::changedFiles);
            auto create = args(QStringLiteral("new.txt")); create.insert(QStringLiteral("content"), QStringLiteral("content"));
            api.respond = [&](QTcpSocket *socket) { AgentApi::reply(socket, api.requests.size() == 1 ? answer({call(QStringLiteral("create_text"), create)}) : answer()); };
            QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("新建文件"), &error)); QTRY_COMPARE(approvals.size(), 1); QVERIFY(agent.busy()); QVERIFY(!QFile::exists(root.filePath(QStringLiteral("new.txt"))));
            if (decision == 2) { agent.cancel(); agent.resolveApproval(true); QTRY_COMPARE(failed.size(), 1); QVERIFY(complete.isEmpty()); QCOMPARE(api.requests.size(), 1); }
            else { agent.resolveApproval(decision == 1); QTRY_COMPARE(complete.size(), 1); QVERIFY(failed.isEmpty()); QCOMPARE(changed.size(), decision); const auto result = QJsonDocument::fromJson(api.requests.last().value(QStringLiteral("messages")).toArray().last().toObject().value(QStringLiteral("content")).toString().toUtf8()).object(); QCOMPARE(result.value(QStringLiteral("ok")).toBool(), decision == 1); }
            QCOMPARE(QFile::exists(root.filePath(QStringLiteral("new.txt"))), decision == 1);
        }
    }
    void approvalRevalidationAndSequentialBatch() {
        QTemporaryDir root; AgentApi api; Agent agent; QString error;
        const QJsonArray calls{call(QStringLiteral("create_folder"), args(QStringLiteral("folder")), QStringLiteral("one")), call(QStringLiteral("create_text"), {{QStringLiteral("path"), QStringLiteral("folder/new.txt")}, {QStringLiteral("content"), QStringLiteral("new content")}}, QStringLiteral("two"))};
        api.respond = [&](QTcpSocket *socket) { AgentApi::reply(socket, api.requests.size() == 1 ? answer(calls) : answer()); };
        QSignalSpy approvals(&agent, &Agent::approvalRequested), complete(&agent, &Agent::completed), changed(&agent, &Agent::changedFiles);
        QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("先建目录，再建文件"), &error));
        QTRY_COMPARE(approvals.size(), 1); agent.resolveApproval(true); QTRY_COMPARE(approvals.size(), 2);
        QVERIFY(QFileInfo(root.filePath(QStringLiteral("folder"))).isDir()); QVERIFY(!QFile::exists(root.filePath(QStringLiteral("folder/new.txt"))));
        QVERIFY(writeFixture(root.filePath(QStringLiteral("folder/new.txt")), "created-by-another-process"));
        agent.resolveApproval(true); QTRY_COMPARE(complete.size(), 1); QCOMPARE(changed.size(), 1);
        const auto messages = api.requests.last().value(QStringLiteral("messages")).toArray(); QCOMPARE(messages.size(), 5);
        QCOMPARE(messages.at(3).toObject().value(QStringLiteral("tool_call_id")).toString(), QStringLiteral("one"));
        const auto last = QJsonDocument::fromJson(messages.last().toObject().value(QStringLiteral("content")).toString().toUtf8()).object(); QVERIFY(!last.value(QStringLiteral("ok")).toBool());
        QCOMPARE(AgentTools::execute(root.path(), QStringLiteral("read_text"), args(QStringLiteral("folder/new.txt")), false, {}).value(QStringLiteral("untrusted_text")).toString(), QStringLiteral("created-by-another-process"));
    }
    void cancelDuringReadPreventsStaleContinuation() {
        QTemporaryDir root; QVERIFY(writeFixture(root.filePath(QStringLiteral("note.txt")), "old-result")); AgentApi api; Agent agent; QString error;
        QSignalSpy failed(&agent, &Agent::failed), complete(&agent, &Agent::completed);
        api.respond = [&](QTcpSocket *socket) { AgentApi::reply(socket, api.requests.size() == 1 ? answer({call(QStringLiteral("read_text"), args(QStringLiteral("note.txt")))}) : answer()); };
        bool stopped = false;
        connect(&agent, &Agent::activity, &agent, [&](const QString &text) { if (!stopped && text.startsWith(QStringLiteral("read_text · 完成"))) { stopped = true; agent.cancel(); } });
        QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("read"), &error)); QTRY_COMPARE(failed.size(), 1); QVERIFY(stopped); QTest::qWait(30); QCOMPARE(api.requests.size(), 1);
        QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("new task"), &error)); QTRY_COMPARE(complete.size(), 1); QCOMPARE(api.requests.size(), 2); QVERIFY(!QJsonDocument(api.requests.last()).toJson().contains("old-result"));
    }
    void invalidProtocol_data() {
        QTest::addColumn<QByteArray>("body"); QTest::addColumn<int>("status");
        QTest::newRow("http-auth") << QByteArray("private-provider-error") << 401;
        QTest::newRow("redirect") << QByteArray() << 302;
        QTest::newRow("invalid-json") << QByteArray("private-provider-error") << 200;
        QTest::newRow("sse-not-requested") << QByteArray("data: {}\n\n") << 200;
        QTest::newRow("too-large") << QByteArray(2 * 1024 * 1024 + 1, 'x') << 200;
        QTest::newRow("truncated") << answer({call(QStringLiteral("create_folder"), args(QStringLiteral("new")))}, {}, QStringLiteral("length")) << 200;
        auto bad = call(QStringLiteral("create_folder"), args(QStringLiteral("new"))); auto function = bad.value(QStringLiteral("function")).toObject(); function.insert(QStringLiteral("arguments"), QStringLiteral("{")); bad.insert(QStringLiteral("function"), function);
        QTest::newRow("bad-arguments") << answer({bad}) << 200;
        const auto duplicate = call(QStringLiteral("create_folder"), args(QStringLiteral("new")));
        QTest::newRow("duplicate-id") << answer({duplicate, duplicate}) << 200;
    }
    void invalidProtocol() {
        QFETCH(QByteArray, body); QFETCH(int, status); AgentApi api; Agent agent; QTemporaryDir root; QString error;
        api.respond = [body, status](QTcpSocket *socket) { AgentApi::reply(socket, body, status); };
        QSignalSpy failed(&agent, &Agent::failed), complete(&agent, &Agent::completed), approvals(&agent, &Agent::approvalRequested);
        QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("test"), &error)); QTRY_COMPARE(failed.size(), 1); QVERIFY(complete.isEmpty()); QVERIFY(approvals.isEmpty()); QVERIFY(!agent.busy()); QVERIFY(!failed.first().first().toString().contains(QStringLiteral("private-provider-error"))); QCOMPARE(QDir(root.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size(), 0);
    }
    void unknownToolAndLoopLimit() {
        AgentApi api; Agent agent; QTemporaryDir root; QString error;
        api.respond = [](QTcpSocket *socket) { AgentApi::reply(socket, answer({call(QStringLiteral("run_command"), args(QStringLiteral(".")))})); };
        QSignalSpy failed(&agent, &Agent::failed); QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("test"), &error)); QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(api.requests.size(), 13); const auto result = api.requests.at(1).value(QStringLiteral("messages")).toArray().last().toObject().value(QStringLiteral("content")).toString(); QVERIFY(result.contains(QStringLiteral("未知工具"))); QVERIFY(!agent.busy());
    }
    void cancelNetworkAndRestart() {
        AgentApi api; Agent agent; QTemporaryDir root; QString error; QSignalSpy failed(&agent, &Agent::failed), complete(&agent, &Agent::completed);
        QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("slow"), &error)); QTRY_COMPARE(api.requests.size(), 1); agent.cancel(); QCOMPARE(failed.size(), 1); QVERIFY(!agent.busy());
        api.respond = [](QTcpSocket *socket) { AgentApi::reply(socket, answer()); }; QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("new"), &error)); QTRY_COMPARE(complete.size(), 1); QCOMPARE(failed.size(), 1);
        auto invalid = api.settings(); invalid.apiKey = QStringLiteral("key\r\ninjection"); QVERIFY(!agent.start(invalid, root.path(), QStringLiteral("test"), &error));
        QVERIFY(!agent.start(api.settings(), QStringLiteral("//server/share"), QStringLiteral("test"), &error));
    }
    void deadlinesCancelRequestsAndPendingApproval() {
        QTemporaryDir root; AgentApi api; Agent agent; QString error; QSignalSpy failed(&agent, &Agent::failed), approvals(&agent, &Agent::approvalRequested);
        QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("slow"), &error)); QTRY_COMPARE(api.requests.size(), 1);
        auto *requestDeadline = agent.findChild<QTimer *>(QStringLiteral("agentRequestDeadline")); QVERIFY(requestDeadline); requestDeadline->start(1); QTRY_COMPARE(failed.size(), 1); QVERIFY(!agent.busy());
        requestDeadline->setInterval(180000);
        api.respond = [](QTcpSocket *socket) { AgentApi::reply(socket, answer({call(QStringLiteral("create_folder"), args(QStringLiteral("new")))})); };
        QVERIFY(agent.start(api.settings(), root.path(), QStringLiteral("waiting"), &error)); QTRY_COMPARE(approvals.size(), 1);
        auto *taskDeadline = agent.findChild<QTimer *>(QStringLiteral("agentTaskDeadline")); QVERIFY(taskDeadline); taskDeadline->start(1); QTRY_COMPARE(failed.size(), 2); agent.resolveApproval(true);
        QVERIFY(!agent.busy()); QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("new")))); QCOMPARE(api.requests.size(), 2);
    }
    void panelAgentAuthorizationApprovalAndChatIsolation() {
        QTemporaryDir root; AgentApi api; auto settings = api.settings(); AgentHost host;
        const QPalette previousPalette = qApp->palette(); const QString previousSheet = qApp->styleSheet();
        const auto restoreAppearance = qScopeGuard([&] { qApp->setPalette(previousPalette); qApp->setStyleSheet(previousSheet); });
        const QPalette palette = appearancePalette(QStringLiteral("macos"), true, QStringLiteral("blue")); qApp->setPalette(palette); qApp->setStyleSheet(appearanceStyleSheet(QStringLiteral("macos"), true, palette));
        AiPanel panel(&settings, &host);
        panel.setAttribute(Qt::WA_DontShowOnScreen); panel.resize(520, 950); panel.show();
        auto *mode = panel.findChild<QComboBox *>(QStringLiteral("aiMode")); auto *send = panel.findChild<QPushButton *>(QStringLiteral("aiSend")); auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("aiInput")); auto *scope = panel.findChild<QPushButton *>(QStringLiteral("aiAgentScope"));
        QVERIFY(mode); QVERIFY(send); QVERIFY(input); QVERIFY(scope); QVERIFY(!scope->isEnabled()); QVERIFY(api.requests.isEmpty());
        auto *transcript = panel.findChild<QTextBrowser *>(QStringLiteral("aiTranscript")); QVERIFY(transcript); QVERIFY(transcript->toPlainText().trimmed().isEmpty());
        mode->setCurrentIndex(1); input->setPlainText(QStringLiteral("test")); send->click(); QVERIFY(api.requests.isEmpty()); QVERIFY(panel.findChild<QLabel *>(QStringLiteral("aiStatus"))->text().contains(QStringLiteral("授权目录")));
        bool chose = false, consented = false; QTimer dialogs;
        connect(&dialogs, &QTimer::timeout, &panel, [&] {
            if (auto *fileDialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) { fileDialog->setDirectory(root.path()); chose = true; QMetaObject::invokeMethod(fileDialog, "accept"); }
            else if (auto *notice = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) { consented = true; notice->done(notice->objectName() == QStringLiteral("aiFullAccessWarning") ? QMessageBox::Yes : QMessageBox::Ok); }
        });
        dialogs.start(20); QTimer::singleShot(8000, &dialogs, [] { if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        scope->click(); dialogs.stop(); QVERIFY(chose); QVERIFY(consented); QVERIFY(panel.findChild<QLabel *>(QStringLiteral("aiAgentRoot"))->text().contains(root.path(), Qt::CaseInsensitive) || panel.findChild<QLabel *>(QStringLiteral("aiAgentRoot"))->text().contains(QDir::toNativeSeparators(root.path()), Qt::CaseInsensitive));
        auto create = args(QStringLiteral("hello.txt")); create.insert(QStringLiteral("content"), QStringLiteral("Hello TwinFile Agent"));
        api.respond = [&](QTcpSocket *socket) { AgentApi::reply(socket, api.requests.size() == 1 ? answer({call(QStringLiteral("create_text"), create)}) : answer({}, QStringLiteral("已完成 <b>只显示文字</b>"))); };
        input->setPlainText(QStringLiteral("新建 hello.txt")); send->click(); QTRY_VERIFY(panel.findChild<QDialog *>(QStringLiteral("aiAgentApproval"))); QVERIFY(panel.busy()); QVERIFY(!mode->isEnabled()); QVERIFY(!scope->isEnabled()); QVERIFY(!panel.findChild<QPushButton *>(QStringLiteral("aiConfigure"))->isEnabled());
        auto *approval = panel.findChild<QDialog *>(QStringLiteral("aiAgentApproval")); QVERIFY(approval->findChild<QPlainTextEdit *>()->toPlainText().contains(QStringLiteral("Hello TwinFile Agent")));
        const QString output = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        if (!output.isEmpty()) { QVERIFY(panel.grab().save(QDir(output).filePath(QStringLiteral("native-agent-panel.png")))); QVERIFY(approval->grab().save(QDir(output).filePath(QStringLiteral("native-agent-approval.png")))); }
        approval->accept(); QTRY_VERIFY(!panel.busy()); QCOMPARE(host.refreshed, 1); QVERIFY(QFile::exists(root.filePath(QStringLiteral("hello.txt"))));
        QVERIFY(panel.findChild<QTextBrowser *>(QStringLiteral("aiTranscript"))->toPlainText().contains(QStringLiteral("<b>只显示文字</b>")));
        QVERIFY(transcript->toPlainText().contains(QStringLiteral("展开步骤")));
        QVERIFY(QMetaObject::invokeMethod(transcript, "anchorClicked", Q_ARG(QUrl, QUrl(QStringLiteral("twinfile-steps:0"))))); QVERIFY(transcript->toPlainText().contains(QStringLiteral("收起步骤")));
        QVERIFY(QMetaObject::invokeMethod(transcript, "anchorClicked", Q_ARG(QUrl, QUrl(QStringLiteral("twinfile-steps:0"))))); QVERIFY(transcript->toPlainText().contains(QStringLiteral("展开步骤")));
        if (!output.isEmpty()) QVERIFY(panel.grab().save(QDir(output).filePath(QStringLiteral("native-codex-conversation.png"))));
        const QString authorized = panel.findChild<QLabel *>(QStringLiteral("aiAgentRoot"))->text();
        dialogs.start(10); auto *permission = panel.findChild<QComboBox *>(QStringLiteral("aiPermission")); permission->setCurrentIndex(1); dialogs.stop(); QCOMPARE(permission->currentIndex(), 1);
        panel.clearConversation(); QCOMPARE(permission->currentIndex(), 0); QCOMPARE(panel.findChild<QLabel *>(QStringLiteral("aiAgentRoot"))->text(), authorized);
        mode->setCurrentIndex(0); QVERIFY(!scope->isEnabled()); input->setPlainText(QStringLiteral("普通聊天")); send->click(); QTRY_VERIFY(!panel.busy());
        QVERIFY(!api.requests.last().contains(QStringLiteral("tools"))); QVERIFY(!QJsonDocument(api.requests.last()).toJson().contains("hello.txt")); QCOMPARE(api.requests.last().value(QStringLiteral("messages")).toArray().size(), 2);
        panel.revokeAgentAccess(); mode->setCurrentIndex(1); input->setPlainText(QStringLiteral("需要重新授权")); send->click(); QTest::qWait(20); QCOMPARE(api.requests.size(), 3); QVERIFY(!panel.busy());
    }
};

QTEST_MAIN(AgentTests)
#include "agent_tests.moc"
