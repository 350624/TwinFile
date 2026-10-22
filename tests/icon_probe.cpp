#include "android.h"
#include "thumbnails.h"
#include "window.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QTest>
#include <QTextStream>

using namespace twinfile;

int main(int argc, char **argv) {
    QApplication application(argc, argv); application.setStyle(QStringLiteral("Fusion"));
    if (application.arguments().size() != 3) return 2;
    const QString source = QFileInfo(application.arguments()[1]).absoluteFilePath(), output = QFileInfo(application.arguments()[2]).absoluteFilePath();
    if (!QDir().mkpath(output)) return 2;
    qputenv("TWINFILE_TEST_MODE", "1"); qputenv("TWINFILE_SETTINGS", QDir(output).filePath(QStringLiteral("probe-settings.json")).toUtf8());
    qputenv("TWINFILE_TOOL_CACHE", QDir(output).filePath(QStringLiteral("probe-tools")).toUtf8()); qputenv("TWINFILE_THUMBNAIL_CACHE", QDir(output).filePath(QStringLiteral("probe-thumbnails")).toUtf8());
    QJsonArray rows; QList<QPair<QString, QImage>> icons; int failures = 0;
    for (const QFileInfo &file : QDir(source).entryInfoList(QDir::Files, QDir::Name)) {
        if (!isApk(file.fileName())) continue;
        Entry entry; entry.name = file.fileName(); entry.path = file.absoluteFilePath(); entry.size = file.size(); entry.modified = file.lastModified();
        QElapsedTimer timer; timer.start(); const QImage decoded = apkIcon(entry.path); const qint64 decodeMs = timer.elapsed();
        timer.restart(); const QImage first = loadThumbnail(entry); const qint64 firstMs = timer.elapsed();
        timer.restart(); const QImage warm = loadThumbnail(entry); const qint64 warmMicros = timer.nsecsElapsed() / 1000;
        ZipReader archive(entry.path); QString resourceError; const auto resources = androidResources(archive.fileData(QStringLiteral("resources.arsc"), 64 * 1024 * 1024), {}, &resourceError);
        const bool valid = !decoded.isNull() && !first.isNull() && first == warm;
        if (!valid) ++failures;
        rows.append(QJsonObject{{QStringLiteral("file"), entry.path}, {QStringLiteral("ok"), valid}, {QStringLiteral("nativeDecodeMs"), decodeMs}, {QStringLiteral("firstLoadMs"), firstMs}, {QStringLiteral("memoryCacheMicros"), warmMicros}, {QStringLiteral("nativeResourceCount"), resources.size()}, {QStringLiteral("resourceError"), resourceError}});
        if (!decoded.isNull()) { decoded.save(QDir(output).filePath(file.fileName() + QStringLiteral(".png"))); icons.append({file.fileName(), decoded}); }
    }
    if (icons.isEmpty()) return 3;
    QImage sheet(800, ((icons.size() + 3) / 4) * 195, QImage::Format_RGB32); sheet.fill(QColor(QStringLiteral("#282c34")));
    QPainter painter(&sheet); painter.setRenderHint(QPainter::SmoothPixmapTransform); painter.setPen(Qt::white); QFont font(QStringLiteral("Microsoft YaHei UI"), 9); painter.setFont(font);
    for (int index = 0; index < icons.size(); ++index) {
        const QPoint origin((index % 4) * 200, (index / 4) * 195);
        painter.drawImage(QRect(origin + QPoint(36, 4), QSize(128, 128)), icons[index].second);
        painter.drawText(QRect(origin + QPoint(6, 137), QSize(188, 55)), Qt::AlignHCenter | Qt::TextWordWrap, icons[index].first);
    }
    painter.end(); sheet.save(QDir(output).filePath(QStringLiteral("apk-icon-contact-sheet.png")));
    {
        Window window; window.setAttribute(Qt::WA_DontShowOnScreen); window.show(); window.setReducedMotion(true); Pane *pane = window.leftPane(); pane->navigate({Source::Local, source, {}, {}});
        QElapsedTimer timer; timer.start(); while (pane->fileModel()->rowCount() == 0 && timer.elapsed() < 5000) QTest::qWait(20);
        for (const QString &sample : {QStringLiteral("HXAudio"), QStringLiteral("MT2")}) {
            for (int row = 0; row < pane->fileModel()->rowCount(); ++row) if (isApk(pane->fileModel()->entry(row).name) && pane->fileModel()->entry(row).name.startsWith(sample)) { pane->view()->scrollTo(pane->fileModel()->index(row), QAbstractItemView::PositionAtTop); break; }
            QTest::qWait(1500); window.grab().save(QDir(output).filePath(QStringLiteral("native-icons-") + sample + QStringLiteral(".png")));
        }
    }
    QJsonObject report{{QStringLiteral("samples"), rows}, {QStringLiteral("failures"), failures}, {QStringLiteral("aaptFallbackExtracted"), QFileInfo(QDir(output).filePath(QStringLiteral("probe-tools/aapt2.exe"))).exists()}, {QStringLiteral("userApksModified"), false}};
    QFile file(QDir(output).filePath(QStringLiteral("icon-verification.json"))); if (!file.open(QIODevice::WriteOnly)) return 4;
    file.write(QJsonDocument(report).toJson()); QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << Qt::endl;
    return failures ? 1 : 0;
}
