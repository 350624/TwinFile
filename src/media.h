#pragma once
#include <QDialog>
#include <QString>
#include <QList>
#include <QByteArray>
#include "core.h"

namespace twinfile {
struct LyricWord { qint64 start = 0, end = 0; int offset = 0, length = 0; };
struct LyricLine { qint64 time = 0; QString text; QList<LyricWord> words; };
struct MediaTags { QString title, artist, lyrics; QByteArray cover; };
MediaTags readMediaTags(const QString &path);
int mediaIndex(int current, int count, int mode, bool forward = true);
QList<Entry> mediaPlaylist(const QList<Entry> &entries, const Entry &current);
QList<LyricLine> parseLyrics(const QString &text);
bool isMedia(const QString &name);
class MediaDialog : public QDialog {
public:
    MediaDialog(const QString &path, QWidget *parent, const QList<Entry> &neighbors = {}, const Entry &current = {});
    ~MediaDialog() override;
private:
    Cancellation cancel_;
};
}
