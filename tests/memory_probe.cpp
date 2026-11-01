#include "window.h"
#include "thumbnails.h"

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPushButton>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QThreadPool>
#include <windows.h>
#include <psapi.h>

using namespace twinfile;

static bool waitUntil(const std::function<bool()> &ready, int timeout = 10000) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < timeout) QTest::qWait(10);
    return ready();
}

static QJsonObject memory(const QString &stage) {
    QTest::qWait(500);
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters))) return {};
    return {{QStringLiteral("stage"), stage}, {QStringLiteral("working_set_bytes"), double(counters.WorkingSetSize)},
            {QStringLiteral("private_bytes"), double(counters.PrivateUsage)}};
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("TwinFileNative"));
    app.setStyle(QStringLiteral("Fusion"));
    QImageReader::setAllocationLimit(256);
    QThreadPool::globalInstance()->setMaxThreadCount(4);
    QTemporaryDir root;
    if (!root.isValid()) return 2;
    qputenv("TWINFILE_TEST_MODE", "1");
    qputenv("TWINFILE_SETTINGS", root.filePath(QStringLiteral("settings.json")).toUtf8());
    qputenv("TWINFILE_THUMBNAIL_CACHE", root.filePath(QStringLiteral("cache")).toUtf8());
    if (!writeSettings(settingsPath(), {{QStringLiteral("extension_visible"), false}, {QStringLiteral("theme"), QStringLiteral("miuix")},
                                        {QStringLiteral("color_mode"), QStringLiteral("dark")}})) return 3;
    const QString left = root.filePath(QStringLiteral("left")), right = root.filePath(QStringLiteral("right")), idle = root.filePath(QStringLiteral("idle"));
    if (!QDir().mkpath(left) || !QDir().mkpath(right) || !QDir().mkpath(idle)) return 4;
    for (int side = 0; side < 2; ++side) {
        for (int number = 0; number < 512; ++number) {
            QImage image(128, 128, QImage::Format_ARGB32);
            for (int y = 0; y < image.height(); ++y) {
                auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
                for (int x = 0; x < image.width(); ++x) line[x] = qRgb((x + number * 7) % 256, (y + number * 13) % 256, (x + y + side * 80) % 256);
            }
            if (!image.save(QDir(side ? right : left).filePath(QStringLiteral("image-%1.png").arg(number, 4, 10, QChar('0'))))) return 5;
        }
    }
    Window window;
    window.setAttribute(Qt::WA_ShowWithoutActivating);
    window.show();
    for (Pane *pane : {window.leftPane(), window.rightPane()}) pane->navigate({Source::Local, idle, {}, {}});
    if (!waitUntil([&] { return window.leftPane()->location().path == idle && window.rightPane()->location().path == idle
                               && window.leftPane()->fileModel()->rowCount() == 0 && window.rightPane()->fileModel()->rowCount() == 0; })) return 6;
    QJsonArray samples;
    samples.append(memory(QStringLiteral("idle_assistant_hidden")));
    window.findChild<QPushButton *>(QStringLiteral("assistantToggle"))->click();
    samples.append(memory(QStringLiteral("assistant_open")));
    window.findChild<QPushButton *>(QStringLiteral("assistantToggle"))->click();
    window.leftPane()->navigate({Source::Local, left, {}, {}});
    window.rightPane()->navigate({Source::Local, right, {}, {}});
    if (!waitUntil([&] { return window.leftPane()->fileModel()->rowCount() == 512 && window.rightPane()->fileModel()->rowCount() == 512; })) return 7;
    auto scroll = [&](int row) {
        for (Pane *pane : {window.leftPane(), window.rightPane()}) {
            pane->view()->scrollTo(pane->fileModel()->index(row), QAbstractItemView::PositionAtTop);
        }
        return waitUntil([&] {
            for (Pane *pane : {window.leftPane(), window.rightPane()}) {
                auto *view = pane->view();
                for (int y = 4; y < view->viewport()->height(); y += 20) {
                    const auto index = view->indexAt(QPoint(4, y));
                    if (index.isValid() && pane->fileModel()->thumbnail(index.row()).isNull()) return false;
                }
            }
            return true;
        }, 1000);
    };
    int incompletePages = 0;
    for (int row = 0; row < 512; row += 8) {
        if (!scroll(row)) ++incompletePages;
        window.setActive(row % 16 ? window.leftPane() : window.rightPane());
    }
    samples.append(memory(QStringLiteral("after_1024_thumbnails")));
    if (!scroll(0)) ++incompletePages;
    samples.append(memory(QStringLiteral("revisit_first_page")));
    for (Pane *pane : {window.leftPane(), window.rightPane()}) pane->navigate({Source::Local, idle, {}, {}});
    if (!waitUntil([&] { return window.leftPane()->fileModel()->rowCount() == 0 && window.rightPane()->fileModel()->rowCount() == 0; })) return 10;
    samples.append(memory(QStringLiteral("leave_image_directories")));
    QTextStream(stdout) << QJsonDocument(QJsonObject{{QStringLiteral("version"), QStringLiteral(TWINFILE_VERSION)},
                                                   {QStringLiteral("incomplete_pages"), incompletePages},
                                                   {QStringLiteral("samples"), samples}}).toJson();
    window.close();
    thumbnailPool()->waitForDone();
    QThreadPool::globalInstance()->waitForDone();
    return 0;
}
