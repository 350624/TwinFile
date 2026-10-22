#include "window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QTextStream>
#include <QTemporaryDir>
#include <QTimer>
#include <QThreadPool>

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("TwinFileNative"));
    application.setApplicationVersion(QStringLiteral(TWINFILE_VERSION));
    application.setWindowIcon(QIcon(QStringLiteral(":/app.ico")));
    application.setStyle(QStringLiteral("Fusion"));
    QImageReader::setAllocationLimit(256);
    QThreadPool::globalInstance()->setMaxThreadCount(4);
    QCommandLineParser parser; parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({QStringLiteral("smoke-test"), QStringLiteral("Isolated launch check; no ADB and no user settings")});
    parser.addOption({QStringLiteral("screenshot"), QStringLiteral("Save a window screenshot"), QStringLiteral("path")});
    parser.process(application);
    std::unique_ptr<QTemporaryDir> temporary;
    if (parser.isSet(QStringLiteral("smoke-test"))) {
        temporary = std::make_unique<QTemporaryDir>();
        qputenv("TWINFILE_TEST_MODE", "1"); qputenv("TWINFILE_SETTINGS", temporary->filePath(QStringLiteral("settings.json")).toUtf8());
        qputenv("TWINFILE_THUMBNAIL_CACHE", temporary->filePath(QStringLiteral("thumbnails")).toUtf8());
    }
    twinfile::Window window;
    if (parser.isSet(QStringLiteral("smoke-test"))) window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    if (parser.isSet(QStringLiteral("smoke-test")) || parser.isSet(QStringLiteral("screenshot"))) {
        QTimer::singleShot(1500, &window, [&] {
            if (parser.isSet(QStringLiteral("screenshot")) && !window.grab().save(parser.value(QStringLiteral("screenshot")))) application.exit(2);
            else if (parser.isSet(QStringLiteral("smoke-test"))) {
                const bool valid = !window.windowIcon().isNull() && window.leftPane()->view() && window.rightPane()->view();
                window.close(); application.exit(valid ? 0 : 3);
            }
        });
    }
    return application.exec();
}
