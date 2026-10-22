#include "android.h"
#include "operations.h"
#include "android_strings.h"

#include <QBuffer>
#include <QDir>
#include <QThread>
#include <QFileInfo>
#include <QImageReader>
#include <QLockFile>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringDecoder>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QtEndian>
#include <QXmlStreamReader>
#include <webp/decode.h>
#include <algorithm>
#include <cstring>

namespace twinfile {

QDomDocument androidXml(const QByteArray &bytes, QString *error, int nodeLimit, int depthLimit) {
    auto failed = [error] { if (error) *error = QStringLiteral("Android XML 无效或过于复杂"); return QDomDocument(); };
    if (bytes.size() > 4 * 1024 * 1024 || nodeLimit < 1 || nodeLimit > 100000 || depthLimit < 1 || depthLimit > 128) return failed();
    QDomDocument document;
    if (bytes.trimmed().startsWith('<')) {
        QXmlStreamReader reader(bytes);
        QList<QDomElement> elements;
        int count = 0;
        while (!reader.atEnd()) {
            const auto token = reader.readNext();
            if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) return failed();
            if (token == QXmlStreamReader::StartElement) {
                if (++count > nodeLimit || elements.size() >= depthLimit || reader.attributes().size() > 128) return failed();
                QDomElement element = document.createElement(reader.qualifiedName().toString());
                for (const auto &declaration : reader.namespaceDeclarations()) element.setAttribute(declaration.prefix().isEmpty() ? QStringLiteral("xmlns") : QStringLiteral("xmlns:") + declaration.prefix().toString(), declaration.namespaceUri().toString());
                for (const auto &attribute : reader.attributes()) element.setAttribute(attribute.qualifiedName().toString(), attribute.value().toString());
                if (elements.isEmpty()) document.appendChild(element); else elements.last().appendChild(element);
                elements.append(element);
            } else if (token == QXmlStreamReader::EndElement) {
                if (elements.isEmpty()) return failed();
                elements.removeLast();
            } else if (token == QXmlStreamReader::Characters && !elements.isEmpty() && !reader.isWhitespace()) elements.last().appendChild(document.createTextNode(reader.text().toString()));
        }
        if (reader.hasError() || document.documentElement().isNull() || !elements.isEmpty()) return failed();
        return document;
    }
    auto shortAt = [&](qsizetype offset) -> quint16 { return qFromLittleEndian<quint16>(bytes.constData() + offset); };
    auto wordAt = [&](qsizetype offset) -> quint32 { return qFromLittleEndian<quint32>(bytes.constData() + offset); };
    if (bytes.size() < 36 || shortAt(0) != 3 || wordAt(4) != quint32(bytes.size())) return failed();
    QStringList strings;
    QList<QDomElement> stack;
    int nodes = 0;
    for (qsizetype offset = shortAt(2); offset + 8 <= bytes.size();) {
        const quint16 type = shortAt(offset), header = shortAt(offset + 2);
        const quint32 length = wordAt(offset + 4);
        if (header < 8 || length < header || length > quint32(bytes.size() - offset)) return failed();
        const qsizetype end = offset + length;
        if (type == 1) {
            if (header < 28 || !strings.isEmpty()) return failed();
            const quint32 count = wordAt(offset + 8), start = wordAt(offset + 20);
            if (count > 65536 || quint64(header) + quint64(count) * 4 > length || start < header || start > length) return failed();
            const bool utf8 = wordAt(offset + 16) & 0x100;
            for (quint32 index = 0; index < count; ++index) {
                const quint32 relative = wordAt(offset + header + index * 4);
                if (quint64(start) + relative >= length) return failed();
                qsizetype position = offset + start + relative;
                auto readLength = [&]() -> qint64 {
                    if (position + (utf8 ? 1 : 2) > end) return -1;
                    quint32 value = utf8 ? quint8(bytes[position++]) : shortAt(position);
                    if (!utf8) position += 2;
                    const quint32 high = utf8 ? 0x80 : 0x8000;
                    if (value & high) {
                        if (position + (utf8 ? 1 : 2) > end) return -1;
                        value = ((value & (high - 1)) << (utf8 ? 8 : 16)) | (utf8 ? quint8(bytes[position++]) : shortAt(position));
                        if (!utf8) position += 2;
                    }
                    return value;
                };
                qint64 countBytes = readLength();
                if (countBytes < 0) return failed();
                if (utf8) countBytes = readLength(); else countBytes *= 2;
                if (countBytes < 0 || countBytes > end - position - (utf8 ? 1 : 2)) return failed();
                bool valid; const QString text = androidStringPoolText(QByteArrayView(bytes.constData() + position, countBytes), utf8, &valid);
                if (!valid) return failed();
                strings.append(text);
            }
        } else if (type == 0x102) {
            const qsizetype base = offset + header;
            if (base + 20 > end || ++nodes > nodeLimit || stack.size() >= depthLimit) return failed();
            const quint32 name = wordAt(base + 4);
            const quint16 start = shortAt(base + 8), stride = shortAt(base + 10), count = shortAt(base + 12);
            if (name >= quint32(strings.size()) || stride < 20 || start < 20 || count > 128 || base + start + quint64(stride) * count > quint64(end)) return failed();
            QDomElement element = document.createElement(strings[name]);
            for (quint16 index = 0; index < count; ++index) {
                const qsizetype attr = base + start + index * stride;
                const quint32 key = wordAt(attr + 4), raw = wordAt(attr + 8), value = wordAt(attr + 16);
                const quint8 kind = quint8(bytes[attr + 15]);
                if (key >= quint32(strings.size()) || shortAt(attr + 12) < 8) return failed();
                QString text;
                if (kind == 3) { if (value >= quint32(strings.size())) return failed(); text = strings[value]; }
                else if (kind == 1) text = QStringLiteral("@0x%1").arg(value, 8, 16, QChar('0'));
                else if (kind == 4) { float number; std::memcpy(&number, &value, sizeof(number)); text = QString::number(number); }
                else if (kind >= 0x1c && kind <= 0x1f) text = QStringLiteral("#%1").arg(value, 8, 16, QChar('0'));
                else if (raw < quint32(strings.size())) text = strings[raw];
                else text = QString::number(qint32(value));
                element.setAttribute(strings[key], text);
            }
            if (stack.isEmpty()) { if (!document.documentElement().isNull()) return failed(); document.appendChild(element); }
            else stack.last().appendChild(element);
            stack.append(element);
        } else if (type == 0x103) {
            if (stack.isEmpty() || offset + header + 8 > end) return failed();
            const quint32 name = wordAt(offset + header + 4);
            if (name >= quint32(strings.size()) || strings[name] != stack.last().tagName()) return failed();
            stack.removeLast();
        }
        offset = end;
    }
    if (document.documentElement().isNull() || !stack.isEmpty()) return failed();
    return document;
}

AndroidResources androidResources(const QString &dump) {
    AndroidResources resources;
    static const QRegularExpression resource(QStringLiteral(R"(^\s*resource (0x[0-9a-fA-F]{8}) \S+(?: PUBLIC)?\s*$)"));
    static const QRegularExpression config(QStringLiteral(R"(^\s*\(([^)]*)\)\s+(.+)$)"));
    static const QRegularExpression file(QStringLiteral(R"re(^\(file\)\s+([^\s]+)(?:\s+.*)?$)re"));
    QHash<QString, int> ranks;
    QString current;
    for (const QString &line : dump.split(QChar('\n'))) {
        const auto key = resource.match(line);
        if (key.hasMatch()) { current = QChar('@') + key.captured(1).toLower(); if (resources.size() > 100000) break; continue; }
        if (current.isEmpty()) continue;
        const auto match = config.match(line);
        if (!match.hasMatch()) continue;
        QString value = match.captured(2).trimmed();
        const auto path = file.match(value);
        if (path.hasMatch()) value = path.captured(1);
        if (value.startsWith(QChar('"')) && value.endsWith(QChar('"'))) value = value.mid(1, value.size() - 2);
        const QString configuration = match.captured(1);
        int rank = configuration.contains(QStringLiteral("night")) ? -100 : 0;
        if (configuration.contains(QStringLiteral("xxxhdpi"))) rank += 6;
        else if (configuration.contains(QStringLiteral("xxhdpi"))) rank += 5;
        else if (configuration.contains(QStringLiteral("xhdpi"))) rank += 4;
        else if (configuration.contains(QStringLiteral("hdpi"))) rank += 3;
        if (!ranks.contains(current) || rank > ranks.value(current)) { resources.insert(current, value); ranks.insert(current, rank); }
    }
    return resources;
}

static QString attr(const QDomElement &element, const QString &name, const QString &fallback = {}) {
    return element.attribute(name, element.attribute(QStringLiteral("android:") + name, fallback));
}

static QColor drawableColor(QString value) {
    if (value.startsWith(QChar('#')) && (value.size() == 4 || value.size() == 5)) { QString expanded = QStringLiteral("#"); for (qsizetype index = 1; index < value.size(); ++index) { expanded += value[index]; expanded += value[index]; } value = expanded; }
    return QColor(value);
}

QImage androidBitmap(const QByteArray &bytes, int size) {
    if (size < 1 || size > 192 || bytes.isEmpty() || bytes.size() > 4 * 1024 * 1024) return {};
    if (bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP") {
        WebPDecoderConfig config;
        if (!WebPInitDecoderConfig(&config) || WebPGetFeatures(reinterpret_cast<const uint8_t *>(bytes.constData()), bytes.size(), &config.input) != VP8_STATUS_OK) return {};
        if (config.input.width < 1 || config.input.height < 1 || qint64(config.input.width) * config.input.height > 16000000) return {};
        const QSize fitted = QSize(config.input.width, config.input.height).scaled(size, size, Qt::KeepAspectRatio);
        QImage image(fitted, QImage::Format_RGBA8888); if (image.isNull()) return {};
        config.options.use_scaling = 1; config.options.scaled_width = image.width(); config.options.scaled_height = image.height();
        config.output.colorspace = MODE_RGBA; config.output.is_external_memory = 1; config.output.u.RGBA.rgba = image.bits(); config.output.u.RGBA.stride = image.bytesPerLine(); config.output.u.RGBA.size = image.sizeInBytes();
        const auto status = WebPDecode(reinterpret_cast<const uint8_t *>(bytes.constData()), bytes.size(), &config);
        WebPFreeDecBuffer(&config.output); return status == VP8_STATUS_OK ? image : QImage();
    }
    QBuffer input; input.setData(bytes); input.open(QIODevice::ReadOnly); QImageReader reader(&input);
    if (!QList<QByteArray>{"png", "jpeg", "jpg", "gif"}.contains(reader.format().toLower())) return {};
    const QSize dimensions = reader.size();
    if (!dimensions.isValid() || qint64(dimensions.width()) * dimensions.height() > 16000000) return {};
    reader.setScaledSize(dimensions.scaled(size, size, Qt::KeepAspectRatio)); return reader.read();
}

class DrawableRenderer {
public:
    DrawableRenderer(ZipReader &archive, const AndroidResources &resources, int size) : archive_(archive), resources_(resources), size_(size) {}
    QImage render(QString value, int depth = 0) {
        if (++visits_ > 2048 || depth > 16 || size_ < 1 || size_ > 192) return {};
        if (value.startsWith(QChar('@'))) {
            if (value == QStringLiteral("@android:color/transparent")) return render(QStringLiteral("#00000000"), depth + 1);
            if (value == QStringLiteral("@android:color/white")) return render(QStringLiteral("#ffffffff"), depth + 1);
            if (value == QStringLiteral("@android:color/black")) return render(QStringLiteral("#ff000000"), depth + 1);
            value = resources_.value(value.toLower());
            if (value.isEmpty()) return {};
            return render(value, depth + 1);
        }
        if (value.startsWith(QChar('#'))) {
            const QColor color = drawableColor(value); if (!color.isValid()) return {};
            QImage image(size_, size_, QImage::Format_ARGB32_Premultiplied); image.fill(color); return image;
        }
        const bool inlineXml = value.startsWith(QChar('<'));
        if (!inlineXml && safeArchiveName(value).isEmpty()) return {};
        const QByteArray bytes = inlineXml ? value.toUtf8() : archive_.fileData(value, 4 * 1024 * 1024);
        if (bytes.isEmpty()) return {};
        if (!inlineXml && !value.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)) {
            return androidBitmap(bytes, size_);
        }
        QString error;
        const QDomDocument document = androidXml(bytes, &error);
        const QDomElement root = document.documentElement();
        if (root.isNull()) return {};
        QImage image(size_, size_, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        if (root.tagName() == QStringLiteral("adaptive-icon") || root.tagName() == QStringLiteral("layer-list")) {
            QPainter painter(&image); painter.setRenderHint(QPainter::SmoothPixmapTransform); painter.setRenderHint(QPainter::Antialiasing);
            const bool adaptive = root.tagName() == QStringLiteral("adaptive-icon");
            if (adaptive) { QPainterPath mask; mask.addRoundedRect(QRectF(0, 0, size_, size_), size_ * 0.22, size_ * 0.22); painter.setClipPath(mask); }
            bool painted = false;
            bool background = false, foreground = false;
            for (QDomElement child = root.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
                if (adaptive && child.tagName() != QStringLiteral("background") && child.tagName() != QStringLiteral("foreground")) continue;
                QString drawable = attr(child, QStringLiteral("drawable"));
                if (drawable.isEmpty() && !child.firstChildElement().isNull()) { QDomDocument fragment; fragment.appendChild(fragment.importNode(child.firstChildElement(), true)); fragment.documentElement().setAttribute(QStringLiteral("xmlns:android"), QStringLiteral("http://schemas.android.com/apk/res/android")); fragment.documentElement().setAttribute(QStringLiteral("xmlns:aapt"), QStringLiteral("http://schemas.android.com/aapt")); drawable = fragment.toString(); }
                const QImage layer = render(drawable, depth + 1);
                if (layer.isNull()) return {};
                if (adaptive) {
                    painter.drawImage(QRectF(-size_ * 0.25, -size_ * 0.25, size_ * 1.5, size_ * 1.5), layer);
                    background |= child.tagName() == QStringLiteral("background"); foreground |= child.tagName() == QStringLiteral("foreground");
                } else painter.drawImage(QRect(0, 0, size_, size_), layer);
                painted = true;
            }
            return painted && (!adaptive || (background && foreground)) ? image : QImage();
        }
        if (root.tagName() == QStringLiteral("bitmap")) return render(attr(root, QStringLiteral("src")), depth + 1);
        if (root.tagName() == QStringLiteral("selector")) {
            for (QDomElement child = root.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
                const QImage selected = render(attr(child, QStringLiteral("drawable")), depth + 1); if (!selected.isNull()) return selected;
            }
            return {};
        }
        if (root.tagName() == QStringLiteral("shape")) {
            if (attr(root, QStringLiteral("shape"), QStringLiteral("0")) != QStringLiteral("0") && attr(root, QStringLiteral("shape")) != QStringLiteral("rectangle")) return {};
            const auto solid = root.firstChildElement(QStringLiteral("solid"));
            if (solid.isNull() || !root.firstChildElement(QStringLiteral("gradient")).isNull() || !root.firstChildElement(QStringLiteral("stroke")).isNull()) return {};
            const QString colorText = attr(solid, QStringLiteral("color"));
            if (!colorText.startsWith(QChar('#'))) return render(colorText, depth + 1);
            const QColor color = drawableColor(colorText); if (!color.isValid()) return {};
            const auto corners = root.firstChildElement(QStringLiteral("corners"));
            QString radiusText = attr(corners, QStringLiteral("radius"), QStringLiteral("0")); radiusText.remove(QRegularExpression(QStringLiteral("[a-zA-Z]+$")));
            QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing); painter.setPen(Qt::NoPen); painter.setBrush(color); const double radius = qBound(0.0, radiusText.toDouble(), double(size_) / 2); painter.drawRoundedRect(QRectF(0, 0, size_, size_), radius, radius); return image;
        }
        if (root.tagName() != QStringLiteral("vector")) return {};
        QDomDocument svg;
        QDomElement canvas = svg.createElement(QStringLiteral("svg"));
        canvas.setAttribute(QStringLiteral("xmlns"), QStringLiteral("http://www.w3.org/2000/svg"));
        canvas.setAttribute(QStringLiteral("width"), size_); canvas.setAttribute(QStringLiteral("height"), size_);
        canvas.setAttribute(QStringLiteral("viewBox"), QStringLiteral("0 0 %1 %2").arg(attr(root, QStringLiteral("viewportWidth"), QStringLiteral("24")), attr(root, QStringLiteral("viewportHeight"), QStringLiteral("24"))));
        svg.appendChild(canvas);
        QDomElement definitions = svg.createElement(QStringLiteral("defs")); canvas.appendChild(definitions);
        canvas.setAttribute(QStringLiteral("opacity"), attr(root, QStringLiteral("alpha"), QStringLiteral("1")));
        std::function<bool(QDomElement, QDomElement, int)> convert;
        auto resolve = [&](QString value) {
            for (int iteration = 0; value.startsWith(QChar('@')) && iteration < 16; ++iteration) {
                if (value == QStringLiteral("@android:color/transparent")) return QStringLiteral("#00000000");
                if (value == QStringLiteral("@android:color/white")) return QStringLiteral("#ffffffff");
                if (value == QStringLiteral("@android:color/black")) return QStringLiteral("#ff000000");
                value = resources_.value(value.toLower());
            }
            return value;
        };
        std::function<bool(QDomElement &, const QString &, QString, double, int)> paint;
        paint = [&](QDomElement &element, const QString &kind, QString value, double alpha, int level) -> bool {
            if (level > 16 || ++visits_ > 2048) return false;
            value = resolve(value);
            if (value.endsWith(QStringLiteral(".xml")) || value.startsWith(QChar('<'))) {
                QString error; const auto document = androidXml(value.startsWith(QChar('<')) ? value.toUtf8() : archive_.fileData(value, 4 * 1024 * 1024), &error); const auto node = document.documentElement(); if (node.isNull()) return false;
                if (node.tagName() == QStringLiteral("selector")) { QDomElement selected = node.firstChildElement(); for (auto item = node.firstChildElement(); !item.isNull(); item = item.nextSiblingElement()) { bool state = false; const auto attributes = item.attributes(); for (int index = 0; index < attributes.size(); ++index) if (attributes.item(index).nodeName().contains(QStringLiteral("state_"))) state = true; if (!state) { selected = item; break; } } return paint(element, kind, attr(selected, QStringLiteral("color"), QStringLiteral("#00000000")), alpha, level + 1); }
                if (node.tagName() != QStringLiteral("gradient")) return false;
                const QString type = attr(node, QStringLiteral("type"), QStringLiteral("0")); const bool radial = type == QStringLiteral("1") || type == QStringLiteral("radial"); if (!radial && type != QStringLiteral("0") && type != QStringLiteral("linear")) return false;
                auto gradient = svg.createElement(radial ? QStringLiteral("radialGradient") : QStringLiteral("linearGradient")); const QString id = QStringLiteral("gradient%1").arg(definitions.childNodes().size()); gradient.setAttribute(QStringLiteral("id"), id); gradient.setAttribute(QStringLiteral("gradientUnits"), QStringLiteral("userSpaceOnUse"));
                if (radial) { gradient.setAttribute(QStringLiteral("cx"), attr(node, QStringLiteral("centerX"), QStringLiteral("0"))); gradient.setAttribute(QStringLiteral("cy"), attr(node, QStringLiteral("centerY"), QStringLiteral("0"))); gradient.setAttribute(QStringLiteral("r"), attr(node, QStringLiteral("gradientRadius"), QStringLiteral("1"))); }
                else for (const auto &coordinate : QList<QPair<QString, QString>>{{QStringLiteral("x1"), QStringLiteral("startX")}, {QStringLiteral("y1"), QStringLiteral("startY")}, {QStringLiteral("x2"), QStringLiteral("endX")}, {QStringLiteral("y2"), QStringLiteral("endY")}}) gradient.setAttribute(coordinate.first, attr(node, coordinate.second, QStringLiteral("0")));
                QList<QPair<QString, QString>> stops; for (auto stop = node.firstChildElement(); !stop.isNull(); stop = stop.nextSiblingElement()) stops.append({attr(stop, QStringLiteral("offset"), QStringLiteral("0")), attr(stop, QStringLiteral("color"))});
                if (stops.isEmpty()) { stops.append({QStringLiteral("0"), attr(node, QStringLiteral("startColor"))}); if (!attr(node, QStringLiteral("centerColor")).isEmpty()) stops.append({QStringLiteral("0.5"), attr(node, QStringLiteral("centerColor"))}); stops.append({QStringLiteral("1"), attr(node, QStringLiteral("endColor"))}); }
                for (const auto &entry : stops) { const QColor color = drawableColor(resolve(entry.second)); if (!color.isValid()) return false; auto stop = svg.createElement(QStringLiteral("stop")); stop.setAttribute(QStringLiteral("offset"), qBound(0.0, entry.first.toDouble(), 1.0)); stop.setAttribute(QStringLiteral("stop-color"), color.name(QColor::HexRgb)); stop.setAttribute(QStringLiteral("stop-opacity"), color.alphaF()); gradient.appendChild(stop); }
                definitions.appendChild(gradient); element.setAttribute(kind, QStringLiteral("url(#%1)").arg(id)); element.setAttribute(kind + QStringLiteral("-opacity"), qBound(0.0, alpha, 1.0)); return true;
            }
            const QColor color = drawableColor(value);
            if (!color.isValid()) return false;
            element.setAttribute(kind, color.name(QColor::HexRgb));
            element.setAttribute(kind + QStringLiteral("-opacity"), color.alphaF() * qBound(0.0, alpha, 1.0)); return true;
        };
        convert = [&](QDomElement parent, QDomElement input, int level) -> bool {
            if (level > 16 || ++visits_ > 2048) return false;
            for (QDomElement child = input.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
                if (child.tagName() == QStringLiteral("group")) {
                    QDomElement group = svg.createElement(QStringLiteral("g"));
                    const QString pivotX = attr(child, QStringLiteral("pivotX"), QStringLiteral("0")), pivotY = attr(child, QStringLiteral("pivotY"), QStringLiteral("0"));
                    group.setAttribute(QStringLiteral("transform"), QStringLiteral("translate(%1 %2) translate(%3 %4) rotate(%5) scale(%6 %7) translate(-%3 -%4)")
                        .arg(attr(child, QStringLiteral("translateX"), QStringLiteral("0")), attr(child, QStringLiteral("translateY"), QStringLiteral("0")), pivotX, pivotY,
                             attr(child, QStringLiteral("rotation"), QStringLiteral("0")), attr(child, QStringLiteral("scaleX"), QStringLiteral("1")), attr(child, QStringLiteral("scaleY"), QStringLiteral("1"))));
                    parent.appendChild(group); if (!convert(group, child, level + 1)) return false;
                } else if (child.tagName() == QStringLiteral("path")) {
                    if (attr(child, QStringLiteral("trimPathStart"), QStringLiteral("0")).toDouble() != 0 || attr(child, QStringLiteral("trimPathEnd"), QStringLiteral("1")).toDouble() != 1) return false;
                    const QString data = attr(child, QStringLiteral("pathData")); if (data.size() > 256 * 1024) return false;
                    QDomElement path = svg.createElement(QStringLiteral("path")); path.setAttribute(QStringLiteral("d"), data);
                    QString fill = attr(child, QStringLiteral("fillColor"), QStringLiteral("#00000000"));
                    for (auto attribute = child.firstChildElement(); !attribute.isNull(); attribute = attribute.nextSiblingElement()) if (attribute.tagName().endsWith(QStringLiteral("attr")) && attr(attribute, QStringLiteral("name")).endsWith(QStringLiteral("fillColor")) && !attribute.firstChildElement().isNull()) { QDomDocument fragment; fragment.appendChild(fragment.importNode(attribute.firstChildElement(), true)); fragment.documentElement().setAttribute(QStringLiteral("xmlns:android"), QStringLiteral("http://schemas.android.com/apk/res/android")); fill = fragment.toString(); }
                    if (!paint(path, QStringLiteral("fill"), fill, attr(child, QStringLiteral("fillAlpha"), QStringLiteral("1")).toDouble(), level + 1) || !paint(path, QStringLiteral("stroke"), attr(child, QStringLiteral("strokeColor"), QStringLiteral("#00000000")), attr(child, QStringLiteral("strokeAlpha"), QStringLiteral("1")).toDouble(), level + 1)) return false;
                    path.setAttribute(QStringLiteral("stroke-width"), attr(child, QStringLiteral("strokeWidth"), QStringLiteral("0")));
                    path.setAttribute(QStringLiteral("stroke-linecap"), attr(child, QStringLiteral("strokeLineCap")) == QStringLiteral("1") ? QStringLiteral("round") : attr(child, QStringLiteral("strokeLineCap")) == QStringLiteral("2") ? QStringLiteral("square") : QStringLiteral("butt"));
                    path.setAttribute(QStringLiteral("stroke-linejoin"), attr(child, QStringLiteral("strokeLineJoin")) == QStringLiteral("1") ? QStringLiteral("round") : attr(child, QStringLiteral("strokeLineJoin")) == QStringLiteral("2") ? QStringLiteral("bevel") : QStringLiteral("miter"));
                    path.setAttribute(QStringLiteral("stroke-miterlimit"), attr(child, QStringLiteral("strokeMiterLimit"), QStringLiteral("4")));
                    path.setAttribute(QStringLiteral("fill-rule"), attr(child, QStringLiteral("fillType")) == QStringLiteral("1") ? QStringLiteral("evenodd") : QStringLiteral("nonzero"));
                    parent.appendChild(path);
                } else return false;
            }
            return true;
        };
        if (!convert(canvas, root, 0)) return {};
        QSvgRenderer renderer(svg.toByteArray()); if (!renderer.isValid()) return {};
        QPainter painter(&image); renderer.render(&painter); return image;
    }
private:
    ZipReader &archive_;
    const AndroidResources &resources_;
    int size_;
    int visits_ = 0;
};

QImage androidDrawable(ZipReader &archive, const AndroidResources &resources, const QString &name, int size) { return DrawableRenderer(archive, resources, size).render(name); }

QString aaptPath() {
    const QString supplied = toolPath(QStringLiteral("aapt2.exe")); if (!supplied.isEmpty()) return supplied;
    const QString directory = qEnvironmentVariable("TWINFILE_TOOL_CACHE", QDir(qEnvironmentVariable("APPDATA", QDir::homePath())).filePath(QStringLiteral("TwinFileNative/tools")));
    if (!QDir().mkpath(directory)) return {};
    const QString destination = QDir(directory).filePath(QStringLiteral("aapt2.exe"));
    QLockFile lock(destination + QStringLiteral(".lock")); if (!lock.tryLock(5000)) return {};
    if (QFileInfo(destination).isFile()) return destination;
    ZipReader archive(toolPath(QStringLiteral("apktool.jar")));
    const QByteArray executable = archive.fileData(QStringLiteral("prebuilt/windows/aapt2.exe"), 32 * 1024 * 1024);
    if (!executable.startsWith("MZ")) return {};
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly) || output.write(executable) != executable.size() || !output.commit()) return {};
    return destination;
}

QImage apkIcon(const QString &path, const Cancellation &cancel) {
    if (cancel && cancel->load()) return {};
    ZipReader archive(path); if (archive.status() != ZipReader::NoError) return {};
    QString error; const auto manifest = androidXml(archive.fileData(QStringLiteral("AndroidManifest.xml"), 4 * 1024 * 1024), &error, 100000, 128);
    const auto application = manifest.documentElement().firstChildElement(QStringLiteral("application"));
    QStringList nativeNames;
    for (auto component = application.firstChildElement(); !component.isNull(); component = component.nextSiblingElement()) {
        if (component.tagName() != QStringLiteral("activity") && component.tagName() != QStringLiteral("activity-alias")) continue;
        const QString enabled = attr(component, QStringLiteral("enabled"), QStringLiteral("true"));
        if (enabled == QStringLiteral("false") || enabled == QStringLiteral("0")) continue;
        bool launcher = false;
        for (auto filter = component.firstChildElement(QStringLiteral("intent-filter")); !filter.isNull(); filter = filter.nextSiblingElement(QStringLiteral("intent-filter"))) {
            bool main = false, category = false;
            for (auto child = filter.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
                main |= child.tagName() == QStringLiteral("action") && attr(child, QStringLiteral("name")) == QStringLiteral("android.intent.action.MAIN");
                category |= child.tagName() == QStringLiteral("category") && attr(child, QStringLiteral("name")) == QStringLiteral("android.intent.category.LAUNCHER");
            }
            launcher |= main && category;
        }
        if (launcher) { if (!attr(component, QStringLiteral("icon")).isEmpty()) nativeNames.append(attr(component, QStringLiteral("icon"))); break; }
    }
    nativeNames.append(attr(application, QStringLiteral("icon"))); nativeNames.append(attr(application, QStringLiteral("roundIcon"))); nativeNames.removeAll(QString()); nativeNames.removeDuplicates();
    AndroidResources resources;
    if (!nativeNames.isEmpty()) resources = androidResources(archive.fileData(QStringLiteral("resources.arsc"), 64 * 1024 * 1024), cancel);
    for (const QString &name : nativeNames) {
        if (cancel && cancel->load()) return {};
        const QImage image = androidDrawable(archive, resources, name); if (!image.isNull()) return image;
    }
    if (cancel && cancel->load()) return {};
    const QString aapt = aaptPath(); if (aapt.isEmpty()) return {};
    const Result badging = runProcess(aapt, {QStringLiteral("dump"), QStringLiteral("badging"), path}, 4000, cancel);
    if (!badging.ok) return {};
    static const QRegularExpression expression(QStringLiteral(R"(^application-icon-(\d+):\s*'([^']+)'\s*$)"));
    static const QRegularExpression primary(QStringLiteral(R"(\bicon='([^']+)')"));
    QList<QPair<int, QString>> names;
    for (const QString &line : badging.output.split(QChar('\n'))) {
        const auto match = expression.match(line.trimmed());
        if (match.hasMatch()) names.append({match.captured(1).toInt(), match.captured(2)});
        else if (line.startsWith(QStringLiteral("application:"))) { const auto found = primary.match(line); if (found.hasMatch()) names.append({-1, found.captured(1)}); }
    }
    std::sort(names.begin(), names.end(), [](const auto &first, const auto &second) { return first.first > second.first; });
    bool loadedResources = !resources.isEmpty();
    QSet<QString> attempted;
    for (const auto &candidate : names.mid(0, 16)) {
        if (cancel && cancel->load()) return {};
        if (attempted.contains(candidate.second)) continue;
        attempted.insert(candidate.second);
        if (candidate.second.endsWith(QStringLiteral(".xml")) && !loadedResources) {
            const Result dump = runProcess(aapt, {QStringLiteral("dump"), QStringLiteral("resources"), path}, 4000, cancel);
            if (dump.ok) resources = androidResources(dump.output);
            loadedResources = true;
        }
        const QImage image = androidDrawable(archive, resources, candidate.second);
        if (!image.isNull()) return image;
    }
    return {};
}

Result buildApk(const QString &source, const QString &destination, bool sign, const Cancellation &cancel, const QMap<QString, QString> &fields, const QString &icon, bool preserveAdaptive) {
    auto fail = [](const QString &error) { return Result{false, error, {}, {}}; };
    if (!QFileInfo(QDir(source).filePath(QStringLiteral("apktool.yml"))).isFile() || QFileInfo::exists(destination)) return fail(QStringLiteral("工程无效或输出文件已经存在"));
    const QString java = toolPath(QStringLiteral("runtime/bin/java.exe"));
    QTemporaryDir staging(QFileInfo(destination).dir().filePath(QStringLiteral(".twinfile-apk-XXXXXX")));
    if (!staging.isValid()) return fail(QStringLiteral("无法创建回编译临时目录"));
    const QString working = staging.filePath(QStringLiteral("project"));
    Result result = prepareApkProject(source, working, fields, icon, cancel, preserveAdaptive);
    if (!result.ok) return result;
    const QString unsignedApk = staging.filePath(QStringLiteral("unsigned.apk"));
    for (int attempt = 0; attempt < 6; ++attempt) {
        result = runProcess(java, {QStringLiteral("-Xmx2g"), QStringLiteral("-Dfile.encoding=UTF-8"), QStringLiteral("-jar"), toolPath(QStringLiteral("apktool.jar")), QStringLiteral("b"), QStringLiteral("-p"), staging.filePath(QStringLiteral("framework")), QStringLiteral("-j"), QString::number(qMax(1, QThread::idealThreadCount())), working, QStringLiteral("-o"), unsignedApk}, 3600000, cancel);
        if (result.ok) break;
        if (cancel && cancel->load()) return result;
        const auto match = QRegularExpression(QStringLiteral("attribute\\s+['\"]?(android:[\\w.]+)['\"]?\\s+not\\s+found")).match(result.error + QChar('\n') + result.output);
        if (!match.hasMatch()) return result;
        const QString path = QDir(working).filePath(QStringLiteral("AndroidManifest.xml")); QFile input(path);
        if (!input.open(QIODevice::ReadOnly)) return result;
        const QByteArray original = input.readAll(); input.close();
        QString text = QString::fromUtf8(original); text.remove(QRegularExpression(QStringLiteral("\\s+%1\\s*=\\s*(?:\"[^\"]*\"|'[^']*')").arg(QRegularExpression::escape(match.captured(1)))));
        if (text.toUtf8() == original) return result;
        QSaveFile output(path); const QByteArray bytes = text.toUtf8(); if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) return fail(QStringLiteral("暂存清单修复失败"));
    }
    if (!result.ok) return result;
    result = restoreApkManifest(working, unsignedApk, cancel); if (!result.ok) return result;
    if (sign) return signApk(unsignedApk, destination, cancel);
    if (cancel && cancel->load()) return fail(QStringLiteral("已取消"));
    if (!QFile::rename(unsignedApk, destination)) return fail(QStringLiteral("保存失败或目标已经存在"));
    return {true, {}, QStringLiteral("未签名 APK 已生成"), {destination}};
}

Result signApk(const QString &source, const QString &destination, const Cancellation &cancel) {
    auto fail = [](const QString &error) { return Result{false, error, {}, {}}; };
    if (!QFileInfo(source).isFile() || QFileInfo::exists(destination)) return fail(QStringLiteral("来源无效或输出已存在"));
    QTemporaryDir staging(QFileInfo(destination).dir().filePath(QStringLiteral(".twinfile-sign-XXXXXX")));
    if (!staging.isValid()) return fail(QStringLiteral("不能创建签名暂存目录"));
    const QString java = toolPath(QStringLiteral("runtime/bin/java.exe"));
    QString completed;
    Result result;
    {
        const QString signer = toolPath(QStringLiteral("apksigner.jar"));
        if (signer.isEmpty()) return fail(QStringLiteral("找不到 apksigner.jar"));
        const QString directory = QFileInfo(settingsPath()).absolutePath();
        if (!QDir().mkpath(directory)) return fail(QStringLiteral("无法创建签名配置目录"));
        const QString keystore = QDir(directory).filePath(QStringLiteral("debug.keystore"));
        QLockFile lock(keystore + QStringLiteral(".lock")); if (!lock.tryLock(1000)) return fail(QStringLiteral("签名密钥正被其它任务使用"));
        if (!QFileInfo::exists(keystore)) {
            const QString temporary = staging.filePath(QStringLiteral("debug.keystore"));
            result = runProcess(toolPath(QStringLiteral("runtime/bin/keytool.exe")), {QStringLiteral("-genkeypair"), QStringLiteral("-keystore"), temporary, QStringLiteral("-alias"), QStringLiteral("twinfilekey"), QStringLiteral("-storepass"), QStringLiteral("twinfile-debug"), QStringLiteral("-keypass"), QStringLiteral("twinfile-debug"), QStringLiteral("-dname"), QStringLiteral("CN=TwinFile Debug,O=TwinFile,C=CN"), QStringLiteral("-validity"), QStringLiteral("10950"), QStringLiteral("-keyalg"), QStringLiteral("RSA"), QStringLiteral("-keysize"), QStringLiteral("2048")}, 60000, cancel);
            if (!result.ok) return result;
            if (!QFile::copy(temporary, keystore)) return fail(QStringLiteral("无法保存签名密钥"));
        }
        const QString aligned = staging.filePath(QStringLiteral("aligned.apk"));
        if (!QFile::copy(source, aligned)) return fail(QStringLiteral("无法暂存待对齐 APK"));
        ArchiveChanges clean;
        { ZipReader reader(aligned); const QRegularExpression signatures(QStringLiteral("^META-INF/(?:MANIFEST\\.MF|[^/]+\\.(?:SF|RSA|DSA|EC))$"), QRegularExpression::CaseInsensitiveOption); for (const auto &entry : reader.fileInfoList()) if (signatures.match(entry.filePath).hasMatch()) clean.remove.append(entry.filePath); }
        result = editArchive(aligned, clean, cancel); if (!result.ok) return result;
        completed = staging.filePath(QStringLiteral("signed.apk"));
        result = runProcess(java, {QStringLiteral("-Dfile.encoding=UTF-8"), QStringLiteral("-jar"), signer, QStringLiteral("sign"), QStringLiteral("--ks"), keystore, QStringLiteral("--ks-pass"), QStringLiteral("pass:twinfile-debug"), QStringLiteral("--key-pass"), QStringLiteral("pass:twinfile-debug"), QStringLiteral("--ks-key-alias"), QStringLiteral("twinfilekey"), QStringLiteral("--out"), completed, aligned}, 300000, cancel);
        if (!result.ok) return result;
        result = runProcess(java, {QStringLiteral("-jar"), signer, QStringLiteral("verify"), QStringLiteral("--verbose"), completed}, 120000, cancel);
        if (!result.ok) return result;
    }
    if (cancel && cancel->load()) return fail(QStringLiteral("已取消"));
    if (!QFile::rename(completed, destination)) return fail(QStringLiteral("保存失败或目标已经存在"));
    return {true, {}, QStringLiteral("APK 已用独立调试密钥签名并校验，不能覆盖原作者签名的已安装应用。"), {destination}};
}

}
