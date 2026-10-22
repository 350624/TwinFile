#pragma once

#include "core.h"
#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QListView>
#include <QStyledItemDelegate>
#include <QVariantAnimation>

namespace twinfile {

class FileModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { EntryRole = Qt::UserRole + 1, ImageRole };
    explicit FileModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction; }
    void replace(QList<Entry> entries);
    const QList<Entry> &entries() const { return entries_; }
    Entry entry(int row) const;
    void setThumbnail(const QString &key, const QImage &image);
    QImage thumbnail(int row) const;
private:
    QList<Entry> entries_;
    QHash<QString, QImage> thumbnails_;
    QHash<QString, int> rows_;
};

QIcon entryIcon(const Entry &entry);

class FileDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override;
    void paint(QPainter *, const QStyleOptionViewItem &, const QModelIndex &) const override;
};

class FileView : public QListView {
    Q_OBJECT
public:
    enum class Transition { None, Refresh, EnterFolder, LeaveFolder };
    Q_ENUM(Transition)
    explicit FileView(QWidget *parent = nullptr);
    void deselect();
    void setAnimationsEnabled(bool enabled);
    bool animationsEnabled() const { return animationsEnabled_; }
    void startTransition(Transition transition);
    void stopTransition();
    bool isTransitionRunning() const { return transitionAnimation_->state() == QAbstractAnimation::Running; }
    Transition transition() const { return transition_; }
    qreal transitionProgress() const;
signals:
    void activatedPane();
    void previewRequested();
    void transitionStarted(twinfile::FileView::Transition transition);
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void startDrag(Qt::DropActions) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;
    void scrollContentsBy(int horizontal, int vertical) override;
private:
    QVariantAnimation *transitionAnimation_ = nullptr;
    Transition transition_ = Transition::None;
    bool animationsEnabled_ = true;
};

}

