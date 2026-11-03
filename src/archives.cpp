#include "archives.h"
#include "i18n.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <archive.h>
#include <archive_entry.h>
#include <memory>

namespace {
using Reader = std::unique_ptr<archive, int (*)(archive *)>;

Reader open(const QString &path, QString *error)
{
    Reader a(archive_read_new(), archive_read_free);
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());
    if (archive_read_open_filename(a.get(), QFile::encodeName(path).constData(), 1 << 16) != ARCHIVE_OK) {
        if (error) *error = QString::fromUtf8(archive_error_string(a.get()));
        return Reader(nullptr, archive_read_free);
    }
    return a;
}

QString cleanPath(archive_entry *e)
{
    const char *u = archive_entry_pathname_utf8(e);
    QString p = u ? QString::fromUtf8(u) : QFile::decodeName(archive_entry_pathname(e));
    while (p.startsWith("./")) p.remove(0, 2);
    while (p.startsWith('/')) p.remove(0, 1);
    while (p.endsWith('/')) p.chop(1);
    return p;
}
}

namespace Archive {
Listing list(const QString &path, int cap)
{
    Listing out;
    Reader a = open(path, &out.error);
    if (!a) return out;
    archive_entry *e;
    int r;
    while ((r = archive_read_next_header(a.get(), &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        Entry en;
        en.path = cleanPath(e);
        if (en.path.isEmpty() || en.path == ".") continue;
        en.isDir = archive_entry_filetype(e) == AE_IFDIR;
        if (archive_entry_size_is_set(e)) en.size = archive_entry_size(e);
        if (archive_entry_mtime_is_set(e)) en.mtime = archive_entry_mtime(e);
        en.encrypted = archive_entry_is_encrypted(e);
        out.encrypted |= en.encrypted;
        out.entries << en;
        if (out.entries.size() >= cap) {
            out.truncated = true;
            break;
        }
        archive_read_data_skip(a.get());
    }
    if (r == ARCHIVE_FATAL || r == ARCHIVE_FAILED) {
        if (archive_read_has_encrypted_entries(a.get()) > 0) out.encrypted = true;
        if (out.entries.isEmpty()) out.error = QString::fromUtf8(archive_error_string(a.get()));
    }
    return out;
}

bool extract(const QString &path, const QString &inner, const QString &dest, QString *error)
{
    Reader a = open(path, error);
    if (!a) return false;
    archive_entry *e;
    int r;
    while ((r = archive_read_next_header(a.get(), &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        if (cleanPath(e) != inner) {
            archive_read_data_skip(a.get());
            continue;
        }
        QFile f(dest);
        if (!f.open(QIODevice::WriteOnly)) {
            if (error) *error = f.errorString();
            return false;
        }
        char buf[1 << 16];
        la_ssize_t n;
        while ((n = archive_read_data(a.get(), buf, sizeof buf)) > 0)
            if (f.write(buf, n) != n) {
                if (error) *error = f.errorString();
                return false;
            }
        if (n < 0) {
            if (error) *error = archive_entry_is_encrypted(e) ? i18n("Está cifrado: hace falta la contraseña.") : QString::fromUtf8(archive_error_string(a.get()));
            f.remove();
            return false;
        }
        return true;
    }
    if (error && error->isEmpty()) *error = i18n("No está en el comprimido.");
    return false;
}

QByteArray read(const QString &path, const QString &inner, qint64 maxBytes)
{
    Reader a = open(path, nullptr);
    if (!a) return {};
    archive_entry *e;
    while (archive_read_next_header(a.get(), &e) == ARCHIVE_OK) {
        if (cleanPath(e) != inner) {
            archive_read_data_skip(a.get());
            continue;
        }
        QByteArray out;
        char buf[1 << 16];
        la_ssize_t n;
        while ((n = archive_read_data(a.get(), buf, sizeof buf)) > 0 && out.size() < maxBytes) out.append(buf, n);
        return n < 0 ? QByteArray() : out;
    }
    return {};
}

QString privateDir()
{
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/luna";
    const QString base = cache + "/members";
    if (!QDir().mkpath(base)) return {};
    for (const QString &d : {cache, base})   // the cache may be shared with other tools: keep ours private
        QFile::setPermissions(d, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QDateTime old = QDateTime::currentDateTime().addDays(-1);
    for (const QFileInfo &fi : QDir(base).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
        if (fi.lastModified() < old) QDir(fi.absoluteFilePath()).removeRecursively();
    QTemporaryDir dir(base + "/XXXXXX");   // mkdtemp: random name, 0700
    if (!dir.isValid()) return {};
    dir.setAutoRemove(false);   // the app it opens in reads it after we return
    return dir.path();
}

QString memberFileName(const QString &inner)
{
    const QString name = inner.section('/', -1);
    return name.isEmpty() || name == "." || name == ".." ? QStringLiteral("archivo") : name;
}
}
