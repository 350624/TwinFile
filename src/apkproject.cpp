#include "android.h"
#include "operations.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QImageReader>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>

namespace twinfile {
namespace {
Result failed(const QString &error) { return {false, error, {}, {}}; }
QByteArray read(const QString &path) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly) || input.size() > 4 * 1024 * 1024) return {};
    return input.readAll();
}
bool write(const QString &path, const QByteArray &bytes) {
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}
QString attribute(const QDomElement &element, const QString &name) { return element.attribute(QStringLiteral("android:") + name, element.attribute(name)); }
}
QString repairResourceXml(const QString &text, bool manifest) {
    if (text.size() > 4 * 1024 * 1024) return {};
    static const QRegularExpression tagName(QStringLiteral("^<(/?)([^\\s/>]+)"));
    static const QRegularExpression attributePattern(QStringLiteral("\\s+([A-Za-z_:][\\w.:-]*)\\s*=\\s*(\"[^\"]*\"|'[^']*')"));
    static const QRegularExpression badNumber(QStringLiteral("(^|\\.)[0-9]"));
    static const QRegularExpression placeholder(QStringLiteral("^MTClass[0-9]+$"));
    const QStringList components{QStringLiteral("application"), QStringLiteral("activity"), QStringLiteral("activity-alias"), QStringLiteral("service"), QStringLiteral("receiver"), QStringLiteral("provider"), QStringLiteral("instrumentation")};
    const QStringList classes{QStringLiteral("android:name"), QStringLiteral("android:targetActivity"), QStringLiteral("android:appComponentFactory"), QStringLiteral("android:backupAgent"), QStringLiteral("android:manageSpaceActivity"), QStringLiteral("android:parentActivityName")};
    QString output;
    qsizetype position = 0;
    while (position < text.size()) {
        const qsizetype start = text.indexOf(QChar('<'), position);
        if (start < 0) { output += text.mid(position); break; }
        output += text.mid(position, start - position);
        qsizetype end = start + 1;
        QChar quote;
        for (; end < text.size(); ++end) {
            const QChar character = text[end];
            if (!quote.isNull()) { if (character == quote) quote = QChar(); }
            else if (character == QChar('"') || character == QChar('\'')) quote = character;
            else if (character == QChar('>')) break;
        }
        if (end >= text.size()) return {};
        QString tag = text.mid(start, end - start + 1);
        const auto nameMatch = tagName.match(tag);
        if (nameMatch.hasMatch() && !tag.startsWith(QStringLiteral("<!")) && !tag.startsWith(QStringLiteral("<?"))) {
            const QString name = nameMatch.captured(2);
            QString newName = name;
            if (placeholder.match(name).hasMatch() || std::any_of(name.cbegin(), name.cend(), [](QChar character) { return character.unicode() > 127; })) newName = QStringLiteral("MTClass");
            QString repaired = QChar('<') + nameMatch.captured(1) + newName;
            qsizetype copied = nameMatch.capturedEnd();
            QSet<QString> seen;
            auto matches = attributePattern.globalMatch(tag, copied);
            while (matches.hasNext()) {
                const auto match = matches.next();
                repaired += tag.mid(copied, match.capturedStart() - copied);
                const QString key = match.captured(1);
                if (!seen.contains(key)) {
                    seen.insert(key);
                    const QString quoted = match.captured(2), value = quoted.mid(1, quoted.size() - 2);
                    const bool badClass = manifest && components.contains(name) && classes.contains(key) && (badNumber.match(value).hasMatch() || std::any_of(value.cbegin(), value.cend(), [](QChar character) { return character.unicode() > 127; }));
                    if (badClass) repaired += QChar(' ') + key + QStringLiteral("=\"com.winmt.obfuscated.c") + QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Md5).toHex().left(10)) + QChar('"');
                    else repaired += match.captured();
                }
                copied = match.capturedEnd();
            }
            repaired += tag.mid(copied);
            tag = repaired;
        }
        output += tag;
        position = end + 1;
    }
    return output;
}
Result prepareApkProject(const QString &source, const QString &destination, const QMap<QString, QString> &fields, const QString &icon, const Cancellation &cancel, bool preserveAdaptive) {
    if (!QFileInfo(QDir(source).filePath(QStringLiteral("apktool.yml"))).isFile() || QFileInfo::exists(destination)) return failed(QStringLiteral("工程无效或暂存工程已经存在"));
    QTemporaryDir staging(QFileInfo(destination).dir().filePath(QStringLiteral(".twinfile-project-XXXXXX")));
    if (!staging.isValid()) return failed(QStringLiteral("不能创建工程暂存目录"));
    const Result copied = transferFiles({source}, staging.path(), false, cancel);
    if (!copied.ok) return copied;
    const QString working = staging.filePath(QFileInfo(source).fileName());
    QStringList xmlFiles{QDir(working).filePath(QStringLiteral("AndroidManifest.xml"))};
    QDirIterator iterator(QDir(working).filePath(QStringLiteral("res")), {QStringLiteral("*.xml")}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (iterator.hasNext()) { if (xmlFiles.size() >= 200000) return failed(QStringLiteral("资源文件数量超限")); xmlFiles.append(iterator.next()); }
    int repairs = 0;
    for (const QString &path : xmlFiles) {
        if (cancel && cancel->load()) return failed(QStringLiteral("已取消"));
        const QByteArray original = read(path);
        if (original.isEmpty()) return failed(QStringLiteral("无法读取或资源 XML 超过 4 MiB：") + path);
        const bool manifest = path == xmlFiles.first();
        const QString repaired = repairResourceXml(QString::fromUtf8(original), manifest);
        QString error;
        const QDomDocument document = androidXml(repaired.toUtf8(), &error, 100000, 128);
        if (document.isNull()) return failed(QStringLiteral("资源 XML 无法安全修复：") + path + QChar('\n') + error);
        if (repaired.toUtf8() != original) { if (!write(path, repaired.toUtf8())) return failed(QStringLiteral("写入暂存工程失败")); ++repairs; }
    }
    QMap<QString, QString> changes = fields;
    if (!icon.isEmpty()) {
        QImageReader reader(icon); reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (!size.isValid() || qint64(size.width()) * size.height() > 64000000 || QFileInfo(icon).size() > 128 * 1024 * 1024) return failed(QStringLiteral("替换图标过大或无效"));
        const QImage image = reader.read();
        if (image.isNull()) return failed(reader.errorString());
        const QString name = QStringLiteral("twinfile_icon_") + QUuid::createUuid().toString(QUuid::Id128).left(12);
        const QList<QPair<QString, int>> densities{{QStringLiteral("mdpi"), 48}, {QStringLiteral("hdpi"), 72}, {QStringLiteral("xhdpi"), 96}, {QStringLiteral("xxhdpi"), 144}, {QStringLiteral("xxxhdpi"), 192}};
        for (const auto &density : densities) {
            const QString directory = QDir(working).filePath(QStringLiteral("res/mipmap-") + density.first);
            if (!QDir().mkpath(directory)) return failed(QStringLiteral("不能创建图标资源目录"));
            QImage resized(density.second, density.second, QImage::Format_ARGB32_Premultiplied); resized.fill(Qt::transparent);
            { QPainter painter(&resized); const auto fitted = image.scaled(resized.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation); painter.drawImage(QPoint((resized.width() - fitted.width()) / 2, (resized.height() - fitted.height()) / 2), fitted); }
            if (!resized.save(QDir(directory).filePath(name + QStringLiteral(".png")))) return failed(QStringLiteral("保存图标资源失败"));
        }
        changes.insert(QStringLiteral("icon"), QStringLiteral("@mipmap/") + name);
        changes.insert(QStringLiteral("roundIcon"), QStringLiteral("@mipmap/") + name);
        if (preserveAdaptive) {
            QString error;
            const auto manifest = androidXml(read(xmlFiles.first()), &error, 100000, 128);
            if (manifest.isNull()) return failed(error);
            const auto application = manifest.documentElement().firstChildElement(QStringLiteral("application"));
            const QRegularExpression reference(QStringLiteral("^@(mipmap|drawable)/([a-z0-9_]+)$"));
            bool foregroundNeeded = false;
            for (const QString &key : {QStringLiteral("icon"), QStringLiteral("roundIcon")}) {
                const auto match = reference.match(attribute(application, key));
                if (!match.hasMatch()) continue;
                const QString newName = name + QChar('_') + key.toLower();
                bool preserved = false;
                const QDir resources(QDir(working).filePath(QStringLiteral("res")));
                for (const QString &folder : resources.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    if (folder != match.captured(1) && !folder.startsWith(match.captured(1) + QChar('-'))) continue;
                    const QString path = resources.filePath(folder + QChar('/') + match.captured(2) + QStringLiteral(".xml"));
                    if (!QFileInfo::exists(path)) continue;
                    auto document = androidXml(read(path), &error, 100000, 128);
                    if (document.isNull()) return failed(error);
                    auto adaptive = document.documentElement();
                    if (adaptive.tagName() != QStringLiteral("adaptive-icon")) continue;
                    auto foreground = adaptive.firstChildElement(QStringLiteral("foreground"));
                    if (foreground.isNull()) return failed(QStringLiteral("自适应图标没有前景层；请选择整体替换"));
                    while (!foreground.firstChild().isNull()) foreground.removeChild(foreground.firstChild());
                    foreground.setAttribute(QStringLiteral("android:drawable"), QStringLiteral("@mipmap/") + name + QStringLiteral("_foreground"));
                    adaptive.setAttribute(QStringLiteral("xmlns:android"), QStringLiteral("http://schemas.android.com/apk/res/android"));
                    const QString outputFolder = resources.filePath(QStringLiteral("mipmap") + folder.mid(match.captured(1).size()));
                    if (!QDir().mkpath(outputFolder) || !write(QDir(outputFolder).filePath(newName + QStringLiteral(".xml")), document.toByteArray(2))) return failed(QStringLiteral("保存自适应图标失败"));
                    preserved = true; foregroundNeeded = true;
                }
                if (!preserved) continue;
                for (const auto &density : densities) {
                    const QDir directory(resources.filePath(QStringLiteral("mipmap-") + density.first));
                    if (!QFile::copy(directory.filePath(name + QStringLiteral(".png")), directory.filePath(newName + QStringLiteral(".png")))) return failed(QStringLiteral("保存兼容图标失败"));
                }
                changes.insert(key, QStringLiteral("@mipmap/") + newName);
            }
            if (foregroundNeeded) for (const auto &density : densities) {
                const int pixels = density.second * 108 / 48;
                QImage resized(pixels, pixels, QImage::Format_ARGB32_Premultiplied); resized.fill(Qt::transparent);
                { QPainter painter(&resized); const auto fitted = image.scaled(resized.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation); painter.drawImage(QPoint((pixels - fitted.width()) / 2, (pixels - fitted.height()) / 2), fitted); }
                if (!resized.save(QDir(working).filePath(QStringLiteral("res/mipmap-") + density.first + QChar('/') + name + QStringLiteral("_foreground.png")))) return failed(QStringLiteral("保存自适应前景失败"));
            }
        }
    }
    if (!changes.isEmpty()) {
        QString error;
        const QByteArray patched = patchAndroidManifest(read(xmlFiles.first()), changes, &error);
        if (patched.isEmpty() || !write(xmlFiles.first(), patched)) return failed(error.isEmpty() ? QStringLiteral("暂存清单保存失败") : error);
        const QString yamlPath = QDir(working).filePath(QStringLiteral("apktool.yml"));
        QString yaml = QString::fromUtf8(read(yamlPath));
        for (const QString &key : {QStringLiteral("versionCode"), QStringLiteral("versionName")}) {
            if (!changes.contains(key)) continue;
            QString value = changes.value(key); value.replace(QChar('\''), QStringLiteral("''"));
            const QRegularExpression pattern(QStringLiteral("(?m)^(\\s*%1:)\\s*[^\\r\\n]*").arg(key));
            auto match = pattern.match(yaml);
            if (match.hasMatch()) yaml.replace(match.capturedStart(), match.capturedLength(), match.captured(1) + QStringLiteral(" '") + value + QChar('\''));
        }
        if (changes.contains(QStringLiteral("package"))) yaml.replace(QRegularExpression(QStringLiteral("(?m)^(\\s*renameManifestPackage:)\\s*[^\\r\\n]*")), QStringLiteral("\\1 null"));
        if (!write(yamlPath, yaml.toUtf8())) return failed(QStringLiteral("暂存工程配置保存失败"));
    }
    if (cancel && cancel->load()) return failed(QStringLiteral("已取消"));
    if (!QDir().rename(working, destination)) return failed(QStringLiteral("无法发布暂存工程或目标已经存在"));
    return {true, {}, QStringLiteral("已在副本修复 %1 个资源文件；原工程未改动").arg(repairs), {destination}};
}
Result restoreApkManifest(const QString &project, const QString &apk, const Cancellation &cancel) {
    QByteArray built;
    { ZipReader reader(apk); built = reader.fileData(QStringLiteral("AndroidManifest.xml"), 4 * 1024 * 1024); }
    QString error;
    const auto document = androidXml(built, &error);
    if (document.isNull()) return failed(error);
    if (!document.toString().contains(QStringLiteral("com.winmt.obfuscated.c"))) return {};
    const QByteArray original = read(QDir(project).filePath(QStringLiteral("original/AndroidManifest.xml")));
    const auto originalDocument = androidXml(original, &error);
    if (originalDocument.isNull()) return failed(QStringLiteral("混淆工程缺少有效 original/AndroidManifest.xml；拒绝生成含虚假组件名的 APK"));
    const auto before = originalDocument.documentElement(), after = document.documentElement();
    QMap<QString, QString> fields;
    if (before.attribute(QStringLiteral("package")) != after.attribute(QStringLiteral("package"))) fields.insert(QStringLiteral("package"), after.attribute(QStringLiteral("package")));
    for (const QString &key : {QStringLiteral("versionCode"), QStringLiteral("versionName")}) if (attribute(before, key) != attribute(after, key)) fields.insert(key, attribute(after, key));
    for (const QString &key : {QStringLiteral("icon"), QStringLiteral("roundIcon"), QStringLiteral("label")}) {
        const QString value = attribute(after.firstChildElement(QStringLiteral("application")), key);
        if (value.isEmpty() || value == attribute(before.firstChildElement(QStringLiteral("application")), key)) continue;
        if (key == QStringLiteral("roundIcon") && attribute(before.firstChildElement(QStringLiteral("application")), key).isEmpty()) continue;
        if (key == QStringLiteral("label") && value.startsWith(QChar('@'))) continue;
        fields.insert(key, value);
    }
    const QByteArray patched = patchAndroidManifest(original, fields, &error);
    if (patched.isEmpty()) return failed(error);
    QTemporaryDir staging;
    const QString path = staging.filePath(QStringLiteral("AndroidManifest.xml"));
    if (!staging.isValid() || !write(path, patched)) return failed(QStringLiteral("不能暂存恢复后的清单"));
    ArchiveChanges changes; changes.files.insert(QStringLiteral("AndroidManifest.xml"), path);
    return editArchive(apk, changes, cancel);
}
}
