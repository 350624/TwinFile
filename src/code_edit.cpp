#include "code_edit.h"
#include <QFontDatabase>
#include <QPainter>
#include <QResizeEvent>
#include <QTextBlock>
#include <functional>
namespace twinfile {
namespace {
class Gutter : public QWidget {
public:
    Gutter(QWidget *parent, std::function<void()> paint) : QWidget(parent), paint_(std::move(paint)) { setAttribute(Qt::WA_OpaquePaintEvent); }
    void paintEvent(QPaintEvent *) override { paint_(); }
private:
    std::function<void()> paint_;
};
}
CodeEdit::CodeEdit(QWidget *parent) : QPlainTextEdit(parent), gutter_(new Gutter(this, [this] { paintGutter(); })) {
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(QChar(' ')) * 4);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { updateGutter(); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this] { gutter_->update(); });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        QTextEdit::ExtraSelection line; line.cursor = textCursor(); line.cursor.clearSelection();
        QColor color = palette().color(QPalette::Highlight); color.setAlpha(24);
        line.format.setBackground(color); line.format.setProperty(QTextFormat::FullWidthSelection, true);
        setExtraSelections({line}); gutter_->update();
    });
    updateGutter();
}
void CodeEdit::updateGutter() {
    const int width = 18 + fontMetrics().horizontalAdvance(QChar('9')) * QString::number(blockCount()).size();
    setViewportMargins(width, 0, 0, 0);
    const QRect area = contentsRect(); gutter_->setGeometry(area.left(), area.top(), width, area.height()); gutter_->update();
}
void CodeEdit::resizeEvent(QResizeEvent *event) { QPlainTextEdit::resizeEvent(event); updateGutter(); }
void CodeEdit::paintGutter() {
    QPainter painter(gutter_); painter.fillRect(gutter_->rect(), palette().color(QPalette::Window));
    QTextBlock block = firstVisibleBlock();
    while (block.isValid()) {
        const QRectF rect = blockBoundingGeometry(block).translated(contentOffset());
        if (rect.top() > gutter_->height()) break;
        if (block.isVisible() && rect.bottom() >= 0) {
            painter.setPen(palette().color(block.blockNumber() == textCursor().blockNumber() ? QPalette::Text : QPalette::PlaceholderText));
            painter.drawText(0, int(rect.top()), gutter_->width() - 8, fontMetrics().height(), Qt::AlignRight, QString::number(block.blockNumber() + 1));
        }
        block = block.next();
    }
}
}
