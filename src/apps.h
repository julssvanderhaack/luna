// Desktop entries, MIME defaults (mimeapps.list, as Omarchy sets them) and
// launching through uwsm-app like the rest of Omarchy.
#pragma once
#include <QDialog>
#include <QHash>
#include <QIcon>
#include <QString>
#include <QStringList>

struct AppInfo {
    QString id;        // foo.desktop
    QString name, comment, exec, icon, path;
    QStringList mimes;
    bool terminal = false, hidden = false;
};

class Apps {
public:
    static Apps &get();
    void load();
    const AppInfo *byId(const QString &id) const;
    QList<const AppInfo *> forMime(const QString &mime) const;   // recommended, default first
    QList<const AppInfo *> all() const;
    QString defaultFor(const QString &mime) const;
    void setDefault(const QString &mime, const QString &appId);
    // workspace > 0: the window opens on that Hyprland workspace, without
    // leaving the current one.
    bool launch(const QString &appId, const QStringList &paths, int workspace = 0);
    // Opens each file with its default app; returns files that have none.
    QStringList openDefault(const QStringList &paths, int workspace = 0);
    void runTerminal(const QString &dir);

private:
    void readMimeApps();
    QHash<QString, AppInfo> m_apps;
    QHash<QString, QStringList> m_defaults, m_added, m_removed;
    QHash<QString, QStringList> m_byMime;
    bool m_loaded = false;
};

class QListWidget;
class QLineEdit;
class QCheckBox;

class OpenWithDialog : public QDialog {
    Q_OBJECT
public:
    OpenWithDialog(const QStringList &paths, QWidget *parent = nullptr);
    QString chosen() const { return m_chosen; }
    bool makeDefault() const;

private:
    void fill(const QString &filter);
    QStringList m_paths;
    QString m_mime, m_chosen;
    QListWidget *m_list;
    QLineEdit *m_search;
    QCheckBox *m_default;
};
