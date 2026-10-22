#pragma once

#include <QByteArrayView>
#include <QStringDecoder>
#include <QtEndian>

namespace twinfile {

inline QString androidStringPoolText(QByteArrayView bytes, bool utf8, bool *valid) {
    *valid = true;
    if (!utf8) {
        if (bytes.size() % 2) { *valid = false; return {}; }
        QString result(bytes.size() / 2, Qt::Uninitialized);
        for (qsizetype index = 0; index < result.size(); ++index) result[index] = QChar(qFromLittleEndian<quint16>(bytes.data() + index * 2));
        return result;
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless); const QString decoded = decoder(bytes);
    if (!decoder.hasError()) return decoded;
    QString result; result.reserve(bytes.size());
    for (qsizetype position = 0; position < bytes.size();) {
        const quint8 lead = quint8(bytes[position++]); quint32 point; int count;
        if (lead < 0x80) { result.append(QChar(lead)); continue; }
        if ((lead & 0xe0) == 0xc0) { point = lead & 0x1f; count = 1; }
        else if ((lead & 0xf0) == 0xe0) { point = lead & 0x0f; count = 2; }
        else if ((lead & 0xf8) == 0xf0) { point = lead & 7; count = 3; }
        else { *valid = false; return {}; }
        if (position + count > bytes.size()) { *valid = false; return {}; }
        for (int index = 0; index < count; ++index) {
            const quint8 next = quint8(bytes[position++]); if ((next & 0xc0) != 0x80) { *valid = false; return {}; }
            point = (point << 6) | (next & 0x3f);
        }
        if ((count == 1 && point < 0x80 && point != 0) || (count == 2 && point < 0x800) || (count == 3 && point < 0x10000) || point > 0x10ffff) { *valid = false; return {}; }
        if (point <= 0xffff) result.append(QChar(quint16(point)));
        else { result.append(QChar(QChar::highSurrogate(point))); result.append(QChar(QChar::lowSurrogate(point))); }
    }
    return result;
}

}
