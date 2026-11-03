// Reading archives in place with libarchive (zip, tar.*, 7z, rar, iso,
// cab, deb…): list what is inside and pull one member out, without
// extracting the rest. Both block: call them off the GUI thread.
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace Archive {
struct Entry {
    QString path;        // inside the archive, '/'-separated, no leading "./"
    qint64 size = -1;
    qint64 mtime = 0;    // seconds since the epoch, 0 = unknown
    bool isDir = false;
    bool encrypted = false;
};

struct Listing {
    QList<Entry> entries;
    bool truncated = false;   // stopped at the cap
    bool encrypted = false;   // some member (or the header) needs a password
    QString error;            // empty when it could be read
};

Listing list(const QString &archive, int cap = 100000);
// Writes member `inner` to `dest`; false (and *error) when it can't.
bool extract(const QString &archive, const QString &inner, const QString &dest, QString *error = nullptr);
// A small member read into memory (embedded thumbnails), up to maxBytes.
QByteArray read(const QString &archive, const QString &inner, qint64 maxBytes = 8 << 20);
// A new folder only the user can enter (0700, random name) under
// ~/.cache/luna/members for a member opened from the preview; ones older than
// a day are removed. Empty if it can't be made.
QString privateDir();
// The file name a member gets there: its last path part, never "", "." or "..".
QString memberFileName(const QString &inner);
}
