#include "icons.h"
#include "fsmodel.h"
#include "settings.h"
#include "theme.h"
#include <QDir>
#include <QMimeDatabase>
#include <QMimeType>
#include <QFile>
#include <QIconEngine>
#include <QFileInfo>
#include <QPainter>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <sys/stat.h>

namespace {

// Rendered pixmaps per (size, scale, mode): views ask for the same few on
// every repaint of every item.
class PixmapCache {
public:
    template <typename Render>
    QPixmap get(const QSize &size, qreal scale, QIcon::Mode mode, Render render)
    {
        const quint64 key = quint64(size.width()) << 40 | quint64(size.height()) << 24 | quint64(qRound(scale * 100)) << 4 | quint64(mode);
        auto it = m_px.constFind(key);
        if (it != m_px.constEnd()) return *it;
        if (m_px.size() > 24) m_px.clear();
        const QPixmap px = render();
        m_px.insert(key, px);
        return px;
    }

private:
    QHash<quint64, QPixmap> m_px;
};

// Paints one Nerd Font glyph, crisp at any size.
class GlyphEngine : public QIconEngine {
public:
    GlyphEngine(QString text, QColor color) : m_text(std::move(text)), m_color(std::move(color)) {}
    void paint(QPainter *p, const QRect &r, QIcon::Mode mode, QIcon::State) override
    {
        QFont f(Theme::get().nerd);
        f.setPixelSize(std::max(6, int(r.height() * 0.82)));
        p->save();
        p->setRenderHint(QPainter::TextAntialiasing);
        p->setFont(f);
        QColor c = m_color;
        if (mode == QIcon::Disabled) c.setAlpha(110);
        p->setPen(c);
        p->drawText(r, Qt::AlignCenter, m_text);
        p->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return m_cache.get(size, 1.0, mode, [&] {
            QPixmap px(size);
            px.fill(Qt::transparent);
            QPainter p(&px);
            paint(&p, QRect(QPoint(), size), mode, state);
            return px;
        });
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        return m_cache.get(size, scale, mode, [&] {
            QPixmap px(size * scale);
            px.setDevicePixelRatio(scale);
            px.fill(Qt::transparent);
            QPainter p(&px);
            paint(&p, QRect(QPoint(), size), mode, state);
            return px;
        });
    }
    QIconEngine *clone() const override { return new GlyphEngine(m_text, m_color); }

private:
    QString m_text;
    QColor m_color;
    PixmapCache m_cache;
};

QString gsettingsIconTheme()
{
    QProcess p;
    p.start("gsettings", {"get", "org.gnome.desktop.interface", "icon-theme"});
    if (p.waitForFinished(1500)) {
        QString t = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        t.remove('\'');
        if (!t.isEmpty()) return t;
    }
    QSettings gtk(QDir::homePath() + "/.config/gtk-3.0/settings.ini", QSettings::IniFormat);
    return gtk.value("Settings/gtk-icon-theme-name", "Adwaita").toString();
}

struct GlyphStyle { char32_t cp; int color; };

// A theme icon recoloured to one flat colour (what GTK does to -symbolic).
class TintEngine : public QIconEngine {
public:
    TintEngine(QIcon base, QColor color) : m_base(std::move(base)), m_color(std::move(color)) {}
    void paint(QPainter *p, const QRect &r, QIcon::Mode mode, QIcon::State state) override
    {
        p->drawPixmap(r, scaledPixmap(r.size(), mode, state, p->device()->devicePixelRatioF()));
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1.0); }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        return m_cache.get(size, scale, mode, [&] {
            QPixmap px = m_base.pixmap(size, scale, QIcon::Normal, state);
            if (px.isNull()) return px;
            QPainter p(&px);
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            QColor c = m_color;
            if (mode == QIcon::Disabled) c.setAlpha(110);
            p.fillRect(QRect(QPoint(), px.size()), c);
            return px;
        });
    }
    QIconEngine *clone() const override { return new TintEngine(m_base, m_color); }

private:
    QIcon m_base;
    QColor m_color;
    PixmapCache m_cache;
};   // color: 0 fg,1 accent,2 magenta,3 cyan,4 yellow,5 red,6 green,7 blue,8 orange,9 dim

} // namespace

Icons &Icons::get()
{
    static Icons i;
    return i;
}

QStringList Icons::installedThemes()
{
    QStringList out;
    for (const QString &base : QIcon::themeSearchPaths()) {
        QDir d(base);
        for (const QString &t : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (out.contains(t) || t == "default" || t == "hicolor") continue;
            if (QFile::exists(d.filePath(t + "/index.theme")) && !QDir(d.filePath(t + "/cursors")).exists()) out << t;
        }
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

void Icons::reload()
{
    m_cache.clear();
    m_gtkTheme = Settings::get().iconTheme.isEmpty() ? gsettingsIconTheme() : Settings::get().iconTheme;
    QIcon::setThemeName(m_gtkTheme);
    QIcon::setFallbackThemeName("hicolor");
    m_special.clear();
    const QString home = QDir::homePath();
    m_special.insert(home, "home");
    auto add = [&](QStandardPaths::StandardLocation l, const char *kind) {
        QString p = QStandardPaths::writableLocation(l);
        if (!p.isEmpty() && p != home) m_special.insert(p, kind);
    };
    add(QStandardPaths::DesktopLocation, "desktop");
    add(QStandardPaths::DocumentsLocation, "documents");
    add(QStandardPaths::DownloadLocation, "downloads");
    add(QStandardPaths::MusicLocation, "music");
    add(QStandardPaths::PicturesLocation, "pictures");
    add(QStandardPaths::MoviesLocation, "videos");
    m_special.insert(home + "/Projects", "projects");
    m_special.insert(home + "/Dropbox", "dropbox");
    m_special.insert(home + "/.local/share/Trash/files", "trash");
}

QIcon Icons::glyph(char32_t cp, const QColor &c)
{
    return QIcon(new GlyphEngine(QString::fromUcs4(&cp, 1), c));
}

QIcon Icons::themed(const QStringList &names)
{
    for (const QString &n : names)
        if (QIcon::hasThemeIcon(n)) return QIcon::fromTheme(n);
    return QIcon::fromTheme(names.last());
}

QString Icons::folderKind(const QString &path) const { return m_special.value(path); }

static QColor paletteColor(int i)
{
    const Theme &t = Theme::get();
    switch (i) {
    case 1: return t.accent;
    case 2: return t.magenta;
    case 3: return t.cyan;
    case 4: return t.yellow;
    case 5: return t.red;
    case 6: return t.green;
    case 7: return t.blue;
    case 8: return t.orange;
    case 9: return t.fgDim;
    default: return t.fg;
    }
}

QIcon Icons::place(const QString &kind)
{
    const QString key = "place:" + kind;
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return *it;
    QIcon icon;
    if (Settings::get().iconStyle == 1 || kind == "bookmark-add") {
        static const QHash<QString, GlyphStyle> g = {
            {"home", {0xF015, 1}}, {"desktop", {0xF108, 1}}, {"documents", {0xF15C, 1}},
            {"downloads", {0xF019, 1}}, {"music", {0xF001, 1}}, {"pictures", {0xF03E, 1}},
            {"videos", {0xF03D, 1}}, {"trash", {0xF1F8, 1}}, {"trash-full", {0xF1F8, 1}},
            {"drive", {0xF0A0, 1}}, {"usb", {0xF287, 1}}, {"root", {0xF0A0, 1}},
            {"bookmark", {0xF07B, 1}}, {"projects", {0xF121, 1}}, {"dropbox", {0xF16B, 1}},
            {"network", {0xF0E8, 1}}, {"recent", {0xF017, 1}}, {"starred", {0xF005, 4}}, {"bookmark-add", {0xF067, 9}},
        };
        GlyphStyle s = g.value(kind, {0xF07B, 1});
        icon = glyph(s.cp, paletteColor(s.color));
    } else {
        static const QHash<QString, QStringList> n = {
            {"home", {"user-home", "folder-home", "go-home"}},
            {"desktop", {"user-desktop", "folder-desktop"}},
            {"documents", {"folder-documents"}},
            {"downloads", {"folder-download", "folder-downloads"}},
            {"music", {"folder-music"}},
            {"pictures", {"folder-pictures", "folder-images"}},
            {"videos", {"folder-videos", "folder-video"}},
            {"trash", {"user-trash", "trash-empty"}},
            {"trash-full", {"user-trash-full", "user-trash"}},
            {"drive", {"drive-harddisk", "drive-harddisk-system"}},
            {"usb", {"drive-removable-media-usb", "drive-removable-media", "media-removable"}},
            {"root", {"drive-harddisk-system", "drive-harddisk"}},
            {"bookmark", {"folder"}},
            {"projects", {"folder-development", "folder-code", "folder-projects", "folder"}},
            {"dropbox", {"folder-dropbox", "dropbox", "folder"}},
            {"network", {"network-workgroup", "folder-remote"}},
            {"recent", {"document-open-recent", "folder-recent"}},
            {"starred", {"starred", "emblem-favorite", "folder"}},
        };
        icon = themed(n.value(kind, {"folder"}));
    }
    m_cache.insert(key, icon);
    return icon;
}

QIcon Icons::forFolder(const QString &path)
{
    const QString kind = folderKind(path);
    if (!kind.isEmpty()) return place(kind);
    return forMime("inode/directory");
}

QIcon Icons::forMime(const QString &mime)
{
    auto it = m_cache.find(mime);
    if (it != m_cache.end()) return *it;
    QIcon icon;
    if (Settings::get().iconStyle == 1) {
        static const QHash<QString, GlyphStyle> exact = {
            {"inode/directory", {0xF07B, 1}}, {"application/pdf", {0xF1C1, 5}},
            {"application/zip", {0xF1C6, 4}}, {"application/x-tar", {0xF1C6, 4}},
            {"application/x-7z-compressed", {0xF1C6, 4}}, {"application/x-rar", {0xF1C6, 4}},
            {"application/vnd.rar", {0xF1C6, 4}}, {"application/gzip", {0xF1C6, 4}},
            {"application/x-compressed-tar", {0xF1C6, 4}}, {"application/x-xz-compressed-tar", {0xF1C6, 4}},
            {"application/x-zstd-compressed-tar", {0xF1C6, 4}}, {"application/x-bzip2", {0xF1C6, 4}},
            {"application/x-shellscript", {0xF120, 6}}, {"application/x-executable", {0xF013, 6}},
            {"application/x-sharedlib", {0xF013, 9}}, {"application/vnd.appimage", {0xF013, 6}},
            {"application/x-iso9660-image", {0xF0A0, 4}}, {"application/x-cd-image", {0xF0A0, 4}},
            {"application/json", {0xE60B, 4}}, {"application/javascript", {0xE74E, 4}},
            {"text/javascript", {0xE74E, 4}}, {"text/x-python", {0xE606, 7}}, {"text/x-python3", {0xE606, 7}},
            {"text/x-csrc", {0xE61E, 7}}, {"text/x-chdr", {0xE61E, 2}}, {"text/x-c++src", {0xE61D, 7}},
            {"text/x-c++hdr", {0xE61D, 2}}, {"text/rust", {0xE7A8, 8}}, {"text/x-rust", {0xE7A8, 8}},
            {"text/html", {0xE736, 8}}, {"text/css", {0xE749, 7}}, {"text/markdown", {0xE73E, 0}},
            {"text/x-lua", {0xE620, 7}}, {"text/x-go", {0xE626, 3}}, {"text/x-java", {0xE738, 5}},
            {"application/x-desktop", {0xF135, 1}}, {"text/x-makefile", {0xF085, 9}},
            {"application/vnd.oasis.opendocument.text", {0xF1C2, 7}},
            {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", {0xF1C2, 7}},
            {"application/msword", {0xF1C2, 7}},
            {"application/vnd.oasis.opendocument.spreadsheet", {0xF1C3, 6}},
            {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", {0xF1C3, 6}},
            {"application/vnd.ms-excel", {0xF1C3, 6}}, {"text/csv", {0xF1C3, 6}},
            {"application/vnd.oasis.opendocument.presentation", {0xF1C4, 8}},
            {"application/vnd.openxmlformats-officedocument.presentationml.presentation", {0xF1C4, 8}},
            {"application/vnd.ms-powerpoint", {0xF1C4, 8}}, {"inode/symlink", {0xF0C1, 5}},
            {"application/x-bittorrent", {0xF019, 3}}, {"application/vnd.flatpak.ref", {0xF187, 1}},
        };
        GlyphStyle s;
        if (exact.contains(mime)) s = exact.value(mime);
        else if (mime.startsWith("image/")) s = {0xF1C5, 2};
        else if (mime.startsWith("video/")) s = {0xF1C8, 8};
        else if (mime.startsWith("audio/")) s = {0xF1C7, 3};
        else if (mime.startsWith("font/") || mime.contains("font")) s = {0xF031, 0};
        else if (mime.startsWith("text/") || mime.contains("xml") || mime.contains("yaml") || mime.contains("toml"))
            s = {0xF0F6, 0};
        else s = {0xF016, 9};
        icon = glyph(s.cp, paletteColor(s.color));
    } else {
        QStringList names;
        QString m = mime;
        names << m.replace('/', '-');
        if (mime == "inode/directory") names = {"folder"};
        static QMimeDatabase *db = new QMimeDatabase;
        QMimeType t = db->mimeTypeForName(mime);
        if (t.isValid()) names << t.iconName() << t.genericIconName();
        names << "text-x-generic" << "unknown";
        icon = themed(names);
    }
    m_cache.insert(mime, icon);
    return icon;
}

QIcon Icons::forEntry(const FileEntry &e, const QString &path)
{
    if (e.isDir) return forFolder(path);
    QString mime = e.mime;
    if (Settings::get().iconStyle == 1 && (e.mode & S_IXUSR) && mime.startsWith("text/")) mime = "application/x-shellscript";
    return forMime(mime);
}

QIcon Icons::ui(const QString &name)
{
    const QString key = "ui:" + name;
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return *it;
    static const QHash<QString, char32_t> g = {
        {"back", 0xF053}, {"forward", 0xF054}, {"up", 0xF077}, {"search", 0xF002}, {"menu", 0xF0C9},
        {"icons", 0xF00A}, {"compact", 0xF0CA}, {"list", 0xF03A}, {"details", 0xF0CE}, {"tree", 0xF1BB},
        {"columns", 0xF0DB}, {"split", 0xF2D2}, {"sidebar", 0xF2D0}, {"sort", 0xF0DC}, {"refresh", 0xF021},
        {"close", 0xF00D}, {"eject", 0xF052}, {"newfolder", 0xF07B}, {"newtab", 0xF067}, {"terminal", 0xF120},
        {"settings", 0xF013}, {"edit", 0xF040}, {"copy", 0xF0C5}, {"cut", 0xF0C4}, {"paste", 0xF0EA},
        {"trash", 0xF1F8}, {"info", 0xF05A}, {"open", 0xF07C}, {"lock", 0xF023}, {"star", 0xF005},
        {"restore", 0xF0E2}, {"network", 0xF0E8}, {"crumbs", 0xF0C1}, {"chevron", 0xF078}, {"hidden", 0xF070}, {"zoomin", 0xF00E}, {"zoomout", 0xF010}, {"cancel", 0xF05E},
    };
    QIcon icon = glyph(g.value(name, 0xF128), Theme::get().fg);
    m_cache.insert(key, icon);
    return icon;
}

QString Icons::kindForPath(const QString &path)
{
    const QString home = QDir::homePath();
    if (path == home) return "home";
    if (path == "/") return "drive";
    struct X { QStandardPaths::StandardLocation l; const char *kind; };
    for (X x : {X{QStandardPaths::DownloadLocation, "downloads"}, X{QStandardPaths::DocumentsLocation, "documents"},
                X{QStandardPaths::MusicLocation, "music"}, X{QStandardPaths::PicturesLocation, "pictures"},
                X{QStandardPaths::MoviesLocation, "videos"}, X{QStandardPaths::DesktopLocation, "desktop"}}) {
        const QString p = QStandardPaths::writableLocation(x.l);
        if (!p.isEmpty() && p != home && p == path) return x.kind;
    }
    // Same name rules as the bar's Places widget (scripts/list.py)
    static const QHash<QString, QString> names = {
        {"dropbox", "dropbox"}, {"nextcloud", "cloud"}, {"onedrive", "cloud"}, {"google drive", "cloud"}, {"mega", "cloud"}, {"drive", "cloud"},
        {"proyectos", "code"}, {"projects", "code"}, {"code", "code"}, {"src", "code"}, {"dev", "code"}, {"git", "code"}, {"repos", "code"},
        {"juegos", "games"}, {"games", "games"}, {"roms", "games"}, {"libros", "books"}, {"books", "books"},
        {"trabajo", "work"}, {"work", "work"}, {"descargas", "downloads"}, {"downloads", "downloads"},
        {"documentos", "documents"}, {"documents", "documents"}, {QString::fromUtf8("música"), "music"}, {"musica", "music"}, {"music", "music"},
        {QString::fromUtf8("imágenes"), "pictures"}, {"imagenes", "pictures"}, {"pictures", "pictures"}, {"fotos", "pictures"}, {"photos", "pictures"},
        {QString::fromUtf8("vídeos"), "videos"}, {"videos", "videos"}, {"escritorio", "desktop"}, {"desktop", "desktop"},
        {"plantillas", "templates"}, {"templates", "templates"}, {QString::fromUtf8("público"), "public"}, {"publico", "public"}, {"public", "public"},
    };
    QString base = QFileInfo(path).fileName().toLower();
    return names.value(base, "folder");
}

QIcon Icons::sidebar(const QString &kind)
{
    const QString key = "side:" + kind;
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return *it;
    static const QHash<QString, QPair<QStringList, char32_t>> map = {
        {"home", {{"user-home-symbolic"}, 0xF02DC}},
        {"downloads", {{"folder-download-symbolic"}, 0xF01DA}},
        {"documents", {{"folder-documents-symbolic"}, 0xF0219}},
        {"music", {{"folder-music-symbolic"}, 0xF075A}},
        {"pictures", {{"folder-pictures-symbolic"}, 0xF02E9}},
        {"videos", {{"folder-videos-symbolic"}, 0xF0567}},
        {"desktop", {{"user-desktop-symbolic"}, 0xF01C4}},
        {"templates", {{"folder-templates-symbolic"}, 0xF0224}},
        {"public", {{"folder-publicshare-symbolic"}, 0xF0496}},
        {"dropbox", {{"folder-dropbox-symbolic", "dropbox-symbolic"}, 0xF01E3}},
        {"cloud", {{"folder-cloud-symbolic", "folder-remote-symbolic"}, 0xF015F}},
        {"code", {{"folder-code-symbolic", "applications-development-symbolic"}, 0xF0169}},
        {"games", {{"folder-games-symbolic", "applications-games-symbolic"}, 0xF02B4}},
        {"books", {{"folder-books-symbolic", "accessories-dictionary-symbolic"}, 0xF00BA}},
        {"work", {{"folder-work-symbolic"}, 0xF00D6}},
        {"folder", {{"folder-symbolic"}, 0xF024B}},
        {"trash", {{"user-trash-symbolic"}, 0xF0A7A}},
        {"trash-full", {{"user-trash-full-symbolic", "user-trash-symbolic"}, 0xF0A7A}},
        {"drive", {{"drive-harddisk-symbolic"}, 0xF02CA}},
        {"usb", {{"drive-removable-media-symbolic", "media-removable-symbolic"}, 0xF0553}},
        {"network", {{"network-server-symbolic", "folder-remote-symbolic"}, 0xF048D}},
        {"add", {{"list-add-symbolic"}, 0xF0415}},
        {"recent", {{"document-open-recent-symbolic", "history-symbolic"}, 0xF02DA}},
        {"starred", {{"starred-symbolic", "non-starred-symbolic"}, 0xF04CE}},
    };
    auto m = map.value(kind, map.value("folder"));
    const QColor c = Theme::get().fg;
    QIcon icon;
    for (const QString &n : m.first)
        if (QIcon::hasThemeIcon(n)) {
            icon = QIcon(new TintEngine(QIcon::fromTheme(n), c));
            break;
        }
    if (icon.isNull()) icon = glyph(m.second, c);
    m_cache.insert(key, icon);
    return icon;
}

QIcon Icons::app(const QString &iconName)
{
    if (iconName.isEmpty()) return themed({"application-x-executable"});
    if (iconName.startsWith('/')) return QIcon(iconName);
    return themed({iconName, "application-x-executable"});
}
