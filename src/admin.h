// Folders only root can read, the way Nautilus opens them: through GVFS's
// admin backend (gvfsd-admin, started with pkexec) and GIO, from Luna's own
// process. polkit asks for the password the first time and keeps it a few
// minutes. The admin backend serves only the process that mounted it, so
// unlike sftp it never shows up under the FUSE bridge: every access goes
// through the functions here.
//
// Inside Luna such a folder is the path "admin:///etc" (the local path,
// unescaped, after "admin://"). The functions below take that form or a plain
// local path, so a copy can go from one to the other; they block, so call them
// off the GUI thread (the first one may wait while the password is typed).
#pragma once
#include <QString>
#include <QStringList>
#include <QUrl>
#include <atomic>
#include <functional>
#include <vector>

struct FileEntry;

namespace Admin {
bool is(const QString &path);
QString toAdmin(const QString &localPath);    // "/etc" → "admin:///etc"
QString localPart(const QString &path);       // "admin:///etc" → "/etc"; anything else as it is
QString clean(const QString &path);           // QDir::cleanPath that keeps "admin://"
QString parent(const QString &path);
QString fileName(const QString &path);
QUrl url(const QString &path);                // for the clipboard and drags
QString fromUrl(const QUrl &url);             // admin:// or file:// URL → path; "" otherwise

bool list(const QString &dir, std::vector<FileEntry> &out, QString *error);
bool exists(const QString &path, bool *isDir = nullptr);
bool remove(const QString &path, QString *error);   // whole trees; links are removed, never followed
bool rename(const QString &path, const QString &newName, QString *error, QString *newPath = nullptr);
bool makeDir(const QString &path, QString *error);
bool makeFile(const QString &path, QString *error);

struct CopyOptions {
    bool move = false;
    const std::atomic<bool> *cancel = nullptr;
    // Where an item inside a folder goes when its name is taken (like
    // Job::resolve); skip = leave it. Unset: the copy fails on it.
    std::function<QString(const QString &src, const QString &dst, bool &skip)> resolve;
    std::function<void(qint64 bytes)> progress;   // bytes copied since the last call
};
// Copies (or moves) a file, link or folder tree. Links are copied as links;
// permissions follow FileOps::copiedMode (no setuid/setgid), times are kept.
bool copy(const QString &src, const QString &dst, const CopyOptions &options, QStringList *errors);
}
