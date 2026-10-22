#include "fileops.h"
#include "undo.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QMimeData>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static QString errText() { return QString::fromLocal8Bit(strerror(errno)); }
static QByteArray enc(const QString &p) { return QFile::encodeName(p); }

Job::Job(Kind k, QStringList srcs, QString dest) : m_kind(k), m_srcs(std::move(srcs)), m_dest(std::move(dest)) {}

QString Job::title() const
{
    const int n = m_srcs.size();
    const QString what = n == 1 ? QString::fromUtf8("«%1»").arg(QFileInfo(m_srcs[0]).fileName()) : QString::fromUtf8("%1 elementos").arg(n);
    switch (m_kind) {
    case Copy: return QString::fromUtf8("Copiando %1").arg(what);
    case Move: return QString::fromUtf8("Moviendo %1").arg(what);
    case Link: return QString::fromUtf8("Enlazando %1").arg(what);
    case Delete: return QString::fromUtf8("Eliminando %1").arg(what);
    case EmptyTrash: return QString::fromUtf8("Vaciando la papelera");
    case Restore: return QString::fromUtf8("Restaurando %1").arg(what);
    case CopyTo: return QString::fromUtf8("Copiando %1").arg(what);
    case MoveTo: return QString::fromUtf8("Moviendo %1").arg(what);
    }
    return {};
}

void Job::start()
{
    m_thread = QThread::create([this] { run(); });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_thread->start();
}

void Job::scan(const QString &path)
{
    struct stat st;
    if (lstat(enc(path).constData(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            QFileInfo fi = it.fileInfo();
            if (!fi.isDir() || fi.isSymLink()) {
                m_totalBytes += fi.isSymLink() ? 0 : fi.size();
                ++m_totalFiles;
            }
            if (m_cancel) return;
        }
    } else {
        m_totalBytes += S_ISREG(st.st_mode) ? st.st_size : 0;
        ++m_totalFiles;
    }
}

void Job::done(const QString &src, const QString &dst)
{
    m_created << dst;
    m_pairs.append({src, dst});
}

void Job::run()
{
    if (m_kind == EmptyTrash) {
        m_srcs.clear();
        for (const QString &n : QDir(FileOps::trashFiles()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            m_srcs << FileOps::trashFiles() + '/' + n;
    }
    if (m_kind == Copy || m_kind == Move || m_kind == Delete || m_kind == EmptyTrash || m_kind == CopyTo || m_kind == MoveTo)
        for (const QString &s : m_srcs) scan(s);
    else m_totalFiles = m_srcs.size();
    emit progress(0, m_totalBytes, 0, m_totalFiles, {});

    for (int i = 0; i < m_srcs.size(); ++i) {
        const QString &src = m_srcs[i];
        if (m_cancel) break;
        const QString name = QFileInfo(src).fileName();
        switch (m_kind) {
        case CopyTo:
        case MoveTo: {
            const bool move = m_kind == MoveTo;
            QString dst = m_targets.value(i);
            if (dst.isEmpty() || dst == src) break;
            if ((dst + '/').startsWith(src + '/')) {
                m_errors << QString::fromUtf8("No se puede copiar «%1» dentro de sí misma.").arg(name);
                break;
            }
            QDir().mkpath(QFileInfo(dst).absolutePath());
            bool skip = false;
            dst = resolve(src, dst, skip);
            if (skip) break;
            if (move && ::rename(enc(src).constData(), enc(dst).constData()) == 0) {
                done(src, dst);
                ++m_doneFiles;
                emit progress(m_doneBytes, m_totalBytes, m_doneFiles, m_totalFiles, name);
                break;
            }
            if (move && errno != EXDEV && errno != ENOTEMPTY && errno != EEXIST) {
                m_errors << QString::fromUtf8("No se pudo mover «%1»: %2").arg(name, errText());
                break;
            }
            if (copyTree(src, dst, move)) done(src, dst);
            break;
        }
        case Copy:
        case Move: {
            QString dst = m_dest + '/' + name;
            const bool samePlace = QFileInfo(src).absolutePath() == QDir(m_dest).absolutePath();
            if (samePlace && m_kind == Move) break;
            if (samePlace) dst = m_dest + '/' + FileOps::uniqueName(m_dest, name);
            if ((m_dest + '/').startsWith(src + '/')) {
                m_errors << QString::fromUtf8("No se puede copiar «%1» dentro de sí misma.").arg(name);
                break;
            }
            bool skip = false;
            dst = resolve(src, dst, skip);
            if (skip) break;
            if (m_kind == Move && ::rename(enc(src).constData(), enc(dst).constData()) == 0) {
                done(src, dst);
                ++m_doneFiles;
                emit progress(m_doneBytes, m_totalBytes, m_doneFiles, m_totalFiles, name);
                break;
            }
            if (m_kind == Move && errno != EXDEV && errno != ENOTEMPTY && errno != EEXIST) {
                m_errors << QString::fromUtf8("No se pudo mover «%1»: %2").arg(name, errText());
                break;
            }
            if (copyTree(src, dst, m_kind == Move)) done(src, dst);
            break;
        }
        case Link: {
            QString dst = m_dest + '/' + FileOps::uniqueName(m_dest, name);
            if (::symlink(enc(src).constData(), enc(dst).constData()) != 0)
                m_errors << QString::fromUtf8("No se pudo crear el enlace a «%1»: %2").arg(name, errText());
            else done(src, dst);
            ++m_doneFiles;
            break;
        }
        case Delete:
        case EmptyTrash:
            removeTree(src);
            if (m_kind == EmptyTrash) QFile::remove(FileOps::trashInfo() + '/' + name + ".trashinfo");
            break;
        case Restore: {
            QString orig = FileOps::originalPath(src);
            if (orig.isEmpty()) {
                m_errors << QString::fromUtf8("No se sabe de dónde vino «%1».").arg(name);
                break;
            }
            QFileInfo of(orig);
            QDir().mkpath(of.absolutePath());
            if (of.exists() || of.isSymLink()) orig = of.absolutePath() + '/' + FileOps::uniqueName(of.absolutePath(), of.fileName());
            if (::rename(enc(src).constData(), enc(orig).constData()) == 0) {
                QFile::remove(FileOps::trashInfo() + '/' + name + ".trashinfo");
                done(src, orig);
            } else if (errno == EXDEV && copyTree(src, orig, true)) {
                QFile::remove(FileOps::trashInfo() + '/' + name + ".trashinfo");
                done(src, orig);
            } else {
                m_errors << QString::fromUtf8("No se pudo restaurar «%1»: %2").arg(name, errText());
            }
            ++m_doneFiles;
            emit progress(0, 0, m_doneFiles, m_totalFiles, name);
            break;
        }
        }
    }
    if (m_cancel) m_errors << QString::fromUtf8("Cancelado.");
    emit finished(m_errors);
    deleteLater();
}

QString Job::resolve(const QString &src, const QString &dst, bool &skip)
{
    skip = false;
    struct stat st;
    if (lstat(enc(dst).constData(), &st) != 0) return dst;
    // Merging a folder into an existing folder needs no question.
    struct stat ss;
    if (lstat(enc(src).constData(), &ss) == 0 && S_ISDIR(ss.st_mode) && S_ISDIR(st.st_mode)) return dst;
    int answer = m_sticky;
    if (answer < 0) {
        bool all = false;
        int a = Cancel;
        emit conflict(src, dst, &a, &all);
        answer = a;
        if (all) m_sticky = a;
    }
    switch (answer) {
    case Replace:
        removeTree(dst);
        return dst;
    case KeepBoth: {
        QFileInfo fi(dst);
        return fi.absolutePath() + '/' + FileOps::uniqueName(fi.absolutePath(), fi.fileName());
    }
    case Skip:
        skip = true;
        return dst;
    default:
        m_cancel = true;
        skip = true;
        return dst;
    }
}

bool Job::copyFile(const QString &src, const QString &dst)
{
    int in = ::open(enc(src).constData(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        m_errors << QString::fromUtf8("No se pudo leer «%1»: %2").arg(src, errText());
        return false;
    }
    struct stat st;
    fstat(in, &st);
    int out = ::open(enc(dst).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) {
        m_errors << QString::fromUtf8("No se pudo escribir «%1»: %2").arg(dst, errText());
        ::close(in);
        return false;
    }
    const QString name = QFileInfo(src).fileName();
    bool ok = true, useRange = true;
    static thread_local std::vector<char> buf(1 << 20);
    qint64 lastEmit = 0;
    for (;;) {
        if (m_cancel) {
            ok = false;
            break;
        }
        ssize_t n;
        if (useRange) {
            n = copy_file_range(in, nullptr, out, nullptr, 8 << 20, 0);
            if (n < 0 && (errno == EXDEV || errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)) {
                useRange = false;
                continue;
            }
        } else {
            n = ::read(in, buf.data(), buf.size());
            if (n > 0) {
                ssize_t w = 0;
                while (w < n) {
                    ssize_t k = ::write(out, buf.data() + w, size_t(n - w));
                    if (k < 0) {
                        n = -1;
                        break;
                    }
                    w += k;
                }
            }
        }
        if (n < 0) {
            m_errors << QString::fromUtf8("Error copiando «%1»: %2").arg(name, errText());
            ok = false;
            break;
        }
        if (n == 0) break;
        m_doneBytes += n;
        if (m_doneBytes - lastEmit > (4 << 20)) {
            lastEmit = m_doneBytes;
            emit progress(m_doneBytes, m_totalBytes, m_doneFiles, m_totalFiles, name);
        }
    }
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    futimens(out, times);
    ::close(in);
    ::close(out);
    if (!ok) QFile::remove(dst);
    ++m_doneFiles;
    emit progress(m_doneBytes, m_totalBytes, m_doneFiles, m_totalFiles, name);
    return ok;
}

bool Job::copyTree(const QString &src, const QString &dst, bool move)
{
    struct stat st;
    if (lstat(enc(src).constData(), &st) != 0) {
        m_errors << QString::fromUtf8("«%1» ya no existe.").arg(src);
        return false;
    }
    bool ok = true;
    if (S_ISLNK(st.st_mode)) {
        char buf[4096];
        ssize_t n = readlink(enc(src).constData(), buf, sizeof buf - 1);
        if (n < 0 || ::symlink(QByteArray(buf, int(n)).constData(), enc(dst).constData()) != 0) {
            m_errors << QString::fromUtf8("No se pudo copiar el enlace «%1»: %2").arg(src, errText());
            ok = false;
        }
        ++m_doneFiles;
    } else if (S_ISDIR(st.st_mode)) {
        if (::mkdir(enc(dst).constData(), (st.st_mode & 07777) | S_IRWXU) != 0 && errno != EEXIST) {
            m_errors << QString::fromUtf8("No se pudo crear «%1»: %2").arg(dst, errText());
            return false;
        }
        const QStringList kids = QDir(src).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QString &k : kids) {
            if (m_cancel) return false;
            bool skip = false;
            QString target = resolve(src + '/' + k, dst + '/' + k, skip);
            if (skip) {
                ok = false;
                continue;
            }
            ok &= copyTree(src + '/' + k, target, move);
        }
        chmod(enc(dst).constData(), st.st_mode & 07777);
        struct timespec times[2] = {st.st_atim, st.st_mtim};
        utimensat(AT_FDCWD, enc(dst).constData(), times, 0);
    } else {
        ok = copyFile(src, dst);
    }
    if (ok && move) {
        if (S_ISDIR(st.st_mode)) ::rmdir(enc(src).constData());
        else ::unlink(enc(src).constData());
    }
    return ok;
}

bool Job::removeTree(const QString &path)
{
    struct stat st;
    if (lstat(enc(path).constData(), &st) != 0) return true;
    if (S_ISDIR(st.st_mode)) {
        for (const QString &k : QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
            if (m_cancel) return false;
            removeTree(path + '/' + k);
        }
        if (::rmdir(enc(path).constData()) != 0) {
            m_errors << QString::fromUtf8("No se pudo eliminar «%1»: %2").arg(path, errText());
            return false;
        }
        return true;
    }
    if (::unlink(enc(path).constData()) != 0) {
        m_errors << QString::fromUtf8("No se pudo eliminar «%1»: %2").arg(path, errText());
        return false;
    }
    m_doneBytes += S_ISREG(st.st_mode) ? st.st_size : 0;
    ++m_doneFiles;
    if (m_doneFiles % 50 == 0) emit progress(m_doneBytes, m_totalBytes, m_doneFiles, m_totalFiles, QFileInfo(path).fileName());
    return true;
}

// ------------------------------------------------------------------ FileOps
FileOps &FileOps::get()
{
    static FileOps f;
    return f;
}

Job *FileOps::startJob(Job *j)
{
    // Finished copies, moves, links and restores can be undone.
    static const QHash<int, Undo::Type> undoable = {
        {Job::Copy, Undo::Copy}, {Job::Move, Undo::Move}, {Job::Link, Undo::Link}, {Job::Restore, Undo::Restore}};
    if (m_recording && undoable.contains(j->kind()))
        connect(j, &Job::finished, &Undo::get(), [j, type = undoable.value(j->kind())] { Undo::get().record(type, j->pairs()); });
    emit jobStarted(j);
    j->start();
    return j;
}

Job *FileOps::copy(const QStringList &s, const QString &d) { return startJob(new Job(Job::Copy, s, d)); }
Job *FileOps::move(const QStringList &s, const QString &d) { return startJob(new Job(Job::Move, s, d)); }
Job *FileOps::link(const QStringList &s, const QString &d) { return startJob(new Job(Job::Link, s, d)); }
Job *FileOps::deleteForever(const QStringList &p) { return startJob(new Job(Job::Delete, p, {})); }
Job *FileOps::emptyTrash() { return startJob(new Job(Job::EmptyTrash, {}, {})); }
Job *FileOps::restore(const QStringList &p) { return startJob(new Job(Job::Restore, p, {})); }

Job *FileOps::copyTo(const QStringList &s, const QStringList &t)
{
    auto *j = new Job(Job::CopyTo, s, {});
    j->setTargets(t);
    return startJob(j);
}

Job *FileOps::moveTo(const QStringList &s, const QStringList &t)
{
    auto *j = new Job(Job::MoveTo, s, {});
    j->setTargets(t);
    return startJob(j);
}

QString FileOps::trashFiles() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash/files"; }
QString FileOps::trashInfo() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash/info"; }

QString FileOps::originalPath(const QString &trashed)
{
    QFile f(trashInfo() + '/' + QFileInfo(trashed).fileName() + ".trashinfo");
    if (!f.open(QIODevice::ReadOnly)) return {};
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.startsWith("Path=")) return QUrl::fromPercentEncoding(line.mid(5).toUtf8());
    }
    return {};
}

QStringList FileOps::trash(const QStringList &paths, QList<QPair<QString, QString>> *done)
{
    QStringList errors;
    QList<QPair<QString, QString>> moved;
    for (const QString &p : paths) {
        QFile f(p);
        if (!f.moveToTrash()) errors << QString::fromUtf8("No se pudo mover a la papelera «%1»: %2").arg(QFileInfo(p).fileName(), f.errorString());
        else moved.append({p, f.fileName()});   // QFile now names the trashed copy
    }
    if (m_recording) Undo::get().record(Undo::Trash, moved);
    if (done) *done = moved;
    return errors;
}

QString FileOps::uniqueName(const QString &dir, const QString &name)
{
    if (!QFileInfo::exists(dir + '/' + name) && !QFileInfo(dir + '/' + name).isSymLink()) return name;
    QString base = name, ext;
    QFileInfo fi(name);
    if (!name.startsWith('.') && !fi.suffix().isEmpty()) {
        ext = "." + fi.completeSuffix();
        if (ext.size() > 10) ext = "." + fi.suffix();
        base = name.left(name.size() - ext.size());
    }
    static const QRegularExpression copyRe(" \\(copia(?: \\d+)?\\)$");
    base.remove(copyRe);
    for (int i = 1;; ++i) {
        QString n = base + (i == 1 ? QString::fromUtf8(" (copia)") : QString::fromUtf8(" (copia %1)").arg(i)) + ext;
        if (!QFileInfo::exists(dir + '/' + n)) return n;
    }
}

void FileOps::toClipboard(const QStringList &paths, bool cut)
{
    auto *m = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnome = cut ? "cut" : "copy";
    for (const QString &p : paths) {
        QUrl u = QUrl::fromLocalFile(p);
        urls << u;
        gnome += "\n" + u.toEncoded();
    }
    m->setUrls(urls);
    m->setData("x-special/gnome-copied-files", gnome);
    if (cut) m->setData("application/x-kde-cutselection", "1");
    m->setText(paths.join('\n'));
    QApplication::clipboard()->setMimeData(m);
}

QStringList FileOps::fromClipboard(bool *cut)
{
    const QMimeData *m = QApplication::clipboard()->mimeData();
    QStringList out;
    if (cut) *cut = false;
    if (!m) return out;
    if (m->hasFormat("x-special/gnome-copied-files")) {
        QList<QByteArray> lines = m->data("x-special/gnome-copied-files").split('\n');
        if (!lines.isEmpty() && cut) *cut = lines.first().trimmed() == "cut";
    } else if (m->hasFormat("application/x-kde-cutselection") && cut) {
        *cut = m->data("application/x-kde-cutselection") == "1";
    }
    for (const QUrl &u : m->urls())
        if (u.isLocalFile()) out << u.toLocalFile();
    if (out.isEmpty() && m->hasText()) {
        for (const QString &l : m->text().split('\n', Qt::SkipEmptyParts)) {
            QString p = l.trimmed();
            if (p.startsWith("file://")) p = QUrl(p).toLocalFile();
            if (p.startsWith('/') && QFileInfo::exists(p)) out << p;
        }
    }
    return out;
}

bool FileOps::sameDevice(const QString &a, const QString &b)
{
    struct stat sa, sb;
    if (stat(enc(a).constData(), &sa) != 0 || stat(enc(b).constData(), &sb) != 0) return false;
    return sa.st_dev == sb.st_dev;
}
