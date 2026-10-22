// Virtual locations listed by the panes like folders: "Recientes" (files
// opened lately, from Luna and from the desktop's recently-used.xbel) and
// "Destacados" (items the user starred, kept in Luna's settings).
#pragma once
#include <QIcon>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QStringList>

namespace Places {
inline const QString recent = QStringLiteral("recent:///");
inline const QString starred = QStringLiteral("starred:///");

bool isVirtual(const QString &path);
QString displayName(const QString &path);   // "Recientes", "Papelera", "Carpeta personal", folder name…
QStringList contents(const QString &path);  // the paths a virtual location shows
}

class Starred : public QObject {
    Q_OBJECT
public:
    static Starred &get();
    bool contains(const QString &path) const { return m_paths.contains(path); }
    QStringList paths() const { return m_order; }
    void add(const QStringList &paths);
    void remove(const QStringList &paths);
    // Moves and renames done in Luna keep their star.
    void remap(const QList<QPair<QString, QString>> &moves);

signals:
    void changed();

private:
    Starred();
    void save();
    QStringList m_order;   // most recently starred first
    QSet<QString> m_paths;
};

class Recent {
public:
    static void add(const QStringList &files);   // opened from Luna
    static QStringList files();                  // newest first, existing only
};
