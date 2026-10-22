#include "media.h"
#include "editor.h"
#include "core.h"
#include "operations.h"
#include <QAudioOutput>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QBuffer>
#include <QImageReader>
#include <QSaveFile>
#include <QSlider>
#include <QSet>
#include <QTextEdit>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QtConcurrent>
#include <algorithm>

namespace twinfile {
bool isMedia(const QString &name) {
    static const QStringList extensions{QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("flac"), QStringLiteral("aac"), QStringLiteral("m4a"), QStringLiteral("ogg"), QStringLiteral("opus"), QStringLiteral("wma"), QStringLiteral("ape"), QStringLiteral("aiff"), QStringLiteral("aif"), QStringLiteral("alac"), QStringLiteral("amr"), QStringLiteral("ac3"), QStringLiteral("dts"), QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mkv"), QStringLiteral("avi"), QStringLiteral("mov"), QStringLiteral("wmv"), QStringLiteral("webm"), QStringLiteral("flv"), QStringLiteral("ts"), QStringLiteral("m2ts"), QStringLiteral("mpeg"), QStringLiteral("mpg"), QStringLiteral("3gp"), QStringLiteral("rmvb"), QStringLiteral("vob")};
    static const QStringList additional{QStringLiteral("oga"), QStringLiteral("mid"), QStringLiteral("midi"), QStringLiteral("mka"), QStringLiteral("mp2"), QStringLiteral("m3u"), QStringLiteral("wv"), QStringLiteral("rm"), QStringLiteral("3g2"), QStringLiteral("ogv"), QStringLiteral("asf"), QStringLiteral("f4v")};
    return extensions.contains(QFileInfo(name).suffix().toLower()) || additional.contains(QFileInfo(name).suffix().toLower());
}
int mediaIndex(int current, int count, int mode, bool forward) {
    if (count <= 0) return -1;
    current = qBound(0, current, count - 1);
    if (count == 1 || mode == 1) return current;
    if (mode == 2) { const int selected = QRandomGenerator::global()->bounded(count - 1); return selected >= current ? selected + 1 : selected; }
    return (current + (forward ? 1 : count - 1)) % count;
}
QList<Entry> mediaPlaylist(const QList<Entry> &entries, const Entry &current) {
    QList<Entry> result;
    QSet<QString> seen;
    for (const auto &entry : entries) if (!entry.directory && isMedia(entry.name) && !seen.contains(entry.key()) && result.size() < 5000) { result.append(entry); seen.insert(entry.key()); }
    if (!current.name.isEmpty() && !current.directory && isMedia(current.name) && !seen.contains(current.key())) { if (result.size() == 5000) result.removeLast(); result.append(current); }
    return result;
}
QList<LyricLine> parseLyrics(const QString &text) {
    QList<LyricLine> result; if (text.size() > 2 * 1024 * 1024) return result;
    static const QRegularExpression timestamp(QStringLiteral("\\[(\\d{1,3}):(\\d{1,2})(?:[.:](\\d{1,3}))?\\]"));
    static const QRegularExpression wordPattern(QStringLiteral("<(\\d+),(\\d+)>([^<]*)|<(\\d{1,3}):(\\d{1,2})[.:](\\d{1,3})>([^<]*)"));
    static const QRegularExpression offsetPattern(QStringLiteral("\\[offset:([+-]?\\d+)\\]"), QRegularExpression::CaseInsensitiveOption); const qint64 offset = offsetPattern.match(text).captured(1).toLongLong();
    for (const QString &line : text.split(QChar('\n'))) {
        auto matches = timestamp.globalMatch(line); QList<qint64> times; qsizetype end = 0;
        while (matches.hasNext()) { const auto match = matches.next(); const int seconds = match.captured(2).toInt(); if (seconds >= 60) continue; const qint64 time = (match.captured(1).toLongLong() * 60 + seconds) * 1000 + match.captured(3).leftJustified(3, QChar('0')).toInt() + offset; times.append(qMax<qint64>(0, time)); end = match.capturedEnd(); }
        const QString rest = line.mid(end).trimmed();
        for (qint64 time : times) {
            LyricLine lyric; lyric.time = time;
            auto words = wordPattern.globalMatch(rest);
            qsizetype copied = 0;
            while (words.hasNext()) {
                const auto word = words.next();
                lyric.text += rest.mid(copied, word.capturedStart() - copied);
                const bool relative = !word.captured(1).isEmpty();
                const QString text = word.captured(relative ? 3 : 7);
                const qint64 start = relative ? time + word.captured(1).toLongLong() : (word.captured(4).toLongLong() * 60 + word.captured(5).toLongLong()) * 1000 + word.captured(6).leftJustified(3, QChar('0')).toLongLong() + offset;
                const qint64 finish = relative ? start + word.captured(2).toLongLong() : start + 1000;
                lyric.words.append({start, finish, int(lyric.text.size()), int(text.size())});
                lyric.text += text;
                copied = word.capturedEnd();
            }
            lyric.text += rest.mid(copied);
            result.append(lyric);
        }
        if (result.size() >= 100000) break;
    }
    std::stable_sort(result.begin(), result.end(), [](const auto &first, const auto &second) { return first.time < second.time; }); return result;
}

MediaDialog::MediaDialog(const QString &path, QWidget *parent, const QList<Entry> &sourceEntries, const Entry &original) : QDialog(parent) {
    setAttribute(Qt::WA_DeleteOnClose); setWindowTitle(QFileInfo(path).fileName()); resize(1050, 760); auto *layout = new QVBoxLayout(this);
    auto *playlist = new QComboBox; playlist->setMaxVisibleItems(16); layout->addWidget(playlist);
    const QFileInfo current(path); const auto neighbors = current.dir().entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase); int initial = 0;
    const QList<Entry> remote = sourceEntries.isEmpty() ? QList<Entry>() : mediaPlaylist(sourceEntries, original);
    if (remote.isEmpty()) {
        for (const auto &file : neighbors) if (isMedia(file.fileName()) && playlist->count() < 5000) { if (file.absoluteFilePath() == current.absoluteFilePath()) initial = playlist->count(); playlist->addItem(file.fileName(), file.absoluteFilePath()); }
    } else {
        for (const auto &entry : remote) { if ((!original.name.isEmpty() && entry.key() == original.key()) || (original.name.isEmpty() && entry.name == current.fileName())) initial = playlist->count(); playlist->addItem(entry.name); }
    }
    if (playlist->count() == 0) playlist->addItem(current.fileName(), path);
    playlist->setCurrentIndex(initial);
    auto *cover = new QLabel; cover->setAlignment(Qt::AlignCenter); cover->setMaximumHeight(180); cover->hide(); layout->addWidget(cover);
    auto *video = new QVideoWidget; layout->addWidget(video, 3); auto *lyrics = new QTextEdit; lyrics->setReadOnly(true); lyrics->setMaximumHeight(200); layout->addWidget(lyrics, 1);
    auto *player = new QMediaPlayer(this); auto *audio = new QAudioOutput(this); player->setAudioOutput(audio); player->setVideoOutput(video); audio->setVolume(0.65);
    auto *controls = new QHBoxLayout; auto *previous = new QPushButton(QStringLiteral("上一首")); auto *play = new QPushButton(QStringLiteral("播放 / 暂停")); auto *next = new QPushButton(QStringLiteral("下一首")); auto *seek = new QSlider(Qt::Horizontal); auto *volume = new QSlider(Qt::Horizontal); volume->setRange(0, 100); volume->setValue(65); volume->setMaximumWidth(100);
    controls->addWidget(previous); controls->addWidget(play); controls->addWidget(next); controls->addWidget(seek, 1); controls->addWidget(new QLabel(QStringLiteral("音量"))); controls->addWidget(volume); layout->addLayout(controls);
    auto *order = new QComboBox; order->addItems({QStringLiteral("列表循环"), QStringLiteral("单曲循环"), QStringLiteral("随机播放")}); order->setCurrentIndex(qBound(0, readSettings(settingsPath()).value(QStringLiteral("player_order")).toInt(), 2)); controls->addWidget(order);
    connect(order, &QComboBox::currentIndexChanged, this, [](int value) { auto settings = readSettings(settingsPath()); settings.insert(QStringLiteral("player_order"), value); writeSettings(settingsPath(), settings); });
    auto *lyricTools = new QHBoxLayout; auto *local = new QPushButton(QStringLiteral("载入本地 LRC")); auto *online = new QPushButton(QStringLiteral("在线搜索歌词")); auto *save = new QPushButton(QStringLiteral("保存歌词副本")); lyricTools->addWidget(local); lyricTools->addWidget(online); lyricTools->addWidget(save); layout->addLayout(lyricTools);
    auto *status = new QLabel(QStringLiteral("支持格式取决于随包解码器；在线歌词只在点击并确认后请求。手机/包内切歌会按需暂存，不修改原文件。")); status->setWordWrap(true); layout->addWidget(status);
    struct State { QString text; QList<LyricLine> lines; int highlighted = -1; std::shared_ptr<QTemporaryDir> temporary; }; auto state = std::make_shared<State>();
    auto loadLyrics = [state, lyrics](const QString &text) { state->text = text; state->lines = parseLyrics(text); state->highlighted = -1; QStringList lines; for (const auto &line : state->lines) lines.append(line.text); lyrics->setPlainText(state->lines.isEmpty() ? text : lines.join(QChar('\n'))); };
    auto loadLocal = [loadLyrics](const QString &file) { QFile input(file); if (!input.open(QIODevice::ReadOnly) || input.size() > 2 * 1024 * 1024) return; const QByteArray bytes = input.readAll(); TextDocument decoded; if (!decodeText(bytes, {}, &decoded).ok && !decodeText(bytes, QStringLiteral("GB18030"), &decoded).ok) return; loadLyrics(decoded.text); };
    auto playFile = [player, state, lyrics, loadLocal, loadLyrics, cover, this](const QString &file) {
        state->text.clear(); state->lines.clear(); state->highlighted = -1; lyrics->clear(); cover->clear(); cover->hide();
        const MediaTags tags = readMediaTags(file);
        if (!tags.lyrics.isEmpty()) loadLyrics(tags.lyrics);
        const QString base = QFileInfo(file).dir().filePath(QFileInfo(file).completeBaseName());
        if (QFileInfo::exists(base + QStringLiteral(".lrc"))) loadLocal(base + QStringLiteral(".lrc"));
        else if (QFileInfo::exists(base + QStringLiteral(".txt"))) loadLocal(base + QStringLiteral(".txt"));
        QBuffer buffer; buffer.setData(tags.cover); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer);
        const QSize size = reader.size();
        if (size.isValid() && qint64(size.width()) * size.height() <= 64000000) { reader.setScaledSize(size.scaled(256, 180, Qt::KeepAspectRatio)); const QImage image = reader.read(); if (!image.isNull()) { cover->setPixmap(QPixmap::fromImage(image)); cover->show(); } }
        setWindowTitle(tags.title.isEmpty() ? QFileInfo(file).fileName() : tags.title + QStringLiteral(" — ") + tags.artist);
        player->setSource(QUrl::fromLocalFile(file)); player->play();
    };
    auto activate = [this, remote, sourceEntries, playlist, player, status, state, playFile](int index) {
        if (index < 0) return;
        if (remote.isEmpty()) { playFile(playlist->itemData(index).toString()); return; }
        if (index >= remote.size()) return;
        if (cancel_) cancel_->store(true);
        cancel_ = std::make_shared<std::atomic_bool>(false);
        const auto cancel = cancel_;
        const Entry entry = remote[index];
        auto temporary = std::make_shared<QTemporaryDir>();
        if (!temporary->isValid()) { status->setText(QStringLiteral("无法创建媒体暂存目录")); return; }
        player->stop(); playlist->setEnabled(false); status->setText(QStringLiteral("正在读取：") + entry.name);
        auto *watcher = new QFutureWatcher<Result>(this);
        connect(watcher, &QFutureWatcher<Result>::finished, this, [watcher, temporary, playlist, status, state, playFile] {
            const Result result = watcher->result(); watcher->deleteLater(); playlist->setEnabled(true);
            if (!result.ok || result.paths.isEmpty()) { status->setText(result.error); return; }
            playFile(result.paths.first()); state->temporary = temporary; status->setText(QStringLiteral("手机 / 包内媒体 · 按需暂存，关闭后清理"));
        });
        watcher->setFuture(QtConcurrent::run([entry, sourceEntries, temporary, cancel] {
            const Result result = materializeEntry(entry, temporary->path(), cancel);
            if (!result.ok) return result;
            const QString base = QFileInfo(entry.name).completeBaseName();
            for (const auto &candidate : sourceEntries) {
                if (!candidate.directory && candidate.size <= 2 * 1024 * 1024 && (candidate.name == base + QStringLiteral(".lrc") || candidate.name == base + QStringLiteral(".txt"))) {
                    const Result lyrics = materializeEntry(candidate, temporary->path(), cancel);
                    if (!lyrics.ok && cancel->load()) return lyrics;
                }
            }
            return result;
        }));
    };
    connect(playlist, &QComboBox::currentIndexChanged, this, activate);
    auto advance = [playlist, player, order](bool forward) { if (!playlist->isEnabled()) return; const int index = mediaIndex(playlist->currentIndex(), playlist->count(), order->currentIndex(), forward); if (index < 0) return; if (index == playlist->currentIndex()) { player->setPosition(0); player->play(); } else playlist->setCurrentIndex(index); };
    connect(previous, &QPushButton::clicked, playlist, [advance] { advance(false); });
    connect(next, &QPushButton::clicked, playlist, [advance] { advance(true); });
    connect(player, &QMediaPlayer::mediaStatusChanged, playlist, [advance](QMediaPlayer::MediaStatus state) { if (state == QMediaPlayer::EndOfMedia) advance(true); });
    connect(play, &QPushButton::clicked, player, [player] { if (player->playbackState() == QMediaPlayer::PlayingState) player->pause(); else player->play(); });
    connect(player, &QMediaPlayer::durationChanged, seek, [seek](qint64 value) { seek->setRange(0, int(qMin<qint64>(value, INT_MAX))); });
    connect(player, &QMediaPlayer::positionChanged, this, [seek, state, lyrics](qint64 value) {
        if (!seek->isSliderDown()) seek->setValue(int(qMin<qint64>(value, INT_MAX)));
        if (state->lines.isEmpty()) return;
        const auto iterator = std::upper_bound(state->lines.cbegin(), state->lines.cend(), value, [](qint64 time, const LyricLine &line) { return time < line.time; });
        const int index = int(iterator - state->lines.cbegin()) - 1;
        if (index < 0) { lyrics->setExtraSelections({}); return; }
        const bool changed = index != state->highlighted; state->highlighted = index;
        QTextCursor cursor(lyrics->document()); cursor.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor, index); const int lineStart = cursor.position(); cursor.select(QTextCursor::BlockUnderCursor);
        QTextEdit::ExtraSelection selected; selected.cursor = cursor; selected.format.setBackground(lyrics->palette().highlight()); selected.format.setForeground(lyrics->palette().highlightedText()); QList<QTextEdit::ExtraSelection> selections{selected};
        for (const auto &word : state->lines[index].words) if (value >= word.start && value < word.end) { QTextEdit::ExtraSelection highlighted; highlighted.cursor = QTextCursor(lyrics->document()); highlighted.cursor.setPosition(lineStart + word.offset); highlighted.cursor.setPosition(lineStart + word.offset + word.length, QTextCursor::KeepAnchor); highlighted.format.setBackground(QColor(QStringLiteral("#ffe08a"))); highlighted.format.setForeground(Qt::black); selections.append(highlighted); break; }
        lyrics->setExtraSelections(selections);
        if (changed) { lyrics->setTextCursor(cursor); lyrics->ensureCursorVisible(); }
    });
    connect(seek, &QSlider::sliderMoved, player, [player](int value) { player->setPosition(value); }); connect(volume, &QSlider::valueChanged, audio, [audio](int value) { audio->setVolume(float(value) / 100); });
    connect(player, &QMediaPlayer::errorOccurred, status, [status](QMediaPlayer::Error, const QString &error) { status->setText(error); });
    connect(local, &QPushButton::clicked, this, [this, loadLocal] { const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("载入 LRC"), {}, QStringLiteral("LRC / 文本 (*.lrc *.txt)")); if (!file.isEmpty()) loadLocal(file); });
    connect(save, &QPushButton::clicked, this, [this, state] { if (state->text.isEmpty()) return; const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("保存歌词副本"), {}, QStringLiteral("LRC (*.lrc)")); if (file.isEmpty()) return; QSaveFile output(file); const QByteArray bytes = state->text.toUtf8(); if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) QMessageBox::warning(this, QStringLiteral("保存失败"), output.errorString()); });
    auto *network = new QNetworkAccessManager(this);
    auto *netease = new QPushButton(QStringLiteral("网易云搜索歌词")); lyricTools->addWidget(netease);
    using JsonCallback = std::function<void(const QJsonDocument &)>;
    auto requestJson = [this, network, status, netease](const QUrl &url, JsonCallback callback) {
        QNetworkRequest request(url); request.setRawHeader("User-Agent", "Mozilla/5.0 TwinFileNative/3.1"); request.setRawHeader("Referer", "https://music.163.com/"); request.setTransferTimeout(15000);
        auto *reply = network->get(request); auto bytes = std::make_shared<QByteArray>(); netease->setEnabled(false);
        connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] { const qint64 remaining = 2 * 1024 * 1024 + 1 - bytes->size(); if (remaining <= 0) { reply->abort(); return; } *bytes += reply->read(remaining); if (bytes->size() > 2 * 1024 * 1024) reply->abort(); });
        connect(reply, &QNetworkReply::finished, this, [reply, bytes, callback, netease, status] { netease->setEnabled(true); reply->deleteLater(); if (reply->error() != QNetworkReply::NoError) { status->setText(reply->errorString()); return; } if (bytes->size() > 2 * 1024 * 1024) { status->setText(QStringLiteral("歌词响应超过 2 MiB")); return; } QJsonParseError error; const auto document = QJsonDocument::fromJson(*bytes, &error); if (error.error != QJsonParseError::NoError) { status->setText(error.errorString()); return; } callback(document); });
    };
    connect(netease, &QPushButton::clicked, this, [this, requestJson, playlist, loadLyrics, status] {
        bool accepted = false; const QString keyword = QInputDialog::getText(this, QStringLiteral("网易云歌词搜索"), QStringLiteral("将把歌曲名 / 歌手发送给网易云，请确认搜索关键词"), QLineEdit::Normal, QFileInfo(playlist->currentText()).completeBaseName(), &accepted);
        if (!accepted || keyword.trimmed().isEmpty()) return;
        QUrl url(QStringLiteral("https://music.163.com/api/search/get/web")); QUrlQuery query; query.addQueryItem(QStringLiteral("s"), keyword.left(500)); query.addQueryItem(QStringLiteral("type"), QStringLiteral("1")); query.addQueryItem(QStringLiteral("limit"), QStringLiteral("20")); query.addQueryItem(QStringLiteral("offset"), QStringLiteral("0")); url.setQuery(query); status->setText(QStringLiteral("正在搜索网易云歌词…"));
        requestJson(url, [this, requestJson, loadLyrics, status](const QJsonDocument &document) {
            const auto songs = document.object().value(QStringLiteral("result")).toObject().value(QStringLiteral("songs")).toArray(); QStringList names;
            for (const auto &song : songs) { const auto item = song.toObject(); QStringList artists; for (const auto &artist : item.value(QStringLiteral("artists")).toArray()) artists.append(artist.toObject().value(QStringLiteral("name")).toString()); names.append(item.value(QStringLiteral("name")).toString() + QStringLiteral(" — ") + artists.join(QStringLiteral(" / "))); if (names.size() >= 20) break; }
            if (names.isEmpty()) { status->setText(QStringLiteral("未找到歌曲，或服务暂不可用")); return; }
            bool accepted = false; const QString choice = QInputDialog::getItem(this, QStringLiteral("选择歌曲"), QStringLiteral("请核对歌手，避免加载错误版本"), names, 0, false, &accepted); if (!accepted) return;
            const QString id = QString::number(songs[names.indexOf(choice)].toObject().value(QStringLiteral("id")).toInteger()); QUrl url(QStringLiteral("https://music.163.com/api/song/lyric")); QUrlQuery query; query.addQueryItem(QStringLiteral("id"), id); query.addQueryItem(QStringLiteral("lv"), QStringLiteral("1")); query.addQueryItem(QStringLiteral("kv"), QStringLiteral("1")); query.addQueryItem(QStringLiteral("tv"), QStringLiteral("-1")); url.setQuery(query);
            requestJson(url, [loadLyrics, status](const QJsonDocument &document) { const auto object = document.object(); const QString text = object.value(QStringLiteral("lrc")).toObject().value(QStringLiteral("lyric")).toString(); if (text.isEmpty()) { status->setText(QStringLiteral("此版本没有可用歌词")); return; } loadLyrics(text); status->setText(QStringLiteral("歌词来源：网易云（仅显示，未写入歌曲文件）")); });
        });
    });
    connect(online, &QPushButton::clicked, this, [this, network, player, playlist, online, status, loadLyrics] {
        bool accepted; const QString guess = player->metaData().stringValue(QMediaMetaData::Title); const QString keyword = QInputDialog::getText(this, QStringLiteral("搜索在线歌词"), QStringLiteral("歌曲名 / 歌手（将向 LRCLIB 发送这些文字）"), QLineEdit::Normal, guess.isEmpty() ? QFileInfo(playlist->currentText()).completeBaseName() : guess, &accepted); if (!accepted || keyword.trimmed().isEmpty()) return;
        QUrl url(QStringLiteral("https://lrclib.net/api/search")); QUrlQuery query; query.addQueryItem(QStringLiteral("q"), keyword.left(500)); url.setQuery(query); QNetworkRequest request(url); request.setRawHeader("User-Agent", "TwinFileNative/3.1"); request.setTransferTimeout(15000); QNetworkReply *reply = network->get(request); auto bytes = std::make_shared<QByteArray>(); online->setEnabled(false); status->setText(QStringLiteral("正在搜索歌词…"));
        connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] { *bytes += reply->read(2 * 1024 * 1024 + 1 - bytes->size()); if (bytes->size() > 2 * 1024 * 1024) reply->abort(); });
        connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, online, status, loadLyrics] { online->setEnabled(true); reply->deleteLater(); if (reply->error() != QNetworkReply::NoError) { status->setText(reply->errorString()); return; } const QJsonArray hits = QJsonDocument::fromJson(*bytes).array(); QStringList names; for (const auto &value : hits) { const auto item = value.toObject(); names.append(item.value(QStringLiteral("trackName")).toString() + QStringLiteral(" — ") + item.value(QStringLiteral("artistName")).toString() + QStringLiteral(" · ") + QString::number(item.value(QStringLiteral("duration")).toDouble()) + QStringLiteral(" s")); if (names.size() >= 50) break; } if (names.isEmpty()) { status->setText(QStringLiteral("未找到歌词")); return; } bool accepted; const QString choice = QInputDialog::getItem(this, QStringLiteral("选择歌词版本"), QStringLiteral("歌曲 / 歌手 / 时长"), names, 0, false, &accepted); if (!accepted) return; const auto item = hits[names.indexOf(choice)].toObject(); const QString timed = item.value(QStringLiteral("syncedLyrics")).toString(); loadLyrics(timed.isEmpty() ? item.value(QStringLiteral("plainLyrics")).toString() : timed); status->setText(QStringLiteral("歌词来源：LRCLIB（未自动写入歌曲文件）")); });
    });
    activate(initial);
}
MediaDialog::~MediaDialog() { if (cancel_) cancel_->store(true); }

}
