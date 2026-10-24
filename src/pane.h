// One file pane: path bar, six interchangeable views over the same model,
// quick filter / recursive search and every per-folder action.
#pragma once
#include "settings.h"
#include <QFrame>
#include <QModelIndex>
#include <QPointer>
#include <QStringList>

class FsModel;
class SortProxy;
class QAbstractItemView;
class QColumnView;
class QItemSelectionModel;
class QLabel;
class QLineEdit;
class QListView;
class QMenu;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QTreeView;

class Pane : public QFrame {
    Q_OBJECT
public:
    explicit Pane(QWidget *parent = nullptr);
    void setPath(const QString &path, bool record = true, const QStringList &select = {});
    QString path() const { return m_path; }
    bool canBack() const { return !m_back.isEmpty(); }
    bool canForward() const { return !m_forward.isEmpty(); }
    void back();
    void forward();
    void up();
    void setViewMode(ViewMode m);
    ViewMode viewMode() const { return m_mode; }
    void zoom(int delta);
    void setActive(bool a);
    bool isTrash() const;
    bool isVirtual() const;   // Recientes or Destacados
    QStringList selectedPaths() const;
    void applySettings();
    void focusView();

    // actions (also used by the main window's shortcuts)
    void openSelection(bool newTab = false);
    void openWithDialog();
    void renameSelected();
    void newFolder();
    void newFile();
    void copySelection();
    void cutSelection();
    void paste();
    void trashSelection();
    void deleteSelection();
    void restoreSelection();
    void selectAll();
    void invertSelection();
    void showFilter(const QString &initial = {});
    void closeFilter();
    bool filterVisible() const;
    void refresh();
    void openTerminal();
    void setSplitMode(bool split);
    void showFolderMenu(const QPoint &globalPos);
    void setOtherPane(Pane *other) { m_other = other; }
    void transferSelection(bool move, const QString &dest = {});   // empty dest: ask
    void copyPath();
    void compressSelection();
    void extractSelection();
    void createLink();

signals:
    void pathChanged(const QString &path);
    void activated(Pane *pane);
    void status(const QString &text);
    void openInNewTab(const QString &path);
    void openInOtherPane(const QString &path);
    void bookmarkRequested(const QString &path);
    void propertiesRequested(const QStringList &paths);
    void historyChanged();

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    QAbstractItemView *view() const;
    QModelIndex sourceIndex(const QModelIndex &proxyIndex) const;
    QStringList pathsFor(const QModelIndexList &proxyRows) const;
    QModelIndexList selectedItems() const;
    void activateIndex(const QModelIndex &idx, bool newTab = false);
    void openFiles(const QStringList &paths);
    void contextMenu(const QPoint &globalPos, bool onItem);
    QMenu *openWithMenu(const QStringList &paths, QWidget *parent);
    QMenu *transferMenu(bool move, QWidget *parent);
    void handleDrop(const QList<QUrl> &urls, const QString &dest);
    void updateStatus();
    void updateTitle();
    void updateOverlay();
    void selectPaths(const QStringList &paths, bool edit = false);
    void setupView(QAbstractItemView *v);
    void applyIconSizes();
    void adjustColumns(QTreeView *t);
    bool recursiveSearch() const;

    QString m_path;
    QStringList m_back, m_forward;
    ViewMode m_mode = ViewMode::Icons;
    FsModel *m_model = nullptr;
    SortProxy *m_proxy = nullptr;
    QItemSelectionModel *m_selection = nullptr;
    QWidget *m_title = nullptr;
    QLabel *m_titleLabel = nullptr;
    QStackedWidget *m_stack = nullptr;
    QListView *m_icons = nullptr, *m_compact = nullptr;
    QTreeView *m_list = nullptr, *m_details = nullptr, *m_tree = nullptr;
    QColumnView *m_columns = nullptr;
    QWidget *m_trashBar = nullptr;
    QWidget *m_filterBar = nullptr;
    QLineEdit *m_filter = nullptr;
    QToolButton *m_recursive = nullptr;
    QLabel *m_overlay = nullptr;
    QStringList m_pendingSelect;
    bool m_pendingEdit = false;
    bool m_skipActivate = false;   // tree: the double click already toggled the folder
    qint64 m_freeBytes = -1;
    QList<int> m_sortKey;   // sort/filter settings last applied to the proxy
    QPointer<Pane> m_other;
};
