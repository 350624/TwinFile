#include "download.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QStandardPaths>
#include <QFileInfo>
#include <QFile>
#include <QUrl>

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("TwinFile Downloader"));
    QCoreApplication::setApplicationVersion(QStringLiteral(TWINFILE_VERSION));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption urlOption(QStringLiteral("url"), QStringLiteral("Download a direct HTTP or HTTPS URL"), QStringLiteral("url"));
    QCommandLineOption fileOption(QStringLiteral("filename"), QStringLiteral("Suggested output filename"), QStringLiteral("filename"));
    parser.addOption(urlOption);
    parser.addOption(fileOption);
    parser.process(application);

    const QString directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QUrl url(parser.value(urlOption), QUrl::StrictMode);
    const bool hasInitialUrl = !parser.value(urlOption).isEmpty() && url.isValid();
    QString fileName = QFileInfo(parser.value(fileOption)).fileName();
    if (fileName.isEmpty() && hasInitialUrl) fileName = QFileInfo(url.path()).fileName();
    if (fileName.isEmpty()) fileName = QStringLiteral("download.bin");
    if (hasInitialUrl) {
        const QString stem = QFileInfo(fileName).completeBaseName();
        const QString suffix = QFileInfo(fileName).suffix();
        QString candidate = fileName;
        for (int index = 1; QFile::exists(QDir(directory).filePath(candidate)); ++index) {
            candidate = stem + QStringLiteral(" (%1)").arg(index) + (suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix);
        }
        fileName = candidate;
    }
    twinfile::DownloadDialog dialog(directory, nullptr, url, fileName, hasInitialUrl);
    dialog.setWindowTitle(QStringLiteral("TwinFile Downloader"));
    dialog.show();
    return application.exec();
}
