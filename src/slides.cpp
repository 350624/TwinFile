#include "slides.h"
#include "appearance.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QResizeEvent>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#ifdef TWINFILE_SLIDE_BRIDGE
static void initializeSlideResources() { Q_INIT_RESOURCE(slide_resources); }
#endif
namespace twinfile {
bool isPresentation(const QString &name) { return name.endsWith(QStringLiteral(".pptx"), Qt::CaseInsensitive); }
SlidesDialog::SlidesDialog(const QString &path, QWidget *parent, std::shared_ptr<QTemporaryDir> temporary)
    : QDialog(parent), temporary_(std::move(temporary)) {
    setAttribute(Qt::WA_DeleteOnClose); setObjectName(QStringLiteral("slidesDialog"));
    setWindowTitle(QFileInfo(path).fileName()); resize(1120, 760);
    auto *layout = new QVBoxLayout(this); auto *bar = new QHBoxLayout; bar->setSpacing(4); layout->addLayout(bar);
    auto button = [&](const QString &name, const QString &tip, const QString &symbol) {
        auto *control = new QToolButton; control->setObjectName(name); control->setToolTip(tip); control->setAccessibleName(tip); control->setProperty("slideSymbol", symbol); control->setIconSize(QSize(20, 20)); control->setToolButtonStyle(Qt::ToolButtonIconOnly); control->setFixedSize(34, 34);
        control->setStyleSheet(QStringLiteral("QToolButton { padding: 0px; border-radius: 6px; }"));
        bar->addWidget(control); return control;
    };
    previous_ = button(QStringLiteral("slidePrevious"), QStringLiteral("上一页"), QStringLiteral("back"));
    page_ = new QSpinBox; page_->setObjectName(QStringLiteral("slidePage")); page_->setRange(1, 1); page_->setEnabled(false); page_->setFixedWidth(76); page_->setAccessibleName(QStringLiteral("页码")); bar->addWidget(page_);
    count_ = new QLabel(QStringLiteral("/ —")); bar->addWidget(count_);
    next_ = button(QStringLiteral("slideNext"), QStringLiteral("下一页"), QStringLiteral("forward"));
    bar->addStretch();
    auto *zoomOut = button(QStringLiteral("slideZoomOut"), QStringLiteral("缩小"), QStringLiteral("zoom-out"));
    auto *zoomIn = button(QStringLiteral("slideZoomIn"), QStringLiteral("放大"), QStringLiteral("zoom-in"));
    auto *fitButton = button(QStringLiteral("slideFit"), QStringLiteral("适应窗口"), QStringLiteral("fit"));
    auto *showText = button(QStringLiteral("slideShowText"), QStringLiteral("幻灯片文字"), QStringLiteral("text")); showText->setCheckable(true);
    updateIcons();
    auto *split = new QSplitter(Qt::Vertical); layout->addWidget(split, 1);
    view_ = new QGraphicsView; view_->setObjectName(QStringLiteral("slideCanvas")); view_->setRenderHint(QPainter::SmoothPixmapTransform); view_->setDragMode(QGraphicsView::ScrollHandDrag); view_->setMinimumSize(120, 100);
    auto *scene = new QGraphicsScene(view_); view_->setScene(scene); image_ = scene->addPixmap(QPixmap()); split->addWidget(view_);
    text_ = new QPlainTextEdit; text_->setObjectName(QStringLiteral("slideText")); text_->setReadOnly(true); text_->setMaximumHeight(220); split->addWidget(text_); text_->hide();
    status_ = new QLabel(QStringLiteral("正在读取演示文稿…")); status_->setObjectName(QStringLiteral("slideStatus")); status_->setWordWrap(true); layout->addWidget(status_);
    previous_->setEnabled(false); next_->setEnabled(false);
    connect(previous_, &QToolButton::clicked, this, [this] { page_->setValue(page_->value() - 1); });
    connect(next_, &QToolButton::clicked, this, [this] { page_->setValue(page_->value() + 1); });
    connect(page_, &QSpinBox::valueChanged, this, [this] { requestPage(); });
    connect(zoomIn, &QToolButton::clicked, this, [this] { if (view_->transform().m11() < 4) { fitting_ = false; view_->scale(1.25, 1.25); } });
    connect(zoomOut, &QToolButton::clicked, this, [this] { if (view_->transform().m11() > 0.1) { fitting_ = false; view_->scale(0.8, 0.8); } });
    connect(fitButton, &QToolButton::clicked, this, [this] { fitting_ = true; fit(); });
    connect(showText, &QToolButton::toggled, this, [this](bool visible) { text_->setVisible(visible); QTimer::singleShot(0, this, [this] { if (fitting_) fit(); }); });
    for (const auto &binding : QList<QPair<QKeySequence, int>>{{QKeySequence(Qt::Key_PageDown), 1}, {QKeySequence(Qt::Key_PageUp), -1}}) {
        auto *shortcut = new QShortcut(binding.first, this); connect(shortcut, &QShortcut::activated, this, [this, step = binding.second] { page_->setValue(page_->value() + step); });
    }
    timeout_.setSingleShot(true); timeout_.setInterval(60000); connect(&timeout_, &QTimer::timeout, this, [this] { fail(QStringLiteral("读取或渲染超过 60 秒，已停止")); });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { readOutput(); });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] { process_.readAllStandardError(); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) { if (!failed_) fail(QStringLiteral("幻灯片渲染进程无法运行：") + process_.errorString()); });
    connect(&process_, &QProcess::finished, this, [this](int, QProcess::ExitStatus) { readOutput(); if (!failed_) fail(QStringLiteral("幻灯片渲染进程已退出")); });
    QTimer::singleShot(0, this, [this, path] {
#ifdef TWINFILE_SLIDE_BRIDGE
        initializeSlideResources();
        if (!workspace_.isValid()) { fail(QStringLiteral("无法创建预览暂存目录")); return; }
        if (!QFileInfo(path).isFile() || QFileInfo(path).size() > 128 * 1024 * 1024) { fail(QStringLiteral("PPTX 无法读取或超过 128 MiB")); return; }
        QFile helper(QStringLiteral(":/slides/SlideBridge.class"));
        if (!helper.copy(workspace_.filePath(QStringLiteral("SlideBridge.class")))) { fail(QStringLiteral("无法准备幻灯片渲染器")); return; }
        const QString java = toolPath(QStringLiteral("runtime/bin/java.exe"));
        const QString library = toolPath(QStringLiteral("slides/poi-ooxml-5.5.1.jar"));
        if (java.isEmpty() || library.isEmpty()) { fail(QStringLiteral("缺少内置幻灯片组件，请使用完整发行版")); return; }
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const QString &name : {QStringLiteral("JAVA_TOOL_OPTIONS"), QStringLiteral("_JAVA_OPTIONS"), QStringLiteral("JDK_JAVA_OPTIONS"), QStringLiteral("CLASSPATH")}) environment.remove(name);
        process_.setProcessEnvironment(environment); process_.setWorkingDirectory(workspace_.path()); process_.setProgram(java);
        process_.setArguments({QStringLiteral("-Xmx512m"), QStringLiteral("-Djava.awt.headless=true"), QStringLiteral("-Dfile.encoding=UTF-8"), QStringLiteral("-Dlog4j2.loggerContextFactory=org.apache.logging.log4j.simple.SimpleLoggerContextFactory"), QStringLiteral("-Djavax.xml.accessExternalDTD="), QStringLiteral("-Djavax.xml.accessExternalSchema="), QStringLiteral("-Djavax.xml.accessExternalStylesheet="), QStringLiteral("-cp"), workspace_.path() + QChar(';') + QFileInfo(library).absolutePath() + QStringLiteral("/*"), QStringLiteral("SlideBridge"), QFileInfo(path).absoluteFilePath(), workspace_.path()});
        timeout_.start(); process_.start();
#else
        Q_UNUSED(path); fail(QStringLiteral("此构建未包含幻灯片查看器组件"));
#endif
    });
}
SlidesDialog::~SlidesDialog() {
    process_.disconnect(this); timeout_.stop();
    if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(5000); }
}
void SlidesDialog::fail(const QString &message) {
    if (failed_) return;
    failed_ = true; timeout_.stop(); status_->setText(QStringLiteral("无法预览：") + message); page_->setEnabled(false); previous_->setEnabled(false); next_->setEnabled(false); process_.kill();
}
void SlidesDialog::readOutput() {
    pending_ += process_.readAllStandardOutput();
    if (pending_.size() > 8192) { fail(QStringLiteral("渲染器响应过大")); return; }
    while (pending_.contains('\n')) {
        const int end = int(pending_.indexOf('\n')); const QByteArray line = pending_.left(end).trimmed(); pending_.remove(0, end + 1);
        if (failed_) continue;
        if (line.startsWith("ERROR\t")) { fail(QString::fromUtf8(QByteArray::fromBase64(line.mid(6)))); continue; }
        const auto fields = line.split('\t'); bool valid = false; const int value = fields.value(1).toInt(&valid);
        if (fields.size() != 2 || !valid) { fail(QStringLiteral("渲染器响应无效")); return; }
        if (fields[0] == "READY" && pages_ == 0 && value >= 1 && value <= 500) {
            pages_ = value; page_->setRange(1, pages_); page_->setEnabled(true); count_->setText(QStringLiteral("/ %1").arg(pages_)); requestPage();
        } else if (fields[0] == "PAGE" && value == rendering_) {
            QImageReader reader(workspace_.filePath(QStringLiteral("page.png"))); const QSize dimensions = reader.size();
            if (!dimensions.isValid() || dimensions.width() > 1600 || dimensions.height() > 1200) { fail(QStringLiteral("预览图尺寸无效")); return; }
            const QImage image = reader.read(); reader.setFileName(QString()); if (image.isNull()) { fail(QStringLiteral("无法读取预览图")); return; }
            image_->setPixmap(QPixmap::fromImage(image)); view_->scene()->setSceneRect(image_->boundingRect()); if (fitting_) fit();
            QFile content(workspace_.filePath(QStringLiteral("page.txt"))); if (!content.open(QIODevice::ReadOnly) || content.size() > 1024 * 1024) { fail(QStringLiteral("无法读取幻灯片文字")); return; }
            text_->setPlainText(QString::fromUtf8(content.readAll())); content.close();
            rendering_ = -1; timeout_.stop(); status_->setText(QStringLiteral("第 %1 / %2 页 · 只读").arg(value + 1).arg(pages_));
            previous_->setEnabled(page_->value() > 1); next_->setEnabled(page_->value() < pages_);
            if (page_->value() != value + 1) requestPage();
        } else { fail(QStringLiteral("渲染器状态无效")); return; }
    }
}
void SlidesDialog::requestPage() {
    if (failed_ || pages_ == 0 || rendering_ >= 0) return;
    rendering_ = page_->value() - 1; status_->setText(QStringLiteral("正在渲染第 %1 页…").arg(rendering_ + 1));
    previous_->setEnabled(page_->value() > 1); next_->setEnabled(page_->value() < pages_);
    process_.write("RENDER\t" + QByteArray::number(rendering_) + '\n'); timeout_.start();
}
void SlidesDialog::fit() { if (!image_->pixmap().isNull()) view_->fitInView(image_->boundingRect(), Qt::KeepAspectRatio); }
void SlidesDialog::updateIcons() {
    for (auto *button : findChildren<QToolButton *>()) {
        const QString symbol = button->property("slideSymbol").toString();
        if (symbol.isEmpty()) continue;
        QIcon icon = actionSymbol(symbol, palette().color(QPalette::ButtonText));
        icon.addPixmap(actionSymbol(symbol, palette().color(QPalette::Disabled, QPalette::ButtonText)).pixmap(20, 20), QIcon::Disabled);
        icon.addPixmap(actionSymbol(symbol, palette().color(QPalette::HighlightedText)).pixmap(20, 20), QIcon::Normal, QIcon::On);
        button->setIcon(icon);
    }
}
void SlidesDialog::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) updateIcons();
}
void SlidesDialog::resizeEvent(QResizeEvent *event) { QDialog::resizeEvent(event); if (fitting_) fit(); }
}
