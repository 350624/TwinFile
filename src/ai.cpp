#include "ai.h"
#include "core.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <windows.h>
#include <wincrypt.h>

namespace twinfile {

static constexpr qsizetype MaxResponse = 2 * 1024 * 1024;
static constexpr qsizetype MaxText = 128 * 1024;

static QByteArray protectKey(const QByteArray &value, bool decrypt) {
    if (value.isEmpty()) return {};
    DATA_BLOB input{static_cast<DWORD>(value.size()), reinterpret_cast<BYTE *>(const_cast<char *>(value.constData()))};
    DATA_BLOB output{};
    const bool success = decrypt ? CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
                                 : CryptProtectData(&input, L"TwinFile AI", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!success) return {};
    const QByteArray result(reinterpret_cast<char *>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}

QJsonObject AiSettings::publicJson() const {
    return {{QStringLiteral("endpoint"), endpoint}, {QStringLiteral("model"), model},
            {QStringLiteral("system_prompt"), systemPrompt}, {QStringLiteral("remember_key"), rememberKey}};
}

AiSettings AiSettings::fromJson(const QJsonObject &object) {
    AiSettings settings;
    settings.endpoint = object.value(QStringLiteral("endpoint")).toString().left(2048);
    settings.model = object.value(QStringLiteral("model")).toString().left(256);
    settings.systemPrompt = object.value(QStringLiteral("system_prompt")).toString(settings.systemPrompt).left(8192);
    settings.rememberKey = object.value(QStringLiteral("remember_key")).toBool();
    return settings;
}

QUrl AiSettings::requestUrl(const QString &endpoint, QString *error) {
    if (error) error->clear();
    QUrl url(endpoint.trimmed(), QUrl::StrictMode);
    const QString host = url.host().toLower();
    const bool local = host == QStringLiteral("localhost") || host == QStringLiteral("127.0.0.1") || host == QStringLiteral("::1");
    if (endpoint.size() > 2048 || !url.isValid() || host.isEmpty() || !url.userInfo().isEmpty() || url.hasFragment() || url.hasQuery() ||
        (url.scheme() != QStringLiteral("https") && !(local && url.scheme() == QStringLiteral("http")))) {
        if (error) *error = QStringLiteral("请输入 HTTPS 接口地址（本机 localhost/127.0.0.1/::1 可用 HTTP），不要包含账号、查询参数或片段。");
        return {};
    }
    QString path = url.path();
    while (path.endsWith(u'/')) path.chop(1);
    if (!path.endsWith(QStringLiteral("/chat/completions"))) path += QStringLiteral("/chat/completions");
    url.setPath(path);
    return url;
}

QUrl AiSettings::modelsUrl(const QString &endpoint, QString *error) {
    QUrl url = requestUrl(endpoint, error);
    if (url.isEmpty()) return {};
    QString path = url.path(); path.chop(QStringLiteral("/chat/completions").size());
    url.setPath(path + QStringLiteral("/models"));
    return url;
}

AiModelClient::AiModelClient(QObject *parent) : QObject(parent), network_(this) {
    deadline_.setSingleShot(true); deadline_.setInterval(45000);
    connect(&deadline_, &QTimer::timeout, this, [this] { fail(QStringLiteral("获取模型超时，可以重试或手动填写模型 ID")); });
}

AiModelClient::~AiModelClient() { cancel(); }

void AiModelClient::cancel() {
    deadline_.stop();
    if (reply_) { auto *reply = reply_.data(); reply_ = nullptr; disconnect(reply, nullptr, this, nullptr); reply->abort(); reply->deleteLater(); }
    buffer_.clear();
}

void AiModelClient::fail(const QString &message) { cancel(); emit failed(message); }

bool AiModelClient::fetch(const QString &endpoint, const QString &key, QString *error) {
    if (error) error->clear();
    if (busy()) { if (error) *error = QStringLiteral("正在获取模型，请先取消"); return false; }
    const QUrl url = AiSettings::modelsUrl(endpoint, error);
    if (url.isEmpty()) return false;
    if (key.size() > 8192 || key.contains(u'\r') || key.contains(u'\n')) { if (error) *error = QStringLiteral("密钥过长或包含换行"); return false; }
    QNetworkRequest request(url); request.setRawHeader("Accept", "application/json");
    if (!key.isEmpty()) request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy); request.setTransferTimeout(15000);
    buffer_.clear(); reply_ = network_.get(request); reply_->setReadBufferSize(128 * 1024);
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        if (!reply_) return;
        buffer_ += reply_->readAll();
        if (buffer_.size() > MaxResponse) fail(QStringLiteral("模型列表超过 2 MiB，已停止接收，可手动填写模型 ID"));
    });
    connect(reply_, &QNetworkReply::finished, this, [this] {
        if (!reply_) return;
        const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status < 200 || status >= 300) {
            fail(status >= 300 && status < 400 ? QStringLiteral("已阻止重定向，密钥不会转发；请填写最终接口地址")
                 : status ? QStringLiteral("获取模型返回 HTTP %1；请核对地址和密钥，服务也可能不支持 /models，可手动填写").arg(status)
                          : QStringLiteral("连接失败，请检查网络、证书和接口地址")); return;
        }
        if (reply_->error() != QNetworkReply::NoError) { fail(QStringLiteral("获取模型时连接中断或超时，可重试或手动填写")); return; }
        buffer_ += reply_->readAll();
        if (buffer_.size() > MaxResponse) { fail(QStringLiteral("模型列表超过 2 MiB")); return; }
        QJsonParseError parse; const auto document = QJsonDocument::fromJson(buffer_, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject() || !document.object().value(QStringLiteral("data")).isArray() || document.object().contains(QStringLiteral("error"))) {
            fail(QStringLiteral("服务未返回兼容的模型列表，可手动填写模型 ID")); return;
        }
        const auto data = document.object().value(QStringLiteral("data")).toArray();
        if (data.size() > 4096) { fail(QStringLiteral("模型条目超过 4096 个，可手动填写模型 ID")); return; }
        QStringList models; QSet<QString> seen;
        for (const auto &value : data) {
            const QString id = value.toObject().value(QStringLiteral("id")).toString().trimmed();
            bool valid = !id.isEmpty() && id.size() <= 256;
            for (const QChar character : id) if (character.category() == QChar::Other_Control) valid = false;
            if (valid && !seen.contains(id)) { models.append(id); seen.insert(id); }
        }
        if (models.isEmpty()) { fail(QStringLiteral("服务没有返回可选模型，可手动填写模型 ID")); return; }
        models.sort(Qt::CaseInsensitive); cancel(); emit received(models);
    });
    deadline_.start(); return true;
}

AiSettings AiSettings::load(const QString &directory, QString *error) {
    if (error) error->clear();
    QFile file(QDir(directory).filePath(QStringLiteral("ai.json")));
    if (!file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) {
        if (error) *error = QStringLiteral("AI 设置无法读取或超过大小上限");
        return {};
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("AI 设置格式无效");
        return {};
    }
    AiSettings settings = fromJson(document.object());
    if (settings.rememberKey) {
        const QByteArray encrypted = QByteArray::fromBase64(document.object().value(QStringLiteral("protected_key")).toString().toLatin1());
        const QByteArray key = protectKey(encrypted, true);
        if (!encrypted.isEmpty() && key.isEmpty() && error) *error = QStringLiteral("无法解密 API 密钥，请在当前 Windows 账户下重新输入");
        settings.apiKey = QString::fromUtf8(key);
    }
    return settings;
}

bool AiSettings::save(const QString &directory, QString *error) const {
    if (error) error->clear();
    auto object = publicJson();
    if (rememberKey && !apiKey.isEmpty()) {
        const QByteArray encrypted = protectKey(apiKey.toUtf8(), false);
        if (encrypted.isEmpty()) { if (error) *error = QStringLiteral("Windows 密钥保护失败，未保存设置"); return false; }
        object.insert(QStringLiteral("protected_key"), QString::fromLatin1(encrypted.toBase64()));
    }
    return writeSettings(QDir(directory).filePath(QStringLiteral("ai.json")), object, error);
}

AiClient::AiClient(QObject *parent) : QObject(parent), network_(this) {
    deadline_.setSingleShot(true);
    deadline_.setInterval(180000);
    connect(&deadline_, &QTimer::timeout, this, [this] { finishFailure(QStringLiteral("请求超时，请检查服务或重试")); });
}

AiClient::~AiClient() { disposeReply(); }

bool AiClient::send(const AiSettings &settings, const QJsonArray &messages, QString *error) {
    if (error) error->clear();
    if (busy()) { if (error) *error = QStringLiteral("请先停止当前回答"); return false; }
    const QUrl url = AiSettings::requestUrl(settings.endpoint, error);
    if (url.isEmpty()) return false;
    if (settings.model.trimmed().isEmpty() || settings.model.size() > 256 || settings.apiKey.size() > 8192 || settings.apiKey.contains(u'\r') || settings.apiKey.contains(u'\n')) {
        if (error) *error = QStringLiteral("请填写有效模型名称和 API 密钥（本地免密服务可留空密钥）");
        return false;
    }
    if (messages.isEmpty() || messages.size() > 41 || settings.systemPrompt.size() > 8192) {
        if (error) *error = QStringLiteral("对话过长，请新建对话后重试");
        return false;
    }
    qsizetype textSize = settings.systemPrompt.size();
    QJsonArray payload;
    if (!settings.systemPrompt.isEmpty()) payload.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), settings.systemPrompt}});
    for (const auto &value : messages) {
        const auto message = value.toObject();
        const QString role = message.value(QStringLiteral("role")).toString();
        const QString content = message.value(QStringLiteral("content")).toString();
        if ((role != QStringLiteral("user") && role != QStringLiteral("assistant")) || content.isEmpty()) {
            if (error) *error = QStringLiteral("对话消息格式无效");
            return false;
        }
        textSize += content.size();
        payload.append(QJsonObject{{QStringLiteral("role"), role}, {QStringLiteral("content"), content}});
    }
    if (textSize > MaxText) { if (error) *error = QStringLiteral("对话与附件超过 128K 字符，请移除附件或新建对话"); return false; }
    const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("model"), settings.model.trimmed()}, {QStringLiteral("messages"), payload}, {QStringLiteral("stream"), true}}).toJson(QJsonDocument::Compact);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "text/event-stream, application/json");
    if (!settings.apiKey.isEmpty()) request.setRawHeader("Authorization", "Bearer " + settings.apiKey.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(60000);
    buffer_.clear(); eventData_.clear(); text_.clear(); received_ = 0; done_ = false; streaming_ = false;
    reply_ = network_.post(request, body);
    reply_->setReadBufferSize(256 * 1024);
    connect(reply_, &QNetworkReply::readyRead, this, &AiClient::consume);
    connect(reply_, &QNetworkReply::finished, this, [this] {
        if (!reply_) return;
        const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status < 200 || status >= 300) {
            const QString message = status >= 300 && status < 400 ? QStringLiteral("接口重定向已阻止，请填写最终地址，避免密钥被转发")
                : status ? QStringLiteral("服务返回 HTTP %1，请核对地址、密钥、模型或额度").arg(status)
                         : QStringLiteral("连接失败，请检查网络、证书和接口地址");
            finishFailure(message); return;
        }
        if (reply_->error() != QNetworkReply::NoError) { finishFailure(QStringLiteral("连接中断或超时，回答未完成")); return; }
        consume();
        if (!reply_) return;
        if (streaming_) {
            if (!buffer_.isEmpty()) {
                QByteArray line = buffer_; if (line.endsWith('\r')) line.chop(1);
                if (line.startsWith("data:")) eventData_ += line.mid(5).trimmed();
            }
            if (!eventData_.isEmpty() && !consumeEvent(eventData_)) return;
            if (!done_) { finishFailure(QStringLiteral("流式回答意外结束，请重试")); return; }
        } else {
            QJsonParseError parse;
            const auto object = QJsonDocument::fromJson(buffer_, &parse).object();
            if (parse.error != QJsonParseError::NoError || object.contains(QStringLiteral("error"))) { finishFailure(QStringLiteral("接口响应不是兼容的聊天 JSON")); return; }
            const auto choices = object.value(QStringLiteral("choices")).toArray();
            const QString reason = choices.isEmpty() ? QString() : choices.first().toObject().value(QStringLiteral("finish_reason")).toString();
            if (!reason.isEmpty() && reason != QStringLiteral("stop")) { finishFailure(QStringLiteral("服务提前终止回答，请检查长度限制或服务能力")); return; }
            text_ = choices.isEmpty() ? QString() : choices.first().toObject().value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString();
        }
        if (text_.trimmed().isEmpty() || text_.size() > MaxText) { finishFailure(QStringLiteral("服务没有返回有效文字，或回答超过大小上限")); return; }
        const QString answer = text_;
        disposeReply();
        emit completed(answer);
    });
    deadline_.start();
    return true;
}

void AiClient::consume() {
    if (!reply_) return;
    const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300) { reply_->readAll(); return; }
    streaming_ = reply_->header(QNetworkRequest::ContentTypeHeader).toString().startsWith(QStringLiteral("text/event-stream"), Qt::CaseInsensitive);
    const QByteArray bytes = reply_->readAll();
    received_ += bytes.size();
    if (received_ > MaxResponse) { finishFailure(QStringLiteral("响应超过 2 MiB，已停止接收")); return; }
    buffer_ += bytes;
    if (!streaming_) return;
    while (reply_) {
        const qsizetype newline = buffer_.indexOf('\n');
        if (newline < 0) break;
        QByteArray line = buffer_.left(newline); buffer_.remove(0, newline + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) {
            const QByteArray event = eventData_; eventData_.clear();
            if (!event.isEmpty() && !consumeEvent(event)) return;
        } else if (line.startsWith("data:")) {
            QByteArray value = line.mid(5); if (value.startsWith(' ')) value.remove(0, 1);
            if (!eventData_.isEmpty()) eventData_ += '\n';
            eventData_ += value;
        }
    }
}

bool AiClient::consumeEvent(const QByteArray &data) {
    if (data.trimmed() == "[DONE]") {
        done_ = true;
        if (text_.trimmed().isEmpty()) { finishFailure(QStringLiteral("服务没有返回有效文字")); return false; }
        const QString answer = text_;
        disposeReply();
        emit completed(answer);
        return false;
    }
    if (done_) return true;
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(data, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject() || document.object().contains(QStringLiteral("error"))) {
        finishFailure(QStringLiteral("接口返回了无效流式数据或服务错误")); return false;
    }
    const auto choices = document.object().value(QStringLiteral("choices")).toArray();
    const auto choice = choices.isEmpty() ? QJsonObject() : choices.first().toObject();
    const auto delta = choice.value(QStringLiteral("delta")).toObject();
    text_ += delta.value(QStringLiteral("content")).toString();
    if (text_.size() > MaxText) { finishFailure(QStringLiteral("回答超过 128K 字符，已停止接收")); return false; }
    const QString reason = choice.value(QStringLiteral("finish_reason")).toString();
    if (!reason.isEmpty() && reason != QStringLiteral("stop")) { finishFailure(QStringLiteral("服务提前终止回答（长度限制、内容过滤或不支持的工具调用）")); return false; }
    if (reason == QStringLiteral("stop")) done_ = true;
    emit textChanged(text_);
    return true;
}

void AiClient::disposeReply() {
    deadline_.stop();
    if (!reply_) return;
    auto *reply = reply_.data(); reply_ = nullptr;
    reply->disconnect(this);
    if (reply->isRunning()) reply->abort();
    reply->deleteLater();
}

void AiClient::finishFailure(const QString &message) {
    if (!reply_) return;
    disposeReply();
    emit failed(message);
}

void AiClient::cancel() { finishFailure(QStringLiteral("已停止回答，未完成内容不会加入后续对话")); }

}
