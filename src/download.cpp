#include "download.h"
#include "task_progress.h"
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkReply>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStorageInfo>
#include <QVBoxLayout>
#include <algorithm>

namespace twinfile {
namespace {
constexpr qint64 MaximumSize = 1024LL * 1024 * 1024 * 1024;
constexpr qint64 MinimumPart = 256 * 1024;

QNetworkRequest requestFor(const QUrl &url) {
    QNetworkRequest request(url);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("User-Agent", "TwinFile/" TWINFILE_VERSION);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setMaximumRedirectsAllowed(5);
    request.setTransferTimeout(60000);
    return request;
}

bool identity(QNetworkReply *reply) {
    const auto encoding = reply->rawHeader("Content-Encoding").trimmed().toLower();
    return encoding.isEmpty() || encoding == "identity";
}

bool contentRange(QNetworkReply *reply, qint64 start, qint64 end, qint64 total) {
    return reply->rawHeader("Content-Range") == QByteArray("bytes ") + QByteArray::number(start) + '-' + QByteArray::number(end) + '/' + QByteArray::number(total);
}

QString sizeText(qint64 bytes) {
    if (bytes < 0) return QStringLiteral("未知大小");
    return QLocale().formattedDataSize(bytes);
}
}

struct DirectDownload::Part {
    QNetworkAccessManager *network = nullptr;
    QPointer<QNetworkReply> reply;
    qint64 start = 0, end = 0, received = 0, requestStart = 0;
    qint64 lastDataAt = 0, windowAt = 0, windowBytes = 0;
    int retries = 0;
    bool validated = false, done = false, waiting = false;
};

DirectDownload::DirectDownload(QObject *parent) : QObject(parent), network_(this), watchdog_(this), deadline_(this) {
    progressTimer_.setInterval(250);
    connect(&progressTimer_, &QTimer::timeout, this, &DirectDownload::notify);
    watchdog_.setInterval(1000);
    watchdog_.setObjectName(QStringLiteral("downloadWatchdog"));
    connect(&watchdog_, &QTimer::timeout, this, &DirectDownload::watchTransfers);
    deadline_.setSingleShot(true); deadline_.setInterval(24 * 60 * 60 * 1000);
    deadline_.setObjectName(QStringLiteral("downloadDeadline"));
    connect(&deadline_, &QTimer::timeout, this, [this] { finish(QStringLiteral("下载超时，请重新开始")); });
}

DirectDownload::~DirectDownload() { active_ = false; cleanup(); }

QList<QPair<qint64, qint64>> DirectDownload::ranges(qint64 size, int connections) {
    QList<QPair<qint64, qint64>> result;
    if (size <= 0 || size > MaximumSize || connections < 1 || connections > 512) return result;
    const int count = int(qMin<qint64>(connections, qMax<qint64>(1, size / MinimumPart)));
    for (int index = 0; index < count; ++index) {
        result.append({size * index / count, size * (index + 1) / count - 1});
    }
    return result;
}

bool DirectDownload::start(const QUrl &url, const QString &destination, int connections, QString *error) {
    auto reject = [&](const QString &message) { if (error) *error = message; return false; };
    if (active_) return reject(QStringLiteral("已有下载正在进行"));
    if (!url.isValid() || (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http")) || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment()) return reject(QStringLiteral("请填写有效的 HTTP / HTTPS 直链，不支持内嵌账号或片段"));
    if (connections < 1 || connections > 512) return reject(QStringLiteral("并发范围为 1–512"));
    const QFileInfo target(destination);
    if (!target.isAbsolute() || !target.dir().exists() || target.fileName().isEmpty() || target.exists() || target.isSymLink()) return reject(QStringLiteral("请选择已存在目录中的新文件名，不覆盖已有文件"));
#ifdef Q_OS_WIN
    const QString name = target.fileName();
    if (name.contains(QRegularExpression(QStringLiteral("[<>:\"|?*\\x00-\\x1f]"))) || name.endsWith(u'.') || name.endsWith(u' ') || QRegularExpression(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"), QRegularExpression::CaseInsensitiveOption).match(name).hasMatch()) return reject(QStringLiteral("文件名无效"));
#endif
    cleanup();
    output_ = std::make_unique<QTemporaryFile>(target.dir().absoluteFilePath(QStringLiteral(".twinfile-download-XXXXXX.part")));
    if (!output_->open()) { const auto message = output_->errorString(); output_.reset(); return reject(QStringLiteral("无法创建下载临时文件：") + message); }
    url_ = url; destination_ = target.absoluteFilePath(); requested_ = connections;
    total_ = -1; received_ = 0; connections_ = 1; validator_.clear(); etag_.clear(); modified_.clear(); probeBytes_.clear();
    singleResponse_ = false; singleValidated_ = false; active_ = true; ++generation_;
    lastProgressBytes_ = 0; lastProgressAt_ = 0;
    elapsed_.start(); progressTimer_.start(); deadline_.start(); watchdog_.start();
    emit status(QStringLiteral("正在检测分段支持…"));
    const auto generation = generation_;
    QTimer::singleShot(0, this, [this, generation] { if (active_ && generation == generation_) probe(); });
    return true;
}

void DirectDownload::probe() {
    auto request = requestFor(url_); request.setRawHeader("Range", "bytes=0-0"); request.setTransferTimeout(20000);
    reply_ = network_.get(request); reply_->setReadBufferSize(256 * 1024);
    connect(reply_, &QNetworkReply::readyRead, this, &DirectDownload::probeData);
    connect(reply_, &QNetworkReply::finished, this, &DirectDownload::probeFinished);
}

void DirectDownload::probeData() {
    if (!active_ || !reply_) return;
    if (reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
        if (!singleResponse_) { singleResponse_ = true; emit status(QStringLiteral("服务器未提供分段，使用单连接")); }
        singleData();
    } else if (reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 206) {
        probeBytes_ += reply_->read(2 - probeBytes_.size());
        if (probeBytes_.size() > 1 || reply_->bytesAvailable()) finish(QStringLiteral("服务器分段探测响应无效"));
    }
}

void DirectDownload::probeFinished() {
    if (!active_ || !reply_) return;
    if (singleResponse_ || reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) { singleFinished(); return; }
    probeData(); if (!active_ || !reply_) return;
    const int code = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == 400 || code == 405 || code == 416 || code == 501) { reply_->deleteLater(); reply_ = nullptr; single(); return; }
    if (reply_->error() != QNetworkReply::NoError || reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206) { finish(QStringLiteral("无法下载：HTTP %1 / 网络错误 %2").arg(reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()).arg(int(reply_->error()))); return; }
    const auto match = QRegularExpression(QStringLiteral("^bytes 0-0/([0-9]+)$")).match(QString::fromLatin1(reply_->rawHeader("Content-Range")));
    bool valid = false; total_ = match.captured(1).toLongLong(&valid);
    if (!match.hasMatch() || !valid || total_ <= 0 || total_ > MaximumSize || probeBytes_.size() != 1 || !identity(reply_)) { finish(QStringLiteral("服务器返回无效的分段信息")); return; }
    const QStorageInfo storage(QFileInfo(destination_).dir().absolutePath());
    if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < total_) { finish(QStringLiteral("目标磁盘空间不足")); return; }
    url_ = reply_->url(); etag_ = reply_->rawHeader("ETag"); modified_ = reply_->rawHeader("Last-Modified");
    if (etag_.startsWith('"') && etag_.endsWith('"')) validator_ = etag_;
    else validator_ = modified_;
    reply_->deleteLater(); reply_ = nullptr;
    if (validator_.isEmpty() || requested_ == 1 || ranges(total_, requested_).size() == 1) { single(); return; }
    const auto planned = ranges(total_, requested_); connections_ = planned.size();
    emit status(QStringLiteral("分段下载中"));
    for (const auto &range : planned) {
        auto part = std::make_shared<Part>(); part->start = range.first; part->end = range.second;
        if (parts_.size() % 6 == 0) networks_.append(new QNetworkAccessManager(this));
        part->network = networks_.last(); parts_.append(part);
    }
    for (const auto &part : parts_) requestPart(part);
}

void DirectDownload::requestPart(const std::shared_ptr<Part> &part) {
    if (!active_) return;
    part->waiting = false;
    part->validated = false; part->requestStart = part->start + part->received;
    part->lastDataAt = part->windowAt = elapsed_.elapsed(); part->windowBytes = part->received;
    auto request = requestFor(url_);
    request.setRawHeader("Range", "bytes=" + QByteArray::number(part->requestStart) + '-' + QByteArray::number(part->end));
    request.setRawHeader("If-Range", validator_);
    part->reply = part->network->get(request); part->reply->setReadBufferSize(64 * 1024);
    connect(part->reply, &QNetworkReply::readyRead, this, [this, part] { partData(part); });
    connect(part->reply, &QNetworkReply::finished, this, [this, part] { partFinished(part); });
}

void DirectDownload::partData(const std::shared_ptr<Part> &part) {
    if (!active_ || !part->reply) return;
    if (!part->validated) {
        const int code = part->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!code) return;
        if (code == 429 || code >= 500) { part->reply->readAll(); return; }
        if (code != 206 || !contentRange(part->reply, part->requestStart, part->end, total_) || !identity(part->reply) || (!etag_.isEmpty() && part->reply->rawHeader("ETag") != etag_) || (!modified_.isEmpty() && part->reply->rawHeader("Last-Modified") != modified_)) { finish(QStringLiteral("分段响应或文件版本已变化，已停止以防文件损坏")); return; }
        part->validated = true;
    }
    while (active_ && part->reply && part->reply->bytesAvailable()) {
        const QByteArray bytes = part->reply->read(64 * 1024);
        if (bytes.isEmpty()) break;
        if (bytes.size() > part->end - part->start + 1 - part->received) { finish(QStringLiteral("分段数据超出请求范围")); return; }
        if (!write(part->start + part->received, bytes)) return;
        part->received += bytes.size(); received_ += bytes.size();
        part->lastDataAt = elapsed_.elapsed();
    }
}

void DirectDownload::partFinished(const std::shared_ptr<Part> &part) {
    if (!active_ || !part->reply) return;
    partData(part); if (!active_ || !part->reply) return;
    const auto error = part->reply->error();
    const int code = part->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    part->reply->deleteLater(); part->reply = nullptr;
    if (part->validated && error == QNetworkReply::NoError && part->received == part->end - part->start + 1) {
        part->done = true;
        if (std::all_of(parts_.cbegin(), parts_.cend(), [](const auto &item) { return item->done; })) finish();
        return;
    }
    if ((code == 429 || code >= 500 || error == QNetworkReply::RemoteHostClosedError || error == QNetworkReply::TimeoutError || error == QNetworkReply::OperationCanceledError || error == QNetworkReply::TemporaryNetworkFailureError) && part->start + part->received <= part->end) {
        retryPart(part, QStringLiteral("连接中断"));
    } else finish(QStringLiteral("分段下载失败：HTTP %1 / 网络错误 %2，可尝试降低并发").arg(code).arg(int(error)));
}

void DirectDownload::retryPart(const std::shared_ptr<Part> &part, const QString &reason, bool redistribute) {
    if (!active_ || part->done || part->waiting) return;
    if (part->received >= part->end - part->start + 1) { finish(QStringLiteral("服务器分段响应未正常结束，请重新开始")); return; }
    if (++part->retries > 2) { finish(QStringLiteral("%1，分段重试已用尽，可尝试降低并发").arg(reason)); return; }
    if (part->reply) {
        disconnect(part->reply, nullptr, this, nullptr);
        part->reply->abort(); part->reply->deleteLater(); part->reply = nullptr;
    }
    QList<std::shared_ptr<Part>> pending{part};
    if (redistribute) {
        int unfinished = 0;
        for (const auto &item : parts_) if (!item->done && item != part) ++unfinished;
        const auto planned = ranges(part->end - part->start + 1 - part->received, qMin(4, requested_ - unfinished));
        if (planned.size() > 1) {
            pending.clear();
            const qint64 start = part->start + part->received;
            for (const auto &range : planned) {
                auto child = std::make_shared<Part>();
                child->start = start + range.first; child->end = start + range.second;
                child->retries = part->retries;
                parts_.append(child); pending.append(child);
            }
            part->end = start - 1; part->done = true;
        }
    }
    const auto generation = generation_;
    for (const auto &item : pending) {
        item->waiting = true;
        if (!recoveryNetwork_ || recoverySlots_ == 6) {
            recoveryNetwork_ = new QNetworkAccessManager(this);
            networks_.append(recoveryNetwork_); recoverySlots_ = 0;
        }
        item->network = recoveryNetwork_; ++recoverySlots_;
        QTimer::singleShot(item->retries * 1000, this, [this, item, generation] { if (active_ && generation == generation_) requestPart(item); });
    }
    emit status(QStringLiteral("%1，正在恢复 %2 个未完成分段（%3/2）…").arg(reason).arg(pending.size()).arg(part->retries));
}

void DirectDownload::watchTransfers() {
    if (!active_ || parts_.isEmpty()) return;
    const qint64 now = elapsed_.elapsed();
    const auto snapshot = parts_;
    for (const auto &part : snapshot) {
        if (!active_) return;
        if (part->done || part->waiting || !part->reply) continue;
        const bool tail = total_ > 0 && received_ >= total_ * 9 / 10;
        const int idleLimit = part->validated ? stallTimeoutMs_ : handshakeTimeoutMs_;
        if (now - part->lastDataAt >= idleLimit) { retryPart(part, QStringLiteral("分段长时间无数据"), tail); continue; }
        if (now - part->windowAt >= slowWindowMs_) {
            if (tail && part->received - part->windowBytes < 32 * 1024) retryPart(part, QStringLiteral("尾段长时间低速"), true);
            else { part->windowAt = now; part->windowBytes = part->received; }
        }
    }
}

void DirectDownload::single() {
    connections_ = 1; singleValidated_ = false;
    emit status(validator_.isEmpty() ? QStringLiteral("无文件版本标识，使用单连接保证一致性") : QStringLiteral("单连接下载"));
    auto request = requestFor(url_); reply_ = network_.get(request); reply_->setReadBufferSize(256 * 1024);
    connect(reply_, &QNetworkReply::readyRead, this, &DirectDownload::singleData);
    connect(reply_, &QNetworkReply::finished, this, &DirectDownload::singleFinished);
}

void DirectDownload::singleData() {
    if (!active_ || !reply_) return;
    if (!singleValidated_) {
        const int code = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!code) return;
        if (code != 200 || !identity(reply_)) { finish(QStringLiteral("服务器返回无效的下载响应：HTTP %1").arg(code)); return; }
        const QByteArray length = reply_->rawHeader("Content-Length"); bool valid = false;
        const qint64 size = length.toLongLong(&valid);
        if (!length.isEmpty() && (!valid || size < 0 || size > MaximumSize)) { finish(QStringLiteral("下载大小无效或超过 1 TiB")); return; }
        if (total_ >= 0 && valid && total_ != size) { finish(QStringLiteral("文件大小已变化")); return; }
        if ((!etag_.isEmpty() && reply_->rawHeader("ETag") != etag_) || (!modified_.isEmpty() && reply_->rawHeader("Last-Modified") != modified_)) { finish(QStringLiteral("文件版本已变化")); return; }
        if (valid) total_ = size;
        const QStorageInfo storage(QFileInfo(destination_).dir().absolutePath());
        if (total_ >= 0 && storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < total_) { finish(QStringLiteral("目标磁盘空间不足")); return; }
        singleValidated_ = true;
    }
    while (active_ && reply_ && reply_->bytesAvailable()) {
        const auto bytes = reply_->read(256 * 1024); if (bytes.isEmpty()) break;
        if (received_ + bytes.size() > MaximumSize || (total_ >= 0 && received_ + bytes.size() > total_)) { finish(QStringLiteral("下载内容超出声明大小")); return; }
        if (!write(received_, bytes)) return;
        received_ += bytes.size();
    }
}

void DirectDownload::singleFinished() {
    if (!active_ || !reply_) return;
    singleData(); if (!active_ || !reply_) return;
    if (reply_->error() != QNetworkReply::NoError || !singleValidated_ || (total_ >= 0 && received_ != total_)) { finish(QStringLiteral("下载中断或文件不完整，请重新开始")); return; }
    total_ = received_; finish();
}

bool DirectDownload::write(qint64 offset, const QByteArray &bytes) {
    if (!output_ || !output_->seek(offset) || output_->write(bytes) != bytes.size()) { finish(QStringLiteral("写入失败：") + (output_ ? output_->errorString() : QStringLiteral("临时文件已关闭"))); return false; }
    return true;
}

void DirectDownload::notify() {
    if (!active_) return;
    const qint64 now = elapsed_.elapsed();
    const double speed = double(received_ - lastProgressBytes_) * 1000.0 / qMax<qint64>(1, now - lastProgressAt_);
    lastProgressBytes_ = received_; lastProgressAt_ = now;
    int count = parts_.isEmpty() && reply_ ? 1 : 0;
    for (const auto &part : parts_) if (!part->done && part->reply) ++count;
    emit progress(received_, total_, speed, count);
}

void DirectDownload::cleanup() {
    progressTimer_.stop(); deadline_.stop(); watchdog_.stop();
    if (reply_) { disconnect(reply_, nullptr, this, nullptr); reply_->abort(); reply_->deleteLater(); reply_ = nullptr; }
    for (const auto &part : parts_) {
        if (part->reply) { disconnect(part->reply, nullptr, this, nullptr); part->reply->abort(); }
    }
    parts_.clear();
    for (auto *network : networks_) network->deleteLater();
    networks_.clear();
    recoveryNetwork_ = nullptr; recoverySlots_ = 0;
    output_.reset();
}

void DirectDownload::finish(const QString &error) {
    if (!active_) return;
    QString failure = error;
    if (failure.isEmpty()) {
        if (!output_ || received_ != total_ || !output_->flush() || output_->size() != total_) failure = QStringLiteral("文件大小或磁盘写入校验失败");
        else {
            output_->close();
            if (!output_->rename(destination_)) failure = QStringLiteral("无法保存成品，目标可能已存在：") + output_->errorString();
            else output_->setAutoRemove(false);
        }
    }
    notify(); active_ = false; ++generation_; cleanup();
    if (failure.isEmpty()) emit completed(destination_); else emit failed(failure);
}

void DirectDownload::cancel() { finish(QStringLiteral("已取消，临时文件已清理")); }

DownloadDialog::DownloadDialog(const QString &directory, QWidget *parent, const QUrl &initialUrl, const QString &initialFileName, bool startImmediately) : QDialog(parent), download_(this) {
    setObjectName(QStringLiteral("directDownloadDialog")); setWindowTitle(QStringLiteral("直链下载")); resize(600, 330);
    auto *layout = new QVBoxLayout(this); layout->setSpacing(12);
    auto *form = new QFormLayout;
    auto *url = new QLineEdit; url->setObjectName(QStringLiteral("downloadUrl")); url->setPlaceholderText(QStringLiteral("https://…")); url->setClearButtonEnabled(true);
    auto *path = new QLineEdit; path->setObjectName(QStringLiteral("downloadPath")); path->setPlaceholderText(QStringLiteral("选择保存文件"));
    auto *browse = new QPushButton(QStringLiteral("选择位置…")); browse->setObjectName(QStringLiteral("downloadBrowse")); auto *pathRow = new QHBoxLayout; pathRow->addWidget(path); pathRow->addWidget(browse);
    auto *connections = new QSpinBox; connections->setObjectName(QStringLiteral("downloadConnections")); connections->setRange(1, 512); connections->setValue(16); connections->setSuffix(QStringLiteral(" 并发"));
    form->addRow(QStringLiteral("下载链接"), url); form->addRow(QStringLiteral("保存到"), pathRow); form->addRow(QStringLiteral("连接数"), connections); layout->addLayout(form);
    auto *status = new QLabel(QStringLiteral("就绪")); status->setObjectName(QStringLiteral("downloadStatus")); status->setWordWrap(true); layout->addWidget(status);
    auto *bar = new QProgressBar; bar->setObjectName(QStringLiteral("downloadProgress")); bar->setRange(0, 1000); bar->setValue(0); layout->addWidget(bar);
    auto *detail = new QLabel(QStringLiteral("支持 HTTP / HTTPS · 不覆盖已有文件")); detail->setObjectName(QStringLiteral("downloadDetail")); detail->setWordWrap(true); layout->addWidget(detail);
    auto *hint = new QLabel(QStringLiteral("最高 512 并发；不支持分段时自动单连接。并发越高不一定越快。")); hint->setWordWrap(true); hint->setObjectName(QStringLiteral("subtle")); layout->addWidget(hint); layout->addStretch();
    auto *buttons = new QHBoxLayout; auto *folder = new QPushButton(QStringLiteral("打开目录")); auto *start = new QPushButton(QStringLiteral("开始下载")); auto *stop = new QPushButton(QStringLiteral("取消下载")); auto *close = new QPushButton(QStringLiteral("关闭"));
    start->setObjectName(QStringLiteral("downloadStart")); stop->setObjectName(QStringLiteral("downloadCancel")); stop->setEnabled(false); start->setDefault(true);
    buttons->addWidget(folder); buttons->addStretch(); buttons->addWidget(start); buttons->addWidget(stop); buttons->addWidget(close); layout->addLayout(buttons);
    connect(browse, &QPushButton::clicked, this, [this, directory, path, url] {
        const QString name = QFileInfo(QUrl(url->text().trimmed()).path()).fileName();
        const QString chosen = QFileDialog::getSaveFileName(this, QStringLiteral("保存下载文件（不覆盖）"), path->text().isEmpty() ? QDir(directory).filePath(name.isEmpty() ? QStringLiteral("download.bin") : name) : path->text());
        if (!chosen.isEmpty()) path->setText(chosen);
    });
    connect(folder, &QPushButton::clicked, this, [path, directory] { QDesktopServices::openUrl(QUrl::fromLocalFile(path->text().isEmpty() ? directory : QFileInfo(path->text()).absolutePath())); });
    auto state = [url, path, browse, connections, start, stop](bool busy) { url->setEnabled(!busy); path->setEnabled(!busy); browse->setEnabled(!busy); connections->setEnabled(!busy); start->setEnabled(!busy); stop->setEnabled(busy); };
    connect(start, &QPushButton::clicked, this, [this, url, path, connections, bar, detail, state] {
        if (taskProgress_ && taskProgress_->active()) { detail->setText(QStringLiteral("请等待当前任务完成")); return; }
        QString error;
        if (!download_.start(QUrl(url->text().trimmed(), QUrl::StrictMode), path->text().trimmed(), connections->value(), &error)) { detail->setText(error); return; }
        bar->setRange(0, 0); state(true);
        emit destinationSelected(path->text().trimmed());
        if (taskProgress_) { taskProgress_->begin(QStringLiteral("下载 · ") + QFileInfo(path->text()).fileName(), [this] { download_.cancel(); }); hide(); }
    });
    connect(stop, &QPushButton::clicked, &download_, &DirectDownload::cancel);
    connect(close, &QPushButton::clicked, this, &DownloadDialog::close);
    connect(&download_, &DirectDownload::status, status, &QLabel::setText);
    connect(&download_, &DirectDownload::progress, this, [this, bar, detail, status](qint64 received, qint64 total, double speed, int count) {
        if (total < 0) bar->setRange(0, 0); else { bar->setRange(0, 1000); bar->setValue(total ? int(received * 1000 / total) : 1000); }
        detail->setText(QStringLiteral("%1 / %2 · %3/s · %4 并发").arg(sizeText(received), sizeText(total), sizeText(qint64(speed))).arg(count));
        if (taskProgress_) taskProgress_->updateProgress(received, total, status->text(), speed);
    });
    connect(&download_, &DirectDownload::completed, this, [this, bar, detail, status, state](const QString &saved) { state(false); bar->setRange(0, 1000); bar->setValue(1000); status->setText(QStringLiteral("下载完成")); detail->setText(QStringLiteral("已完成：") + saved); if (taskProgress_) taskProgress_->finish(QStringLiteral("已保存：") + saved, true); });
    connect(&download_, &DirectDownload::failed, this, [this, bar, detail, status, state](const QString &error) { state(false); bar->setRange(0, 1000); status->setText(QStringLiteral("已停止")); detail->setText(error); if (taskProgress_) taskProgress_->finish(error, false); });
    connect(url, &QLineEdit::textChanged, this, [path, directory](const QString &value) {
        if (!path->text().isEmpty() && path->text() != path->property("automaticPath").toString()) return;
        const QUrl parsed(value);
        if (!parsed.isValid() || parsed.host().isEmpty()) return;
        const QString name = QFileInfo(parsed.path()).fileName();
        if (!name.isEmpty()) {
            const QString automatic = QDir(directory).filePath(name);
            path->setProperty("automaticPath", automatic); path->setText(automatic);
        }
    });
    if (!initialUrl.isEmpty() && initialUrl.isValid()) {
        url->setText(initialUrl.toString(QUrl::FullyEncoded));
        const QString fileName = QFileInfo(initialFileName).fileName();
        path->setText(QDir(directory).filePath(fileName.isEmpty() ? QStringLiteral("download.bin") : fileName));
        if (startImmediately) QTimer::singleShot(0, start, &QPushButton::click);
    } else if (parent == nullptr) {
        path->setText(QDir(directory).filePath(QStringLiteral("download.bin")));
    }
}

void DownloadDialog::setTaskProgress(TaskProgress *progress) {
    taskProgress_ = progress;
    if (progress) {
        findChild<QProgressBar *>(QStringLiteral("downloadProgress"))->hide();
        findChild<QPushButton *>(QStringLiteral("downloadCancel"))->hide();
    }
}

void DownloadDialog::closeEvent(QCloseEvent *event) {
    if (download_.busy() && QMessageBox::question(this, QStringLiteral("停止下载？"), QStringLiteral("关闭窗口将取消下载并清理临时文件。"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) { event->ignore(); return; }
    download_.cancel(); event->accept();
}

void DownloadDialog::reject() { close(); }
}
