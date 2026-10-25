#include "model.h"

#include <QApplication>
#include <QDrag>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QStyle>
#include <QSet>
#include <QUrl>
#include <QtMath>

namespace twinfile {
namespace {

QPixmap maskedEntryIcon(const Entry &entry, const QImage &image, int size, qreal ratio, const QPalette &palette) {
    const QIcon fallback = image.isNull() ? entryIcon(entry) : QIcon();
    const bool preview = !entry.directory && isImage(entry.name) && QFileInfo(entry.name).suffix().compare(QStringLiteral("ico"), Qt::CaseInsensitive) != 0;
    const QString key = QStringLiteral("twinfile-masked-icon:%1:%2:%3:%4:%5:%6")
        .arg(image.isNull() ? fallback.cacheKey() : image.cacheKey()).arg(preview).arg(size)
        .arg(ratio, 0, 'g', 12).arg(palette.base().color().rgba()).arg(palette.text().color().rgba());
    QPixmap pixmap;
    if (QPixmapCache::find(key, &pixmap)) return pixmap;
    pixmap = QPixmap(qCeil(size * ratio), qCeil(size * ratio));
    pixmap.setDevicePixelRatio(ratio); pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing); painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF bounds(0, 0, size, size);
    const qreal radius = size * 0.24;
    QPainterPath mask; mask.addRoundedRect(bounds, radius, radius);
    painter.setClipPath(mask);
    QColor tint = palette.text().color(); tint.setAlpha(palette.base().color().lightness() < 128 ? 15 : 10);
    painter.fillPath(mask, palette.base()); painter.fillPath(mask, tint);
    if (!image.isNull()) {
        const QRectF content = preview ? bounds : bounds.adjusted(2, 2, -2, -2);
        QPainterPath contentMask; contentMask.addRoundedRect(content, preview ? radius : radius - 2, preview ? radius : radius - 2);
        painter.setClipPath(contentMask, Qt::IntersectClip);
        if (preview) {
            const qreal side = qMin(image.width(), image.height());
            const QRectF crop((image.width() - side) / 2, (image.height() - side) / 2, side, side);
            painter.drawImage(content, image, crop);
        } else {
            const QSizeF fitted = QSizeF(image.size()).scaled(content.size(), Qt::KeepAspectRatio);
            painter.drawImage(QRectF(content.center() - QPointF(fitted.width() / 2, fitted.height() / 2), fitted), image);
        }
        painter.setClipPath(mask);
    } else fallback.paint(&painter, bounds.toRect());
    QColor edge = palette.text().color(); edge.setAlpha(palette.base().color().lightness() < 128 ? 22 : 16);
    painter.setPen(QPen(edge, 1)); painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(bounds.adjusted(0.5, 0.5, -0.5, -0.5), radius - 0.5, radius - 0.5);
    painter.end(); QPixmapCache::insert(key, pixmap);
    return pixmap;
}

}

FileModel::FileModel(QObject *parent) : QAbstractListModel(parent) {}
int FileModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(entries_.size()); }
Entry FileModel::entry(int row) const { return row >= 0 && row < entries_.size() ? entries_[row] : Entry(); }

QVariant FileModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= entries_.size()) return {};
    const Entry &entry = entries_[index.row()];
    if (role == Qt::DisplayRole) return entry.name;
    if (role == Qt::ToolTipRole) return entry.source == Source::Archive ? entry.inner : entry.path;
    if (role == EntryRole) return QVariant::fromValue(entry);
    if (role == ImageRole) return thumbnails_.value(entry.key());
    return {};
}

Qt::ItemFlags FileModel::flags(const QModelIndex &index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    const Entry entry = this->entry(index.row());
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | (entry.source == Source::Local ? Qt::ItemIsDragEnabled : Qt::NoItemFlags);
}

QStringList FileModel::mimeTypes() const { return {QStringLiteral("text/uri-list")}; }
QMimeData *FileModel::mimeData(const QModelIndexList &indexes) const {
    if (indexes.isEmpty()) return nullptr;
    QList<QUrl> urls;
    QSet<int> rows;
    for (const QModelIndex &index : indexes) {
        if (!index.isValid() || rows.contains(index.row())) continue;
        rows.insert(index.row());
        const Entry item = entry(index.row());
        if (item.source != Source::Local || !QFileInfo::exists(item.path)) return nullptr;
        urls.append(QUrl::fromLocalFile(QFileInfo(item.path).absoluteFilePath()));
    }
    if (urls.isEmpty()) return nullptr;
    auto *data = new QMimeData;
    data->setUrls(urls);
    return data;
}

void FileModel::replace(QList<Entry> entries) {
    QHash<QString, Entry> old;
    for (const Entry &entry : entries_) if (thumbnails_.contains(entry.key())) old.insert(entry.key(), entry);
    beginResetModel(); entries_ = std::move(entries); rows_.clear();
    QHash<QString, QImage> retained;
    for (int row = 0; row < entries_.size(); ++row) {
        const Entry &entry = entries_[row]; const QString key = entry.key(); rows_.insert(key, row);
        if (old.contains(key) && old.value(key).modified == entry.modified && old.value(key).size == entry.size) retained.insert(key, thumbnails_.value(key));
    }
    thumbnails_ = std::move(retained); endResetModel();
}

QImage FileModel::thumbnail(int row) const { return thumbnails_.value(entry(row).key()); }
void FileModel::setThumbnail(const QString &key, const QImage &image) {
    if (image.isNull() || !rows_.contains(key)) return;
    if (thumbnails_.size() >= 300 && !thumbnails_.contains(key)) thumbnails_.erase(thumbnails_.begin());
    thumbnails_.insert(key, image);
    const int row = rows_.value(key); emit dataChanged(index(row), index(row), {ImageRole});
}

QIcon entryIcon(const Entry &entry) {
    static QHash<QString, QIcon> icons;
    const QString suffix = QFileInfo(entry.name).suffix().toLower();
    const QString kind = entry.source == Source::Device ? QStringLiteral("phone")
        : entry.source == Source::Drive ? QStringLiteral("drive") : entry.directory ? QStringLiteral("folder")
        : isApk(entry.name) ? QStringLiteral("apk") : isArchive(entry.name) ? QStringLiteral("zip")
        : isImage(entry.name) ? QStringLiteral("image") : suffix == QStringLiteral("pdf") ? QStringLiteral("pdf")
        : QStringList{QStringLiteral("json"), QStringLiteral("xml"), QStringLiteral("yaml"), QStringLiteral("yml"), QStringLiteral("cpp"), QStringLiteral("h")}.contains(suffix) ? QStringLiteral("code")
        : QStringList{QStringLiteral("wav"), QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("ogg"), QStringLiteral("m4a")}.contains(suffix) ? QStringLiteral("audio")
        : QStringList{QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("avi"), QStringLiteral("webm")}.contains(suffix) ? QStringLiteral("video") : QStringLiteral("file");
    const bool mac = qApp->property("macosStyle").toBool();
    const bool mt = qApp->property("mtStyle").toBool();
    const QString cacheKey = kind + (mt ? QStringLiteral("-mt") : mac ? QStringLiteral("-macos") : QString());
    if (icons.contains(cacheKey)) return icons[cacheKey];
    QPixmap pixmap(96, 96); pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
    if (mt) {
        painter.setPen(Qt::NoPen); painter.setBrush(QColor(QStringLiteral("#303030"))); painter.drawRoundedRect(QRectF(1, 1, 94, 94), 15, 15);
        painter.setBrush(Qt::white);
        if (kind == QStringLiteral("folder")) {
            QPainterPath folder; folder.moveTo(25, 31); folder.lineTo(42, 31); folder.lineTo(49, 38); folder.lineTo(72, 38);
            folder.quadTo(76, 38, 76, 42); folder.lineTo(76, 66); folder.quadTo(76, 71, 71, 71); folder.lineTo(25, 71);
            folder.quadTo(20, 71, 20, 66); folder.lineTo(20, 36); folder.quadTo(20, 31, 25, 31); painter.drawPath(folder);
        } else {
            painter.drawRoundedRect(QRectF(29, 22, 38, 52), 3, 3); painter.setPen(QPen(QColor(QStringLiteral("#303030")), 3));
            if (kind == QStringLiteral("phone") || kind == QStringLiteral("drive")) painter.drawRect(QRectF(35, 29, 26, 32));
            else {
                QFont font = painter.font(); font.setPixelSize(13); font.setBold(true); painter.setFont(font);
                painter.drawText(QRect(29, 28, 38, 40), Qt::AlignCenter, kind == QStringLiteral("file") ? QStringLiteral("=") : kind.toUpper());
            }
        }
        painter.end(); icons.insert(cacheKey, QIcon(pixmap)); return icons[cacheKey];
    }
    const QColor color = kind == QStringLiteral("folder") ? QColor(QStringLiteral("#48c8f6"))
        : kind == QStringLiteral("apk") ? QColor(QStringLiteral("#8cc549"))
        : kind == QStringLiteral("zip") ? QColor(QStringLiteral("#ffbd42")) : kind == QStringLiteral("pdf") ? QColor(QStringLiteral("#f08085"))
        : kind == QStringLiteral("code") ? QColor(QStringLiteral("#65c8bb")) : kind == QStringLiteral("audio") ? QColor(QStringLiteral("#b595e5"))
        : kind == QStringLiteral("video") ? QColor(QStringLiteral("#829ff0")) : QColor(QStringLiteral("#9eace3"));
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    if (kind == QStringLiteral("folder")) {
        QLinearGradient gradient(0, 23, 0, 78); gradient.setColorAt(0, QColor(mac ? QStringLiteral("#84dfff") : QStringLiteral("#83b7ff"))); gradient.setColorAt(1, QColor(mac ? QStringLiteral("#1c9fea") : QStringLiteral("#5086ed"))); painter.setBrush(gradient);
        QPainterPath shape; shape.moveTo(15, 29); shape.quadTo(15, 23, 21, 23);
        shape.lineTo(39, 23); shape.lineTo(48, 32); shape.lineTo(77, 32); shape.quadTo(83, 32, 83, 38);
        shape.lineTo(83, 72); shape.quadTo(83, 78, 77, 78); shape.lineTo(21, 78); shape.quadTo(15, 78, 15, 72); shape.closeSubpath(); painter.drawPath(shape);
    } else if (kind == QStringLiteral("phone")) {
        painter.drawRoundedRect(QRectF(26, 12, 44, 73), 9, 9);
        painter.setBrush(QColor(QStringLiteral("#213146"))); painter.drawRoundedRect(QRectF(32, 20, 32, 52), 3, 3);
        painter.setBrush(color); painter.drawEllipse(QPointF(48, 78), 3, 3);
    } else if (kind == QStringLiteral("drive")) {
        painter.drawRoundedRect(QRectF(15, 22, 66, 52), 9, 9);
        painter.setBrush(color.darker(135)); painter.drawRoundedRect(QRectF(15, 55, 66, 19), 6, 6);
        painter.setBrush(QColor(QStringLiteral("#e7efff"))); painter.drawEllipse(QPointF(68, 64), 3, 3);
    } else {
        QPainterPath document; document.moveTo(24, 13); document.lineTo(58, 13); document.lineTo(76, 31); document.lineTo(76, 77); document.quadTo(76, 83, 70, 83); document.lineTo(24, 83); document.quadTo(18, 83, 18, 77); document.lineTo(18, 19); document.quadTo(18, 13, 24, 13); painter.drawPath(document);
        painter.setBrush(color.lighter(130)); QPainterPath fold; fold.moveTo(58, 13); fold.lineTo(58, 31); fold.lineTo(76, 31); fold.closeSubpath(); painter.drawPath(fold);
        painter.setPen(QPen(QColor(QStringLiteral("#25334d")), 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (kind == QStringLiteral("file")) {
            painter.drawLine(QPointF(31, 45), QPointF(62, 45)); painter.drawLine(QPointF(31, 56), QPointF(62, 56)); painter.drawLine(QPointF(31, 67), QPointF(52, 67));
        } else if (kind == QStringLiteral("code")) {
            painter.drawPolyline(QPolygonF{QPointF(39, 44), QPointF(29, 54), QPointF(39, 64)}); painter.drawPolyline(QPolygonF{QPointF(56, 44), QPointF(66, 54), QPointF(56, 64)});
        } else if (kind == QStringLiteral("audio")) {
            painter.drawLine(QPointF(48, 40), QPointF(48, 66)); painter.drawLine(QPointF(48, 40), QPointF(62, 43)); painter.setBrush(QColor(QStringLiteral("#25334d"))); painter.drawEllipse(QPointF(40, 66), 8, 6);
        } else if (kind == QStringLiteral("video")) {
            painter.setBrush(QColor(QStringLiteral("#25334d"))); painter.drawPolygon(QPolygonF{QPointF(39, 41), QPointF(61, 54), QPointF(39, 67)});
        } else if (kind == QStringLiteral("image")) {
            painter.drawPolyline(QPolygonF{QPointF(29, 65), QPointF(42, 50), QPointF(51, 59), QPointF(58, 51), QPointF(66, 65)});
            painter.setBrush(QColor(QStringLiteral("#25334d"))); painter.setPen(Qt::NoPen); painter.drawEllipse(QPointF(58, 41), 4, 4);
        } else {
            QFont font = painter.font(); font.setBold(true); font.setPixelSize(16); painter.setFont(font);
            painter.drawText(QRectF(20, 38, 54, 34), Qt::AlignCenter, kind.toUpper());
        }
    }
    painter.end(); icons.insert(cacheKey, QIcon(pixmap)); return icons[cacheKey];
}

QSize FileDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const { return {180, 54}; }
void FileDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const Entry entry = index.data(FileModel::EntryRole).value<Entry>();
    painter->save(); painter->setRenderHint(QPainter::Antialiasing);
    if (const auto *view = qobject_cast<const FileView *>(parent()); view && view->isTransitionRunning()) {
        const qreal remaining = 1.0 - view->transitionProgress();
        painter->setOpacity(painter->opacity() * (1.0 - 0.75 * remaining));
        if (view->transition() == FileView::Transition::Refresh) painter->translate(0, 8 * remaining);
        else painter->translate((view->transition() == FileView::Transition::LeaveFolder ? -18 : 18) * remaining, 0);
    }
    const bool mt = qApp->property("mtStyle").toBool();
    const QRect body = mt ? option.rect : option.rect.adjusted(3, 2, -5, -2);
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    if (selected || option.state.testFlag(QStyle::State_MouseOver)) {
        painter->setPen(Qt::NoPen); painter->setBrush(selected ? option.palette.highlight() : option.palette.alternateBase()); painter->drawRoundedRect(body, mt ? 0 : 8, mt ? 0 : 8);
    }
    const int iconSize = mt ? 40 : 36;
    const QRect iconBox(body.left() + 8, body.top() + (body.height() - iconSize) / 2, iconSize, iconSize);
    const QImage image = index.data(FileModel::ImageRole).value<QImage>();
    painter->drawPixmap(iconBox.topLeft(), maskedEntryIcon(entry, image, iconSize, painter->device()->devicePixelRatioF(), option.palette));
    const QRect title(iconBox.right() + 12, body.top() + 2, body.width() - iconSize - 32, 22);
    QFont titleFont = option.font; if (mt) titleFont.setPixelSize(15); painter->setFont(titleFont);
    painter->setPen(selected ? option.palette.highlightedText().color() : option.palette.text().color());
    painter->drawText(title, Qt::AlignLeft | Qt::AlignVCenter, painter->fontMetrics().elidedText(entry.name, Qt::ElideMiddle, title.width()));
    QColor secondary = selected ? option.palette.highlightedText().color() : option.palette.placeholderText().color(); painter->setPen(secondary);
    QFont font = option.font; font.setPixelSize(mt ? 12 : 11); painter->setFont(font);
    QString detail = entry.detail;
    if (detail.isEmpty()) detail = (entry.directory ? QString() : humanSize(entry.size) + QStringLiteral(" · ")) + entry.modified.toString(mt ? QStringLiteral("yy-MM-dd HH:mm") : QStringLiteral("yyyy-MM-dd HH:mm"));
    painter->drawText(title.translated(0, 21), Qt::AlignLeft | Qt::AlignVCenter, painter->fontMetrics().elidedText(detail, Qt::ElideMiddle, title.width()));
    painter->restore();
}

FileView::FileView(QWidget *parent) : QListView(parent) {
    setObjectName(QStringLiteral("fileView"));
    setSelectionMode(QAbstractItemView::ExtendedSelection); setUniformItemSizes(true); setMouseTracking(true);
    setDragEnabled(true); setDragDropMode(QAbstractItemView::DragOnly); setDefaultDropAction(Qt::CopyAction);
    setAcceptDrops(false); setItemDelegate(new FileDelegate(this)); setSpacing(0); setFrameShape(QFrame::NoFrame);
    transitionAnimation_ = new QVariantAnimation(this);
    transitionAnimation_->setStartValue(0.0); transitionAnimation_->setEndValue(1.0); transitionAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(transitionAnimation_, &QVariantAnimation::valueChanged, this, [this] { viewport()->update(); });
    connect(transitionAnimation_, &QVariantAnimation::finished, this, [this] { transition_ = Transition::None; viewport()->update(); });
}

void FileView::setAnimationsEnabled(bool enabled) { animationsEnabled_ = enabled; if (!enabled) stopTransition(); }
qreal FileView::transitionProgress() const { return isTransitionRunning() ? transitionAnimation_->currentValue().toReal() : 1.0; }
void FileView::startTransition(Transition transition) {
    stopTransition();
    if (!animationsEnabled_ || transition == Transition::None || !isVisible()) return;
    doItemsLayout();
    transition_ = transition;
    transitionAnimation_->setDuration(transition == Transition::Refresh ? 180 : 220);
    transitionAnimation_->start(); emit transitionStarted(transition);
}
void FileView::stopTransition() {
    if (transition_ == Transition::None) return;
    transitionAnimation_->stop(); transition_ = Transition::None; viewport()->update();
}
void FileView::deselect() { stopTransition(); clearSelection(); setCurrentIndex({}); }
void FileView::mousePressEvent(QMouseEvent *event) {
    stopTransition();
    emit activatedPane();
    const bool blank = event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier && !indexAt(event->position().toPoint()).isValid();
    QListView::mousePressEvent(event); if (blank) deselect();
}
void FileView::focusInEvent(QFocusEvent *event) { QListView::focusInEvent(event); emit activatedPane(); }
void FileView::keyPressEvent(QKeyEvent *event) {
    stopTransition();
    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier) { emit previewRequested(); event->accept(); return; }
    QListView::keyPressEvent(event);
}
void FileView::startDrag(Qt::DropActions) {
    stopTransition();
    const QModelIndexList selected = selectionModel()->selectedRows();
    if (selected.isEmpty()) return;
    std::unique_ptr<QMimeData> data(model()->mimeData(selected));
    if (!data) return;
    QModelIndex primary = currentIndex();
    if (!selectionModel()->isSelected(primary)) primary = selected.first();
    const Entry entry = primary.data(FileModel::EntryRole).value<Entry>();
    const qreal ratio = devicePixelRatioF(); QPixmap pixmap(qRound(64 * ratio), qRound(64 * ratio));
    pixmap.setDevicePixelRatio(ratio); pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing); painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QImage image = primary.data(FileModel::ImageRole).value<QImage>();
    painter.drawPixmap(QPoint(4, 4), maskedEntryIcon(entry, image, 48, ratio, palette()));
    if (selected.size() > 1) {
        const QString label = selected.size() > 999 ? QStringLiteral("999+") : QString::number(selected.size());
        QFont font = painter.font(); font.setPixelSize(12); font.setBold(true); painter.setFont(font);
        const int width = qMax(22, painter.fontMetrics().horizontalAdvance(label) + 12);
        const QRectF badge(62 - width, 40, width, 22);
        painter.setPen(Qt::NoPen); painter.setBrush(palette().highlight()); painter.drawRoundedRect(badge, 11, 11);
        painter.setPen(palette().highlightedText().color()); painter.drawText(badge, Qt::AlignCenter, label);
    }
    painter.end();
    QDrag drag(this); drag.setMimeData(data.release()); drag.setPixmap(pixmap); drag.setHotSpot(QPoint(8, 8)); drag.exec(Qt::CopyAction, Qt::CopyAction);
}

void FileView::wheelEvent(QWheelEvent *event) { stopTransition(); QListView::wheelEvent(event); }
void FileView::resizeEvent(QResizeEvent *event) { stopTransition(); QListView::resizeEvent(event); }
void FileView::hideEvent(QHideEvent *event) { stopTransition(); QListView::hideEvent(event); }
void FileView::changeEvent(QEvent *event) {
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) stopTransition();
    QListView::changeEvent(event);
}
void FileView::scrollContentsBy(int horizontal, int vertical) { stopTransition(); QListView::scrollContentsBy(horizontal, vertical); }

}
