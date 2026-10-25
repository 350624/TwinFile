#include "core.h"
#include "devices.h"
#include "model.h"
#include "pane.h"
#include "window.h"
#include "zip.h"
#include "android.h"
#include "thumbnails.h"
#include "android_strings.h"
#include "shell.h"
#include "appearance.h"
#include "dex.h"
#include "properties.h"
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QTimer>
#include <QBuffer>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QMimeData>
#include <QPainter>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTextStream>
#include <QThread>
#include <QUrl>
#include <QtEndian>
#include <windows.h>

using namespace twinfile;

static bool writeFixture(const QString &path, const QByteArray &bytes = QByteArray("fixture")) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

static QByteArray contents(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll();
}

static QByteArray resourceTableFixture(bool sparse = false, bool compact = false, bool offset16 = false) {
    auto shortValue = [](quint16 value) { QByteArray bytes(2, '\0'); qToLittleEndian(value, bytes.data()); return bytes; };
    auto wordValue = [](quint32 value) { QByteArray bytes(4, '\0'); qToLittleEndian(value, bytes.data()); return bytes; };
    auto chunk = [&](quint16 type, quint16 header, const QByteArray &body) { return shortValue(type) + shortValue(header) + wordValue(body.size() + 8) + body; };
    const QByteArray text("res/icon.webp"); QByteArray stringBody; stringBody += char(text.size()); stringBody += char(text.size()); stringBody += text; stringBody += '\0';
    while (stringBody.size() % 4) stringBody += '\0';
    const QByteArray pool = chunk(1, 28, wordValue(1) + wordValue(0) + wordValue(0x100) + wordValue(32) + wordValue(0) + wordValue(0) + stringBody);
    auto entry = [&](quint8 type, quint32 data) { return compact ? shortValue(0) + shortValue(8 | (quint16(type) << 8)) + wordValue(data) : shortValue(8) + shortValue(0) + wordValue(0) + shortValue(8) + char(0) + char(type) + wordValue(data); };
    const QByteArray first = entry(3, 0), second = entry(0x1c, 0xff22cc44);
    QByteArray offsets;
    if (sparse) offsets = shortValue(0) + shortValue(0) + shortValue(1) + shortValue(first.size() / 4);
    else if (offset16) offsets = shortValue(0) + shortValue(first.size() / 4);
    else offsets = wordValue(0) + wordValue(first.size());
    QByteArray config(64, '\0'); qToLittleEndian<quint32>(64, config.data()); qToLittleEndian<quint16>(480, config.data() + 14); qToLittleEndian<quint16>(33, config.data() + 24);
    const QByteArray typeBody = QByteArray(1, char(1)) + char(sparse ? 1 : offset16 ? 2 : 0) + shortValue(0) + wordValue(2) + wordValue(84 + offsets.size()) + config + offsets + first + second;
    const QByteArray package = chunk(0x200, 288, wordValue(0x7f) + QByteArray(276, '\0') + chunk(0x201, 84, typeBody));
    return chunk(2, 12, wordValue(1) + pool + package);
}

class CountingFileDelegate : public FileDelegate {
public:
    using FileDelegate::FileDelegate;
    mutable int paintCount = 0;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        ++paintCount; FileDelegate::paint(painter, option, index);
    }
};

class NativeTests : public QObject {
    Q_OBJECT
private slots:
    void propertiesCountLocalContentsAndSkipJunctions() {
        QTemporaryDir root, outside; QVERIFY(root.isValid()); QVERIFY(outside.isValid());
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("nested/empty"))));
        QVERIFY(writeFixture(root.filePath(QStringLiteral("nested/data.bin")), QByteArray(1024, 'x')));
        QVERIFY(writeFixture(root.filePath(QStringLiteral(".hidden")), QByteArray(3, 'h')));
        QVERIFY(writeFixture(root.filePath(QStringLiteral("archive.zip")), QByteArray(5, 'z')));
        QVERIFY(writeFixture(outside.filePath(QStringLiteral("outside.bin")), QByteArray(8192, 'o')));
        Entry folder; folder.name = QStringLiteral("folder"); folder.path = root.path(); folder.directory = true;
        const auto counted = inspectEntries({folder}); QVERIFY2(counted.error.isEmpty(), qPrintable(counted.error));
        QCOMPARE(counted.bytes, 1032); QCOMPARE(counted.files, 3); QCOMPARE(counted.directories, 2);
        Entry file; file.path = root.filePath(QStringLiteral("nested/data.bin")); file.size = 0;
        const auto fresh = inspectEntries({file}); QVERIFY(fresh.error.isEmpty()); QCOMPARE(fresh.bytes, 1024); QCOMPARE(fresh.files, 1);
        const auto combined = inspectEntries({file, folder, file}); QVERIFY(combined.error.isEmpty()); QCOMPARE(combined.bytes, 1032); QCOMPARE(combined.files, 3); QCOMPARE(combined.directories, 2);
        Entry empty; empty.path = root.filePath(QStringLiteral("nested/empty")); empty.directory = true;
        const auto zero = inspectEntries({empty}); QVERIFY(zero.error.isEmpty()); QCOMPARE(zero.bytes, 0); QCOMPARE(zero.files, 0); QCOMPARE(zero.directories, 0);
        const QString junction = root.filePath(QStringLiteral("link"));
        const auto linked = runProcess(QStringLiteral("C:/Windows/System32/cmd.exe"), {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"), QDir::toNativeSeparators(junction), QDir::toNativeSeparators(outside.path())});
        QVERIFY2(linked.ok, qPrintable(linked.error));
        const auto cleanup = qScopeGuard([&] { RemoveDirectoryW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(junction).utf16())); });
        const auto bounded = inspectEntries({folder}); QVERIFY(bounded.error.isEmpty()); QCOMPARE(bounded.bytes, 1032); QCOMPARE(bounded.links, 1); QCOMPARE(bounded.directories, 2);
    }
    void propertiesCountArchiveUncompressedContents() {
        QTemporaryDir root; const QString archive = root.filePath(QStringLiteral("fixture.zip"));
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("dir/nested/data.bin"), QByteArray(4096, 'x')); writer.addFile(QStringLiteral("dir/.hidden"), QByteArray(3, 'h')); writer.addDirectory(QStringLiteral("dir/empty")); writer.addFile(QStringLiteral("directory/other.bin"), QByteArray(100, 'o')); writer.addSymLink(QStringLiteral("dir/link"), QStringLiteral("../directory")); writer.close(); QCOMPARE(writer.status(), ZipWriter::NoError); }
        Entry folder; folder.source = Source::Archive; folder.path = archive; folder.inner = QStringLiteral("dir"); folder.directory = true;
        const auto counted = inspectEntries({folder}); QVERIFY2(counted.error.isEmpty(), qPrintable(counted.error));
        QCOMPARE(counted.bytes, 4099); QCOMPARE(counted.files, 2); QCOMPARE(counted.directories, 2); QCOMPARE(counted.links, 1);
        Entry file = folder; file.inner = QStringLiteral("dir/nested/data.bin"); file.directory = false; file.size = 0;
        const auto exact = inspectEntries({file}); QVERIFY(exact.error.isEmpty()); QCOMPARE(exact.bytes, 4096); QCOMPARE(exact.directories, 0);
        const auto combined = inspectEntries({file, folder}); QVERIFY(combined.error.isEmpty()); QCOMPARE(combined.bytes, 4099); QCOMPARE(combined.files, 2);
        Entry second = file; second.inner = QStringLiteral("directory/other.bin");
        const auto multiple = inspectEntries({file, second}); QVERIFY(multiple.error.isEmpty()); QCOMPARE(multiple.bytes, 4196); QCOMPARE(multiple.files, 2); QCOMPARE(multiple.directories, 0);
        folder.inner = QStringLiteral("dir/empty"); const auto empty = inspectEntries({folder}); QVERIFY(empty.error.isEmpty()); QCOMPARE(empty.bytes, 0);
        folder.inner = QStringLiteral("missing"); QVERIFY(!inspectEntries({folder}).error.isEmpty());
    }
    void propertiesCancellationAndMissingEntries() {
        QTemporaryDir root; Entry folder; folder.path = root.path(); folder.directory = true;
        auto cancel = std::make_shared<std::atomic_bool>(true); QVERIFY(inspectEntries({folder}, cancel).error.contains(QStringLiteral("取消")));
        Entry missing; missing.path = root.filePath(QStringLiteral("missing")); QVERIFY(!inspectEntries({missing}).error.isEmpty());
        QVERIFY(writeFixture(root.filePath(QStringLiteral("ok.bin")), QByteArray(16, 'x')));
        Entry file; file.path = root.filePath(QStringLiteral("ok.bin")); const auto partial = inspectEntries({file, missing}); QVERIFY(!partial.error.isEmpty()); QCOMPARE(partial.bytes, 16);
        const QString archive = root.filePath(QStringLiteral("broken.zip")); QVERIFY(writeFixture(archive, QByteArray("broken")));
        Entry invalid; invalid.source = Source::Archive; invalid.path = archive; invalid.directory = true; QVERIFY(!inspectEntries({invalid}).error.isEmpty());
    }
    void initTestCase() {
        QVERIFY(settings_.isValid()); qputenv("TWINFILE_TEST_MODE", "1");
        qputenv("TWINFILE_SETTINGS", settings_.filePath(QStringLiteral("settings.json")).toUtf8());
        qputenv("TWINFILE_TOOL_CACHE", settings_.filePath(QStringLiteral("tools")).toUtf8());
        qputenv("TWINFILE_THUMBNAIL_CACHE", settings_.filePath(QStringLiteral("thumbnails")).toUtf8());
        qApp->setWindowIcon(QIcon(QStringLiteral(":/app.ico")));
    }
    void apkSuffix_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<bool>("accepted");
        QTest::newRow("apk") << QStringLiteral("应用.apk") << true;
        QTest::newRow("caps") << QStringLiteral("应用.APK") << true;
        QTest::newRow("qq") << QStringLiteral("应用.apk.1") << true;
        QTest::newRow("qq2") << QStringLiteral("应用.APK.234") << true;
        QTest::newRow("ordinary") << QStringLiteral("file.1") << false;
        QTest::newRow("text") << QStringLiteral("file.txt.1") << false;
        QTest::newRow("false") << QStringLiteral("file.apk.abc") << false;
    }
    void apkSuffix() { QFETCH(QString, name); QFETCH(bool, accepted); QCOMPARE(isApk(name), accepted); }
    void dexValidationAndSuffix() {
        QVERIFY(isDex(QStringLiteral("classes.DEX"))); QVERIFY(!isDex(QStringLiteral("classes.dex.zip"))); QVERIFY(!isText(QStringLiteral("classes.dex")));
        QTemporaryDir directory; const QString path = directory.filePath(QStringLiteral("classes.dex"));
        QByteArray bytes(140, '\0'); bytes.replace(0, 8, QByteArray("dex\n035\0", 8));
        qToLittleEndian<quint32>(140, bytes.data() + 32); qToLittleEndian<quint32>(112, bytes.data() + 36); qToLittleEndian<quint32>(0x12345678, bytes.data() + 40);
        QVERIFY(writeFixture(path, bytes)); QVERIFY(validateDex(path).ok);
        bytes[4] = '9'; QVERIFY(writeFixture(path, bytes)); QVERIFY(!validateDex(path).ok); bytes[4] = '0';
        qToLittleEndian<quint32>(3, bytes.data() + 96); qToLittleEndian<quint32>(130, bytes.data() + 100);
        QVERIFY(writeFixture(path, bytes)); QVERIFY(!validateDex(path).ok);
        QVERIFY(writeFixture(path, QByteArray("not a dex"))); QVERIFY(!validateDex(path).ok);
    }
    void dexSmaliRoundTripAndFailedCompile() {
        QTemporaryDir directory; const QString seed = makeDexSeed(directory); QVERIFY(!seed.isEmpty());
        const QString workspace = directory.filePath(QStringLiteral("smali")); QVERIFY(QDir().mkpath(workspace));
        const QByteArray smali(".class public Lcom/example/Hello;\n.super Ljava/lang/Object;\n\n.method public static message()Ljava/lang/String;\n    .locals 1\n    const-string v0, \"Hello TwinFile\"\n    return-object v0\n.end method\n");
        QVERIFY(writeFixture(QDir(workspace).filePath(QStringLiteral("00000.smali")), smali));
        const QString compiled = directory.filePath(QStringLiteral("compiled.dex"));
        Result result = convertDex(seed, workspace, compiled); QVERIFY2(result.ok, qPrintable(result.error)); QVERIFY(validateDex(compiled).ok);
        const QString decoded = directory.filePath(QStringLiteral("decoded")); result = convertDex(compiled, decoded, {}); QVERIFY2(result.ok, qPrintable(result.error));
        const QString classPath = QDir(decoded).filePath(QStringLiteral("00000.smali")); QVERIFY(contents(classPath).contains("Hello TwinFile"));
        QByteArray modified = contents(classPath); modified.replace("Hello TwinFile", "Changed successfully"); QVERIFY(writeFixture(classPath, modified));
        const QString updated = directory.filePath(QStringLiteral("updated.dex")); result = convertDex(compiled, decoded, updated); QVERIFY2(result.ok, qPrintable(result.error));
        const QString checked = directory.filePath(QStringLiteral("checked")); QVERIFY(convertDex(updated, checked, {}).ok); QVERIFY(contents(QDir(checked).filePath(QStringLiteral("00000.smali"))).contains("Changed successfully"));
        QVERIFY(writeFixture(classPath, QByteArray(".class broken\nnot-smali"))); const QByteArray unchanged = contents(compiled);
        QVERIFY(!convertDex(compiled, decoded, directory.filePath(QStringLiteral("invalid.dex"))).ok); QCOMPARE(contents(compiled), unchanged);
        auto cancel = std::make_shared<std::atomic_bool>(true); QVERIFY(!convertDex(compiled, directory.filePath(QStringLiteral("cancelled")), {}, cancel).ok);
        QVERIFY(!convertDex(compiled, decoded, updated).ok);
    }
    void dexSaveDetectsConflictsAndWritesArchive() {
        QTemporaryDir directory; const QString seed = makeDexSeed(directory); QVERIFY(!seed.isEmpty());
        const QByteArray expected = fileDigest(seed); const QString compiled = directory.filePath(QStringLiteral("output.dex")); QVERIFY(QFile::copy(seed, compiled));
        QVERIFY(saveDex(seed, compiled, expected).ok);
        const QString archive = directory.filePath(QStringLiteral("app.apk")); QVERIFY(createArchive({seed}, archive).ok);
        Entry entry; entry.source = Source::Archive; entry.path = archive; entry.name = QFileInfo(seed).fileName(); entry.inner = entry.name;
        QVERIFY(saveDex(seed, compiled, expected, entry).ok); QString error; QCOMPARE(readArchiveFile(archive, entry.inner, 1024 * 1024, &error), contents(compiled)); QVERIFY(error.isEmpty());
        QVERIFY(writeFixture(seed, QByteArray("externally modified"))); QVERIFY(!saveDex(seed, compiled, expected).ok); QCOMPARE(contents(seed), QByteArray("externally modified"));
        auto cancel = std::make_shared<std::atomic_bool>(true); QVERIFY(!saveDex(seed, compiled, expected, {}, cancel).ok);
    }
    void dexEditorRendersAndEdits_data() {
        QTest::addColumn<QString>("theme"); QTest::addColumn<bool>("dark");
        for (const QString &theme : themeIds()) { QTest::newRow(qPrintable(theme + QStringLiteral("-dark"))) << theme << true; QTest::newRow(qPrintable(theme + QStringLiteral("-light"))) << theme << false; }
    }
    void dexEditorRendersAndEdits() {
        QFETCH(QString, theme); QFETCH(bool, dark); const auto previousPalette = qApp->palette(); const QString previousStyle = qApp->styleSheet();
        const auto restore = qScopeGuard([&] { qApp->setPalette(previousPalette); qApp->setStyleSheet(previousStyle); });
        const auto palette = appearancePalette(theme, dark, QStringLiteral("#5b8def"));
        qApp->setPalette(palette); qApp->setStyleSheet(appearanceStyleSheet(theme, dark, palette));
        QTemporaryDir directory; const QString seed = makeDexSeed(directory); QVERIFY(!seed.isEmpty());
        const QString workspace = directory.filePath(QStringLiteral("source")); QVERIFY(QDir().mkpath(workspace));
        QVERIFY(writeFixture(QDir(workspace).filePath(QStringLiteral("00000.smali")), QByteArray(".class public Lcom/example/MainActivity;\n.super Ljava/lang/Object;\n\n.field public static final VERSION:I = 0x1\n\n.method public static greeting()Ljava/lang/String;\n    .locals 1\n    const-string v0, \"Hello TwinFile\"\n    return-object v0\n.end method\n")));
        const QString path = directory.filePath(QStringLiteral("classes.dex")); const Result result = convertDex(seed, workspace, path); QVERIFY2(result.ok, qPrintable(result.error));
        auto *dialog = new DexDialog(path, nullptr); QPointer<DexDialog> guard(dialog); dialog->setAttribute(Qt::WA_DontShowOnScreen); dialog->show();
        const auto cleanup = qScopeGuard([&] { if (guard) delete guard; });
        auto *save = dialog->findChild<QPushButton *>(QStringLiteral("dexSave")); auto *tree = dialog->findChild<QTreeWidget *>(QStringLiteral("dexClasses")); auto *editor = dialog->findChild<QPlainTextEdit *>(QStringLiteral("dexCode"));
        QVERIFY(save); QVERIFY(tree); QVERIFY(editor); QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000); QCOMPARE(tree->topLevelItemCount(), 1); QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("com.example"));
        QVERIFY(editor->toPlainText().contains(QStringLiteral("Hello TwinFile"))); QCOMPARE(dialog->findChild<QComboBox *>(QStringLiteral("dexMembers"))->count(), 2);
        auto *filter = dialog->findChild<QLineEdit *>(QStringLiteral("dexFilter")); filter->setText(QStringLiteral("missing")); QVERIFY(tree->topLevelItem(0)->child(0)->isHidden()); filter->clear(); QVERIFY(!tree->topLevelItem(0)->child(0)->isHidden());
        const QString evidence = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT"); if (!evidence.isEmpty()) QVERIFY(dialog->grab().save(QDir(evidence).filePath(QStringLiteral("native-dex-editor-%1-%2.png").arg(theme, dark ? QStringLiteral("dark") : QStringLiteral("light")))));
        auto cursor = editor->textCursor(); cursor.select(QTextCursor::Document); cursor.insertText(editor->toPlainText().replace(QStringLiteral("Hello TwinFile"), QStringLiteral("Edited in Dex++")));
        QTimer approval; connect(&approval, &QTimer::timeout, dialog, [] { for (auto *widget : QApplication::topLevelWidgets()) if (auto *box = qobject_cast<QMessageBox *>(widget)) if (auto *button = box->button(QMessageBox::Yes)) button->click(); }); approval.start(25);
        save->click(); QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000); approval.stop(); QVERIFY2(dialog->findChild<QLabel *>(QStringLiteral("dexStatus"))->text().startsWith(QStringLiteral("已编译保存")), qPrintable(dialog->findChild<QLabel *>(QStringLiteral("dexStatus"))->text()));
        const QString checked = directory.filePath(QStringLiteral("after")); QVERIFY(convertDex(path, checked, {}).ok); QVERIFY(contents(QDir(checked).filePath(QStringLiteral("00000.smali"))).contains("Edited in Dex++"));
    }
    void dexRealApkRoundTripAndWindowRouting() {
        QTemporaryDir directory; ZipReader apk(toolPath(QStringLiteral("hfx/HybridFileXfer-phone.apk")));
        const QByteArray dex = apk.fileData(QStringLiteral("classes.dex")); QVERIFY(dex.size() > 1000);
        const QString source = directory.filePath(QStringLiteral("classes.dex")); QVERIFY(writeFixture(source, dex));
        const QString decoded = directory.filePath(QStringLiteral("decoded")); Result result = convertDex(source, decoded, {}); QVERIFY2(result.ok, qPrintable(result.error));
        const QStringList classes = QDir(decoded).entryList({QStringLiteral("*.smali")}, QDir::Files); QVERIFY(classes.size() > 10);
        const QString output = directory.filePath(QStringLiteral("roundtrip.dex")); result = convertDex(source, decoded, output); QVERIFY2(result.ok, qPrintable(result.error)); QVERIFY(validateDex(output).ok);
        const QString checked = directory.filePath(QStringLiteral("checked")); result = convertDex(output, checked, {}); QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(QDir(checked).entryList({QStringLiteral("*.smali")}, QDir::Files).size(), classes.size());
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.setReducedMotion(true); window.show(); auto *pane = window.leftPane(); pane->navigate({Source::Local, directory.path(), {}, {}}); QTRY_VERIFY(pane->fileModel()->rowCount() >= 4);
        int row = -1; for (int index = 0; index < pane->fileModel()->rowCount(); ++index) if (pane->fileModel()->entry(index).name == QStringLiteral("classes.dex")) row = index; QVERIFY(row >= 0);
        window.setActive(pane); QApplication::setActiveWindow(&window); pane->view()->setFocus(); QTRY_VERIFY(pane->view()->hasFocus()); pane->view()->setCurrentIndex(pane->fileModel()->index(row)); QTest::keyClick(pane->view(), Qt::Key_Return);
        QTRY_VERIFY(window.findChild<QDialog *>(QStringLiteral("dexEditor"))); auto *dialog = window.findChild<QDialog *>(QStringLiteral("dexEditor")); QTRY_VERIFY_WITH_TIMEOUT(dialog->findChild<QPushButton *>(QStringLiteral("dexSave"))->isEnabled(), 30000);
        QTimer fallback; connect(&fallback, &QTimer::timeout, &window, [] { for (auto *widget : QApplication::topLevelWidgets()) if (auto *box = qobject_cast<QMessageBox *>(widget)) if (auto *button = box->button(QMessageBox::Yes)) button->click(); }); fallback.start(8000); dialog->close(); QVERIFY(!dialog->isVisible());
    }
    void unsafeArchiveNames_data() {
        QTest::addColumn<QString>("name");
        for (const QString &name : {QStringLiteral("../escape"), QStringLiteral("/absolute"), QStringLiteral("C:/absolute"), QStringLiteral("a/../../bad"), QStringLiteral("a\\bad"), QStringLiteral("a/CON.txt"), QStringLiteral("a/file."), QStringLiteral("a/./file")}) QTest::newRow(qPrintable(name)) << name;
    }
    void unsafeArchiveNames() { QFETCH(QString, name); QVERIFY(safeArchiveName(name).isEmpty()); }
    void validUnicodeArchiveName() { QCOMPARE(safeArchiveName(QStringLiteral("文件夹/中文 空格.txt")), QStringLiteral("文件夹/中文 空格.txt")); }
    void filenameValidation() {
        for (const QString &name : {QStringLiteral("NUL"), QStringLiteral("COM1.log"), QStringLiteral("x/y"), QStringLiteral("x:y"), QStringLiteral("file ")}) QVERIFY(!validateName(name).isEmpty());
        QVERIFY(validateName(QStringLiteral("中文 空格.txt")).isEmpty());
    }
    void shellArgumentsQuoted() { QCOMPARE(shellQuote(QStringLiteral("a'b 中文")), QStringLiteral("'a'\\''b 中文'")); }
    void localListingFiltersAndSorts() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("folder")));
        QVERIFY(writeFixture(directory.filePath(QStringLiteral("second.txt")), QByteArray(200, 'x')));
        QVERIFY(writeFixture(directory.filePath(QStringLiteral("first.txt")), QByteArray(10, 'x')));
        Listing listing = listLocal(directory.path(), false, QStringLiteral("size"), true);
        QVERIFY(listing.error.isEmpty()); QCOMPARE(listing.entries.size(), 3);
        QCOMPARE(listing.entries[0].name, QStringLiteral("folder")); QCOMPARE(listing.entries[1].name, QStringLiteral("second.txt"));
    }
    void unicodeCopyPreservesSource() {
        QTemporaryDir directory; QDir root(directory.path()); QVERIFY(root.mkdir(QStringLiteral("target")));
        const QString source = directory.filePath(QStringLiteral("中文 空格 #%.txt")); QVERIFY(writeFixture(source, QByteArray("original")));
        const Result result = transferFiles({source}, directory.filePath(QStringLiteral("target")), false);
        QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(contents(source), QByteArray("original")); QCOMPARE(contents(result.paths.first()), QByteArray("original"));
    }
    void copyConflictDoesNotOverwriteOrPartiallyStart() {
        QTemporaryDir directory; QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("target")));
        const QString first = directory.filePath(QStringLiteral("first.txt")), second = directory.filePath(QStringLiteral("second.txt"));
        QVERIFY(writeFixture(first)); QVERIFY(writeFixture(second)); QVERIFY(writeFixture(directory.filePath(QStringLiteral("target/second.txt")), QByteArray("keep")));
        QVERIFY(!transferFiles({first, second}, directory.filePath(QStringLiteral("target")), false).ok);
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("target/first.txt")))); QCOMPARE(contents(directory.filePath(QStringLiteral("target/second.txt"))), QByteArray("keep"));
    }
    void copyIntoSelfRejected() {
        QTemporaryDir directory; QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("child")));
        QVERIFY(!transferFiles({directory.path()}, directory.filePath(QStringLiteral("child")), false).ok);
    }
    void parentAndChildSelectionRejected() {
        QTemporaryDir directory; QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("source"))); QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("target")));
        const QString source = directory.filePath(QStringLiteral("source")), file = directory.filePath(QStringLiteral("source/file.txt")); QVERIFY(writeFixture(file));
        QVERIFY(!transferFiles({source, file}, directory.filePath(QStringLiteral("target")), true).ok); QVERIFY(QFileInfo::exists(file));
    }
    void cancelledCopyDoesNotCreateTarget() {
        QTemporaryDir directory; QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("target"))); const QString source = directory.filePath(QStringLiteral("first.txt")); QVERIFY(writeFixture(source));
        auto cancel = std::make_shared<std::atomic_bool>(true);
        QVERIFY(!transferFiles({source}, directory.filePath(QStringLiteral("target")), false, cancel).ok);
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("target/first.txt")))); QVERIFY(QFileInfo::exists(source));
    }
    void sameVolumeMoveUsesNativeRename() {
        QTemporaryDir directory; QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("target"))); const QString source = directory.filePath(QStringLiteral("move.txt")); QVERIFY(writeFixture(source));
        const Result result = transferFiles({source}, directory.filePath(QStringLiteral("target")), true);
        QVERIFY2(result.ok, qPrintable(result.error)); QVERIFY(!QFileInfo::exists(source)); QCOMPARE(contents(result.paths.first()), QByteArray("fixture"));
    }
    void batchRenameAllowsNameSwaps() {
        QTemporaryDir directory; const QString first = directory.filePath(QStringLiteral("first.txt")), second = directory.filePath(QStringLiteral("second.txt"));
        QVERIFY(writeFixture(first, QByteArray("first"))); QVERIFY(writeFixture(second, QByteArray("second")));
        const Result result = batchRename({first, second}, {QStringLiteral("second.txt"), QStringLiteral("first.txt")});
        QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(contents(first), QByteArray("second")); QCOMPARE(contents(second), QByteArray("first"));
    }
    void batchRenamePreflightsConflicts() {
        QTemporaryDir directory; const QString source = directory.filePath(QStringLiteral("first.txt")), target = directory.filePath(QStringLiteral("second.txt")); QVERIFY(writeFixture(source)); QVERIFY(writeFixture(target, QByteArray("keep")));
        QVERIFY(!batchRename({source}, {QStringLiteral("second.txt")}).ok); QCOMPARE(contents(target), QByteArray("keep")); QVERIFY(QFileInfo::exists(source));
    }
    void archiveRoundTripPreservesUnicode() {
        QTemporaryDir directory; const QString source = directory.filePath(QStringLiteral("中文 空格.txt")); QVERIFY(writeFixture(source, QByteArray("archive test"))); QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("output")));
        const QString archive = directory.filePath(QStringLiteral("test.zip")); const Result zipped = createArchive({source}, archive);
        QVERIFY2(zipped.ok, qPrintable(zipped.error)); const Listing listing = listArchive(archive, {}); QVERIFY(listing.error.isEmpty()); QCOMPARE(listing.entries.size(), 1); QCOMPARE(listing.entries.first().name, QFileInfo(source).fileName());
        const Result extracted = extractArchive(archive, directory.filePath(QStringLiteral("output"))); QVERIFY2(extracted.ok, qPrintable(extracted.error)); QCOMPARE(contents(directory.filePath(QStringLiteral("output/中文 空格.txt"))), QByteArray("archive test"));
    }
    void archiveRejectsOutputInsideSourceThroughJunction() {
        QTemporaryDir source, aliases;
        QVERIFY(QDir(source.path()).mkdir(QStringLiteral("child")));
        QVERIFY(writeFixture(source.filePath(QStringLiteral("original.txt")), "keep"));
        const QString junction = aliases.filePath(QStringLiteral("linked"));
        const auto created = runProcess(QStringLiteral("C:/Windows/System32/cmd.exe"), {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"), QDir::toNativeSeparators(junction), QDir::toNativeSeparators(source.filePath(QStringLiteral("child")))});
        QVERIFY2(created.ok, qPrintable(created.error));
        const auto cleanup = qScopeGuard([&] { RemoveDirectoryW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(junction).utf16())); });
        auto cancel = std::make_shared<std::atomic_bool>(true);
        const Result copied = transferFiles({source.path()}, junction, false, cancel);
        QVERIFY(!copied.ok); QVERIFY2(copied.error.contains(QStringLiteral("自身及子文件夹")), qPrintable(copied.error));
        const Result result = createArchive({source.path()}, QDir(junction).filePath(QStringLiteral("self.zip")), cancel);
        QVERIFY(!result.ok); QVERIFY2(result.error.contains(QStringLiteral("来源文件夹内部")), qPrintable(result.error));
        QVERIFY(!QFileInfo::exists(source.filePath(QStringLiteral("child/self.zip")))); QCOMPARE(contents(source.filePath(QStringLiteral("original.txt"))), QByteArray("keep"));
    }
    void archiveAllowsUnrelatedJunctionDestination() {
        QTemporaryDir source, destination, aliases;
        const QString original = source.filePath(QStringLiteral("original.txt")); QVERIFY(writeFixture(original, "keep"));
        const QString junction = aliases.filePath(QStringLiteral("linked"));
        const auto created = runProcess(QStringLiteral("C:/Windows/System32/cmd.exe"), {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"), QDir::toNativeSeparators(junction), QDir::toNativeSeparators(destination.path())});
        QVERIFY2(created.ok, qPrintable(created.error));
        const auto cleanup = qScopeGuard([&] { RemoveDirectoryW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(junction).utf16())); });
        const Result result = createArchive({original}, QDir(junction).filePath(QStringLiteral("valid.zip")));
        QVERIFY2(result.ok, qPrintable(result.error));
        QString error; QCOMPARE(readArchiveFile(destination.filePath(QStringLiteral("valid.zip")), QStringLiteral("original.txt"), 64, &error), QByteArray("keep")); QVERIFY(error.isEmpty());
    }
    void archiveTraversalRejectedBeforeWriting() {
        QTemporaryDir directory; const QString archive = directory.filePath(QStringLiteral("bad.zip"));
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("good.txt"), QByteArray("good")); writer.addFile(QStringLiteral("../escape.txt"), QByteArray("bad")); writer.close(); }
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("output"))); QVERIFY(!extractArchive(archive, directory.filePath(QStringLiteral("output"))).ok);
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("escape.txt")))); QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("output/good.txt"))));
    }
    void archiveSymlinkRejected() {
        QTemporaryDir directory; const QString archive = directory.filePath(QStringLiteral("bad.zip"));
        { ZipWriter writer(archive); writer.addSymLink(QStringLiteral("link"), QStringLiteral("../outside")); writer.close(); }
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("output"))); QVERIFY(!extractArchive(archive, directory.filePath(QStringLiteral("output"))).ok);
    }
    void archiveDoesNotOverwriteExistingFile() {
        QTemporaryDir directory; const QString archive = directory.filePath(QStringLiteral("test.zip"));
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("same.txt"), QByteArray("new")); writer.close(); }
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("output"))); QVERIFY(writeFixture(directory.filePath(QStringLiteral("output/same.txt")), QByteArray("keep")));
        QVERIFY(!extractArchive(archive, directory.filePath(QStringLiteral("output"))).ok); QCOMPARE(contents(directory.filePath(QStringLiteral("output/same.txt"))), QByteArray("keep"));
    }
    void archiveOutputInsideSourceRejected() {
        QTemporaryDir directory; QVERIFY(!createArchive({directory.path()}, directory.filePath(QStringLiteral("inside.zip"))).ok); QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("inside.zip"))));
    }
    void archiveRejectsOverlappingSources() {
        QTemporaryDir directory; QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("source")));
        const QString file = directory.filePath(QStringLiteral("source/file.txt")); QVERIFY(writeFixture(file));
        const QString archive = directory.filePath(QStringLiteral("result.zip"));
        QVERIFY(!createArchive({directory.filePath(QStringLiteral("source")), file}, archive).ok);
        QVERIFY(!QFileInfo::exists(archive));
    }
    void phonePullRejectsWindowsTraversalBeforeStartingAdb() {
        QTemporaryDir directory; Entry entry; entry.source = Source::Phone; entry.name = QStringLiteral("..\\outside.txt"); entry.path = QStringLiteral("/sdcard/..\\outside.txt"); entry.serial = QStringLiteral("fixture");
        const Result result = phoneTransfer({entry}, {Source::Local, directory.path(), {}, {}}, false);
        QVERIFY(!result.ok); QVERIFY(result.error.contains(QStringLiteral("非法文件名")));
    }
    void archiveReadLimitEnforced() {
        QTemporaryDir directory; const QString archive = directory.filePath(QStringLiteral("test.zip"));
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("file.txt"), QByteArray(100, 'x')); writer.close(); }
        QString error; QVERIFY(readArchiveFile(archive, QStringLiteral("file.txt"), 10, &error).isEmpty()); QVERIFY(!error.isEmpty());
    }
    void zipRejectsDuplicateRawNames() {
        QTemporaryDir directory; const QString archive = directory.filePath(QStringLiteral("duplicate.zip"));
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("same.txt"), QByteArray("first")); writer.addFile(QStringLiteral("same.txt"), QByteArray("second")); }
        ZipReader reader(archive); QCOMPARE(reader.status(), ZipReader::FileError); QVERIFY(reader.fileInfoList().isEmpty());
    }
    void drawableVectorAndAdaptiveLayers() {
        QTemporaryDir directory; const QString archive = directory.filePath(QStringLiteral("drawable.apk.1"));
        { ZipWriter writer(archive);
          writer.addFile(QStringLiteral("res/vector.xml"), QByteArray("<vector xmlns:android=\"http://schemas.android.com/apk/res/android\" android:viewportWidth=\"24\" android:viewportHeight=\"24\"><path android:fillColor=\"#ffff0000\" android:pathData=\"M0,0 H24 V24 H0 Z\"/></vector>"));
          writer.addFile(QStringLiteral("res/adaptive.xml"), QByteArray("<adaptive-icon><background drawable=\"#ff00ff00\"/><foreground drawable=\"@0x7f010001\"/></adaptive-icon>")); }
        ZipReader reader(archive); AndroidResources resources{{QStringLiteral("@0x7f010001"), QStringLiteral("res/vector.xml")}};
        const QImage vector = androidDrawable(reader, resources, QStringLiteral("res/vector.xml"));
        QVERIFY(!vector.isNull()); QCOMPARE(vector.pixelColor(64, 64), QColor(Qt::red));
        const QImage adaptive = androidDrawable(reader, resources, QStringLiteral("res/adaptive.xml"));
        QVERIFY(!adaptive.isNull()); QCOMPARE(adaptive.pixelColor(64, 64), QColor(Qt::red)); QCOMPARE(adaptive.pixelColor(0, 0).alpha(), 0);
    }
    void drawableResourceResolutionBounded() {
        const AndroidResources resources = androidResources(QStringLiteral("resource 0x7f010001 pkg:drawable/icon\n  (mdpi) (file) res/mdpi/icon.png type=PNG\n  (xxxhdpi) (file) res/xxxhdpi/icon.png type=PNG\n"));
        QCOMPARE(resources.value(QStringLiteral("@0x7f010001")), QStringLiteral("res/xxxhdpi/icon.png"));
        QTemporaryDir directory; const QString archive = directory.filePath(QStringLiteral("empty.zip"));
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("dummy"), QByteArray("dummy")); }
        ZipReader reader(archive); const AndroidResources cycle{{QStringLiteral("@0x7f010001"), QStringLiteral("@0x7f010001")}};
        QVERIFY(androidDrawable(reader, cycle, QStringLiteral("@0x7f010001")).isNull());
        QString error; QVERIFY(androidXml(QByteArray("<broken"), &error).isNull()); QVERIFY(!error.isEmpty());
        error.clear(); QVERIFY(androidXml(QByteArray(50, '\0'), &error).isNull()); QVERIFY(!error.isEmpty());
        error.clear(); QVERIFY(androidXml(QByteArray("<!DOCTYPE x [<!ENTITY a 'bad'>]><x>&a;</x>"), &error).isNull()); QVERIFY(!error.isEmpty());
        error.clear(); QVERIFY(androidXml(QByteArray("<x>") + QByteArray("<x>").repeated(20) + QByteArray("</x>").repeated(21), &error).isNull()); QVERIFY(!error.isEmpty());
    }
    void nativeResourceTables_data() {
        QTest::addColumn<bool>("sparse"); QTest::addColumn<bool>("compact"); QTest::addColumn<bool>("offset16");
        QTest::newRow("dense") << false << false << false;
        QTest::newRow("sparse") << true << false << false;
        QTest::newRow("compact") << false << true << false;
        QTest::newRow("offset16") << false << false << true;
    }
    void androidStringPoolsSupportModifiedUtf8() {
        bool valid; const QByteArray modified = QByteArray::fromHex("eda0bdedb880");
        QCOMPARE(androidStringPoolText(modified, true, &valid), QString::fromUtf8("😀")); QVERIFY(valid);
        QCOMPARE(androidStringPoolText(QByteArray::fromHex("c080"), true, &valid), QString(QChar(0))); QVERIFY(valid);
        QVERIFY(androidStringPoolText(QByteArray::fromHex("f08080"), true, &valid).isEmpty()); QVERIFY(!valid);
        QCOMPARE(androidStringPoolText(QByteArray("plain"), true, &valid), QStringLiteral("plain")); QVERIFY(valid);
    }
    void nativeResourceTables() {
        QFETCH(bool, sparse); QFETCH(bool, compact); QFETCH(bool, offset16);
        const QByteArray table = resourceTableFixture(sparse, compact, offset16); const auto resources = androidResources(table);
        QCOMPARE(resources.value(QStringLiteral("@0x7f010000")), QStringLiteral("res/icon.webp")); QCOMPARE(resources.value(QStringLiteral("@0x7f010001")), QStringLiteral("#ff22cc44"));
        QVERIFY(androidResources(table.first(table.size() - 1)).isEmpty());
        auto cancel = std::make_shared<std::atomic_bool>(true); QVERIFY(androidResources(table, cancel).isEmpty());
        for (qsizetype length = 0; length < 64; ++length) QVERIFY(androidResources(table.first(length)).isEmpty());
    }
    void adaptiveIconIgnoresMonochromeAndRejectsMissingLayers() {
        QTemporaryDir directory; const QString archive = directory.filePath(QStringLiteral("icon.apk")); { ZipWriter writer(archive); writer.addFile(QStringLiteral("dummy"), QByteArray("test")); }
        ZipReader reader(archive);
        const QString xml = QStringLiteral("<adaptive-icon><background drawable=\"#ffffffff\"/><foreground drawable=\"#ffff0000\"/><monochrome drawable=\"#ff000000\"/></adaptive-icon>");
        const QImage icon = androidDrawable(reader, {}, xml); QVERIFY(!icon.isNull()); QCOMPARE(icon.pixelColor(64, 64), QColor(Qt::red)); QCOMPARE(icon.pixelColor(0, 0).alpha(), 0);
        QVERIFY(androidDrawable(reader, {}, QStringLiteral("<adaptive-icon><background drawable=\"#ffffffff\"/><foreground drawable=\"@0x7f010001\"/></adaptive-icon>")).isNull());
    }
    void webpApkIconsDecodeWithoutQtPluginOrAapt() {
        const QByteArray webp = QByteArray::fromBase64("UklGRh4AAABXRUJQVlA4TBEAAAAvD8ADAAdQ31LUqP+BiOh/AAA=");
        const QImage decoded = androidBitmap(webp); QVERIFY(!decoded.isNull()); QCOMPARE(decoded.pixelColor(64, 64), QColor(20, 190, 70));
        QVERIFY(androidBitmap(webp.first(12)).isNull()); QVERIFY(androidBitmap(webp, 4096).isNull());
        QTemporaryDir directory; const QString archive = directory.filePath(QStringLiteral("图标.apk.1"));
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("AndroidManifest.xml"), QByteArray("<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"><application android:icon=\"@0x7f010000\"/></manifest>")); writer.addFile(QStringLiteral("resources.arsc"), resourceTableFixture()); writer.addFile(QStringLiteral("res/icon.webp"), webp); }
        const QImage icon = apkIcon(archive); QCOMPARE(icon, decoded);
    }
    void thumbnailCacheTracksIdentityAndOnlyHandlesSupportedFiles() {
        QTemporaryDir directory; const QString path = directory.filePath(QStringLiteral("file.png")); QImage image(40, 40, QImage::Format_ARGB32); image.fill(Qt::red); QVERIFY(image.save(path));
        const auto listing = listLocal(directory.path(), false, QStringLiteral("name"), false); QCOMPARE(listing.entries.size(), 1); Entry entry = listing.entries.first();
        const QImage first = loadThumbnail(entry); QVERIFY(!first.isNull()); QCOMPARE(cachedThumbnail(entry), first); QCOMPARE(loadThumbnail(entry), first);
        QVERIFY(QFileInfo(QDir(qEnvironmentVariable("TWINFILE_THUMBNAIL_CACHE")).filePath(thumbnailCacheKey(entry) + QStringLiteral(".png"))).isFile());
        Entry changed = entry; changed.size += 1; QVERIFY(thumbnailCacheKey(changed) != thumbnailCacheKey(entry)); QVERIFY(cachedThumbnail(changed).isNull());
        changed = entry; changed.modified = changed.modified.addMSecs(1); QVERIFY(cachedThumbnail(changed).isNull());
        image.fill(Qt::blue); QVERIFY(image.save(path));
        QFile updated(path); QVERIFY(updated.open(QIODevice::ReadWrite)); QVERIFY(updated.setFileTime(entry.modified.addSecs(2), QFileDevice::FileModificationTime)); updated.close();
        const auto refreshed = listLocal(directory.path(), false, QStringLiteral("name"), false); const QImage replacement = loadThumbnail(refreshed.entries.first()); QVERIFY(!replacement.isNull()); QCOMPARE(replacement.pixelColor(64, 64), QColor(Qt::blue));
        entry.name = QStringLiteral("file.zip"); QVERIFY(!supportsThumbnail(entry)); QVERIFY(loadThumbnail(entry).isNull());
        entry.name = QStringLiteral("file.exe"); QVERIFY(supportsThumbnail(entry)); entry.source = Source::Phone; QVERIFY(!supportsThumbnail(entry)); QCOMPARE(thumbnailPool()->maxThreadCount(), 4);
    }
    void windowsExecutableIconsAcceptQtPaths() {
        const QString path = QDir::fromNativeSeparators(QCoreApplication::applicationFilePath()); QVERIFY(path.contains(QChar('/')));
        QVERIFY(!shellThumbnail(path).isNull());
        auto cancel = std::make_shared<std::atomic_bool>(true); QVERIFY(shellThumbnail(path, cancel).isNull());
    }
    void thumbnailResultsArrivePerFileAndSurviveFolderRevisit() {
        QTemporaryDir directory;
        for (int number = 0; number < 12; ++number) { QImage image(32, 32, QImage::Format_ARGB32); image.fill(QColor::fromHsv(number * 20, 200, 200)); QVERIFY(image.save(directory.filePath(QStringLiteral("image-%1.png").arg(number)))); }
        Pane pane; pane.setAttribute(Qt::WA_DontShowOnScreen); pane.resize(500, 900); pane.show(); pane.navigate({Source::Local, directory.path(), {}, {}}); QTRY_COMPARE(pane.fileModel()->rowCount(), 12);
        QSignalSpy changed(pane.fileModel(), &FileModel::dataChanged);
        QTRY_VERIFY_WITH_TIMEOUT(!pane.fileModel()->thumbnail(11).isNull(), 5000); QVERIFY(!changed.isEmpty());
        for (const auto &signal : changed) QCOMPARE(signal[0].value<QModelIndex>().row(), signal[1].value<QModelIndex>().row());
        const QImage icon = pane.fileModel()->thumbnail(0); QVERIFY(!icon.isNull()); pane.navigate({}); pane.navigate({Source::Local, directory.path(), {}, {}}); QTRY_COMPARE(pane.fileModel()->rowCount(), 12); QTRY_COMPARE(pane.fileModel()->thumbnail(0), icon);
    }
    void bundledApkIconAndBinaryManifest() {
        const QString path = toolPath(QStringLiteral("hfx/HybridFileXfer-phone.apk"));
        if (path.isEmpty()) QSKIP("Optional bundled APK not present");
        ZipReader archive(path); const QByteArray manifest = archive.fileData(QStringLiteral("AndroidManifest.xml"));
        QVERIFY(!manifest.isEmpty()); QString error;
        const QDomDocument document = androidXml(manifest, &error);
        QVERIFY2(!document.isNull(), qPrintable(error)); QCOMPARE(document.documentElement().tagName(), QStringLiteral("manifest"));
        const QImage icon = apkIcon(path); QVERIFY2(!icon.isNull(), "Native aapt2 / APK icon decoding failed");
    }
    void zipCaseCollisionPreflightProtectsDestination() {
        QTemporaryDir directory; const QString archive = directory.filePath(QStringLiteral("case.zip"));
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("same.txt"), QByteArray("first")); writer.addFile(QStringLiteral("SAME.txt"), QByteArray("second")); }
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("output")));
        QVERIFY(!extractArchive(archive, directory.filePath(QStringLiteral("output"))).ok);
        QVERIFY(QDir(directory.filePath(QStringLiteral("output"))).entryList(QDir::Files).isEmpty());
    }
    void thumbnailsSurviveUnchangedRefresh() {
        FileModel model; Entry entry; entry.name = QStringLiteral("icon.apk"); entry.path = QStringLiteral("D:/fixture/icon.apk"); entry.size = 10;
        model.replace({entry}); QImage thumbnail(8, 8, QImage::Format_ARGB32); thumbnail.fill(Qt::blue); model.setThumbnail(entry.key(), thumbnail);
        model.replace({entry}); QCOMPARE(model.thumbnail(0), thumbnail);
        entry.size = 20; model.replace({entry}); QVERIFY(model.thumbnail(0).isNull());
    }
    void fileIconsUseRoundedMasksAndCorrectFit_data() {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("ratio"); QTest::addColumn<bool>("selected");
        for (bool dark : {false, true}) for (int ratio : {1, 2}) for (bool selected : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2x-%3").arg(dark ? QStringLiteral("dark") : QStringLiteral("light")).arg(ratio).arg(selected ? QStringLiteral("selected") : QStringLiteral("idle")))) << dark << ratio << selected;
    }
    void fileIconsUseRoundedMasksAndCorrectFit() {
        QFETCH(bool, dark); QFETCH(int, ratio); QFETCH(bool, selected);
        const QPalette palette = appearancePalette(QStringLiteral("twinfile"), dark, QStringLiteral("#5b8def"));
        const QColor background = selected ? palette.highlight().color() : palette.base().color();
        FileModel model; FileDelegate delegate; Entry entry; entry.name = QStringLiteral("应用.apk.1"); entry.path = QStringLiteral("D:/fixture/app.apk.1");
        auto render = [&](const QImage &source) {
            model.replace({entry}); if (!source.isNull()) model.setThumbnail(entry.key(), source);
            QImage output(400 * ratio, 54 * ratio, QImage::Format_ARGB32_Premultiplied); output.setDevicePixelRatio(ratio); output.fill(palette.base().color());
            QPainter painter(&output); QStyleOptionViewItem option; option.rect = QRect(0, 0, 400, 54); option.palette = palette; option.font = qApp->font(); option.fontMetrics = QFontMetrics(option.font);
            option.state = QStyle::State_Enabled | (selected ? QStyle::State_Selected : QStyle::State_None);
            delegate.paint(&painter, option, model.index(0)); painter.end(); return output;
        };
        auto pixel = [ratio](const QImage &image, int horizontal, int vertical) { return image.pixelColor(horizontal * ratio, vertical * ratio); };
        QImage opaque(80, 80, QImage::Format_ARGB32); opaque.fill(Qt::red);
        const QImage original = opaque.copy(); const QImage apk = render(opaque);
        QCOMPARE(pixel(apk, 11, 9), background); QCOMPARE(pixel(apk, 29, 27), QColor(Qt::red)); QCOMPARE(opaque, original); QCOMPARE(model.thumbnail(0), original);
        QCOMPARE(render(opaque), apk);
        opaque.fill(Qt::blue); QCOMPARE(pixel(render(opaque), 29, 27), QColor(Qt::blue));
        for (const QSize &size : {QSize(16, 160), QSize(160, 16)}) {
            entry.name = QStringLiteral("图片.png"); QImage image(size, QImage::Format_ARGB32); image.fill(Qt::red);
            QPainter source(&image); source.fillRect(QRect((size.width() - 16) / 2, (size.height() - 16) / 2, 16, 16), Qt::blue); source.end();
            const QImage preview = render(image);
            QCOMPARE(pixel(preview, 11, 9), background); QCOMPARE(pixel(preview, 16, 27), QColor(Qt::blue)); QCOMPARE(pixel(preview, 41, 27), QColor(Qt::blue));
        }
        entry.name = QStringLiteral("程序.exe"); QImage wide(80, 40, QImage::Format_ARGB32); wide.fill(Qt::red);
        QPainter source(&wide); source.fillRect(QRect(40, 0, 40, 40), Qt::blue); source.end();
        const QImage executable = render(wide);
        QCOMPARE(pixel(executable, 16, 27), QColor(Qt::red)); QCOMPARE(pixel(executable, 41, 27), QColor(Qt::blue));
        QVERIFY(pixel(executable, 29, 13) != QColor(Qt::red)); QVERIFY(pixel(executable, 29, 13) != QColor(Qt::blue));
        entry.name = QStringLiteral("程序.ico"); QCOMPARE(render(wide).copy(11 * ratio, 9 * ratio, 36 * ratio, 36 * ratio), executable.copy(11 * ratio, 9 * ratio, 36 * ratio, 36 * ratio));
        QImage transparent(80, 80, QImage::Format_ARGB32); transparent.fill(Qt::transparent);
        QPainter symbol(&transparent); symbol.fillRect(QRect(20, 20, 40, 40), Qt::green); symbol.end();
        const QImage application = render(transparent); QCOMPARE(pixel(application, 29, 27), QColor(Qt::green)); QVERIFY(pixel(application, 15, 27) != QColor(Qt::green));
        for (const QString &name : {QStringLiteral("归档.zip"), QStringLiteral("文档.pdf"), QStringLiteral("照片.png"), QStringLiteral("设置.json"), QStringLiteral("音乐.flac"), QStringLiteral("视频.mp4"), QStringLiteral("笔记.txt")}) {
            entry.name = name; model.replace({}); const QImage fallback = render({});
            QCOMPARE(pixel(fallback, 11, 9), background); QVERIFY(pixel(fallback, 29, 27) != background);
            bool titleVisible = false;
            for (int horizontal = 60; horizontal < 180; ++horizontal) for (int vertical = 8; vertical < 24; ++vertical)
                if (pixel(fallback, horizontal, vertical) != background) titleVisible = true;
            QVERIFY(titleVisible);
        }
    }
    void maskedFileIconsRenderWithoutUserSettings_data() {
        QTest::addColumn<bool>("dark"); QTest::newRow("dark") << true; QTest::newRow("light") << false;
    }
    void maskedFileIconsRenderWithoutUserSettings() {
        QFETCH(bool, dark);
        const QPalette previousPalette = qApp->palette(); const QString previousStyle = qApp->styleSheet();
        const auto restore = qScopeGuard([&] { qApp->setPalette(previousPalette); qApp->setStyleSheet(previousStyle); });
        const QPalette palette = appearancePalette(QStringLiteral("twinfile"), dark, QStringLiteral("#5b8def"));
        qApp->setPalette(palette); qApp->setStyleSheet(appearanceStyleSheet(QStringLiteral("twinfile"), dark, palette));
        FileModel model; FileView view; view.setAttribute(Qt::WA_DontShowOnScreen); view.setAnimationsEnabled(false); view.setModel(&model); view.resize(440, 650);
        QList<Entry> entries;
        for (const QString &name : {QStringLiteral("工作文件夹"), QStringLiteral("HybridFileXfer.apk"), QStringLiteral("TwinFile.exe"), QStringLiteral("手机长截图.png"), QStringLiteral("宽幅照片.jpg"), QStringLiteral("资料归档.zip"), QStringLiteral("使用说明.pdf"), QStringLiteral("配置.json"), QStringLiteral("音乐.flac"), QStringLiteral("视频.mp4"), QStringLiteral("笔记.txt")}) {
            Entry entry; entry.name = name; entry.path = QStringLiteral("D:/icon-preview/") + name; entry.directory = entries.isEmpty(); entry.detail = entry.directory ? QStringLiteral("文件夹") : QStringLiteral("圆角图标预览"); entries.append(entry);
        }
        model.replace(entries);
        const QImage apk = apkIcon(toolPath(QStringLiteral("hfx/HybridFileXfer-phone.apk"))); QVERIFY(!apk.isNull()); model.setThumbnail(entries[1].key(), apk);
        const QImage executable = shellThumbnail(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("TwinFile.exe"))); QVERIFY(!executable.isNull()); model.setThumbnail(entries[2].key(), executable);
        QImage screenshot(120, 800, QImage::Format_ARGB32); screenshot.fill(QColor(QStringLiteral("#f2f5f9")));
        QPainter text(&screenshot); text.fillRect(QRect(0, 0, 120, 100), QColor(QStringLiteral("#4f80ed"))); text.setPen(QColor(QStringLiteral("#8a9ab4")));
        for (int vertical = 120; vertical < 800; vertical += 22) text.drawLine(12, vertical, 108, vertical);
        text.end(); model.setThumbnail(entries[3].key(), screenshot);
        QImage landscape(400, 100, QImage::Format_ARGB32); QPainter photo(&landscape);
        QLinearGradient gradient(0, 0, 0, 100); gradient.setColorAt(0, QColor(QStringLiteral("#84c7fc"))); gradient.setColorAt(1, QColor(QStringLiteral("#ddeaff"))); photo.fillRect(landscape.rect(), gradient);
        photo.setPen(Qt::NoPen); photo.setBrush(QColor(QStringLiteral("#457267"))); photo.drawPolygon(QPolygonF{QPointF(0, 100), QPointF(170, 35), QPointF(230, 75), QPointF(310, 28), QPointF(400, 100)}); photo.end(); model.setThumbnail(entries[4].key(), landscape);
        view.show(); view.selectionModel()->select(model.index(1), QItemSelectionModel::Select); QTest::qWait(30);
        QVERIFY(!view.horizontalScrollBar()->isVisible());
        const QString evidence = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        if (!evidence.isEmpty()) QVERIFY(view.grab().save(QDir(evidence).filePath(QStringLiteral("native-icon-mask-%1.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light")))));
    }
    void checksumKnownFixture() {
        QTemporaryDir directory; const QString file = directory.filePath(QStringLiteral("hash.txt")); QVERIFY(writeFixture(file, QByteArray("abc"))); const Result result = checksum({file});
        QVERIFY(result.ok); QVERIFY(result.output.contains(QStringLiteral("900150983cd24fb0d6963f7d28e17f72"))); QVERIFY(result.output.contains(QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
    }
    void searchIgnoresCancelledResult() {
        QTemporaryDir directory; QVERIFY(writeFixture(directory.filePath(QStringLiteral("match.txt")))); auto cancel = std::make_shared<std::atomic_bool>(true);
        QVERIFY(!searchLocal(directory.path(), QStringLiteral("match"), false, cancel).error.isEmpty());
    }
    void settingsAtomicRoundTrip() {
        QTemporaryDir directory; const QString file = directory.filePath(QStringLiteral("nested/settings.json")); QJsonObject settings{{QStringLiteral("theme"), QStringLiteral("dark")}, {QStringLiteral("中文"), QStringLiteral("配置")}};
        QVERIFY(writeSettings(file, settings)); QCOMPARE(readSettings(file), settings);
        QVERIFY(writeFixture(file, QByteArray("bad json"))); QVERIFY(readSettings(file).isEmpty());
    }
    void malformedLocationFallsBackToRoot() { QCOMPARE(Location::fromJson({{QStringLiteral("source"), 99}}).source, Source::Drive); QCOMPARE(Location::fromJson({{QStringLiteral("source"), 0}, {QStringLiteral("path"), QStringLiteral("Z:/nonexistent-path-twinfile")}}).source, Source::Drive); }
    void devicesParseStartupNoiseAndStates() {
        const QList<Entry> devices = parseDevices(QStringLiteral("* daemon started successfully\nList of devices attached\nSERIAL1 device product:test model:Test_Phone\nSERIAL2 unauthorized\nSERIAL3 offline\nnoise garbage\n"));
        QCOMPARE(devices.size(), 3); QCOMPARE(devices[0].name, QStringLiteral("Test Phone")); QCOMPARE(devices[1].state, QStringLiteral("unauthorized")); QVERIFY(devices[1].detail.contains(QStringLiteral("未授权")));
    }
    void deviceMonitorTestModeNeverStartsAdb() { DeviceMonitor monitor; QSignalSpy changed(&monitor, &DeviceMonitor::changed); monitor.start(); monitor.detect(); QTest::qWait(30); QCOMPARE(changed.count(), 0); }
    void nativeProcessPreservesUnicodeArguments() {
        const Result result = runProcess(QCoreApplication::applicationFilePath(), {QStringLiteral("--fixture-echo"), QStringLiteral("中文 空格 #%.apk.1")}, 10000);
        QVERIFY2(result.ok, qPrintable(result.error)); QCOMPARE(result.output.trimmed(), QStringLiteral("中文 空格 #%.apk.1"));
    }
    void nativeProcessTimeoutAndCancellation() {
        QVERIFY(!runProcess(QCoreApplication::applicationFilePath(), {QStringLiteral("--fixture-wait")}, 50).ok);
        auto cancel = std::make_shared<std::atomic_bool>(true); QVERIFY(!runProcess(QCoreApplication::applicationFilePath(), {QStringLiteral("--fixture-wait")}, 10000, cancel).ok);
    }
    void localDragMimePreservesQqSuffixAndCopyOnly() {
        QTemporaryDir directory; const QString file = directory.filePath(QStringLiteral("中文.apk.1")); QVERIFY(writeFixture(file));
        FileModel model; Entry entry; entry.path = file; entry.name = QFileInfo(file).fileName(); model.replace({entry});
        std::unique_ptr<QMimeData> mime(model.mimeData({model.index(0)})); QVERIFY(mime); QCOMPARE(mime->urls().first().toLocalFile(), QDir::toNativeSeparators(file).replace(QChar('\\'), QChar('/'))); QCOMPARE(model.supportedDragActions(), Qt::CopyAction);
        entry.source = Source::Archive; model.replace({entry}); QVERIFY(model.mimeData({model.index(0)}) == nullptr);
    }
    void missingFileCancelsWholeDrag() {
        FileModel model; Entry entry; entry.path = QStringLiteral("Z:/missing-file-unique"); model.replace({entry}); QVERIFY(model.mimeData({model.index(0)}) == nullptr);
    }
    void blankClickClearsSelectionAndCurrent() {
        QTemporaryDir directory; QVERIFY(writeFixture(directory.filePath(QStringLiteral("file.txt")))); Pane pane; pane.resize(500, 500); pane.show(); pane.navigate({Source::Local, directory.path(), {}, {}});
        QTRY_COMPARE(pane.fileModel()->rowCount(), 1); pane.view()->setCurrentIndex(pane.fileModel()->index(0)); QCOMPARE(pane.selected().size(), 1);
        const QPoint blank(20, pane.view()->viewport()->height() - 10); QVERIFY(!pane.view()->indexAt(blank).isValid()); QTest::mouseClick(pane.view()->viewport(), Qt::LeftButton, Qt::NoModifier, blank);
        QVERIFY(pane.selected().isEmpty()); QVERIFY(!pane.view()->currentIndex().isValid());
    }
    void permanentBlankStripClearsSelection() {
        QTemporaryDir directory; QVERIFY(writeFixture(directory.filePath(QStringLiteral("file.txt")))); Pane pane; pane.resize(500, 500); pane.show(); pane.navigate({Source::Local, directory.path(), {}, {}}); QTRY_COMPARE(pane.fileModel()->rowCount(), 1);
        pane.view()->setCurrentIndex(pane.fileModel()->index(0)); auto *strip = pane.findChild<QWidget *>(QStringLiteral("deselectStrip")); QVERIFY(strip); QCOMPARE(strip->height(), 16); QTest::mouseClick(strip, Qt::LeftButton); QVERIFY(pane.selected().isEmpty());
    }
    void refreshPreservesSelectedIdentity() {
        QTemporaryDir directory; QVERIFY(writeFixture(directory.filePath(QStringLiteral("selected.txt")))); Pane pane; pane.navigate({Source::Local, directory.path(), {}, {}}); QTRY_COMPARE(pane.fileModel()->rowCount(), 1); pane.view()->setCurrentIndex(pane.fileModel()->index(0));
        QVERIFY(writeFixture(directory.filePath(QStringLiteral("aaa.txt")))); pane.refresh(); QTRY_COMPARE(pane.fileModel()->rowCount(), 2); QCOMPARE(pane.selected().size(), 1); QCOMPARE(pane.selected().first().name, QStringLiteral("selected.txt"));
    }
    void staleDirectoryCallbackCannotOverwriteNewLocation() {
        QTemporaryDir first, second; QVERIFY(writeFixture(first.filePath(QStringLiteral("first.txt")))); QVERIFY(writeFixture(second.filePath(QStringLiteral("second.txt")))); Pane pane;
        pane.navigate({Source::Local, first.path(), {}, {}}); pane.navigate({Source::Local, second.path(), {}, {}}); QTRY_COMPARE(pane.fileModel()->rowCount(), 1); QTest::qWait(100); QCOMPARE(pane.fileModel()->entry(0).name, QStringLiteral("second.txt"));
    }
    void independentTabsRestorePathAndSelection() {
        QTemporaryDir first, second; QVERIFY(writeFixture(first.filePath(QStringLiteral("first.txt")))); QVERIFY(writeFixture(second.filePath(QStringLiteral("second.txt")))); Pane pane;
        pane.navigate({Source::Local, first.path(), {}, {}}); QTRY_COMPARE(pane.fileModel()->rowCount(), 1); pane.view()->setCurrentIndex(pane.fileModel()->index(0)); pane.newTab(); pane.navigate({Source::Local, second.path(), {}, {}}); QTRY_COMPARE(pane.fileModel()->entry(0).name, QStringLiteral("second.txt"));
        pane.cycleTab(-1); QTRY_COMPARE(pane.fileModel()->entry(0).name, QStringLiteral("first.txt")); QCOMPARE(pane.selected().size(), 1); QCOMPARE(pane.location().path, first.path());
    }
    void currentPaneRefreshDoesNotReloadOtherPane() {
        QTemporaryDir first, second; QVERIFY(writeFixture(first.filePath(QStringLiteral("first.txt")))); QVERIFY(writeFixture(second.filePath(QStringLiteral("second.txt")))); Window window; window.show(); window.leftPane()->navigate({Source::Local, first.path(), {}, {}}); window.rightPane()->navigate({Source::Local, second.path(), {}, {}});
        QTRY_COMPARE(window.leftPane()->fileModel()->rowCount(), 1); QTRY_COMPARE(window.rightPane()->fileModel()->rowCount(), 1);
        window.setReducedMotion(false);
        QSignalSpy leftAnimation(window.leftPane()->view(), &FileView::transitionStarted), rightAnimation(window.rightPane()->view(), &FileView::transitionStarted);
        QVERIFY(writeFixture(first.filePath(QStringLiteral("new-left.txt")))); QVERIFY(writeFixture(second.filePath(QStringLiteral("new-right.txt")))); window.setActive(window.leftPane()); window.refreshActive(); QTRY_COMPARE(window.leftPane()->fileModel()->rowCount(), 2); QCOMPARE(window.rightPane()->fileModel()->rowCount(), 1);
        QCOMPARE(leftAnimation.count(), 1); QCOMPARE(leftAnimation.first().first().value<FileView::Transition>(), FileView::Transition::Refresh); QCOMPARE(rightAnimation.count(), 0);
        window.setActive(window.rightPane()); window.refreshActive(); QTRY_COMPARE(window.rightPane()->fileModel()->rowCount(), 2);
        QCOMPARE(leftAnimation.count(), 1); QCOMPARE(rightAnimation.count(), 1); QCOMPARE(rightAnimation.first().first().value<FileView::Transition>(), FileView::Transition::Refresh);
    }
    void directoryTransitionsFollowNavigationAndArchiveParents() {
        QTemporaryDir directory; QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("child"))); QVERIFY(writeFixture(directory.filePath(QStringLiteral("child/file.txt"))));
        Pane pane; pane.setAttribute(Qt::WA_DontShowOnScreen); pane.resize(500, 500); pane.show();
        pane.navigate({Source::Local, directory.path(), {}, {}}); QTRY_COMPARE(pane.fileModel()->rowCount(), 1);
        QSignalSpy started(pane.view(), &FileView::transitionStarted);
        pane.navigate({Source::Local, directory.filePath(QStringLiteral("child")), {}, {}}); QTRY_COMPARE(started.count(), 1); QCOMPARE(started.last().first().value<FileView::Transition>(), FileView::Transition::EnterFolder);
        pane.goUp(); QTRY_COMPARE(started.count(), 2); QCOMPARE(started.last().first().value<FileView::Transition>(), FileView::Transition::LeaveFolder);
        pane.goBack(); QTRY_COMPARE(started.count(), 3); QCOMPARE(started.last().first().value<FileView::Transition>(), FileView::Transition::EnterFolder);
        pane.goForward(); QTRY_COMPARE(started.count(), 4); QCOMPARE(started.last().first().value<FileView::Transition>(), FileView::Transition::LeaveFolder);
        const QString archive = directory.filePath(QStringLiteral("fixture.zip"));
        { ZipWriter writer(archive); writer.addFile(QStringLiteral("sub/file.txt"), QByteArray("archive")); writer.close(); }
        pane.navigate({Source::Archive, archive, {}, {}}); QTRY_COMPARE(started.count(), 5); QCOMPARE(started.last().first().value<FileView::Transition>(), FileView::Transition::EnterFolder);
        pane.navigate({Source::Archive, archive, QStringLiteral("sub"), {}}); QTRY_COMPARE(started.count(), 6); QCOMPARE(started.last().first().value<FileView::Transition>(), FileView::Transition::EnterFolder);
        pane.goUp(); QTRY_COMPARE(started.count(), 7); QCOMPARE(started.last().first().value<FileView::Transition>(), FileView::Transition::LeaveFolder);
        pane.goBack(); QTRY_COMPARE(started.count(), 8); QCOMPARE(started.last().first().value<FileView::Transition>(), FileView::Transition::EnterFolder);
        pane.goForward(); QTRY_COMPARE(started.count(), 9); QCOMPARE(started.last().first().value<FileView::Transition>(), FileView::Transition::LeaveFolder);
        pane.goUp(); QTRY_COMPARE(started.count(), 10); QCOMPARE(pane.location().source, Source::Local); QCOMPARE(started.last().first().value<FileView::Transition>(), FileView::Transition::LeaveFolder);
    }
    void refreshAnimationPreservesSelectionAndScroll() {
        QTemporaryDir directory;
        for (int number = 0; number < 80; ++number) QVERIFY(writeFixture(directory.filePath(QStringLiteral("file-%1.txt").arg(number, 3, 10, QChar('0')))));
        Pane pane; pane.setAttribute(Qt::WA_DontShowOnScreen); pane.resize(500, 500); pane.show(); pane.navigate({Source::Local, directory.path(), {}, {}}); QTRY_COMPARE(pane.fileModel()->rowCount(), 80);
        pane.view()->setCurrentIndex(pane.fileModel()->index(45)); pane.view()->scrollTo(pane.fileModel()->index(45), QAbstractItemView::PositionAtTop);
        const int scroll = pane.view()->verticalScrollBar()->value(); QVERIFY(scroll > 0);
        QSignalSpy started(pane.view(), &FileView::transitionStarted); pane.refresh(); QTRY_COMPARE(started.count(), 1);
        QCOMPARE(pane.selected().size(), 1); QCOMPARE(pane.selected().first().name, QStringLiteral("file-045.txt")); QCOMPARE(pane.view()->verticalScrollBar()->value(), scroll);
        QTRY_VERIFY_WITH_TIMEOUT(!pane.view()->isTransitionRunning(), 1000); QCOMPARE(pane.view()->transitionProgress(), 1.0);
        const QModelIndex index = pane.view()->indexAt(QPoint(20, 20)); QVERIFY(index.isValid());
        QTest::mouseClick(pane.view()->viewport(), Qt::LeftButton, Qt::NoModifier, pane.view()->visualRect(index).center()); QCOMPARE(pane.selected().first().key(), pane.fileModel()->entry(index.row()).key());
    }
    void rapidRequestsOnlyAnimateLatestAndDeviceUpdatesStayQuiet() {
        QTemporaryDir first, second; QVERIFY(writeFixture(first.filePath(QStringLiteral("first.txt")))); QVERIFY(writeFixture(second.filePath(QStringLiteral("second.txt"))));
        Pane pane; pane.setAttribute(Qt::WA_DontShowOnScreen); pane.resize(500, 500); pane.show(); pane.navigate({Source::Local, first.path(), {}, {}}); QTRY_COMPARE(pane.fileModel()->rowCount(), 1);
        QSignalSpy started(pane.view(), &FileView::transitionStarted);
        for (int repeat = 0; repeat < 20; ++repeat) { pane.refresh(); pane.navigate({Source::Local, first.path(), {}, {}}); }
        pane.navigate({Source::Local, second.path(), {}, {}}); QTRY_COMPARE(started.count(), 1); QTest::qWait(100);
        QCOMPARE(started.count(), 1); QCOMPARE(pane.fileModel()->entry(0).name, QStringLiteral("second.txt")); QCOMPARE(started.last().first().value<FileView::Transition>(), FileView::Transition::EnterFolder);
        QTRY_VERIFY(!pane.view()->isTransitionRunning()); pane.navigate({}); QTRY_COMPARE(started.count(), 2); QTRY_VERIFY(!pane.view()->isTransitionRunning());
        Entry device; device.source = Source::Device; device.name = QStringLiteral("模拟设备"); device.serial = QStringLiteral("fixture"); device.state = QStringLiteral("device");
        pane.setDevices({device}); pane.setDevices({device}); QCOMPARE(started.count(), 2); QVERIFY(!pane.view()->isTransitionRunning());
    }
    void reducedMotionCancelsCurrentAndPendingAnimations() {
        const QJsonObject original = readSettings(settingsPath()); const auto restore = qScopeGuard([original] { writeSettings(settingsPath(), original); });
        QVERIFY(writeSettings(settingsPath(), {{QStringLiteral("reduced_motion"), true}}));
        QTemporaryDir directory; QVERIFY(writeFixture(directory.filePath(QStringLiteral("file.txt"))));
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.show();
        QVERIFY(!window.leftPane()->view()->animationsEnabled()); QVERIFY(!window.rightPane()->view()->animationsEnabled());
        Pane *pane = window.leftPane(); pane->navigate({Source::Local, directory.path(), {}, {}}); QTRY_COMPARE(pane->fileModel()->rowCount(), 1);
        auto *rotation = pane->findChild<QVariantAnimation *>(QStringLiteral("refreshRotation")); QVERIFY(rotation);
        QSignalSpy started(pane->view(), &FileView::transitionStarted); pane->refresh(); QTest::qWait(100); QCOMPARE(started.count(), 0);
        QCOMPARE(rotation->state(), QAbstractAnimation::Stopped);
        window.setReducedMotion(false); pane->refresh(); QCOMPARE(rotation->state(), QAbstractAnimation::Running); QTRY_COMPARE(started.count(), 1); QVERIFY(pane->view()->isTransitionRunning()); QCOMPARE(rotation->state(), QAbstractAnimation::Stopped);
        window.setActive(window.rightPane()); window.setReducedMotion(true); QVERIFY(!pane->view()->isTransitionRunning()); QCOMPARE(pane->view()->transitionProgress(), 1.0); QVERIFY(!window.rightPane()->graphicsEffect());
        window.setReducedMotion(false); pane->refresh(); window.setReducedMotion(true); QCOMPARE(rotation->state(), QAbstractAnimation::Stopped); QTest::qWait(100); QCOMPARE(started.count(), 1); QVERIFY(!pane->view()->isTransitionRunning());
    }
    void viewAnimationCancelsOnInteractionResizeHideAndTheme() {
        FileModel model; Entry entry; entry.name = QStringLiteral("file.txt"); entry.path = QStringLiteral("C:/fixture/file.txt"); model.replace({entry});
        FileView view; view.setAttribute(Qt::WA_DontShowOnScreen); view.setModel(&model); view.resize(500, 500); view.show(); QApplication::processEvents();
        view.startTransition(FileView::Transition::EnterFolder); QVERIFY(view.isTransitionRunning()); QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(20, 20)); QVERIFY(!view.isTransitionRunning()); QCOMPARE(view.selectionModel()->selectedRows().size(), 1);
        view.startTransition(FileView::Transition::LeaveFolder); QTest::keyClick(&view, Qt::Key_Down); QVERIFY(!view.isTransitionRunning());
        view.startTransition(FileView::Transition::Refresh); view.resize(510, 510); QVERIFY(!view.isTransitionRunning());
        view.startTransition(FileView::Transition::Refresh); QPalette palette = view.palette(); palette.setColor(QPalette::Base, QColor(QStringLiteral("#efefed"))); view.setPalette(palette); QVERIFY(!view.isTransitionRunning());
        view.startTransition(FileView::Transition::Refresh); view.deselect(); QVERIFY(!view.isTransitionRunning()); QVERIFY(view.selectionModel()->selectedRows().isEmpty());
        view.startTransition(FileView::Transition::Refresh); view.hide(); QVERIFY(!view.isTransitionRunning()); view.startTransition(FileView::Transition::Refresh); QVERIFY(!view.isTransitionRunning());
    }
    void animationsOnlyRepaintVisibleRows() {
        QList<Entry> entries;
        for (int number = 0; number < 20000; ++number) { Entry entry; entry.name = QStringLiteral("file-%1.txt").arg(number); entry.path = QStringLiteral("C:/fixture/") + entry.name; entries.append(entry); }
        FileModel model; model.replace(std::move(entries)); FileView view; view.setAttribute(Qt::WA_DontShowOnScreen); view.resize(500, 500); view.setModel(&model);
        auto *delegate = new CountingFileDelegate(&view); view.setItemDelegate(delegate); view.show(); QApplication::processEvents(); delegate->paintCount = 0;
        view.startTransition(FileView::Transition::EnterFolder); QTest::qWait(80); QVERIFY(delegate->paintCount > 0); QVERIFY(delegate->paintCount < 500); QVERIFY(view.isTransitionRunning());
        view.verticalScrollBar()->setValue(100); QVERIFY(!view.isTransitionRunning()); QCOMPARE(model.rowCount(), 20000);
    }
    void emptyAndFailedDirectoriesDoNotLeaveAnimationsRunning() {
        QTemporaryDir directory; Pane pane; pane.setAttribute(Qt::WA_DontShowOnScreen); pane.resize(500, 500); pane.show();
        QSignalSpy started(pane.view(), &FileView::transitionStarted); pane.navigate({Source::Local, directory.path(), {}, {}}); QTRY_COMPARE(started.count(), 1); QCOMPARE(pane.fileModel()->rowCount(), 0); QTRY_VERIFY(!pane.view()->isTransitionRunning());
        pane.navigate({Source::Local, directory.filePath(QStringLiteral("missing")), {}, {}}); QTRY_COMPARE(started.count(), 2); QVERIFY(!pane.error().isEmpty()); QCOMPARE(pane.fileModel()->rowCount(), 0); QTRY_VERIFY(!pane.view()->isTransitionRunning());
    }
    void paneAnimationsRenderWithoutUserSettings_data() {
        QTest::addColumn<QString>("theme");
        for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("mt"), QStringLiteral("miuix"), QStringLiteral("cream"), QStringLiteral("bataUI01")}) QTest::newRow(qPrintable(theme)) << theme;
    }
    void paneAnimationsRenderWithoutUserSettings() {
        QFETCH(QString, theme);
        const QJsonObject original = readSettings(settingsPath()); const auto restore = qScopeGuard([original] { writeSettings(settingsPath(), original); });
        QVERIFY(writeSettings(settingsPath(), {{QStringLiteral("reduced_motion"), false}, {QStringLiteral("theme"), theme}}));
        QTemporaryDir directory; QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("示例文件夹"))); QVERIFY(writeFixture(directory.filePath(QStringLiteral("示例文件夹/文件.txt"))));
        for (int number = 0; number < 12; ++number) QVERIFY(writeFixture(directory.filePath(QStringLiteral("示例文件-%1.txt").arg(number))));
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.show(); Pane *pane = window.leftPane(); pane->navigate({Source::Local, directory.path(), {}, {}}); QTRY_COMPARE(pane->fileModel()->rowCount(), 13); QTRY_VERIFY(!pane->view()->isTransitionRunning());
        auto *animation = pane->view()->findChild<QVariantAnimation *>(); QVERIFY(animation);
        QSignalSpy started(pane->view(), &FileView::transitionStarted);
        const QString evidence = qEnvironmentVariable("TWINFILE_VERIFY_SCREENSHOT_ROOT");
        for (const auto transition : {FileView::Transition::Refresh, FileView::Transition::EnterFolder, FileView::Transition::LeaveFolder}) {
            const int count = started.count();
            if (transition == FileView::Transition::Refresh) {
                pane->refresh();
                auto *rotation = pane->findChild<QVariantAnimation *>(QStringLiteral("refreshRotation")); QVERIFY(rotation);
                const bool visible = theme != QStringLiteral("mt"); QCOMPARE(pane->findChild<QToolButton *>(QStringLiteral("refreshCurrentPane"))->isVisible(), visible);
                QCOMPARE(rotation->state(), visible ? QAbstractAnimation::Running : QAbstractAnimation::Stopped); if (visible) rotation->setCurrentTime(160);
                if (!evidence.isEmpty()) QVERIFY(window.grab().save(QDir(evidence).filePath(QStringLiteral("native-animation-loading-%1.png").arg(theme))));
            }
            else if (transition == FileView::Transition::EnterFolder) pane->navigate({Source::Local, directory.filePath(QStringLiteral("示例文件夹")), {}, {}});
            else pane->goUp();
            QTRY_COMPARE(started.count(), count + 1); QCOMPARE(started.last().first().value<FileView::Transition>(), transition);
            animation->setCurrentTime(60); QVERIFY(pane->view()->transitionProgress() > 0); QVERIFY2(pane->view()->transitionProgress() < 1, qPrintable(QStringLiteral("Transition %1 stopped before rendering").arg(int(transition))));
            const QPixmap middle = pane->view()->viewport()->grab(); QVERIFY(!middle.isNull());
            const QString name = transition == FileView::Transition::Refresh ? QStringLiteral("refresh") : transition == FileView::Transition::EnterFolder ? QStringLiteral("enter") : QStringLiteral("leave");
            if (!evidence.isEmpty()) QVERIFY(window.grab().save(QDir(evidence).filePath(QStringLiteral("native-animation-%1-%2.png").arg(name, theme))));
            animation->setCurrentTime(animation->duration()); QVERIFY(!pane->view()->isTransitionRunning());
            const QPixmap finished = pane->view()->viewport()->grab(); QVERIFY(middle.toImage() != finished.toImage());
            if (!evidence.isEmpty()) QVERIFY(window.grab().save(QDir(evidence).filePath(QStringLiteral("native-animation-%1-%2-finished.png").arg(name, theme))));
        }
    }
    void pathClickActivatesCorrespondingPane() {
        Window window; window.show(); window.setActive(window.leftPane());
        auto *path = window.rightPane()->findChild<QLineEdit *>(); QVERIFY(path);
        QTest::mouseClick(path, Qt::LeftButton); QCOMPARE(window.activePane(), window.rightPane());
    }
    void selectedBrandIconSurvivesThemeSwitching() {
        Window window; const QImage selected = window.windowIcon().pixmap(64, 64).toImage(); QVERIFY(!selected.isNull());
        for (const QString &mode : {QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("cream"), QStringLiteral("bataUI01")}) { window.setTheme(mode); QCOMPARE(window.windowIcon().pixmap(64, 64).toImage(), selected); }
    }
private:
    QString makeDexSeed(QTemporaryDir &directory) {
        QByteArray bytes(140, '\0'); bytes.replace(0, 8, QByteArray("dex\n035\0", 8));
        qToLittleEndian<quint32>(140, bytes.data() + 32); qToLittleEndian<quint32>(112, bytes.data() + 36); qToLittleEndian<quint32>(0x12345678, bytes.data() + 40);
        qToLittleEndian<quint32>(112, bytes.data() + 52); qToLittleEndian<quint32>(28, bytes.data() + 104); qToLittleEndian<quint32>(112, bytes.data() + 108);
        qToLittleEndian<quint32>(2, bytes.data() + 112); qToLittleEndian<quint32>(1, bytes.data() + 120); qToLittleEndian<quint16>(0x1000, bytes.data() + 128); qToLittleEndian<quint32>(1, bytes.data() + 132); qToLittleEndian<quint32>(112, bytes.data() + 136);
        const QString path = directory.filePath(QStringLiteral("seed.dex")); return writeFixture(path, bytes) ? path : QString();
    }
    QTemporaryDir settings_;
};

int main(int argc, char **argv) {
    if (argc > 1 && QByteArray(argv[1]).startsWith("--fixture-")) {
        QCoreApplication application(argc, argv);
        if (application.arguments().contains(QStringLiteral("--fixture-wait"))) { QThread::msleep(5000); return 0; }
        QTextStream(stdout) << application.arguments().value(2); return 0;
    }
    QApplication application(argc, argv); application.setStyle(QStringLiteral("Fusion")); NativeTests tests; return QTest::qExec(&tests, argc, argv);
}

#include "native_tests.moc"
