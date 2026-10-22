#include "updates.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVersionNumber>
#include <QVBoxLayout>

namespace twinfile {

static constexpr qsizetype MaximumResponse = 1024 * 1024;

static bool validVersion(const QString &version) {
    static const QRegularExpression pattern(QStringLiteral("^(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})$"));
    return pattern.match(version).hasMatch();
}

UpdateChecker::UpdateChecker(const QString &currentVersion, QObject *parent, const QUrl &testEndpoint)
    : QObject(parent), currentVersion_(currentVersion), endpoint_(QStringLiteral("https://api.github.com/repos/350624/TwinFile/releases/latest")) {
    setObjectName(QStringLiteral("updateChecker"));
    if (qEnvironmentVariableIsSet("TWINFILE_TEST_MODE") && testEndpoint.scheme() == QStringLiteral("http") &&
        testEndpoint.host() == QStringLiteral("127.0.0.1") && testEndpoint.userInfo().isEmpty() && !testEndpoint.hasQuery() && !testEndpoint.hasFragment()) endpoint_ = testEndpoint;
    deadline_.setSingleShot(true); deadline_.setInterval(30000);
    startup_.setSingleShot(true); startup_.setInterval(2000);
    connect(&deadline_, &QTimer::timeout, this, [this] { finish(QStringLiteral("检查超时，请稍后重试")); });
    connect(&startup_, &QTimer::timeout, this, [this] { if (startupEnabled_ && state_ == State::Idle) check(); });
}

UpdateChecker::~UpdateChecker() { disposeReply(); }

bool UpdateChecker::parseRelease(const QByteArray &data, const QString &currentVersion, UpdateRelease *release, QString *error) {
    auto reject = [&](const QString &message) { if (error) *error = message; return false; };
    if (!release || data.size() > MaximumResponse || !validVersion(currentVersion)) return reject(QStringLiteral("更新数据或本机版本无效"));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return reject(QStringLiteral("更新服务返回了无效数据"));
    const auto object = document.object();
    if (!object.value(QStringLiteral("draft")).isBool() || object.value(QStringLiteral("draft")).toBool() ||
        !object.value(QStringLiteral("prerelease")).isBool() || object.value(QStringLiteral("prerelease")).toBool()) return reject(QStringLiteral("没有可用的正式版本"));
    const QString tag = object.value(QStringLiteral("tag_name")).toString();
    const QString version = tag.mid(1);
    if (!tag.startsWith('v') || !validVersion(version)) return reject(QStringLiteral("发布版本号不受支持"));
    const QString page = QStringLiteral("https://github.com/350624/TwinFile/releases/tag/") + tag;
    if (object.value(QStringLiteral("html_url")).toString() != page) return reject(QStringLiteral("发布地址不属于本项目"));
    const QString installer = QStringLiteral("TwinFile-%1-Windows-x64-Setup.exe").arg(version);
    const QString assetUrl = QStringLiteral("https://github.com/350624/TwinFile/releases/download/%1/%2").arg(tag, installer);
    bool hasInstaller = false;
    for (const auto &value : object.value(QStringLiteral("assets")).toArray()) {
        const auto asset = value.toObject();
        if (asset.value(QStringLiteral("name")).toString() == installer && asset.value(QStringLiteral("state")).toString() == QStringLiteral("uploaded") &&
            asset.value(QStringLiteral("size")).toDouble() > 0 && asset.value(QStringLiteral("browser_download_url")).toString() == assetUrl) hasInstaller = true;
    }
    if (!hasInstaller) return reject(QStringLiteral("此版本尚未提供 Windows 安装包"));
    const auto body = object.value(QStringLiteral("body"));
    if (!body.isUndefined() && !body.isNull() && !body.isString()) return reject(QStringLiteral("更新日志格式无效"));
    QString notes = body.toString();
    if (notes.size() > 65536) notes = notes.left(65536) + QStringLiteral("\n…完整日志请查看发布页");
    *release = {version, notes, QUrl(page), QVersionNumber::fromString(version) > QVersionNumber::fromString(currentVersion)};
    if (error) error->clear();
    return true;
}

void UpdateChecker::scheduleStartup(bool enabled) {
    startupEnabled_ = enabled;
    if (startupScheduled_) return;
    startupScheduled_ = true;
    if (enabled) startup_.start();
}

void UpdateChecker::check() {
    if (state_ == State::Checking) return;
    startup_.stop();
    state_ = State::Checking; error_.clear(); release_ = {}; buffer_.clear();
    QNetworkRequest request(endpoint_);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setRawHeader("User-Agent", "TwinFile/" + currentVersion_.toLatin1());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(15000);
    reply_ = network_.get(request); reply_->setReadBufferSize(MaximumResponse + 1);
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        buffer_.append(reply_->read(MaximumResponse + 1 - buffer_.size()));
        if (buffer_.size() > MaximumResponse) finish(QStringLiteral("更新响应超过大小上限"));
    });
    connect(reply_, &QNetworkReply::finished, this, [this] {
        if (!reply_) return;
        const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 403 || status == 429) { finish(QStringLiteral("GitHub 请求受限，请稍后重试")); return; }
        if (status == 404) { finish(QStringLiteral("暂未找到正式发布版本")); return; }
        if (status != 200 || reply_->error() != QNetworkReply::NoError) { finish(QStringLiteral("无法连接更新服务，请检查网络后重试")); return; }
        buffer_.append(reply_->read(MaximumResponse + 1 - buffer_.size()));
        QString error;
        if (!parseRelease(buffer_, currentVersion_, &release_, &error)) { finish(error); return; }
        finish();
    });
    deadline_.start(); emit stateChanged();
}

void UpdateChecker::disposeReply() {
    deadline_.stop();
    if (reply_) { reply_->disconnect(this); reply_->abort(); reply_->deleteLater(); reply_.clear(); }
    buffer_.clear();
}

void UpdateChecker::finish(const QString &error) {
    disposeReply(); error_ = error;
    state_ = !error.isEmpty() ? State::Error : release_.newer ? State::Available : State::Current;
    emit stateChanged();
}

void UpdateChecker::cancel() {
    startup_.stop();
    if (state_ != State::Checking) return;
    disposeReply(); state_ = State::Idle; error_.clear(); release_ = {}; emit stateChanged();
}

UpdateDialog::UpdateDialog(UpdateChecker *checker, bool startupEnabled, QWidget *parent) : QDialog(parent), checker_(checker) {
    setObjectName(QStringLiteral("updateSettings")); setWindowTitle(QStringLiteral("软件更新")); resize(510, 380);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(22, 20, 22, 20); layout->setSpacing(14);
    auto *version = new QLabel(QStringLiteral("TwinFile %1").arg(checker_->currentVersion())); version->setObjectName(QStringLiteral("appearanceHeading")); layout->addWidget(version);
    auto *startup = new QCheckBox(QStringLiteral("启动时检查更新")); startup->setObjectName(QStringLiteral("updateOnStartup")); startup->setChecked(startupEnabled); layout->addWidget(startup);
    connect(startup, &QCheckBox::toggled, this, &UpdateDialog::startupChanged);
    status_ = new QLabel; status_->setObjectName(QStringLiteral("updateStatus")); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); layout->addWidget(status_);
    notes_ = new QPlainTextEdit; notes_->setObjectName(QStringLiteral("updateNotes")); notes_->setReadOnly(true); layout->addWidget(notes_, 1);
    auto *privacy = new QLabel(QStringLiteral("仅查询官方版本，不上传文件，不自动下载安装。")); privacy->setObjectName(QStringLiteral("subtle")); privacy->setWordWrap(true); layout->addWidget(privacy);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    check_ = buttons->addButton(QStringLiteral("检查更新"), QDialogButtonBox::ActionRole); check_->setObjectName(QStringLiteral("updateCheck")); check_->setAutoDefault(false);
    download_ = buttons->addButton(QStringLiteral("前往下载"), QDialogButtonBox::ActionRole); download_->setObjectName(QStringLiteral("updateDownload")); download_->setAutoDefault(false);
    connect(check_, &QPushButton::clicked, checker_, &UpdateChecker::check);
    connect(download_, &QPushButton::clicked, this, [this] { if (checker_->state() == UpdateChecker::State::Available) QDesktopServices::openUrl(checker_->release().page); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close); layout->addWidget(buttons);
    connect(checker_, &UpdateChecker::stateChanged, this, &UpdateDialog::render); render();
}

void UpdateDialog::render() {
    const auto state = checker_->state();
    check_->setEnabled(state != UpdateChecker::State::Checking);
    download_->setVisible(state == UpdateChecker::State::Available);
    notes_->setVisible(state == UpdateChecker::State::Available);
    notes_->setPlainText(checker_->release().notes);
    switch (state) {
    case UpdateChecker::State::Idle: status_->setText(QStringLiteral("点击检查正式版本")); break;
    case UpdateChecker::State::Checking: status_->setText(QStringLiteral("正在检查更新…")); break;
    case UpdateChecker::State::Current: status_->setText(QStringLiteral("当前已是最新版本")); break;
    case UpdateChecker::State::Available: status_->setText(QStringLiteral("发现新版本 %1").arg(checker_->release().version)); break;
    case UpdateChecker::State::Error: status_->setText(checker_->error()); break;
    }
}

}
