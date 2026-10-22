#include "android.h"
#include "operations.h"
#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringConverter>
#include <QTemporaryDir>
#include <QtEndian>
#include <stdexcept>

namespace twinfile {
namespace {
class AxmlError : public std::runtime_error {
public:
    explicit AxmlError(const QString &message) : std::runtime_error(message.toUtf8().constData()) {}
};
void check(bool condition, const QString &message) { if (!condition) throw AxmlError(message); }
quint16 shortAt(const QByteArray &data, qsizetype offset) { check(offset >= 0 && offset + 2 <= data.size(), QStringLiteral("AXML 字段越界")); return qFromLittleEndian<quint16>(data.constData() + offset); }
quint32 wordAt(const QByteArray &data, qsizetype offset) { check(offset >= 0 && offset + 4 <= data.size(), QStringLiteral("AXML 字段越界")); return qFromLittleEndian<quint32>(data.constData() + offset); }
void word(QByteArray &data, qsizetype offset, quint32 value) { check(offset >= 0 && offset + 4 <= data.size(), QStringLiteral("AXML 写入越界")); qToLittleEndian<quint32>(value, data.data() + offset); }
void appendWord(QByteArray &data, quint32 value) { const qsizetype position = data.size(); data.resize(position + 4); word(data, position, value); }
struct Attribute { QString element, name; qsizetype offset; quint32 value; quint8 type; };
}

QByteArray patchAndroidManifest(const QByteArray &bytes, const QMap<QString, QString> &fields, QString *error) {
    try {
        QString validation; const QDomDocument parsed = androidXml(bytes, &validation, 100000, 128); check(!parsed.isNull() && parsed.documentElement().tagName() == QStringLiteral("manifest"), validation.isEmpty() ? QStringLiteral("不是 Android 清单") : validation);
        if (fields.contains(QStringLiteral("package"))) check(QRegularExpression(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(?:\\.[A-Za-z_][A-Za-z0-9_]*)+$")).match(fields.value(QStringLiteral("package"))).hasMatch(), QStringLiteral("包名格式无效"));
        if (fields.contains(QStringLiteral("versionCode"))) { bool ok = false; const quint64 code = fields.value(QStringLiteral("versionCode")).toULongLong(&ok); check(ok && code <= 0x7fffffff, QStringLiteral("versionCode 应为 0–2147483647")); }
        const QMap<QString, QStringList> componentAttributes{
            {QStringLiteral("application"), {QStringLiteral("name"), QStringLiteral("backupAgent"), QStringLiteral("appComponentFactory"), QStringLiteral("manageSpaceActivity")}},
            {QStringLiteral("activity"), {QStringLiteral("name"), QStringLiteral("parentActivityName")}}, {QStringLiteral("activity-alias"), {QStringLiteral("name"), QStringLiteral("targetActivity")}},
            {QStringLiteral("service"), {QStringLiteral("name")}}, {QStringLiteral("receiver"), {QStringLiteral("name")}}, {QStringLiteral("provider"), {QStringLiteral("name")}}, {QStringLiteral("instrumentation"), {QStringLiteral("name")}}};
        if (bytes.trimmed().startsWith('<')) {
            QDomDocument document = parsed; auto root = document.documentElement();
            const QString oldPackage = root.attribute(QStringLiteral("package"));
            if (fields.contains(QStringLiteral("package")) && fields.value(QStringLiteral("package")) != oldPackage) {
                for (auto component = componentAttributes.cbegin(); component != componentAttributes.cend(); ++component) {
                    const auto nodes = document.elementsByTagName(component.key());
                    for (int index = 0; index < nodes.size(); ++index) {
                        auto element = nodes.at(index).toElement();
                        for (const auto &name : component.value()) {
                            const QString attribute = QStringLiteral("android:") + name;
                            const QString value = element.attribute(attribute);
                            if (value.startsWith(QChar('.'))) element.setAttribute(attribute, oldPackage + value);
                            else if (!value.isEmpty() && !value.contains(QChar('.')) && !value.startsWith(QChar('@')) && !value.startsWith(QChar('?'))) element.setAttribute(attribute, oldPackage + QChar('.') + value);
                        }
                    }
                }
            }
            for (auto item = fields.cbegin(); item != fields.cend(); ++item) {
                if (item.key() == QStringLiteral("package")) root.setAttribute(item.key(), item.value());
                else if (item.key() == QStringLiteral("versionCode") || item.key() == QStringLiteral("versionName")) root.setAttribute(QStringLiteral("android:") + item.key(), item.value());
                else if (QStringList{QStringLiteral("label"), QStringLiteral("icon"), QStringLiteral("roundIcon")}.contains(item.key())) { auto application = root.firstChildElement(QStringLiteral("application")); check(!application.isNull(), QStringLiteral("清单缺少 application")); application.setAttribute(QStringLiteral("android:") + item.key(), item.value()); }
                else check(false, QStringLiteral("不支持的清单字段：") + item.key());
            }
            return document.toByteArray(2);
        }
        const qsizetype poolOffset = shortAt(bytes, 2); check(shortAt(bytes, poolOffset) == 1, QStringLiteral("AXML 首块不是字符串池"));
        const quint16 headerSize = shortAt(bytes, poolOffset + 2); const quint32 poolSize = wordAt(bytes, poolOffset + 4), stringCount = wordAt(bytes, poolOffset + 8), styleCount = wordAt(bytes, poolOffset + 12), flags = wordAt(bytes, poolOffset + 16), stringStart = wordAt(bytes, poolOffset + 20), styleStart = wordAt(bytes, poolOffset + 24);
        check(headerSize >= 28 && poolOffset + poolSize <= bytes.size() && quint64(headerSize) + quint64(stringCount + styleCount) * 4 <= poolSize && styleCount <= stringCount, QStringLiteral("AXML 字符串池损坏"));
        const bool utf8 = flags & 0x100; QStringList strings; const qsizetype poolEnd = poolOffset + poolSize;
        for (quint32 index = 0; index < stringCount; ++index) {
            qsizetype position = poolOffset + stringStart + wordAt(bytes, poolOffset + headerSize + index * 4);
            auto length = [&]() { quint32 value; if (utf8) { check(position < poolEnd, QStringLiteral("AXML 字符串越界")); value = quint8(bytes[position++]); if (value & 128) { check(position < poolEnd, QStringLiteral("AXML 字符串长度越界")); value = ((value & 127) << 8) | quint8(bytes[position++]); } } else { value = shortAt(bytes, position); position += 2; if (value & 0x8000) { value = ((value & 0x7fff) << 16) | shortAt(bytes, position); position += 2; } } return value; };
            quint32 count = length(); if (utf8) count = length(); else { check(count <= 2 * 1024 * 1024, QStringLiteral("AXML 字符串过长")); count *= 2; }
            check(position >= poolOffset && position + count + (utf8 ? 1 : 2) <= poolEnd, QStringLiteral("AXML 字符串数据越界")); QStringDecoder decoder(utf8 ? QStringDecoder::Utf8 : QStringDecoder::Utf16LE); strings.append(decoder(QByteArrayView(bytes.constData() + position, count))); check(!decoder.hasError(), QStringLiteral("AXML 字符编码损坏"));
        }
        QList<Attribute> attributes; const qsizetype tailOffset = poolEnd; QByteArray tail = bytes.mid(tailOffset);
        for (qsizetype offset = tailOffset; offset + 8 <= bytes.size();) {
            const quint16 type = shortAt(bytes, offset), header = shortAt(bytes, offset + 2); const quint32 size = wordAt(bytes, offset + 4); check(size >= header && size >= 8 && offset + size <= bytes.size(), QStringLiteral("AXML 节点越界"));
            if (type == 0x102) {
                const qsizetype base = offset + header; const quint32 elementIndex = wordAt(bytes, base + 4); check(elementIndex < quint32(strings.size()), QStringLiteral("AXML 节点名无效"));
                const quint16 start = shortAt(bytes, base + 8), stride = shortAt(bytes, base + 10), count = shortAt(bytes, base + 12); check(stride >= 20 && quint64(base + start) + quint64(stride) * count <= quint64(offset + size), QStringLiteral("AXML 属性越界"));
                for (quint16 index = 0; index < count; ++index) { const qsizetype position = base + start + index * stride; const quint32 name = wordAt(bytes, position + 4); check(name < quint32(strings.size()), QStringLiteral("AXML 属性名无效")); attributes.append({strings[elementIndex], strings[name], position - tailOffset, wordAt(bytes, position + 16), quint8(bytes[position + 15])}); }
            }
            offset += size;
        }
        QString oldPackage; for (const auto &attribute : attributes) if (attribute.element == QStringLiteral("manifest") && attribute.name == QStringLiteral("package") && attribute.type == 3) oldPackage = strings.value(attribute.value);
        auto setString = [&](const Attribute &attribute, const QString &value) { check(strings.size() < 65536, QStringLiteral("AXML 字符串数量超限")); const quint32 index = quint32(strings.size()); strings.append(value); word(tail, attribute.offset + 8, index); tail[attribute.offset + 15] = char(3); word(tail, attribute.offset + 16, index); };
        const QString package = fields.value(QStringLiteral("package"));
        if (fields.contains(QStringLiteral("package"))) check(QRegularExpression(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(?:\\.[A-Za-z_][A-Za-z0-9_]*)+$")).match(package).hasMatch(), QStringLiteral("包名格式无效"));
        QSet<QString> applied;
        for (const auto &attribute : attributes) {
            if (!package.isEmpty() && package != oldPackage && !oldPackage.isEmpty() && attribute.type == 3 && componentAttributes.value(attribute.element).contains(attribute.name)) { const QString value = strings.value(attribute.value); if (value.startsWith(QChar('.'))) setString(attribute, oldPackage + value); else if (!value.isEmpty() && !value.contains(QChar('.')) && !value.startsWith(QChar('@')) && !value.startsWith(QChar('?'))) setString(attribute, oldPackage + QChar('.') + value); }
            if (!fields.contains(attribute.name)) continue;
            const bool target = attribute.element == QStringLiteral("manifest") ? QStringList{QStringLiteral("package"), QStringLiteral("versionCode"), QStringLiteral("versionName")}.contains(attribute.name) : attribute.element == QStringLiteral("application") && QStringList{QStringLiteral("label"), QStringLiteral("icon"), QStringLiteral("roundIcon")}.contains(attribute.name); if (!target) continue;
            check(!applied.contains(attribute.name), QStringLiteral("清单存在重复目标属性")); applied.insert(attribute.name);
            if (attribute.name == QStringLiteral("versionCode")) { bool ok = false; const quint64 code = fields.value(attribute.name).toULongLong(&ok); check(ok && code <= 0x7fffffff, QStringLiteral("versionCode 应为 0–2147483647")); word(tail, attribute.offset + 8, 0xffffffff); tail[attribute.offset + 15] = char(0x10); word(tail, attribute.offset + 16, quint32(code)); }
            else if (attribute.name == QStringLiteral("icon") || attribute.name == QStringLiteral("roundIcon")) {
                const QString value = fields.value(attribute.name); bool ok = false; const quint32 reference = value.startsWith(QStringLiteral("@0x")) ? value.mid(3).toUInt(&ok, 16) : 0;
                check(ok && reference != 0, QStringLiteral("二进制图标引用必须为现有 @0x 资源 ID")); word(tail, attribute.offset + 8, 0xffffffff); tail[attribute.offset + 15] = char(1); word(tail, attribute.offset + 16, reference);
            } else setString(attribute, fields.value(attribute.name));
        }
        for (const QString &key : fields.keys()) check(applied.contains(key), QStringLiteral("清单缺少或不支持修改此属性：") + key);
        QByteArray pool(28, '\0'), body, offsets;
        for (const QString &text : strings) {
            while (body.size() % 4) body.append('\0'); appendWord(offsets, quint32(body.size()));
            auto length = [&](quint32 count) { if (utf8) { check(count <= 32767, QStringLiteral("AXML UTF-8 字符串过长")); if (count > 127) body.append(char((count >> 8) | 128)); body.append(char(count & 255)); } else { if (count > 32767) { const qsizetype position = body.size(); body.resize(position + 2); qToLittleEndian<quint16>(quint16((count >> 16) | 0x8000), body.data() + position); } const qsizetype position = body.size(); body.resize(position + 2); qToLittleEndian<quint16>(quint16(count), body.data() + position); } };
            if (utf8) { const QByteArray encoded = text.toUtf8(); length(quint32(text.size())); length(quint32(encoded.size())); body += encoded; body.append('\0'); }
            else { QStringEncoder encoder(QStringEncoder::Utf16LE); length(quint32(text.size())); body += encoder(text); body.append(QByteArray(2, '\0')); }
        }
        while (body.size() % 4) body.append('\0'); const QByteArray styles = bytes.mid(poolOffset + headerSize + stringCount * 4, styleCount * 4);
        check(!styleCount || (styleStart > 0 && styleStart <= poolSize), QStringLiteral("AXML 样式池越界")); const QByteArray styleData = styleCount ? bytes.mid(poolOffset + styleStart, poolSize - styleStart) : QByteArray();
        qToLittleEndian<quint16>(1, pool.data()); qToLittleEndian<quint16>(28, pool.data() + 2); word(pool, 8, quint32(strings.size())); word(pool, 12, styleCount); word(pool, 16, flags & ~1U); word(pool, 20, quint32(28 + offsets.size() + styles.size())); word(pool, 24, styleCount ? quint32(28 + offsets.size() + styles.size() + body.size()) : 0);
        pool += offsets; pool += styles; pool += body; pool += styleData; while (pool.size() % 4) pool.append('\0'); word(pool, 4, quint32(pool.size())); QByteArray result = bytes.left(poolOffset) + pool + tail; word(result, 4, quint32(result.size()));
        check(!androidXml(result, &validation).isNull(), QStringLiteral("AXML 改写后校验失败")); return result;
    } catch (const std::exception &exception) { if (error) *error = QString::fromUtf8(exception.what()); return {}; }
}

Result quickEditApk(const QString &source, const QString &destination, const QMap<QString, QString> &fields, const QMap<QString, QString> &icons, const Cancellation &cancel) {
    auto fail = [](const QString &error) { return Result{false, error, {}, {}}; };
    if (QFileInfo::exists(destination)) return fail(QStringLiteral("输出已存在，不覆盖")); QTemporaryDir staging(QFileInfo(destination).dir().filePath(QStringLiteral(".twinfile-quick-apk-XXXXXX"))); if (!staging.isValid()) return fail(QStringLiteral("不能创建 APK 暂存目录"));
    const QString working = staging.filePath(QStringLiteral("working.apk")); if (!QFile::copy(source, working)) return fail(QStringLiteral("不能复制 APK")); ArchiveChanges changes;
    {
        ZipReader archive(source); QString error; const QByteArray manifest = archive.fileData(QStringLiteral("AndroidManifest.xml"), 4 * 1024 * 1024); const QByteArray patched = patchAndroidManifest(manifest, fields, &error); if (patched.isEmpty()) return fail(error);
        const QString manifestPath = staging.filePath(QStringLiteral("AndroidManifest.xml")); QFile output(manifestPath); if (!output.open(QIODevice::WriteOnly) || output.write(patched) != patched.size()) return fail(output.errorString()); output.close(); changes.files.insert(QStringLiteral("AndroidManifest.xml"), manifestPath);
        const QRegularExpression signatures(QStringLiteral("^META-INF/(?:MANIFEST\\.MF|[^/]+\\.(?:SF|RSA|DSA|EC))$"), QRegularExpression::CaseInsensitiveOption);
        for (const auto &entry : archive.fileInfoList()) if (signatures.match(entry.filePath).hasMatch()) changes.remove.append(entry.filePath);
        for (auto item = icons.cbegin(); item != icons.cend(); ++item) {
            if (!isImage(item.key()) || safeArchiveName(item.key()).isEmpty()) return fail(QStringLiteral("快速图标替换只支持现有位图资源，XML/adaptive 图标请使用反编译工程"));
            QImageReader reader(item.value()); reader.setAutoTransform(true); const QSize dimensions = reader.size(); if (!dimensions.isValid() || qint64(dimensions.width()) * dimensions.height() > 64000000) return fail(QStringLiteral("替换图片过大"));
            QImage image = reader.read(); if (image.isNull()) return fail(reader.errorString());
            const QByteArray existing = archive.fileData(item.key(), 32 * 1024 * 1024); if (existing.isEmpty()) return fail(QStringLiteral("快速替换目标必须是包内现有位图"));
            QBuffer originalBuffer; originalBuffer.setData(existing); originalBuffer.open(QIODevice::ReadOnly); QImageReader originalReader(&originalBuffer); const QSize targetSize = originalReader.size();
            if (!targetSize.isValid() || qint64(targetSize.width()) * targetSize.height() > 64000000) return fail(QStringLiteral("原图标尺寸无效"));
            QImage fitted(targetSize, QImage::Format_ARGB32_Premultiplied); fitted.fill(Qt::transparent); { QPainter painter(&fitted); const QImage scaled = image.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation); painter.drawImage(QPoint((fitted.width() - scaled.width()) / 2, (fitted.height() - scaled.height()) / 2), scaled); }
            const QString path = staging.filePath(QStringLiteral("icon-%1.").arg(changes.files.size()) + QFileInfo(item.key()).suffix()); if (!fitted.save(path)) return fail(QStringLiteral("不能编码替换图标")); changes.files.insert(item.key(), path);
        }
    }
    const Result edited = editArchive(working, changes, cancel); if (!edited.ok) return edited; return signApk(working, destination, cancel);
}

}
