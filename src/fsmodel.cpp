#include "fsmodel.h"
#include "icons.h"
#include "remote.h"
#include "settings.h"
#include "theme.h"
#include "thumbs.h"
#include "undo.h"
#include <QCollator>
#include <QCoreApplication>
#include <QFont>
#include <QIcon>
#include <QPointer>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocale>
#include <QMimeData>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThreadPool>
#include <QUrl>
#include <atomic>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <deque>
#include <fnmatch.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unordered_map>

namespace {

QThreadPool &listPool()
{
    static QThreadPool *p = [] {
        auto *pool = new QThreadPool;
        pool->setMaxThreadCount(2);
        return pool;
    }();
    return *p;
}

// QMimeDatabase tries every glob under one global lock (~12 µs a name, most
// of a listing). Nearly all globs are "*.ext", which only look at the name
// from its first dot on, so that tail is an exact cache key unless one of the
// few globs that look at the whole name (Makefile, README*, *~…) applies.
// For the same reason a name without a dot that none of those match has no
// type by name at all.
struct NameGlobs {
    struct Wild {
        QByteArray glob;
        bool cs;
        QChar first, last;   // literal first/last character, to skip fnmatch
    };
    bool ok = false;
    QSet<QString> literals;   // lower-case
    std::vector<Wild> wild;
};

const NameGlobs &nameGlobs()
{
    static const NameGlobs g = [] {
        NameGlobs g;
        auto literal = [](char c) { return c != '*' && c != '?' && c != '[' && c != ']'; };
        for (const QString &f : QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, "mime/globs2")) {
            QFile file(f);
            if (!file.open(QIODevice::ReadOnly)) continue;
            g.ok = true;
            while (!file.atEnd()) {
                const QByteArray line = file.readLine().trimmed();
                if (line.isEmpty() || line.startsWith('#')) continue;
                const QList<QByteArray> parts = line.split(':');   // weight:type:glob[:flags]
                if (parts.size() < 3 || parts[2].isEmpty()) continue;
                const QByteArray &glob = parts[2];
                if (glob.startsWith("*.")) continue;
                if (glob.contains('*') || glob.contains('?') || glob.contains('[')) {
                    NameGlobs::Wild w{glob, parts.size() > 3 && parts[3].contains("cs"), {}, {}};
                    if (literal(glob.front())) w.first = QChar(glob.front()).toLower();
                    if (literal(glob.back())) w.last = QChar(glob.back()).toLower();
                    g.wild.push_back(w);
                } else {
                    g.literals.insert(QString::fromUtf8(glob).toLower());
                }
            }
        }
        return g;
    }();
    return g;
}

QMimeType mimeByName(const QString &name)
{
    static thread_local QMimeDatabase db;
    static thread_local QHash<QString, QMimeType> byTail;
    const NameGlobs &g = nameGlobs();
    if (!g.ok || name.isEmpty()) return db.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    bool special = g.literals.contains(name.toLower());
    const QChar first = name.front().toLower(), last = name.back().toLower();
    for (size_t i = 0; i < g.wild.size() && !special; ++i) {
        const NameGlobs::Wild &w = g.wild[i];
        if ((!w.first.isNull() && w.first != first) || (!w.last.isNull() && w.last != last)) continue;
        special = fnmatch(w.glob.constData(), name.toUtf8().constData(), w.cs ? 0 : FNM_CASEFOLD) == 0;
    }
    if (special) return db.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    const qsizetype dot = name.indexOf('.');
    if (dot < 0) {
        static thread_local const QMimeType none = db.mimeTypeForName(QStringLiteral("application/octet-stream"));
        return none;
    }
    // Qt does not apply "*.old" to a name that is just ".old": hidden names
    // only share a cache entry with themselves ('/' never occurs in a name).
    const QString tail = dot == 0 ? '/' + name : name.mid(dot);
    auto it = byTail.constFind(tail);
    if (it != byTail.constEnd()) return *it;
    if (byTail.size() > 4096) byTail.clear();
    const QMimeType t = db.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    byTail.insert(tail, t);
    return t;
}

// Content sniffing opens and reads the file, so with `later` set it is left
// to the caller (the lister does it after the folder is already on screen).
QString mimeFor(const QString &dirPath, const QString &name, bool isDir, bool *later = nullptr)
{
    if (isDir) return QStringLiteral("inode/directory");
    static thread_local QMimeDatabase db;
    QMimeType t = mimeByName(name);
    if (t.isDefault() && !name.contains('.')) {
        if (later) {
            *later = true;
            return t.name();
        }
        t = db.mimeTypeForFile(dirPath + '/' + name, QMimeDatabase::MatchContent);
    }
    return t.name();
}

QString sniffMime(const QString &path)
{
    static thread_local QMimeDatabase db;
    return db.mimeTypeForFile(path, QMimeDatabase::MatchContent).name();
}

// Bumped when the date settings change or the day turns, so the cached
// "hoy"/"ayer" texts get rebuilt.
quint32 g_formatGen = 1;

void armMidnightTimer()
{
    static QTimer *t = [] {
        auto *timer = new QTimer(QCoreApplication::instance());
        timer->setSingleShot(true);
        QObject::connect(timer, &QTimer::timeout, timer, [timer] {
            ++g_formatGen;
            armMidnightTimer();
        });
        return timer;
    }();
    const QDateTime now = QDateTime::currentDateTime();
    t->start(int(std::min<qint64>(now.msecsTo(QDateTime(now.date().addDays(1), QTime(0, 0, 1))), 24 * 3600 * 1000)));
}

FileEntry fromStat(const QString &dirPath, const QString &name, int dirfd, const char *cname, QStringList *deferMime = nullptr)
{
    FileEntry e;
    e.name = name;
    e.hidden = name.startsWith('.');
    struct stat st;
    if (fstatat(dirfd, cname, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        e.broken = true;
        return e;
    }
    if (S_ISLNK(st.st_mode)) {
        e.isLink = true;
        char buf[4096];
        ssize_t n = readlinkat(dirfd, cname, buf, sizeof buf - 1);
        if (n > 0) e.linkTarget = QString::fromLocal8Bit(buf, int(n));
        struct stat t;
        if (fstatat(dirfd, cname, &t, 0) == 0) st = t;
        else e.broken = true;
    }
    e.isDir = S_ISDIR(st.st_mode);
    e.size = e.isDir ? -1 : st.st_size;
    e.mtime = st.st_mtim.tv_sec;
    e.mode = st.st_mode;
    e.uid = st.st_uid;
    e.gid = st.st_gid;
    bool later = false;
    e.mime = e.broken ? QStringLiteral("inode/symlink") : mimeFor(dirPath, name, e.isDir, deferMime && S_ISREG(st.st_mode) ? &later : nullptr);
    if (later) deferMime->append(name);
    return e;
}

qint64 countEntries(const QString &path)
{
    DIR *d = opendir(QFile::encodeName(path).constData());
    if (!d) return -1;
    qint64 n = 0;
    while (dirent *de = readdir(d)) {
        const char *s = de->d_name;
        if (s[0] == '.' && (s[1] == 0 || (s[1] == '.' && s[2] == 0))) continue;
        if (s[0] == '.' && !Settings::get().showHidden) continue;
        ++n;
    }
    closedir(d);
    return n;
}

class Lister : public QRunnable {
public:
    Lister(QString path, quint64 gen, std::function<void(std::vector<FileEntry>, QString)> done,
           std::function<void(QHash<QString, QString>)> mimes, std::function<void(QHash<QString, qint64>)> counts)
        : m_path(std::move(path)), m_gen(gen), m_done(std::move(done)), m_mimes(std::move(mimes)), m_counts(std::move(counts)) {}
    void run() override
    {
        std::vector<FileEntry> entries;
        QString err;
        QStringList sniff;
        FsModel::listDirectory(m_path, entries, &err, &sniff);
        QStringList dirs;
        for (auto &e : entries)
            if (e.isDir) dirs << e.name;
        auto done = m_done;
        if (auto *app = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(app, [done, entries = std::move(entries), err]() mutable { done(std::move(entries), err); },
                                      Qt::QueuedConnection);
        }
        QElapsedTimer t;
        t.start();
        // Second pass for files without extension: look inside them.
        if (!sniff.isEmpty()) {
            QHash<QString, QString> mimes;
            for (const QString &n : sniff) {
                const QString m = sniffMime(m_path + '/' + n);
                if (m != QLatin1String("application/octet-stream")) mimes.insert(n, m);
                if (t.elapsed() > 3000) break;   // network mounts
            }
            auto cb = m_mimes;
            if (!mimes.isEmpty())
                if (auto *app = QCoreApplication::instance())
                    QMetaObject::invokeMethod(app, [cb, mimes = std::move(mimes)]() mutable { cb(std::move(mimes)); }, Qt::QueuedConnection);
        }
        if (!Settings::get().showItemCounts || dirs.isEmpty() || Remote::isRemotePath(m_path)) return;
        QHash<QString, qint64> counts;
        t.restart();
        for (const QString &d : dirs) {
            counts.insert(d, countEntries(m_path + '/' + d));
            if (t.elapsed() > 3000) break;   // network mounts or huge trees
        }
        auto cb = m_counts;
        if (auto *app = QCoreApplication::instance())
            QMetaObject::invokeMethod(app, [cb, counts]() { cb(counts); }, Qt::QueuedConnection);
    }

private:
    QString m_path;
    quint64 m_gen;
    std::function<void(std::vector<FileEntry>, QString)> m_done;
    std::function<void(QHash<QString, QString>)> m_mimes;
    std::function<void(QHash<QString, qint64>)> m_counts;
};

} // namespace

void FsModel::shutdown()
{
    listPool().clear();
    listPool().waitForDone();
}

// ------------------------------------------------------------- static helpers
bool FsModel::listDirectory(const QString &path, std::vector<FileEntry> &out, QString *err, QStringList *deferMime)
{
    QByteArray p = QFile::encodeName(path);
    int fd = open(p.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        if (err) *err = errno == EACCES ? QString::fromUtf8("No tienes permiso para ver esta carpeta.")
                                        : QString::fromUtf8("No se puede abrir la carpeta: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        return false;
    }
    while (dirent *de = readdir(d)) {
        const char *s = de->d_name;
        if (s[0] == '.' && (s[1] == 0 || (s[1] == '.' && s[2] == 0))) continue;
        QString name = QFile::decodeName(s);
        out.push_back(fromStat(path, name, fd, s, deferMime));
    }
    closedir(d);
    return true;
}

FileEntry FsModel::statPath(const QString &path, bool *ok)
{
    QFileInfo fi(path);
    QByteArray dir = QFile::encodeName(fi.absolutePath());
    int fd = open(dir.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    FileEntry e;
    if (fd < 0) {
        if (ok) *ok = false;
        e.name = fi.fileName();
        return e;
    }
    QByteArray name = QFile::encodeName(fi.fileName());
    e = fromStat(fi.absolutePath(), fi.fileName(), fd, name.constData());
    close(fd);
    if (ok) *ok = !e.broken || e.isLink;
    return e;
}

QString FsModel::formatSize(qint64 b)
{
    if (b < 0) return {};
    const bool iec = Settings::get().sizeIEC;
    const double base = iec ? 1024.0 : 1000.0;
    static const char *si[] = {"B", "kB", "MB", "GB", "TB", "PB"};
    static const char *bi[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = b;
    int u = 0;
    while (v >= base && u < 5) v /= base, ++u;
    static const QLocale loc(QLocale::Spanish, QLocale::Spain);
    QString num = u == 0 ? QString::number(b) : loc.toString(v, 'f', v < 10 ? 1 : 0);
    return num + " " + (iec ? bi[u] : si[u]);
}

QString FsModel::formatDate(qint64 secs)
{
    if (secs <= 0) return {};
    QDateTime dt = QDateTime::fromSecsSinceEpoch(secs);
    static const QLocale loc(QLocale::Spanish, QLocale::Spain);
    switch (Settings::get().dateFormat) {
    case 1: return loc.toString(dt, "d MMM yyyy, HH:mm");
    case 2: return dt.toString("yyyy-MM-dd HH:mm");
    default: break;
    }
    const QDate today = QDate::currentDate(), d = dt.date();
    if (d == today) return QString::fromUtf8("hoy, ") + dt.toString("HH:mm");
    if (d == today.addDays(-1)) return QString::fromUtf8("ayer, ") + dt.toString("HH:mm");
    if (d.daysTo(today) < 7 && d < today) return loc.toString(dt, "dddd, HH:mm");
    if (d.year() == today.year()) return loc.toString(dt, "d MMM");
    return loc.toString(dt, "d MMM yyyy");
}

QString FsModel::permString(quint32 m, bool isDir, bool isLink)
{
    QString s;
    s += isLink ? 'l' : isDir ? 'd' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) s += (m & (0400 >> i)) ? QChar(rwx[i]) : QChar('-');
    if (m & S_ISUID) s[3] = s[3] == 'x' ? 's' : 'S';
    if (m & S_ISGID) s[6] = s[6] == 'x' ? 's' : 'S';
    if (m & S_ISVTX) s[9] = s[9] == 'x' ? 't' : 'T';
    return s;
}

QString FsModel::userName(quint32 uid)
{
    static QHash<quint32, QString> cache;
    auto it = cache.find(uid);
    if (it != cache.end()) return *it;
    passwd *pw = getpwuid(uid);
    QString n = pw ? QString::fromLocal8Bit(pw->pw_name) : QString::number(uid);
    cache.insert(uid, n);
    return n;
}

QString FsModel::groupName(quint32 gid)
{
    static QHash<quint32, QString> cache;
    auto it = cache.find(gid);
    if (it != cache.end()) return *it;
    group *g = getgrgid(gid);
    QString n = g ? QString::fromLocal8Bit(g->gr_name) : QString::number(gid);
    cache.insert(gid, n);
    return n;
}

QString FsModel::mimeComment(const QString &mime)
{
    static QHash<QString, QString> cache;
    auto it = cache.find(mime);
    if (it != cache.end()) return *it;
    QString c = mime == "inode/directory" ? QString::fromUtf8("Carpeta") : QMimeDatabase().mimeTypeForName(mime).comment();
    if (c.isEmpty()) c = mime;
    cache.insert(mime, c);
    return c;
}

// ------------------------------------------------------------------- model
int FsModel::Node::row() const
{
    if (!parent) return 0;
    const auto &k = parent->kids;
    if (rowHint < int(k.size()) && k[rowHint].get() == this) return rowHint;
    for (size_t i = 0; i < k.size(); ++i)
        if (k[i].get() == this) return rowHint = int(i);
    return -1;
}

void FsModel::renumber(Node *n, int from)
{
    for (int i = from; i < int(n->kids.size()); ++i) n->kids[i]->rowHint = i;
}

QCollator &FsModel::collator()
{
    static QCollator c = [] {
        QCollator c(QLocale(QLocale::Spanish, QLocale::Spain));
        c.setNumericMode(true);
        c.setCaseSensitivity(Qt::CaseInsensitive);
        c.setIgnorePunctuation(false);
        return c;
    }();
    return c;
}

const QCollatorSortKey &FsModel::nameKey(const QModelIndex &idx) const
{
    const Node *n = nodeOf(idx);
    if (!n->key) n->key.emplace(collator().sortKey(n->e.name));
    return *n->key;
}

FsModel::FsModel(QObject *parent) : QAbstractItemModel(parent)
{
    m_root = std::make_unique<Node>();
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &FsModel::onDirChanged);
    m_dirtyTimer.setSingleShot(true);
    m_dirtyTimer.setInterval(180);
    connect(&m_dirtyTimer, &QTimer::timeout, this, [this] {
        const auto dirty = m_dirty;
        m_dirty.clear();
        for (const QString &p : dirty)
            if (Node *n = m_byPath.value(p)) {
                if (n->loaded && !n->loading) load(n);
            }
    });
    connect(&Thumbs::get(), &Thumbs::ready, this, &FsModel::thumbReady);
    connect(&Settings::get(), &Settings::updated, this, [] { ++g_formatGen; });
    armMidnightTimer();
    m_monoFont = QFont(Theme::get().mono);
    m_italicFont.setItalic(true);
}

FsModel::~FsModel()
{
    stopSearch();
}

FsModel::Node *FsModel::nodeOf(const QModelIndex &idx) const
{
    return idx.isValid() ? static_cast<Node *>(idx.internalPointer()) : m_root.get();
}

QModelIndex FsModel::indexOf(Node *n, int column) const
{
    if (!n || n == m_root.get()) return {};
    return createIndex(n->row(), column, n);
}

QString FsModel::pathOf(const Node *n) const
{
    if (!n->fullPath.isEmpty()) return n->fullPath;
    if (!n->parent) return m_rootPath;
    QString p = pathOf(n->parent);
    return p.endsWith('/') ? p + n->e.name : p + '/' + n->e.name;
}

QString FsModel::filePath(const QModelIndex &idx) const { return pathOf(nodeOf(idx)); }

const FileEntry *FsModel::entry(const QModelIndex &idx) const { return idx.isValid() ? &nodeOf(idx)->e : nullptr; }

QModelIndex FsModel::indexForPath(const QString &path) const
{
    Node *n = m_byPath.value(path);
    return n ? indexOf(n) : QModelIndex();
}

int FsModel::fileCount() const { return int(m_root->kids.size()); }

qint64 FsModel::totalSize() const
{
    qint64 s = 0;
    for (auto &k : m_root->kids)
        if (!k->e.isDir) s += k->e.size;
    return s;
}

void FsModel::indexNode(Node *n) { m_byPath.insert(pathOf(n), n); }

void FsModel::unindexNode(Node *n)
{
    for (auto &k : n->kids) unindexNode(k.get());
    const QString p = pathOf(n);
    if (m_byPath.value(p) == n) m_byPath.remove(p);
    if (n->loaded && m_watched.remove(p)) m_watcher.removePath(p);
}

void FsModel::watch(Node *n)
{
    const QString p = pathOf(n);
    if (!m_watched.contains(p) && m_watcher.addPath(p)) m_watched.insert(p);
}

void FsModel::setRoot(const QString &path)
{
    stopSearch();
    beginResetModel();
    if (!m_watched.isEmpty()) m_watcher.removePaths(QStringList(m_watched.begin(), m_watched.end()));
    m_watched.clear();
    m_byPath.clear();
    m_dirty.clear();
    m_root = std::make_unique<Node>();
    m_virtual = false;
    m_rootPath = QDir::cleanPath(path);
    m_root->fullPath = m_rootPath;
    m_root->e.isDir = true;
    m_root->e.name = QFileInfo(m_rootPath).fileName();
    ++m_gen;
    m_error.clear();
    endResetModel();
    m_byPath.insert(m_rootPath, m_root.get());
    load(m_root.get());
}

void FsModel::refresh()
{
    if (m_searchMode) return;
    for (auto it = m_byPath.begin(); it != m_byPath.end(); ++it)
        if (it.value()->loaded) m_dirty.insert(it.key());
    m_dirtyTimer.start(0);
}

void FsModel::load(Node *n)
{
    n->loading = true;
    if (n == m_root.get()) {
        m_loading = true;
        emit loadingChanged(true);
    }
    const QString path = pathOf(n);
    const quint64 gen = m_gen;
    QPointer<FsModel> self(this);
    auto *job = new Lister(
        path, gen,
        [self, path, gen](std::vector<FileEntry> entries, QString err) {
            if (self) self->deliver(path, gen, std::move(entries), err);
        },
        [self, path, gen](QHash<QString, QString> mimes) {
            if (self) self->deliverMimes(path, gen, std::move(mimes));
        },
        [self, path, gen](QHash<QString, qint64> counts) {
            if (self) self->deliverCounts(path, gen, std::move(counts));
        });
    listPool().start(job);
}

void FsModel::deliver(const QString &path, quint64 gen, std::vector<FileEntry> entries, const QString &err)
{
    if (gen != m_gen) return;
    Node *n = m_byPath.value(path);
    if (!n) return;
    n->loading = false;
    const bool first = !n->loaded;
    n->loaded = true;
    if (!err.isEmpty() && n == m_root.get()) {
        m_error = err;
        emit errorOccurred(err);
    }
    if (first) {
        // Pre-size counts we already know (keeps the column stable on refresh).
        if (!entries.empty()) {
            QModelIndex parent = indexOf(n);
            beginInsertRows(parent, 0, int(entries.size()) - 1);
            n->kids.reserve(entries.size());
            for (auto &e : entries) {
                auto k = std::make_unique<Node>();
                k->e = std::move(e);
                k->parent = n;
                k->rowHint = int(n->kids.size());
                Node *raw = k.get();
                n->kids.push_back(std::move(k));
                indexNode(raw);
            }
            endInsertRows();
        }
        watch(n);
    } else {
        merge(n, entries);
    }
    if (n == m_root.get()) {
        m_loading = false;
        emit loadingChanged(false);
        if (first) emit rootLoaded();
    }
}

void FsModel::deliverCounts(const QString &path, quint64 gen, QHash<QString, qint64> counts)
{
    if (gen != m_gen) return;
    Node *n = m_byPath.value(path);
    if (!n) return;
    // One signal for the whole span: sorted by size, each one would make the
    // proxy re-sort and the view lay everything out again.
    int first = -1, last = -1;
    for (size_t i = 0; i < n->kids.size(); ++i) {
        Node *k = n->kids[i].get();
        if (!k->e.isDir) continue;
        auto it = counts.find(k->e.name);
        if (it == counts.end() || k->e.size == *it) continue;
        k->e.size = *it;
        if (first < 0) first = int(i);
        last = int(i);
    }
    if (first >= 0)
        emit dataChanged(createIndex(first, Size, n->kids[first].get()), createIndex(last, Size, n->kids[last].get()), {Qt::DisplayRole, SortRole});
}

void FsModel::deliverMimes(const QString &path, quint64 gen, QHash<QString, QString> mimes)
{
    if (gen != m_gen) return;
    Node *n = m_byPath.value(path);
    if (!n) return;
    int first = -1, last = -1;
    for (size_t i = 0; i < n->kids.size(); ++i) {
        Node *k = n->kids[i].get();
        if (k->e.isDir) continue;
        auto it = mimes.find(k->e.name);
        if (it == mimes.end() || k->e.mime == *it) continue;
        k->e.mime = *it;
        if (first < 0) first = int(i);
        last = int(i);
    }
    if (first < 0) return;
    Node *a = n->kids[first].get(), *b = n->kids[last].get();
    emit dataChanged(createIndex(first, Name, a), createIndex(last, Name, b), {Qt::DecorationRole, MimeRole});
    emit dataChanged(createIndex(first, Type, a), createIndex(last, Type, b), {Qt::DisplayRole, SortRole});
}

void FsModel::merge(Node *n, std::vector<FileEntry> &entries)
{
    QHash<QString, int> fresh;
    for (int i = 0; i < int(entries.size()); ++i) fresh.insert(entries[i].name, i);
    QModelIndex parent = indexOf(n);
    // removals, from the end so row numbers stay valid, in contiguous runs
    for (int r = int(n->kids.size()) - 1; r >= 0; --r) {
        if (fresh.contains(n->kids[r]->e.name)) continue;
        const int last = r;
        while (r > 0 && !fresh.contains(n->kids[r - 1]->e.name)) --r;
        beginRemoveRows(parent, r, last);
        for (int i = r; i <= last; ++i) unindexNode(n->kids[i].get());
        n->kids.erase(n->kids.begin() + r, n->kids.begin() + last + 1);
        renumber(n, r);
        endRemoveRows();
    }
    QSet<QString> have;
    for (int r = 0; r < int(n->kids.size()); ++r) {
        Node *k = n->kids[r].get();
        have.insert(k->e.name);
        FileEntry &ne = entries[fresh.value(k->e.name)];
        if (k->e.isDir && ne.isDir && k->e.size >= 0) ne.size = k->e.size;   // keep the item count
        if (ne.mtime != k->e.mtime || ne.size != k->e.size || ne.mode != k->e.mode || ne.isDir != k->e.isDir) {
            const bool wasDir = k->e.isDir;
            const QString mime = k->e.mime;
            k->e = ne;
            k->dateGen = 0;
            // a provisional type for a file without extension: keep the sniffed one
            if (!ne.isDir && ne.mime == QLatin1String("application/octet-stream") && !ne.name.contains('.')) k->e.mime = mime;
            if (wasDir && !ne.isDir && !k->kids.empty()) {
                beginRemoveRows(createIndex(r, 0, k), 0, int(k->kids.size()) - 1);
                for (auto &c : k->kids) unindexNode(c.get());
                k->kids.clear();
                k->loaded = false;
                endRemoveRows();
            }
            emit dataChanged(createIndex(r, 0, k), createIndex(r, ColumnCount - 1, k));
        }
    }
    std::vector<FileEntry *> added;
    for (auto &e : entries)
        if (!have.contains(e.name)) added.push_back(&e);
    if (!added.empty()) {
        int first = int(n->kids.size());
        beginInsertRows(parent, first, first + int(added.size()) - 1);
        for (FileEntry *e : added) {
            auto k = std::make_unique<Node>();
            k->e = std::move(*e);
            k->parent = n;
            k->rowHint = int(n->kids.size());
            Node *raw = k.get();
            n->kids.push_back(std::move(k));
            indexNode(raw);
        }
        endInsertRows();
    }
}

void FsModel::onDirChanged(const QString &path)
{
    m_dirty.insert(path);
    if (!m_dirtyTimer.isActive()) m_dirtyTimer.start();
}

void FsModel::thumbReady(const QString &path)
{
    QModelIndex ix = indexForPath(path);
    if (ix.isValid()) emit dataChanged(ix, ix, {Qt::DecorationRole});
}

// ------------------------------------------------------------------ search
namespace {

struct LinuxDirent64 {
    ino64_t d_ino;
    off64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

QString foldName(const QString &s)
{
    // lower-case and without accents, so "musica" finds "Música"
    QString n = s.normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(n.size());
    for (QChar c : n)
        if (c.category() != QChar::Mark_NonSpacing) out += c.toLower();
    return out;
}

// Case-insensitive ASCII substring test on raw bytes (needle already lower-case).
bool asciiContains(const char *hay, size_t hl, const char *needle, size_t nl)
{
    if (nl == 0) return true;
    if (hl < nl) return false;
    const char first = needle[0];
    for (size_t i = 0; i + nl <= hl; ++i) {
        char c = hay[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != first) continue;
        size_t k = 1;
        for (; k < nl; ++k) {
            char d = hay[i + k];
            if (d >= 'A' && d <= 'Z') d += 32;
            if (d != needle[k]) break;
        }
        if (k == nl) return true;
    }
    return false;
}

bool skipDir(const QByteArray &path)
{
    static const char *pseudo[] = {"/proc", "/sys", "/dev", "/run/user", "/var/lib/docker", "/snap"};
    for (const char *p : pseudo) {
        const size_t n = strlen(p);
        if (size_t(path.size()) >= n && memcmp(path.constData(), p, n) == 0 && (size_t(path.size()) == n || path[int(n)] == '/')) return true;
    }
    return false;
}

} // namespace

void FsModel::searchWorker(QPointer<FsModel> self, std::shared_ptr<std::atomic<bool>> cancel, QByteArray root, QString pattern,
                           quint64 gen, bool hidden)
{
    // Be nice: lowest CPU priority and idle I/O class for this thread only.
    setpriority(PRIO_PROCESS, gettid(), 19);
    syscall(SYS_ioprio_set, 1 /*IOPRIO_WHO_PROCESS*/, 0, (3 << 13) /*IOPRIO_CLASS_IDLE*/);

    const bool glob = pattern.contains('*') || pattern.contains('?') || pattern.contains('[');
    const QString folded = foldName(pattern);
    const QByteArray foldedUtf8 = folded.toUtf8();
    bool asciiPattern = true;
    for (char c : foldedUtf8) asciiPattern &= (unsigned char)c < 0x80;
    const QByteArray globBytes = pattern.toUtf8();
    const bool searchRootIsPseudo = skipDir(root);

    std::vector<std::pair<QString, FileEntry>> batch;
    QElapsedTimer t;
    t.start();
    int total = 0;
    auto flush = [&](bool finished) {
        auto b = std::move(batch);
        batch.clear();
        auto *app = QCoreApplication::instance();
        if (!app) return;
        QMetaObject::invokeMethod(app, [self, b = std::move(b), gen, finished, total]() mutable {
            if (!self || gen != self->m_gen) return;
            self->insertFlat(b);
            emit self->searchProgress(total, finished);
        }, Qt::QueuedConnection);
    };
    auto matches = [&](const char *name, size_t len) {
        if (glob) return fnmatch(globBytes.constData(), name, FNM_CASEFOLD) == 0;
        bool ascii = true;
        for (size_t i = 0; i < len && ascii; ++i) ascii = (unsigned char)name[i] < 0x80;
        if (ascii && asciiPattern) return asciiContains(name, len, foldedUtf8.constData(), size_t(foldedUtf8.size()));
        return foldName(QString::fromUtf8(name, qsizetype(len))).contains(folded);
    };

    std::vector<char> buf(64 * 1024);
    std::deque<QByteArray> queue{root};   // breadth-first: nearby results first
    while (!queue.empty() && !*cancel && total < 20000) {
        QByteArray dir = std::move(queue.front());
        queue.pop_front();
        int fd = open(dir.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) continue;
        QString dirQ;   // decoded lazily, only when something matches
        for (;;) {
            long n = syscall(SYS_getdents64, fd, buf.data(), buf.size());
            if (n <= 0 || *cancel) break;
            for (long off = 0; off < n;) {
                auto *d = reinterpret_cast<LinuxDirent64 *>(buf.data() + off);
                off += d->d_reclen;
                const char *name = d->d_name;
                if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
                if (name[0] == '.' && !hidden) continue;
                const size_t len = strlen(name);
                unsigned char type = d->d_type;
                if (type == DT_UNKNOWN) {
                    struct stat st;
                    if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) type = S_ISDIR(st.st_mode) ? DT_DIR : S_ISLNK(st.st_mode) ? DT_LNK : DT_REG;
                }
                if (type == DT_DIR) {   // symlinked dirs are not followed (no loops)
                    QByteArray sub = dir.endsWith('/') ? dir + name : dir + '/' + name;
                    if (searchRootIsPseudo || !skipDir(sub)) queue.push_back(std::move(sub));
                }
                if (!matches(name, len)) continue;
                if (dirQ.isEmpty()) dirQ = QFile::decodeName(dir);
                const QString nameQ = QFile::decodeName(QByteArray(name, int(len)));
                batch.push_back({dirQ.endsWith('/') ? dirQ + nameQ : dirQ + '/' + nameQ, fromStat(dirQ, nameQ, fd, name)});
                ++total;
                if (batch.size() >= 300 || t.elapsed() > 150) {
                    flush(false);
                    t.restart();
                }
            }
        }
        close(fd);
    }
    if (!*cancel) flush(true);
    if (qEnvironmentVariableIsSet("LUNA_SEARCHTEST")) {
        rusage ru;
        getrusage(RUSAGE_THREAD, &ru);
        fprintf(stderr, "HILO búsqueda: %lld ms de CPU (usuario %ld ms, sistema %ld ms)\n",
                qlonglong(ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000 + ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000),
                long(ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000), long(ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000));
    }
}

void FsModel::insertFlat(std::vector<std::pair<QString, FileEntry>> &items)
{
    if (items.empty()) return;
    Node *r = m_root.get();
    const int first = int(r->kids.size());
    beginInsertRows({}, first, first + int(items.size()) - 1);
    for (auto &p : items) {
        auto k = std::make_unique<Node>();
        k->e = std::move(p.second);
        k->fullPath = p.first;
        k->parent = r;
        k->rowHint = int(r->kids.size());
        m_byPath.insert(p.first, k.get());
        r->kids.push_back(std::move(k));
    }
    endInsertRows();
}

void FsModel::setPaths(const QString &root, const QStringList &paths)
{
    stopSearch();
    beginResetModel();
    if (!m_watched.isEmpty()) m_watcher.removePaths(QStringList(m_watched.begin(), m_watched.end()));
    m_watched.clear();
    m_byPath.clear();
    m_dirty.clear();
    m_rootPath = root;
    m_root = std::make_unique<Node>();
    m_root->fullPath = root;
    m_root->loaded = true;
    ++m_gen;
    m_error.clear();
    m_searchMode = true;
    m_virtual = true;
    endResetModel();
    m_loading = true;
    emit loadingChanged(true);
    const quint64 gen = m_gen;
    QPointer<FsModel> self(this);
    listPool().start([self, paths, gen] {
        std::vector<std::pair<QString, FileEntry>> items;
        for (const QString &p : paths) {
            bool ok = false;
            FileEntry e = statPath(p, &ok);
            if (ok) items.push_back({p, std::move(e)});
        }
        if (auto *app = QCoreApplication::instance())
            QMetaObject::invokeMethod(app, [self, gen, items = std::move(items)]() mutable {
                if (!self || gen != self->m_gen) return;
                self->insertFlat(items);
                self->m_loading = false;
                emit self->loadingChanged(false);
                emit self->rootLoaded();
            }, Qt::QueuedConnection);
    });
}

void FsModel::startSearch(const QString &pattern)
{
    stopSearch();
    beginResetModel();
    if (!m_watched.isEmpty()) m_watcher.removePaths(QStringList(m_watched.begin(), m_watched.end()));
    m_watched.clear();
    m_byPath.clear();
    m_root = std::make_unique<Node>();
    m_root->fullPath = m_rootPath;
    m_root->loaded = true;
    ++m_gen;
    m_searchMode = true;
    endResetModel();

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    m_searchCancel = cancel;
    const QByteArray root = QFile::encodeName(m_rootPath);
    const quint64 gen = m_gen;
    const bool hidden = Settings::get().showHidden;
    QPointer<FsModel> self(this);
    // A dedicated low-priority thread: raising nice is one-way for normal
    // users, so it must not be a pool thread that gets reused.
    std::thread([self, cancel, root, pattern, gen, hidden] {
        searchWorker(self, cancel, root, pattern, gen, hidden);
    }).detach();
}

void FsModel::stopSearch()
{
    if (m_searchCancel) *m_searchCancel = true;
    m_searchCancel.reset();
    m_searchMode = false;
}

// ---------------------------------------------------------- item model API
QModelIndex FsModel::index(int row, int column, const QModelIndex &parent) const
{
    Node *p = nodeOf(parent);
    if (row < 0 || row >= int(p->kids.size()) || column < 0 || column >= ColumnCount) return {};
    return createIndex(row, column, p->kids[row].get());
}

QModelIndex FsModel::parent(const QModelIndex &child) const
{
    if (!child.isValid()) return {};
    Node *n = nodeOf(child);
    return indexOf(n->parent);
}

int FsModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0) return 0;
    return int(nodeOf(parent)->kids.size());
}

int FsModel::columnCount(const QModelIndex &) const { return ColumnCount; }

bool FsModel::hasChildren(const QModelIndex &parent) const
{
    Node *n = nodeOf(parent);
    if (n == m_root.get()) return !n->kids.empty() || !n->loaded;
    if (m_searchMode) return false;
    if (!n->e.isDir) return false;
    return !n->loaded || !n->kids.empty();
}

bool FsModel::canFetchMore(const QModelIndex &parent) const
{
    Node *n = nodeOf(parent);
    return n != m_root.get() && !m_searchMode && n->e.isDir && !n->loaded && !n->loading;
}

void FsModel::fetchMore(const QModelIndex &parent)
{
    Node *n = nodeOf(parent);
    if (n->loaded || n->loading) return;
    load(n);
}

QVariant FsModel::data(const QModelIndex &idx, int role) const
{
    if (!idx.isValid()) return {};
    const Node *n = nodeOf(idx);
    const FileEntry &e = n->e;
    switch (role) {
    case Qt::DisplayRole:
        switch (idx.column()) {
        case Name: return e.name;
        case Size:
            if (e.isDir) return e.size < 0 ? QString() : e.size == 1 ? QString::fromUtf8("1 elemento") : QString::fromUtf8("%1 elementos").arg(e.size);
            return formatSize(e.size);
        case Type:
            if (m_searchMode) {
                QString rel = QFileInfo(n->fullPath).path();
                if (rel.startsWith(m_rootPath)) rel = "." + rel.mid(m_rootPath.size());
                return rel;
            }
            return mimeComment(e.mime);
        case Modified:
            if (n->dateGen != g_formatGen) {
                n->dateText = formatDate(e.mtime);
                n->dateGen = g_formatGen;
            }
            return n->dateText;
        case Permissions: return permString(e.mode, e.isDir, e.isLink);
        case Owner: return userName(e.uid);
        }
        break;
    case Qt::EditRole:
        if (idx.column() == Name) return e.name;
        break;
    case Qt::DecorationRole:
        if (idx.column() == Name) {
            const QString path = pathOf(n);
            if (!e.isDir && Settings::get().thumbnails && Thumbs::canThumb(e.mime, e.size) && !Remote::isRemotePath(path)) {
                QPixmap px = Thumbs::get().thumbnail(path, e.mtime, e.mime);
                if (!px.isNull()) return QIcon(px);
            }
            return Icons::get().forEntry(e, path);
        }
        break;
    case Qt::ToolTipRole: {
        QString t = pathOf(n);
        if (e.isLink) t += QString::fromUtf8("\n→ ") + e.linkTarget;
        return t;
    }
    case Qt::TextAlignmentRole:
        if (idx.column() == Size) return int(Qt::AlignRight | Qt::AlignVCenter);
        break;
    case Qt::ForegroundRole:
        if (e.broken) return Theme::get().red;
        if (e.hidden || idx.column() != Name) return Theme::get().fgDim;
        break;
    case Qt::FontRole:
        if (idx.column() == Permissions) return m_monoFont;
        if (e.isLink && idx.column() == Name) return m_italicFont;
        break;
    case PathRole: return pathOf(n);
    case IsDirRole: return e.isDir;
    case MimeRole: return e.mime;
    case MtimeRole: return e.mtime;
    case HiddenRole: return e.hidden;
    case SizeRole: return e.size;
    case SortRole:
        switch (idx.column()) {
        case Size: return e.size;
        case Type: return m_searchMode ? data(idx, Qt::DisplayRole) : QVariant(mimeComment(e.mime));
        case Modified: return e.mtime;
        case Permissions: return e.mode & 07777;
        case Owner: return userName(e.uid);
        default: return e.name;
        }
    }
    return {};
}

bool FsModel::setData(const QModelIndex &idx, const QVariant &value, int role)
{
    if (role != Qt::EditRole || idx.column() != Name) return false;
    Node *n = nodeOf(idx);
    QString newName = value.toString().trimmed();
    if (newName.isEmpty() || newName == n->e.name) return false;
    if (newName.contains('/')) {
        emit errorOccurred(QString::fromUtf8("El nombre no puede contener «/»."));
        return false;
    }
    const QString oldPath = pathOf(n);
    const QString newPath = QFileInfo(oldPath).path() + '/' + newName;
    if (QFileInfo::exists(newPath)) {
        emit errorOccurred(QString::fromUtf8("Ya existe «%1».").arg(newName));
        return false;
    }
    if (::rename(QFile::encodeName(oldPath).constData(), QFile::encodeName(newPath).constData()) != 0) {
        emit errorOccurred(QString::fromUtf8("No se pudo renombrar: %1").arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }
    Undo::get().record(Undo::Rename, {{oldPath, newPath}});
    unindexNode(n);
    n->e.name = newName;
    n->key.reset();
    n->e.hidden = newName.startsWith('.');
    if (!n->fullPath.isEmpty()) n->fullPath = newPath;
    if (!n->e.isDir) {
        static QMimeDatabase db;
        n->e.mime = db.mimeTypeForFile(newName, QMimeDatabase::MatchExtension).name();
    }
    indexNode(n);
    emit dataChanged(index(idx.row(), 0, idx.parent()), index(idx.row(), ColumnCount - 1, idx.parent()));
    return true;
}

QVariant FsModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case Name: return QString::fromUtf8("Nombre");
    case Size: return QString::fromUtf8("Tamaño");
    case Type: return m_searchMode ? QString::fromUtf8("Ubicación") : QString::fromUtf8("Tipo");
    case Modified: return QString::fromUtf8("Modificado");
    case Permissions: return QString::fromUtf8("Permisos");
    case Owner: return QString::fromUtf8("Propietario");
    }
    return {};
}

Qt::ItemFlags FsModel::flags(const QModelIndex &idx) const
{
    if (!idx.isValid()) return m_virtual ? Qt::NoItemFlags : Qt::ItemIsDropEnabled;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    const FileEntry &e = nodeOf(idx)->e;
    if (idx.column() == Name) f |= Qt::ItemIsEditable;
    if (e.isDir) f |= Qt::ItemIsDropEnabled;
    if (!e.isDir || m_searchMode) f |= Qt::ItemNeverHasChildren;
    return f;
}

QStringList FsModel::mimeTypes() const { return {"text/uri-list"}; }

QMimeData *FsModel::mimeData(const QModelIndexList &indexes) const
{
    QList<QUrl> urls;
    QSet<QString> seen;
    for (const QModelIndex &i : indexes) {
        const QString p = filePath(i);
        if (seen.contains(p)) continue;
        seen.insert(p);
        urls << QUrl::fromLocalFile(p);
    }
    auto *m = new QMimeData;
    m->setUrls(urls);
    return m;
}

bool FsModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int, int, const QModelIndex &parent)
{
    if (!data->hasUrls()) return false;
    QString dest = filePath(parent);
    const FileEntry *e = entry(parent);
    if (e && !e->isDir) dest = QFileInfo(dest).path();
    emit dropRequested(data->urls(), dest, action);
    return false;   // the copy/move happens asynchronously; views must not remove rows
}

// -------------------------------------------------------------------- proxy
SortProxy::SortProxy(QObject *parent) : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    setSortRole(FsModel::SortRole);
    setRecursiveFilteringEnabled(false);
}

void SortProxy::setQuickFilter(const QString &f)
{
    m_filter = f;
    invalidateFilter();
}

void SortProxy::reapply()
{
    invalidate();
}

bool SortProxy::filterAcceptsRow(int row, const QModelIndex &parent) const
{
    const auto *m = static_cast<const FsModel *>(sourceModel());
    const FileEntry *e = m->entry(m->index(row, 0, parent));
    if (!e) return false;
    if (!Settings::get().showHidden && e->hidden) return false;
    if (!m_filter.isEmpty() && !parent.isValid() && !e->name.contains(m_filter, Qt::CaseInsensitive)) return false;
    return true;
}

namespace {
// Sort keys for the Type and Owner columns, one per distinct value.
template <typename Text>
const QCollatorSortKey &keyFor(std::unordered_map<QString, QCollatorSortKey> &cache, const QString &k, Text text)
{
    auto it = cache.find(k);
    if (it == cache.end()) it = cache.emplace(k, FsModel::collator().sortKey(text(k))).first;
    return it->second;
}
}

bool SortProxy::lessThan(const QModelIndex &a, const QModelIndex &b) const
{
    // Straight to the entries: no QVariant round trips, and names compare by
    // precomputed collation keys (a memcmp) instead of a full ICU compare.
    const auto *m = static_cast<const FsModel *>(sourceModel());
    const FileEntry *ea = m->entry(a), *eb = m->entry(b);
    const bool desc = sortOrder() == Qt::DescendingOrder;
    if (Settings::get().foldersFirst && ea->isDir != eb->isDir) return desc ? !ea->isDir : ea->isDir;   // folders stay on top in both orders
    switch (a.column()) {
    case FsModel::Size:
        if (ea->size != eb->size) return ea->size < eb->size;
        break;
    case FsModel::Modified:
        if (ea->mtime != eb->mtime) return ea->mtime < eb->mtime;
        break;
    case FsModel::Permissions:
        if ((ea->mode & 07777) != (eb->mode & 07777)) return (ea->mode & 07777) < (eb->mode & 07777);
        break;
    case FsModel::Type:
        if (m->searching()) {
            const int c = FsModel::collator().compare(a.data(FsModel::SortRole).toString(), b.data(FsModel::SortRole).toString());
            if (c != 0) return c < 0;
        } else if (ea->mime != eb->mime) {
            static std::unordered_map<QString, QCollatorSortKey> types;
            const int c = keyFor(types, ea->mime, FsModel::mimeComment).compare(keyFor(types, eb->mime, FsModel::mimeComment));
            if (c != 0) return c < 0;
        }
        break;
    case FsModel::Owner:
        if (ea->uid != eb->uid) {
            static std::unordered_map<QString, QCollatorSortKey> owners;
            auto same = [](const QString &x) { return x; };
            const int c = keyFor(owners, FsModel::userName(ea->uid), same).compare(keyFor(owners, FsModel::userName(eb->uid), same));
            if (c != 0) return c < 0;
        }
        break;
    default:
        break;
    }
    return m->nameKey(a).compare(m->nameKey(b)) < 0;
}
