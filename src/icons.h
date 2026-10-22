// Icons for files, places and the UI: either the GTK icon theme or
// Nerd Font glyphs tinted with the Omarchy palette.
#pragma once
#include <QHash>
#include <QIcon>
#include <QString>

struct FileEntry;

class Icons {
public:
    static Icons &get();
    void reload();                                  // after theme/settings changes
    QIcon forEntry(const FileEntry &e, const QString &path);
    QIcon forMime(const QString &mime);
    QIcon forFolder(const QString &path);
    QIcon place(const QString &kind);               // home, trash, drive, usb, bookmark...
    QIcon ui(const QString &name);                  // toolbar glyphs, always Omarchy style
    QIcon app(const QString &iconName);
    // Sidebar icons like the bar's Places widget: the GTK theme's -symbolic
    // icon tinted with the text colour, or a Material glyph if missing.
    QIcon sidebar(const QString &kind);
    static QString kindForPath(const QString &path);
    QString gtkTheme() const { return m_gtkTheme; }
    static QStringList installedThemes();

private:
    QIcon glyph(char32_t cp, const QColor &c);
    QIcon themed(const QStringList &names);
    QString folderKind(const QString &path) const;
    QHash<QString, QIcon> m_cache;
    QString m_gtkTheme;
    QHash<QString, QString> m_special;   // path -> kind
};
