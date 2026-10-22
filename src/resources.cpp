#include "android.h"
#include "android_strings.h"

#include <QStringDecoder>
#include <QtEndian>
#include <cstring>
#include <stdexcept>
#include <source_location>

namespace twinfile {
namespace {

class ResourceTable {
public:
    ResourceTable(const QByteArray &bytes, const Cancellation &cancel) : bytes_(bytes), cancel_(cancel) {}
    AndroidResources read() {
        require(bytes_.size() >= 12 && bytes_.size() <= 64 * 1024 * 1024 && shortAt(0) == 2 && wordAt(4) == quint32(bytes_.size()));
        const auto root = chunk(0, bytes_.size()); require(root.header >= 12 && wordAt(8) <= 256);
        for (qsizetype position = root.header; position < root.end;) {
            const auto part = chunk(position, root.end);
            if (part.type == 1) strings_ = pool(position, part);
            else if (part.type == 0x200) package(position, part);
            position = part.end;
        }
        return resources_;
    }
private:
    struct Chunk { quint16 type; quint16 header; qsizetype end; };
    void require(bool valid, const std::source_location &source = std::source_location::current()) const { if (!valid || (cancel_ && cancel_->load())) throw std::runtime_error(QStringLiteral("Invalid Android resource table at check %1").arg(source.line()).toStdString()); }
    quint16 shortAt(qsizetype offset) const { require(offset >= 0 && offset <= bytes_.size() - 2); return qFromLittleEndian<quint16>(bytes_.constData() + offset); }
    quint32 wordAt(qsizetype offset) const { require(offset >= 0 && offset <= bytes_.size() - 4); return qFromLittleEndian<quint32>(bytes_.constData() + offset); }
    Chunk chunk(qsizetype offset, qsizetype end) const {
        require(offset >= 0 && offset <= end - 8);
        const quint16 header = shortAt(offset + 2); const quint32 size = wordAt(offset + 4);
        require(header >= 8 && size >= header && size <= quint64(end - offset));
        return {shortAt(offset), header, offset + size};
    }
    QStringList pool(qsizetype offset, const Chunk &part) const {
        require(part.header >= 28);
        const quint32 count = wordAt(offset + 8), start = wordAt(offset + 20), styles = wordAt(offset + 24);
        const bool utf8 = wordAt(offset + 16) & 0x100;
        const quint64 offsetsEnd = quint64(part.header) + quint64(count) * 4 + quint64(wordAt(offset + 12)) * 4;
        require(count <= 500000 && offsetsEnd <= quint64(part.end - offset) && start >= offsetsEnd && start <= quint64(part.end - offset));
        const qsizetype end = styles ? offset + styles : part.end; require(end >= offset + start && end <= part.end);
        QStringList result; result.reserve(count);
        for (quint32 index = 0; index < count; ++index) {
            const quint32 relative = wordAt(offset + part.header + index * 4); require(relative < quint64(end - offset - start));
            qsizetype position = offset + start + relative;
            auto length = [&]() -> quint32 {
                require(position <= end - (utf8 ? 1 : 2));
                quint32 value = utf8 ? quint8(bytes_[position++]) : shortAt(position);
                if (!utf8) position += 2;
                const quint32 flag = utf8 ? 0x80 : 0x8000;
                if (value & flag) {
                    require(position <= end - (utf8 ? 1 : 2));
                    value = ((value & (flag - 1)) << (utf8 ? 8 : 16)) | (utf8 ? quint8(bytes_[position++]) : shortAt(position));
                    if (!utf8) position += 2;
                }
                return value;
            };
            quint64 size = length(); size = utf8 ? length() : size * 2;
            require(size <= 1024 * 1024 && size + (utf8 ? 1 : 2) <= quint64(end - position));
            require(bytes_[position + size] == 0 && (utf8 || bytes_[position + size + 1] == 0));
            bool valid; result.append(androidStringPoolText(QByteArrayView(bytes_.constData() + position, qsizetype(size)), utf8, &valid)); require(valid);
        }
        return result;
    }
    void package(qsizetype offset, const Chunk &part) {
        require(part.header >= 284); const quint32 packageId = wordAt(offset + 8), typeOffset = part.header >= 288 ? wordAt(offset + 284) : 0;
        require(packageId > 0 && packageId <= 255 && typeOffset <= 255);
        for (qsizetype position = offset + part.header; position < part.end;) {
            const auto child = chunk(position, part.end);
            if (child.type == 0x201) type(position, child, packageId, typeOffset);
            position = child.end;
        }
    }
    int rank(qsizetype offset, const Chunk &part) const {
        require(part.header >= 24); const quint32 size = wordAt(offset + 20); require(size >= 28 && size <= part.header - 20U);
        const qsizetype config = offset + 20;
        const quint16 density = shortAt(config + 14), sdk = shortAt(config + 24);
        int result = int(qMin<quint16>(sdk, 36)) * 100 + (density >= 0xfffe ? 80 : qMin<int>(density / 80, 79));
        if (wordAt(config + 8)) result -= 100000;
        if (size >= 32 && (quint8(bytes_[config + 29]) & 0x30) == 0x20) result -= 100000;
        return result;
    }
    void type(qsizetype offset, const Chunk &part, quint32 packageId, quint32 typeOffset) {
        const int score = rank(offset, part);
        const quint32 typeId = quint8(bytes_[offset + 8]) + typeOffset, flags = quint8(bytes_[offset + 9]);
        const quint32 count = wordAt(offset + 12), start = wordAt(offset + 16); const int stride = flags & 2 ? 2 : 4;
        require(typeId > 0 && typeId <= 255 && count <= 65536 && start >= part.header && start <= quint64(part.end - offset));
        require(quint64(part.header) + quint64(count) * stride <= start && !(flags & ~3U) && (flags & 3) != 3);
        for (quint32 index = 0; index < count; ++index) {
            require(++visits_ <= 2000000);
            const qsizetype item = offset + part.header + index * stride;
            quint32 entryId = index, relative;
            if (flags & 1) { entryId = shortAt(item); relative = quint32(shortAt(item + 2)) * 4; }
            else if (flags & 2) { relative = shortAt(item); if (relative == 0xffff) continue; relative *= 4; }
            else { relative = wordAt(item); if (relative == 0xffffffff) continue; }
            require(quint64(start) + relative + 8 <= quint64(part.end - offset));
            const qsizetype entry = offset + start + relative; const quint16 entryFlags = shortAt(entry + 2);
            if (entryFlags & 1) continue;
            quint8 kind; quint32 data;
            if (entryFlags & 8) { kind = quint8(entryFlags >> 8); data = wordAt(entry + 4); }
            else {
                const quint16 size = shortAt(entry); require(size >= 8 && size <= part.end - entry - 8);
                const qsizetype value = entry + size; require(shortAt(value) >= 8 && shortAt(value) <= part.end - value);
                kind = quint8(bytes_[value + 3]); data = wordAt(value + 4);
            }
            QString value;
            if (kind == 3) { require(data < quint32(strings_.size())); value = strings_[data]; }
            else if (kind == 1 || kind == 7) value = QStringLiteral("@0x%1").arg(data, 8, 16, QChar('0'));
            else if (kind >= 0x1c && kind <= 0x1f) value = QStringLiteral("#%1").arg(data, 8, 16, QChar('0'));
            else if (kind == 4) { float number; std::memcpy(&number, &data, 4); value = QString::number(number); }
            else if (kind == 0x10 || kind == 0x11) value = QString::number(qint32(data));
            if (value.isEmpty()) continue;
            const QString key = QStringLiteral("@0x%1").arg((packageId << 24) | (typeId << 16) | entryId, 8, 16, QChar('0'));
            if (!ranks_.contains(key) || score > ranks_.value(key)) { ranks_.insert(key, score); resources_.insert(key, value); }
        }
    }
    const QByteArray &bytes_;
    Cancellation cancel_;
    QStringList strings_;
    AndroidResources resources_;
    QHash<QString, int> ranks_;
    int visits_ = 0;
};

}

AndroidResources androidResources(const QByteArray &table, const Cancellation &cancel, QString *error) {
    if (error) error->clear();
    try { return ResourceTable(table, cancel).read(); }
    catch (const std::runtime_error &failure) { if (error) *error = QString::fromUtf8(failure.what()); return {}; }
}

}
