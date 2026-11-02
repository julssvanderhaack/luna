#include "office.h"
#include "archives.h"
#include "i18n.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <algorithm>

namespace {
QString cacheDir()
{
    static const QString d = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/luna/office";
    return d;
}

QString cacheName(const QFileInfo &fi)
{
    const QByteArray key = QFile::encodeName(fi.absoluteFilePath()) + '|' + QByteArray::number(fi.size()) + '|' +
                           QByteArray::number(fi.lastModified().toMSecsSinceEpoch());
    return cacheDir() + '/' + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex()) + ".pdf";
}

// Oldest PDFs go once the cache passes 300 MB.
void prune()
{
    QFileInfoList files = QDir(cacheDir()).entryInfoList({"*.pdf"}, QDir::Files, QDir::Time);
    qint64 total = 0;
    for (const QFileInfo &f : files) total += f.size();
    while (total > 300LL * 1024 * 1024 && !files.isEmpty()) {
        total -= files.last().size();
        QFile::remove(files.takeLast().absoluteFilePath());
    }
}
}

namespace Office {
bool isOffice(const QString &mime)
{
    return mime.startsWith("application/vnd.openxmlformats-officedocument.") || mime.startsWith("application/vnd.oasis.opendocument.") ||
           mime == "application/msword" || mime.startsWith("application/vnd.ms-word") || mime.startsWith("application/vnd.ms-excel") ||
           mime.startsWith("application/vnd.ms-powerpoint") || mime == "application/rtf" || mime == "text/rtf";
}

bool haveLibreOffice()
{
    static const bool have = !QStandardPaths::findExecutable("soffice").isEmpty();
    return have;
}

QImage embeddedThumbnail(const QString &path)
{
    for (const char *inner : {"docProps/thumbnail.jpeg", "docProps/thumbnail.jpg", "docProps/thumbnail.png", "Thumbnails/thumbnail.png"}) {
        const QByteArray data = Archive::read(path, QString::fromLatin1(inner));
        if (data.isEmpty()) continue;
        QImage img = QImage::fromData(data);
        if (!img.isNull()) return img;
    }
    return {};
}

QString cachedPdf(const QString &path)
{
    const QString c = cacheName(QFileInfo(path));
    return QFileInfo::exists(c) ? c : QString();
}

QString toPdf(const QString &path, QString *error, bool (*stillWanted)(int), int ticket)
{
    static QMutex one;   // soffice locks its profile: never two at once
    QMutexLocker lock(&one);
    const QFileInfo fi(path);
    const QString out = cacheName(fi);
    if (QFileInfo::exists(out)) return out;
    if (stillWanted && !stillWanted(ticket)) return {};
    QDir().mkpath(cacheDir());
    QTemporaryDir tmp(cacheDir() + "/convert-XXXXXX");
    if (!tmp.isValid()) return {};
    const QString profile = QUrl::fromLocalFile(cacheDir() + "/profile").toString();
    QProcess p;
    p.start("soffice", {"-env:UserInstallation=" + profile, "--headless", "--norestore", "--nolockcheck", "--convert-to", "pdf", "--outdir",
                        tmp.path(), fi.absoluteFilePath()});
    if (!p.waitForFinished(120000)) {
        p.kill();
        p.waitForFinished(2000);
        if (error) *error = i18n("LibreOffice tardó demasiado.");
        return {};
    }
    const QString made = tmp.path() + '/' + fi.completeBaseName() + ".pdf";
    if (!QFileInfo::exists(made)) {
        if (error) *error = QString::fromUtf8(p.readAllStandardError()).trimmed();
        return {};
    }
    QFile::remove(out);
    if (!QFile::rename(made, out)) return {};
    prune();
    return out;
}
}
