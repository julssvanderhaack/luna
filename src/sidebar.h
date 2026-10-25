// Quick-access panel: places, GTK bookmarks (shared with Nautilus) and devices.
#pragma once
#include <QTreeWidget>

class Sidebar : public QTreeWidget {
    Q_OBJECT
public:
    explicit Sidebar(QWidget *parent = nullptr);
    void reload();
    void applySizes();   // text, icon size and spacing from the settings
    void setCurrentPath(const QString &path);
    void addBookmark(const QString &path, const QString &label = {});
    static QString bookmarksFile();

signals:
    void open(const QString &path);
    void openInNewTab(const QString &path);
    void openInOtherPane(const QString &path);
    void dropOnPlace(const QList<QUrl> &urls, const QString &dest);
    void connectRequested(const QString &uri);   // empty: open the dialog

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    struct Bookmark { QString path, label; };
    QList<Bookmark> readBookmarks() const;
    void writeBookmarks(const QList<Bookmark> &b) const;
    void loadDevices();
    void fillDiscovered();   // SFTP servers on the network (the phone), above "Conectar…"
    QTreeWidgetItem *section(const QString &title);
    QTreeWidgetItem *m_places = nullptr, *m_bookmarks = nullptr, *m_devices = nullptr, *m_network = nullptr;
    QString m_current;
    QList<QTreeWidgetItem *> m_found;
    quint64 m_gen = 0;   // bumps on reload, so late lsblk results are dropped
};
