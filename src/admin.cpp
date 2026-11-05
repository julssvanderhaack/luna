#include "admin.h"
#include "fileops.h"
#include "fsmodel.h"
#include "i18n.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMutex>
#include <memory>
#include <sys/stat.h>
#undef signals   // Qt's keyword clashes with a field name in GIO's headers
#include <gio/gio.h>

namespace {
const QString prefix = QStringLiteral("admin://");

struct Unref {
    void operator()(gpointer p) const { if (p) g_object_unref(p); }
};
using File = std::unique_ptr<GFile, Unref>;
using Info = std::unique_ptr<GFileInfo, Unref>;
using Enumerator = std::unique_ptr<GFileEnumerator, Unref>;
using Cancellable = std::unique_ptr<GCancellable, Unref>;

// "admin:///etc/mi archivo" → admin:///etc/mi%20archivo; local paths as they are.
File fileFor(const QString &path)
{
    if (!Admin::is(path)) return File(g_file_new_for_path(QFile::encodeName(path).constData()));
    const QByteArray uri = "admin" + QUrl::fromLocalFile(Admin::localPart(path)).toEncoded().mid(4);
    return File(g_file_new_for_uri(uri.constData()));
}

QString message(GError *e, bool admin)
{
    if (!e) return {};
    if (admin && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
        return i18n("Sin la contraseña de administrador no se puede entrar.");
    if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED) || g_error_matches(e, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
        return i18n("Cancelado.");
    return QString::fromUtf8(e->message);
}

// Mounts admin:/// (pkexec asks for the password) on a private main
// context, so it works from any thread whatever Qt's event loop is.
bool mountAdmin(GError **error)
{
    static QMutex lock;   // one password dialog, not one per thread
    QMutexLocker l(&lock);
    File root(g_file_new_for_uri("admin:///"));
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    struct State {
        bool done = false;
        GError *error = nullptr;
    } st;
    g_file_mount_enclosing_volume(root.get(), G_MOUNT_MOUNT_NONE, nullptr, nullptr,
                                  [](GObject *src, GAsyncResult *res, gpointer data) {
                                      auto *s = static_cast<State *>(data);
                                      g_file_mount_enclosing_volume_finish(G_FILE(src), res, &s->error);
                                      s->done = true;
                                  },
                                  &st);
    while (!st.done) g_main_context_iteration(ctx, TRUE);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    if (st.error && g_error_matches(st.error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) g_clear_error(&st.error);
    if (!st.error) return true;
    g_propagate_error(error, st.error);
    return false;
}

// Runs op; when the admin backend is not mounted (first use, or it went
// away), mounts it and tries once more.
template<typename Op>
bool run(bool admin, GError **error, Op op)
{
    if (op(error)) return true;
    if (!admin || !g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED)) return false;
    g_clear_error(error);
    return mountAdmin(error) && op(error);
}

const char *entryAttrs = "standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target,"
                         "time::modified,unix::mode,unix::uid,unix::gid";

void fillStat(FileEntry &e, GFileInfo *info)
{
    e.isDir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    e.size = e.isDir ? -1 : g_file_info_get_size(info);
    e.mtime = qint64(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED));
    e.mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
    e.uid = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID);
    e.gid = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_GID);
}

GFileType typeOf(GFile *f, GError **error)
{
    Info info(g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, error));
    return info ? g_file_info_get_file_type(info.get()) : G_FILE_TYPE_UNKNOWN;
}

bool removeTree(GFile *f, GError **error)
{
    const GFileType t = typeOf(f, error);
    if (t == G_FILE_TYPE_UNKNOWN && *error) return false;
    if (t == G_FILE_TYPE_DIRECTORY) {
        Enumerator en(g_file_enumerate_children(f, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, error));
        if (!en) return false;
        while (GFileInfo *raw = g_file_enumerator_next_file(en.get(), nullptr, error)) {
            Info info(raw);
            File child(g_file_get_child(f, g_file_info_get_name(info.get())));
            if (!removeTree(child.get(), error)) return false;
        }
        if (*error) return false;
    }
    return g_file_delete(f, nullptr, error);
}

struct Copier {
    const Admin::CopyOptions &o;
    QStringList *errors;
    GCancellable *cancel;
    goffset reported = 0;

    bool cancelled() const { return (o.cancel && *o.cancel) || g_cancellable_is_cancelled(cancel); }

    void fail(const QString &what, GError *e, bool admin)
    {
        if (errors && !g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) errors->append(QStringLiteral("«%1»: %2").arg(what, message(e, admin)));
        g_clear_error(&e);
    }

    static void onProgress(goffset current, goffset, gpointer data)
    {
        auto *self = static_cast<Copier *>(data);
        if (self->o.cancel && *self->o.cancel) g_cancellable_cancel(self->cancel);
        if (self->o.progress && current > self->reported) self->o.progress(current - self->reported);
        self->reported = current;
    }

    // Like cp: the copy gets the source's permissions minus setuid/setgid
    // (the sticky bit stays on folders), and its modification time.
    void finish(GFile *dst, GFileInfo *srcInfo, bool dir)
    {
        const guint32 mode = g_file_info_get_attribute_uint32(srcInfo, G_FILE_ATTRIBUTE_UNIX_MODE);
        if (mode) g_file_set_attribute_uint32(dst, G_FILE_ATTRIBUTE_UNIX_MODE, FileOps::copiedMode(mode, dir), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
        if (g_file_info_has_attribute(srcInfo, G_FILE_ATTRIBUTE_TIME_MODIFIED))
            g_file_set_attribute_uint64(dst, G_FILE_ATTRIBUTE_TIME_MODIFIED, g_file_info_get_attribute_uint64(srcInfo, G_FILE_ATTRIBUTE_TIME_MODIFIED),
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
    }

    bool item(const QString &src, const QString &dst)
    {
        if (cancelled()) return false;
        const bool admin = Admin::is(src) || Admin::is(dst);
        File s = fileFor(src), d = fileFor(dst);
        GError *e = nullptr;
        Info info;
        if (!run(admin, &e, [&](GError **err) {
                info.reset(g_file_query_info(s.get(), "standard::type,unix::mode,time::modified", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, err));
                return bool(info);
            })) {
            fail(Admin::fileName(src), e, admin);
            return false;
        }
        const GFileType type = g_file_info_get_file_type(info.get());

        // Within one backend a move is a rename; GIO says so when it is not.
        if (o.move && g_file_move(s.get(), d.get(), GFileCopyFlags(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE), cancel, nullptr, nullptr, &e))
            return true;
        if (e) {
            const bool tryCopy = g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) || g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE) ||
                                 g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE) || g_error_matches(e, G_IO_ERROR, G_IO_ERROR_EXISTS);
            if (!tryCopy) {
                fail(Admin::fileName(src), e, admin);
                return false;
            }
            g_clear_error(&e);
        }

        if (type == G_FILE_TYPE_DIRECTORY) {
            if (!g_file_make_directory(d.get(), cancel, &e)) {
                if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_EXISTS)) {   // an existing folder: merge into it
                    fail(Admin::fileName(dst), e, admin);
                    return false;
                }
                g_clear_error(&e);
            }
            Enumerator en(g_file_enumerate_children(s.get(), G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &e));
            if (!en) {
                fail(Admin::fileName(src), e, admin);
                return false;
            }
            bool ok = true;
            while (GFileInfo *raw = g_file_enumerator_next_file(en.get(), cancel, &e)) {
                Info child(raw);
                const QString name = QFile::decodeName(g_file_info_get_name(child.get()));
                const QString cs = src + '/' + name;
                QString cd = dst + '/' + name;
                bool skip = false;
                if (o.resolve) cd = o.resolve(cs, cd, skip);
                if (skip) {
                    ok = false;
                    if (cancelled()) break;
                    continue;
                }
                ok &= item(cs, cd);
                if (cancelled()) return false;
            }
            if (e) {
                fail(Admin::fileName(src), e, admin);
                ok = false;
            }
            finish(d.get(), info.get(), true);
            if (ok && o.move && !g_file_delete(s.get(), nullptr, &e)) {
                fail(Admin::fileName(src), e, admin);
                ok = false;
            }
            return ok;
        }

        // Files and links (copied as links). TARGET_DEFAULT_PERMS: never create
        // the copy with a setuid bit, not even for an instant.
        reported = 0;
        const auto flags = GFileCopyFlags(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_TARGET_DEFAULT_PERMS);
        if (!g_file_copy(s.get(), d.get(), flags, cancel, &Copier::onProgress, this, &e)) {
            fail(Admin::fileName(src), e, admin);
            return false;
        }
        if (type != G_FILE_TYPE_SYMBOLIC_LINK) finish(d.get(), info.get(), false);
        if (o.move && !g_file_delete(s.get(), nullptr, &e)) {
            fail(Admin::fileName(src), e, admin);
            return false;
        }
        return true;
    }
};
}

namespace Admin {
bool is(const QString &path) { return path.startsWith(prefix); }

QString toAdmin(const QString &localPath) { return is(localPath) ? localPath : prefix + QDir::cleanPath(localPath); }

QString localPart(const QString &path)
{
    if (!is(path)) return path;
    const QString p = path.mid(prefix.size());
    return p.isEmpty() ? QStringLiteral("/") : p;
}

QString clean(const QString &path) { return is(path) ? prefix + QDir::cleanPath(localPart(path)) : QDir::cleanPath(path); }

QString parent(const QString &path)
{
    const QString p = QFileInfo(localPart(path)).absolutePath();
    return is(path) ? prefix + p : p;
}

QString fileName(const QString &path) { return QFileInfo(localPart(path)).fileName(); }

QUrl url(const QString &path)
{
    QUrl u = QUrl::fromLocalFile(localPart(path));
    if (is(path)) u.setScheme(QStringLiteral("admin"));
    return u;
}

QString fromUrl(const QUrl &u)
{
    if (u.isLocalFile()) return u.toLocalFile();
    if (u.scheme() == QLatin1String("admin") && u.host().isEmpty()) return clean(prefix + u.path(QUrl::FullyDecoded));
    return {};
}

bool list(const QString &dir, std::vector<FileEntry> &out, QString *error)
{
    const bool admin = is(dir);
    File f = fileFor(dir);
    GError *e = nullptr;
    Enumerator en;
    if (!run(admin, &e, [&](GError **err) {
            en.reset(g_file_enumerate_children(f.get(), entryAttrs, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, err));
            return bool(en);
        })) {
        if (error) *error = message(e, admin);
        g_clear_error(&e);
        return false;
    }
    static thread_local QMimeDatabase db;
    while (GFileInfo *raw = g_file_enumerator_next_file(en.get(), nullptr, &e)) {
        Info info(raw);
        FileEntry x;
        x.name = QFile::decodeName(g_file_info_get_name(info.get()));
        x.hidden = x.name.startsWith('.');
        fillStat(x, info.get());
        if (g_file_info_get_is_symlink(info.get())) {
            x.isLink = true;
            if (const char *t = g_file_info_get_symlink_target(info.get())) x.linkTarget = QFile::decodeName(t);
            File child(g_file_get_child(f.get(), g_file_info_get_name(info.get())));
            Info target(g_file_query_info(child.get(), entryAttrs, G_FILE_QUERY_INFO_NONE, nullptr, nullptr));
            // A broken link comes back as the link itself, not as an error.
            if (target && g_file_info_get_file_type(target.get()) != G_FILE_TYPE_SYMBOLIC_LINK) fillStat(x, target.get());
            else x.broken = true;
        }
        x.mime = x.broken ? QStringLiteral("inode/symlink")
                 : x.isDir ? QStringLiteral("inode/directory")
                           : db.mimeTypeForFile(x.name, QMimeDatabase::MatchExtension).name();
        out.push_back(std::move(x));
    }
    if (e) {
        if (error) *error = message(e, admin);
        g_clear_error(&e);
        return false;
    }
    return true;
}

bool exists(const QString &path, bool *isDir)
{
    File f = fileFor(path);
    GError *e = nullptr;
    GFileType t = G_FILE_TYPE_UNKNOWN;
    const bool ok = run(is(path), &e, [&](GError **err) {
        t = typeOf(f.get(), err);
        return t != G_FILE_TYPE_UNKNOWN || !*err;
    });
    g_clear_error(&e);
    if (isDir) *isDir = ok && t == G_FILE_TYPE_DIRECTORY;
    return ok;
}

bool remove(const QString &path, QString *error)
{
    File f = fileFor(path);
    GError *e = nullptr;
    if (run(is(path), &e, [&](GError **err) { return removeTree(f.get(), err); })) return true;
    if (error) *error = message(e, is(path));
    g_clear_error(&e);
    return false;
}

bool rename(const QString &path, const QString &newName, QString *error, QString *newPath)
{
    if (newName.isEmpty() || newName.contains('/') || newName == "." || newName == "..") {
        if (error) *error = i18n("Ese nombre no vale.");
        return false;
    }
    File f = fileFor(path);
    GError *e = nullptr;
    const QByteArray name = newName.toUtf8();
    if (run(is(path), &e, [&](GError **err) { return File(g_file_set_display_name(f.get(), name.constData(), nullptr, err)) != nullptr; })) {
        if (newPath) *newPath = parent(path) + '/' + newName;
        return true;
    }
    if (error) *error = message(e, is(path));
    g_clear_error(&e);
    return false;
}

bool makeDir(const QString &path, QString *error)
{
    File f = fileFor(path);
    GError *e = nullptr;
    if (run(is(path), &e, [&](GError **err) { return bool(g_file_make_directory(f.get(), nullptr, err)); })) return true;
    if (error) *error = message(e, is(path));
    g_clear_error(&e);
    return false;
}

bool makeFile(const QString &path, QString *error)
{
    File f = fileFor(path);
    GError *e = nullptr;
    const bool ok = run(is(path), &e, [&](GError **err) {
        GFileOutputStream *out = g_file_create(f.get(), G_FILE_CREATE_NONE, nullptr, err);
        if (!out) return false;
        g_output_stream_close(G_OUTPUT_STREAM(out), nullptr, nullptr);
        g_object_unref(out);
        return true;
    });
    if (!ok && error) *error = message(e, is(path));
    g_clear_error(&e);
    return ok;
}

bool copy(const QString &src, const QString &dst, const CopyOptions &options, QStringList *errors)
{
    Cancellable cancel(g_cancellable_new());
    Copier c{options, errors, cancel.get()};
    return c.item(src, dst);
}
}
