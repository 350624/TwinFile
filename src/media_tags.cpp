#include "media.h"
#include <QFile>
#include <QStringDecoder>
#include <QtEndian>
#include <stdexcept>

namespace twinfile {
namespace {
constexpr qsizetype metadataLimit = 8 * 1024 * 1024;
quint32 number(const QByteArray &bytes, qsizetype offset, int size = 4, bool little = false) {
    if (offset < 0 || size < 1 || size > 4 || offset + size > bytes.size()) throw std::runtime_error("metadata bounds");
    quint32 result = 0;
    for (int index = 0; index < size; ++index) result = (result << 8) | quint8(bytes[offset + (little ? size - index - 1 : index)]);
    return result;
}
quint32 syncsafe(const QByteArray &bytes, qsizetype offset) {
    quint32 result = 0;
    for (int index = 0; index < 4; ++index) {
        const quint32 value = number(bytes, offset + index, 1);
        if (value & 128) throw std::runtime_error("invalid syncsafe size");
        result = (result << 7) | value;
    }
    return result;
}
QString tagText(const QByteArray &bytes, int encoding) {
    if (encoding == 0) return QString::fromLatin1(bytes).remove(QChar(0)).trimmed();
    QStringDecoder decoder(encoding == 1 ? QStringDecoder::Utf16 : encoding == 2 ? QStringDecoder::Utf16BE : QStringDecoder::Utf8);
    const QString text = decoder(bytes);
    return decoder.hasError() ? QString() : QString(text).remove(QChar(0)).trimmed();
}
qsizetype skipDescription(const QByteArray &bytes, qsizetype offset, int encoding) {
    const int stride = encoding == 1 || encoding == 2 ? 2 : 1;
    while (offset + stride <= bytes.size()) {
        if (bytes[offset] == 0 && (stride == 1 || bytes[offset + 1] == 0)) return offset + stride;
        offset += stride;
    }
    return bytes.size();
}
QByteArray picture(const QByteArray &bytes) {
    qsizetype position = 4;
    position += 4 + number(bytes, position);
    position += 4 + number(bytes, position);
    position += 16;
    const auto size = number(bytes, position);
    position += 4;
    if (size > metadataLimit || position + size > bytes.size()) throw std::runtime_error("picture bounds");
    return bytes.mid(position, size);
}
void vorbis(const QByteArray &bytes, MediaTags &tags) {
    qsizetype position = 4 + number(bytes, 0, 4, true);
    const auto count = number(bytes, position, 4, true);
    position += 4;
    if (count > 100000) throw std::runtime_error("comment count");
    for (quint32 index = 0; index < count; ++index) {
        const auto size = number(bytes, position, 4, true);
        position += 4;
        if (position + size > bytes.size()) throw std::runtime_error("comment bounds");
        const QByteArray item = bytes.mid(position, size);
        position += size;
        const qsizetype separator = item.indexOf('=');
        if (separator < 0) continue;
        const QByteArray key = item.left(separator).toUpper();
        const QString value = QString::fromUtf8(item.mid(separator + 1));
        if (key == "LYRICS" || key == "UNSYNCEDLYRICS") tags.lyrics = value;
        else if (key == "TITLE") tags.title = value;
        else if (key == "ARTIST") tags.artist = value;
    }
}
}

MediaTags readMediaTags(const QString &path) {
    MediaTags tags;
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return tags;
    try {
        const QByteArray header = input.read(10);
        if (header.startsWith("fLaC")) {
            input.seek(4);
            qsizetype total = 0;
            for (int index = 0; index < 10000; ++index) {
                const QByteArray block = input.read(4);
                const quint8 type = quint8(number(block, 0, 1));
                const quint32 size = number(block, 1, 3);
                total += size + 4;
                if (total > metadataLimit) break;
                const QByteArray bytes = input.read(size);
                if (bytes.size() != size) break;
                if ((type & 127) == 4) vorbis(bytes, tags);
                else if ((type & 127) == 6 && tags.cover.isEmpty()) tags.cover = picture(bytes);
                if (type & 128) break;
            }
        } else if (header.startsWith("ID3")) {
            const quint8 version = quint8(number(header, 3, 1));
            const quint8 flags = quint8(number(header, 5, 1));
            const quint32 size = syncsafe(header, 6);
            if (version < 2 || version > 4 || size > metadataLimit || (version == 2 && (flags & 64))) return tags;
            QByteArray bytes = input.read(size);
            if (bytes.size() != size) return tags;
            if (flags & 128) bytes.replace(QByteArray::fromHex("ff00"), QByteArray::fromHex("ff"));
            qsizetype position = 0;
            if ((flags & 64) && version >= 3) position = version == 3 ? 4 + number(bytes, 0) : syncsafe(bytes, 0);
            for (int index = 0; index < 100000 && position + (version == 2 ? 6 : 10) <= bytes.size(); ++index) {
                const int nameSize = version == 2 ? 3 : 4;
                const QByteArray name = bytes.mid(position, nameSize);
                if (name[0] == 0) break;
                const quint32 length = version == 2 ? number(bytes, position + 3, 3) : version == 4 ? syncsafe(bytes, position + 4) : number(bytes, position + 4);
                const quint32 frameFlags = version == 2 ? 0 : number(bytes, position + 8, 2);
                position += version == 2 ? 6 : 10;
                if (length > metadataLimit || position + length > bytes.size()) break;
                QByteArray body = bytes.mid(position, length);
                position += length;
                if (body.isEmpty() || (version == 3 && (frameFlags & 0xe0)) || (version == 4 && (frameFlags & 0x4d))) continue;
                if (version == 4 && (frameFlags & 2)) body.replace(QByteArray::fromHex("ff00"), QByteArray::fromHex("ff"));
                const quint8 encoding = quint8(body[0]);
                if (encoding > 3) continue;
                if (name == "TIT2" || name == "TT2") tags.title = tagText(body.mid(1), encoding);
                else if (name == "TPE1" || name == "TP1") tags.artist = tagText(body.mid(1), encoding);
                else if ((name == "USLT" || name == "ULT") && body.size() >= 4) tags.lyrics = tagText(body.mid(skipDescription(body, 4, encoding)), encoding);
                else if ((name == "APIC" || name == "PIC") && tags.cover.isEmpty()) {
                    qsizetype start = version == 2 ? 4 : skipDescription(body, 1, 0);
                    if (start >= body.size()) continue;
                    start = skipDescription(body, start + 1, encoding);
                    tags.cover = body.mid(start);
                }
            }
        }
    } catch (const std::exception &) {}
    return tags;
}
}
