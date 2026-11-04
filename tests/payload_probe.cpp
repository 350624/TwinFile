#include "payload.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QTextStream>
#include <mutex>

using namespace twinfile;

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    QTextStream output(stdout);
    if (arguments.size() != 4) {
        output << "Usage: twinfile_payload_probe URL partition[,partition] output-directory\n";
        return 2;
    }
    const QString destination = arguments[3];
    if (!QDir().mkpath(destination)) return 2;
    PayloadOptions options; options.downloadConnections = 512;
    QElapsedTimer elapsed; elapsed.start();
    std::mutex mutex; QHash<QString, qint64> sampledAt; qint64 lastBytes = 0;
    const Result result = extractPayload(arguments[1], arguments[2].split(','), destination, 4, {},
        [&](qint64 done, qint64 total, const QString &stage) {
            std::lock_guard<std::mutex> lock(mutex);
            const bool network = stage.startsWith(QStringLiteral("下载 Payload 分区数据"));
            if (network) lastBytes = done;
            if (!network && !stage.startsWith(QStringLiteral("提取分区 · ")) && !stage.startsWith(QStringLiteral("校验分区 · "))) return;
            const QString phase = stage.section(QStringLiteral(" · "), 0, 0);
            const qint64 now = elapsed.elapsed();
            if (sampledAt.contains(phase) && now - sampledAt.value(phase) < 5000 && done < total) return;
            sampledAt[phase] = now;
            output << now << " ms " << done << '/' << total << ' ' << stage << Qt::endl;
        }, options);
    output << "RESULT " << result.ok << " elapsed_ms=" << elapsed.elapsed()
           << " network_bytes=" << lastBytes << " error=" << result.error << Qt::endl;
    for (const QString &partition : arguments[2].split(',')) {
        const QFileInfo file(QDir(destination).filePath(partition + ".img"));
        output << partition << " output_bytes=" << file.size() << Qt::endl;
    }
    return result.ok ? 0 : 1;
}
