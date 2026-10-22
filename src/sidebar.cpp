#include "sidebar.h"
#include "fileops.h"
#include "icons.h"
#include "remote.h"
#include "places.h"
#include "settings.h"
#include "theme.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyledItemDelegate>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

namespace {
enum { PathRole = Qt::UserRole + 1, KindRole, DeviceRole, UsageRole, MountedRole, UriRole };
enum Kind { Place, BookmarkItem, Device, Header };

// Draws a thin usage bar under devices and the eject glyph.
class Delegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &i) const override
    {
        QSize s = QStyledItemDelegate::sizeHint(o, i);
        if (i.data(KindRole).toInt() == Header) return QSize(s.width(), s.height() + 4 + 3 * Settings::get().sidebarSpacing);
        static const int pad[] = {4, 10, 16};   // compacto, normal, amplio
        const int row = Settings::get().sidebarIconSize + pad[std::clamp(Settings::get().sidebarSpacing, 0, 2)];
        return QSize(s.width(), std::max(s.height(), row) + (i.data(UsageRole).isValid() ? 6 : 0));
    }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override
    {
        if (i.data(KindRole).toInt() == Header) {
            p->save();
            QFont f = o.font;
            f.setBold(true);
            f.setPointSizeF(f.pointSizeF() * 0.85);
            p->setFont(f);
            p->setPen(Theme::get().fgDim);
            p->drawText(o.rect.adjusted(10, 2 + 2 * Settings::get().sidebarSpacing, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, i.data().toString().toUpper());
            p->restore();
            return;
        }
        QStyleOptionViewItem opt = o;
        const bool usage = i.data(UsageRole).isValid();
        if (usage) opt.rect.setBottom(opt.rect.bottom() - 6);
        QStyledItemDelegate::paint(p, opt, i);
        if (usage) {
            double f = i.data(UsageRole).toDouble();
            const int indent = Settings::get().sidebarIconSize + 16;   // under the text, not the icon
            QRect bar(o.rect.left() + indent, o.rect.bottom() - 7, o.rect.width() - indent - 36, 3);
            p->save();
            p->setPen(Qt::NoPen);
            p->setBrush(Theme::get().muted);
            p->drawRoundedRect(bar, 1.5, 1.5);
            p->setBrush(f > 0.9 ? Theme::get().red : Theme::get().accent);
            p->drawRoundedRect(QRect(bar.left(), bar.top(), int(bar.width() * f), 3), 1.5, 1.5);
            p->restore();
        }
        if (i.data(MountedRole).toBool()) {
            const int e = std::min(18, Settings::get().sidebarIconSize + 2);
            QRect r(o.rect.right() - 10 - e, o.rect.top() + (o.rect.height() - (usage ? 6 : 0) - e) / 2, e, e);
            Icons::get().ui("eject").paint(p, r);
        }
    }
};
} // namespace

QString Sidebar::bookmarksFile() { return QDir::homePath() + "/.config/gtk-3.0/bookmarks"; }

Sidebar::Sidebar(QWidget *parent) : QTreeWidget(parent)
{
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setIndentation(0);
    setItemDelegate(new Delegate(this));
    applySizes();
    setDragDropMode(QAbstractItemView::DragDrop);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setMinimumWidth(170);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::NoFocus);
    setStyleSheet(QString("QTreeWidget { background: %1; }").arg(Theme::get().bgDark.name()));
    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *it, int) {
        const QString path = it->data(0, PathRole).toString();
        const QString dev = it->data(0, DeviceRole).toString();
        const QString uri = it->data(0, UriRole).toString();
        if (path.isEmpty() && !uri.isEmpty()) {
            emit connectRequested(uri == "connect:" ? QString() : uri);
            return;
        }
        if (!dev.isEmpty() && path.isEmpty()) {
            // Mount, then open.
            auto *p = new QProcess(this);
            connect(p, &QProcess::finished, this, [this, p, dev] {
                p->deleteLater();
                QProcess q;
                q.start("findmnt", {"-n", "-o", "TARGET", "--source", dev});
                q.waitForFinished(2000);
                const QString mp = QString::fromUtf8(q.readAllStandardOutput()).trimmed().section('\n', 0, 0);
                reload();
                if (!mp.isEmpty()) emit open(mp);
            });
            p->start("udisksctl", {"mount", "-b", dev, "--no-user-interaction"});
            return;
        }
        if (!path.isEmpty()) emit open(path);
    });
    // Devices come and go: watch the usual mount roots.
    auto *w = new QFileSystemWatcher(this);
    for (const QString &d : {QString("/run/media/") + qEnvironmentVariable("USER"), QString("/media"), QString("/dev/disk/by-uuid")})
        if (QFileInfo::exists(d)) w->addPath(d);
    if (QFileInfo::exists(bookmarksFile())) w->addPath(bookmarksFile());
    auto *debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(400);
    connect(debounce, &QTimer::timeout, this, &Sidebar::reload);
    connect(w, &QFileSystemWatcher::directoryChanged, debounce, qOverload<>(&QTimer::start));
    connect(w, &QFileSystemWatcher::fileChanged, this, [w, debounce] {
        if (!w->files().contains(bookmarksFile())) w->addPath(bookmarksFile());
        debounce->start();
    });
    reload();
}

QTreeWidgetItem *Sidebar::section(const QString &title)
{
    auto *h = new QTreeWidgetItem(this, {title});
    h->setData(0, KindRole, Header);
    h->setFlags(Qt::ItemIsDropEnabled | Qt::ItemIsEnabled);
    return h;
}

QList<Sidebar::Bookmark> Sidebar::readBookmarks() const
{
    QList<Bookmark> out;
    QFile f(bookmarksFile());
    if (!f.open(QIODevice::ReadOnly)) return out;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (!line.contains("://")) continue;
        int sp = line.indexOf(' ');
        QString url = sp < 0 ? line : line.left(sp);
        QString label = sp < 0 ? QString() : line.mid(sp + 1);
        out << Bookmark{line.startsWith("file://") ? QUrl(url).toLocalFile() : url, label};
    }
    return out;
}

void Sidebar::writeBookmarks(const QList<Bookmark> &b) const
{
    QDir().mkpath(QFileInfo(bookmarksFile()).path());
    QFile f(bookmarksFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    QTextStream out(&f);
    for (const Bookmark &x : b) {
        out << (x.path.contains("://") ? x.path.toUtf8() : QUrl::fromLocalFile(x.path).toEncoded());
        if (!x.label.isEmpty()) out << ' ' << x.label;
        out << '\n';
    }
}

void Sidebar::addBookmark(const QString &path, const QString &label)
{
    auto b = readBookmarks();
    for (auto &x : b)
        if (x.path == path) return;
    b << Bookmark{path, label};
    writeBookmarks(b);
    reload();
}

void Sidebar::applySizes()
{
    const Settings &s = Settings::get();
    setIconSize(QSize(s.sidebarIconSize, s.sidebarIconSize));
    QFont f = QApplication::font(this);   // the class font the platform theme gives tree views
    f.setPointSizeF(s.sidebarFontPt > 0 ? s.sidebarFontPt : std::max(7.0, f.pointSizeF() - 1));
    setFont(f);
    scheduleDelayedItemsLayout();   // rows change height with the sizes
}

void Sidebar::reload()
{
    const Settings &s = Settings::get();
    clear();
    ++m_gen;
    m_places = m_bookmarks = m_devices = nullptr;
    auto item = [&](QTreeWidgetItem *parent, const QString &name, const QString &path, const QIcon &icon, int kind) {
        auto *it = new QTreeWidgetItem(parent, {name});
        it->setIcon(0, icon);
        it->setData(0, PathRole, path);
        it->setData(0, KindRole, kind);
        it->setToolTip(0, path);
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled | (kind == BookmarkItem ? Qt::ItemIsDragEnabled : Qt::NoItemFlags));
        return it;
    };
    if (s.showPlaces) {
        m_places = section(QString::fromUtf8("Lugares"));
        const QString home = QDir::homePath();
        item(m_places, QString::fromUtf8("Carpeta personal"), home, Icons::get().sidebar("home"), Place);
        struct P { QStandardPaths::StandardLocation l; const char *name, *kind; };
        for (P p : {P{QStandardPaths::DesktopLocation, "Escritorio", "desktop"}, P{QStandardPaths::DocumentsLocation, "Documentos", "documents"},
                    P{QStandardPaths::DownloadLocation, "Descargas", "downloads"}, P{QStandardPaths::MusicLocation, "Música", "music"},
                    P{QStandardPaths::PicturesLocation, "Imágenes", "pictures"}, P{QStandardPaths::MoviesLocation, "Vídeos", "videos"}}) {
            QString path = QStandardPaths::writableLocation(p.l);
            if (path.isEmpty() || path == home || !QFileInfo(path).isDir()) continue;
            item(m_places, QString::fromUtf8(p.name), path, Icons::get().sidebar(p.kind), Place);
        }
        item(m_places, QString::fromUtf8("Recientes"), Places::recent, Icons::get().sidebar("recent"), Place);
        item(m_places, QString::fromUtf8("Destacados"), Places::starred, Icons::get().sidebar("starred"), Place);
        const bool full = !QDir(FileOps::trashFiles()).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        item(m_places, QString::fromUtf8("Papelera"), FileOps::trashFiles(), Icons::get().sidebar(full ? "trash-full" : "trash"), Place);
    }
    if (s.showBookmarks) {
        m_bookmarks = section(QString::fromUtf8("Marcadores"));
        QSet<QString> places;
        if (m_places)
            for (int k = 0; k < m_places->childCount(); ++k) places.insert(m_places->child(k)->data(0, PathRole).toString());
        for (const Bookmark &b : readBookmarks()) {
            if (places.contains(b.path)) continue;
            if (b.path.contains("://")) {   // remote bookmark (sftp://, smb://…)
                QUrl u(b.path);
                auto *it = item(m_bookmarks, b.label.isEmpty() ? u.host() + u.path() : b.label, QString(), Icons::get().sidebar("network"), BookmarkItem);
                it->setData(0, UriRole, b.path);
                it->setToolTip(0, b.path);
                continue;
            }
            QString label = b.label.isEmpty() ? QFileInfo(b.path).fileName() : b.label;
            auto *it = item(m_bookmarks, label, b.path, Icons::get().sidebar(Icons::kindForPath(b.path)), BookmarkItem);
            if (!QFileInfo(b.path).exists()) it->setForeground(0, Theme::get().fgDim);
        }
    }
    if (s.showDevices) {
        m_devices = section(QString::fromUtf8("Dispositivos"));
        loadDevices();
    }
    m_network = section(QString::fromUtf8("Red"));
    for (const Remote::Mount &mt : Remote::get().mounts()) {
        auto *it = item(m_network, mt.label, mt.localPath, Icons::get().sidebar("network"), Device);
        it->setData(0, UriRole, mt.uri);
        it->setData(0, MountedRole, true);
        it->setToolTip(0, mt.uri);
    }
    auto *conn = item(m_network, QString::fromUtf8("Conectar a un servidor…"), QString(), Icons::get().sidebar("add"), Place);
    conn->setData(0, UriRole, QString("connect:"));
    expandAll();
    setCurrentPath(m_current);
}

void Sidebar::loadDevices()
{
    auto addDev = [this](const QString &name, const QString &path, const QString &dev, bool removable, double usage, const QString &tip) {
        auto *it = new QTreeWidgetItem(m_devices, {name});
        it->setIcon(0, Icons::get().sidebar(removable ? "usb" : "drive"));
        it->setData(0, PathRole, path);
        it->setData(0, KindRole, Device);
        if (!dev.isEmpty()) it->setData(0, DeviceRole, dev);
        it->setData(0, MountedRole, !path.isEmpty() && removable);
        if (usage >= 0) it->setData(0, UsageRole, usage);
        it->setToolTip(0, tip);
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | (path.isEmpty() ? Qt::NoItemFlags : Qt::ItemIsDropEnabled));
    };
    QStorageInfo root = QStorageInfo::root();
    auto usageOf = [](const QStorageInfo &si) { return si.bytesTotal() > 0 ? 1.0 - double(si.bytesAvailable()) / si.bytesTotal() : -1.0; };
    auto freeText = [](const QStorageInfo &si) {
        auto gb = [](qint64 b) { return QString::number(b / 1e9, 'f', 1) + " GB"; };
        return QString::fromUtf8("%1 libres de %2").arg(gb(si.bytesAvailable()), gb(si.bytesTotal()));
    };
    addDev(QString::fromUtf8("Sistema de archivos"), "/", {}, false, usageOf(root), freeText(root));
    QSet<QString> mountedDevs;
    for (const QStorageInfo &si : QStorageInfo::mountedVolumes()) {
        const QString mp = si.rootPath();
        if (!(mp.startsWith("/run/media/") || mp.startsWith("/media/") || mp.startsWith("/mnt/"))) continue;
        if (!si.isValid() || !si.isReady()) continue;
        const QString dev = QString::fromLocal8Bit(si.device());
        mountedDevs.insert(dev);
        QString name = si.displayName();
        if (name.isEmpty() || name == mp) name = QFileInfo(mp).fileName();
        addDev(name, mp, dev, true, usageOf(si), freeText(si) + "\n" + dev);
    }
    // Plugged-in but unmounted partitions. lsblk runs in the background so
    // building the sidebar (startup, every mount) never waits for it.
    auto *p = new QProcess(this);
    const quint64 gen = m_gen;
    connect(p, &QProcess::errorOccurred, p, &QObject::deleteLater);
    connect(p, &QProcess::finished, this, [this, p, gen, mountedDevs, addDev] {
        p->deleteLater();
        if (gen != m_gen || !m_devices) return;   // rebuilt meanwhile
        std::function<void(const QJsonArray &)> walk = [&](const QJsonArray &arr) {
            for (const auto &v : arr) {
                QJsonObject o = v.toObject();
                walk(o["children"].toArray());
                const bool hot = o["hotplug"].toBool() || o["rm"].toBool();
                const QString dev = o["path"].toString();
                if (!hot || o["fstype"].toString().isEmpty() || !o["mountpoint"].isNull() || mountedDevs.contains(dev)) continue;
                if (o["type"].toString() != "part" && o["type"].toString() != "disk") continue;
                QString label = o["label"].toString();
                double gb = o["size"].toDouble() / 1e9;
                if (label.isEmpty()) label = QString::fromUtf8("Volumen de %1 GB").arg(gb, 0, 'f', 1);
                addDev(label, {}, dev, true, -1, QString::fromUtf8("%1 · sin montar (clic para montar)").arg(dev));
            }
        };
        walk(QJsonDocument::fromJson(p->readAllStandardOutput()).object()["blockdevices"].toArray());
    });
    QTimer::singleShot(3000, p, [p] { p->kill(); });
    p->start("lsblk", {"-J", "-b", "-o", "PATH,LABEL,SIZE,MOUNTPOINT,FSTYPE,HOTPLUG,RM,TYPE"});
}

void Sidebar::setCurrentPath(const QString &path)
{
    m_current = path;
    QTreeWidgetItem *best = nullptr;
    for (QTreeWidgetItemIterator it(this); *it; ++it)
        if ((*it)->data(0, PathRole).toString() == path) {
            best = *it;
            break;
        }
    blockSignals(true);
    clearSelection();
    if (best) best->setSelected(true);
    blockSignals(false);
}

void Sidebar::mouseReleaseEvent(QMouseEvent *e)
{
    QTreeWidgetItem *it = itemAt(e->pos());
    if (it && it->data(0, MountedRole).toBool() && e->pos().x() > viewport()->width() - 34 && !it->data(0, UriRole).toString().isEmpty()) {
        Remote::get().unmount(it->data(0, UriRole).toString(), this);
        emit open(QDir::homePath());
        return;
    }
    if (it && it->data(0, MountedRole).toBool() && e->pos().x() > viewport()->width() - 34) {
        const QString dev = it->data(0, DeviceRole).toString();
        auto *p = new QProcess(this);
        connect(p, &QProcess::finished, this, [this, p, dev](int code) {
            p->deleteLater();
            if (code == 0) {
                QProcess::startDetached("udisksctl", {"power-off", "-b", dev, "--no-user-interaction"});
                emit open(QDir::homePath());
            }
            reload();
        });
        p->start("udisksctl", {"unmount", "-b", dev, "--no-user-interaction"});
        return;
    }
    if (e->button() == Qt::MiddleButton && it && !it->data(0, PathRole).toString().isEmpty()) {
        emit openInNewTab(it->data(0, PathRole).toString());
        return;
    }
    QTreeWidget::mouseReleaseEvent(e);
}

void Sidebar::contextMenuEvent(QContextMenuEvent *e)
{
    QTreeWidgetItem *it = itemAt(e->pos());
    QMenu m(this);
    if (it && it->data(0, KindRole).toInt() != Header) {
        const QString path = it->data(0, PathRole).toString();
        if (!path.isEmpty()) {
            m.addAction(Icons::get().ui("open"), "Abrir", this, [this, path] { emit open(path); });
            m.addAction(Icons::get().ui("newtab"), QString::fromUtf8("Abrir en pestaña nueva"), this, [this, path] { emit openInNewTab(path); });
            m.addAction(Icons::get().ui("split"), "Abrir en el otro panel", this, [this, path] { emit openInOtherPane(path); });
        }
        if (it->data(0, KindRole).toInt() == BookmarkItem) {
            const QString path = it->data(0, UriRole).toString().isEmpty() ? it->data(0, PathRole).toString() : it->data(0, UriRole).toString();
            m.addSeparator();
            m.addAction(Icons::get().ui("edit"), "Renombrar marcador…", this, [this, it, path] {
                bool ok;
                QString t = QInputDialog::getText(this, "Renombrar marcador", "Nombre:", QLineEdit::Normal, it->text(0), &ok);
                if (!ok) return;
                auto b = readBookmarks();
                for (auto &x : b)
                    if (x.path == path) x.label = t.trimmed() == QFileInfo(path).fileName() ? QString() : t.trimmed();
                writeBookmarks(b);
                reload();
            });
            m.addAction(Icons::get().ui("close"), "Quitar marcador", this, [this, path] {
                auto b = readBookmarks();
                b.erase(std::remove_if(b.begin(), b.end(), [&](const Bookmark &x) { return x.path == path; }), b.end());
                writeBookmarks(b);
                reload();
            });
        }
        if (it->data(0, MountedRole).toBool() && !it->data(0, UriRole).toString().isEmpty()) {
            const QString uri = it->data(0, UriRole).toString();
            m.addSeparator();
            m.addAction(Icons::get().ui("eject"), "Desconectar", this, [this, uri] { Remote::get().unmount(uri, this); });
            m.addAction(Icons::get().ui("star"), QString::fromUtf8("Añadir a marcadores"), this, [this, uri] { addBookmark(uri); });
        } else if (it->data(0, MountedRole).toBool()) {
            const QString dev = it->data(0, DeviceRole).toString();
            m.addSeparator();
            m.addAction(Icons::get().ui("eject"), "Expulsar", this, [this, dev] {
                if (QProcess::execute("udisksctl", {"unmount", "-b", dev, "--no-user-interaction"}) == 0)
                    QProcess::startDetached("udisksctl", {"power-off", "-b", dev, "--no-user-interaction"});
                reload();
            });
        }
        if (path == FileOps::trashFiles()) {
            m.addSeparator();
            m.addAction(Icons::get().ui("trash"), "Vaciar papelera", this, [] { FileOps::get().emptyTrash(); });
        }
    }
    m.addSeparator();
    m.addAction(QString::fromUtf8("Mostrar lugares"), this, [] {
        Settings::get().showPlaces = !Settings::get().showPlaces;
        Settings::get().changed();
    })->setCheckable(true);
    m.actions().last()->setChecked(Settings::get().showPlaces);
    auto *ab = m.addAction(QString::fromUtf8("Mostrar marcadores"), this, [] {
        Settings::get().showBookmarks = !Settings::get().showBookmarks;
        Settings::get().changed();
    });
    ab->setCheckable(true);
    ab->setChecked(Settings::get().showBookmarks);
    auto *ad = m.addAction(QString::fromUtf8("Mostrar dispositivos"), this, [] {
        Settings::get().showDevices = !Settings::get().showDevices;
        Settings::get().changed();
    });
    ad->setCheckable(true);
    ad->setChecked(Settings::get().showDevices);
    m.exec(e->globalPos());
}

void Sidebar::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->source() == this || e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void Sidebar::dragMoveEvent(QDragMoveEvent *e)
{
    QTreeWidget::dragMoveEvent(e);
    if (e->source() == this || e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void Sidebar::dropEvent(QDropEvent *e)
{
    QTreeWidgetItem *target = itemAt(e->position().toPoint());
    // Reordering bookmarks inside the sidebar
    if (e->source() == this) {
        QTreeWidgetItem *moving = currentItem();
        if (!moving || moving->data(0, KindRole).toInt() != BookmarkItem || !m_bookmarks) return;
        auto b = readBookmarks();
        const QString from = moving->data(0, PathRole).toString();
        auto it = std::find_if(b.begin(), b.end(), [&](const Bookmark &x) { return x.path == from; });
        if (it == b.end()) return;
        Bookmark mv = *it;
        b.erase(it);
        int pos = b.size();
        if (target && target->data(0, KindRole).toInt() == BookmarkItem) {
            const QString to = target->data(0, PathRole).toString();
            for (int i = 0; i < b.size(); ++i)
                if (b[i].path == to) pos = i;
        }
        b.insert(pos, mv);
        writeBookmarks(b);
        reload();
        e->acceptProposedAction();
        return;
    }
    QList<QUrl> urls = e->mimeData()->urls();
    const int kind = target ? target->data(0, KindRole).toInt() : Header;
    const bool ontoBookmarks = target == m_bookmarks || (kind == Header && target != m_devices && target != m_places) || !target;
    if (ontoBookmarks) {
        for (const QUrl &u : urls)
            if (u.isLocalFile() && QFileInfo(u.toLocalFile()).isDir()) addBookmark(u.toLocalFile());
        e->acceptProposedAction();
        return;
    }
    const QString dest = target->data(0, PathRole).toString();
    if (!dest.isEmpty()) {
        emit dropOnPlace(urls, dest);
        e->acceptProposedAction();
    }
}
