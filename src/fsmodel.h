// Directory model fed by a background lister (readdir + fstatat). Lazily
// loads subfolders for the tree and column views, watches loaded folders
// and merges changes in place so views keep selection and scroll.
#pragma once
#include <QAbstractItemModel>
#include <QFont>
#include <QCollator>
#include <QFileSystemWatcher>
#include <QHash>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

struct FileEntry {
    QString name;
    qint64 size = 0;        // bytes; for folders the number of items (-1 unknown)
    qint64 mtime = 0;       // seconds since epoch
    quint32 mode = 0;       // st_mode (of the target for symlinks)
    quint32 uid = 0, gid = 0;
    bool isDir = false, isLink = false, broken = false, hidden = false;
    QString mime;
    QString linkTarget;
};

class FsModel : public QAbstractItemModel {
    Q_OBJECT
public:
    enum Column { Name, Size, Type, Modified, Permissions, Owner, ColumnCount };
    enum Role { PathRole = Qt::UserRole + 1, IsDirRole, MimeRole, SortRole, MtimeRole, HiddenRole, SizeRole };

    explicit FsModel(QObject *parent = nullptr);
    ~FsModel() override;

    void setRoot(const QString &path);
    QString root() const { return m_rootPath; }
    void refresh();
    bool isLoading() const { return m_loading; }
    QString error() const { return m_error; }

    // Recursive name search below the root; results stream in.
    void startSearch(const QString &pattern);
    void stopSearch();
    bool searching() const { return m_searchMode; }
    // A flat list of arbitrary paths (Recientes, Destacados) under a
    // virtual root; shown like search results.
    void setPaths(const QString &root, const QStringList &paths);

    QString filePath(const QModelIndex &idx) const;
    const FileEntry *entry(const QModelIndex &idx) const;
    QModelIndex indexForPath(const QString &path) const;
    // Precomputed natural-order key of the name (what the proxy sorts by).
    const QCollatorSortKey &nameKey(const QModelIndex &idx) const;
    static QCollator &collator();
    int fileCount() const;
    qint64 totalSize() const;

    // QAbstractItemModel
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    bool hasChildren(const QModelIndex &parent = {}) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;
    QVariant data(const QModelIndex &idx, int role) const override;
    bool setData(const QModelIndex &idx, const QVariant &value, int role) override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &idx) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) override;
    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction; }
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction; }

    // helpers shared with other widgets
    static QString formatSize(qint64 bytes);
    static QString formatDate(qint64 secs);
    static QString permString(quint32 mode, bool isDir, bool isLink);
    static QString userName(quint32 uid);
    static QString groupName(quint32 gid);
    static QString mimeComment(const QString &mime);
    static bool listDirectory(const QString &path, std::vector<FileEntry> &out, QString *err, QStringList *deferMime = nullptr);
    static FileEntry statPath(const QString &path, bool *ok = nullptr);
    static void shutdown();   // drop queued listings and wait for running ones

signals:
    void loadingChanged(bool loading);
    void rootLoaded();
    void searchProgress(int found, bool finished);
    void errorOccurred(const QString &msg);
    void dropRequested(const QList<QUrl> &urls, const QString &destDir, Qt::DropAction action);

private:
    struct Node {
        FileEntry e;
        QString fullPath;   // set for the root and for search results
        Node *parent = nullptr;
        std::vector<std::unique_ptr<Node>> kids;
        bool loaded = false, loading = false;
        mutable int rowHint = 0;                          // position in parent->kids
        mutable std::optional<QCollatorSortKey> key;      // built on first sort
        mutable QString dateText;                         // formatted mtime
        mutable quint32 dateGen = 0;                      // formatGeneration() it was made in
        int row() const;
    };
    void renumber(Node *n, int from = 0);
    Node *nodeOf(const QModelIndex &idx) const;
    QModelIndex indexOf(Node *n, int column = 0) const;
    QString pathOf(const Node *n) const;
    void load(Node *n);
    void deliver(const QString &path, quint64 gen, std::vector<FileEntry> entries, const QString &err);
    void deliverCounts(const QString &path, quint64 gen, QHash<QString, qint64> counts);
    void deliverMimes(const QString &path, quint64 gen, QHash<QString, QString> mimes);
    void merge(Node *n, std::vector<FileEntry> &entries);
    void indexNode(Node *n);
    void unindexNode(Node *n);
    void watch(Node *n);
    void onDirChanged(const QString &path);
    void thumbReady(const QString &path);
    void insertFlat(std::vector<std::pair<QString, FileEntry>> &items);
    static void searchWorker(QPointer<FsModel> self, std::shared_ptr<std::atomic<bool>> cancel, QByteArray root, QString pattern,
                             quint64 gen, bool hidden);

    std::unique_ptr<Node> m_root;
    QString m_rootPath;
    quint64 m_gen = 0;           // bumps on setRoot so stale listings are dropped
    QHash<QString, Node *> m_byPath;
    QFileSystemWatcher m_watcher;
    QSet<QString> m_watched;
    QSet<QString> m_dirty;
    QTimer m_dirtyTimer;
    bool m_loading = false;
    QString m_error;
    bool m_searchMode = false;
    bool m_virtual = false;      // root is not a folder: nothing can be dropped on it
    std::shared_ptr<std::atomic<bool>> m_searchCancel;
    QFont m_monoFont, m_italicFont;
};

// Sorting (folders first, natural order) and filtering (hidden, quick filter).
class SortProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit SortProxy(QObject *parent = nullptr);
    void setQuickFilter(const QString &f);
    QString quickFilter() const { return m_filter; }
    void reapply();

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
    bool lessThan(const QModelIndex &a, const QModelIndex &b) const override;

private:
    QString m_filter;
};
