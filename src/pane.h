#pragma once

#include "core.h"
#include "model.h"
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QSet>
#include <QTabBar>
#include <QTimer>

namespace twinfile {

class RefreshButton;

class Pane : public QFrame {
    Q_OBJECT
public:
    explicit Pane(QWidget *parent = nullptr);
    ~Pane() override;
    Location location() const;
    QList<Entry> selected() const;
    FileView *view() const { return view_; }
    FileModel *fileModel() const { return model_; }
    QString error() const { return error_; }
    void setDevices(const QList<Entry> &devices);
    void setOptions(bool hidden, QString sort, bool descending);
    void setAnimationsEnabled(bool enabled);
    QJsonObject workspace() const;
    void restore(const QJsonObject &workspace);
    void navigate(const Location &location);
    void refresh(bool preserve = true);
    void newTab();
    void closeTab();
    void cycleTab(int direction);
    void goBack();
    void goForward();
    void goUp();
    void focusPath();
    void deselect();
    void search(const QString &pattern);
    void registerArchiveParent(const QString &path, const Location &parent) { archiveParents_.insert(path, parent); }
signals:
    void activated(twinfile::Pane *pane);
    void openRequested(const twinfile::Entry &entry);
    void previewRequested();
    void menuRequested(const QPoint &position);
    void locationChanged();
private:
    struct Tab {
        Location location;
        QList<Location> history;
        int historyIndex = 0;
        int scroll = 0;
        QSet<QString> selection;
        QString current;
    };
    void saveView();
    void reload(bool preserve, FileView::Transition transition);
    void navigateTo(const Location &destination, FileView::Transition transition);
    void apply(QList<Entry> entries, bool restoreView);
    void updateFooter();
    void openIndex(const QModelIndex &index);
    void scheduleThumbnails();
    void loadThumbnails();
    bool eventFilter(QObject *object, QEvent *event) override;
    QTabBar *tabs_ = nullptr;
    QLineEdit *path_ = nullptr;
    FileView *view_ = nullptr;
    FileModel *model_ = nullptr;
    QLabel *footer_ = nullptr;
    QWidget *blank_ = nullptr;
    RefreshButton *refresh_ = nullptr;
    QList<Tab> states_;
    QList<Entry> devices_;
    int tab_ = 0;
    quint64 generation_ = 0;
    bool switching_ = false;
    bool hidden_ = false;
    bool descending_ = false;
    QString sort_ = QStringLiteral("name");
    QString error_;
    QTimer thumbnailTimer_;
    QSet<QString> thumbnailsRequested_;
    QSet<QString> thumbnailsFailed_;
    QHash<QString, int> thumbnailAttempts_;
    int thumbnailJobs_ = 0;
    Cancellation thumbnailCancel_;
    Cancellation searchCancel_;
    QMap<QString, Location> archiveParents_;
};

}
