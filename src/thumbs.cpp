#include "thumbs.h"
#include "settings.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QUrl>
#include <atomic>
#include <QDateTime>
#include <QMutex>
#include <QTransform>
#include <exiv2/exiv2.hpp>
#include <poppler-qt6.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {
constexpr int kSize = 256;

QThreadPool &pool()
{
    static QThreadPool *p = [] {
        auto *t = new QThreadPool;
        t->setMaxThreadCount(std::max(2, QThread::idealThreadCount() - 1));
        return t;
    }();
    return *p;
}

QString cacheDir(const char *sub)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/thumbnails/" + sub;
}

QString hashFor(const QString &path)
{
    QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
    return QString::fromLatin1(QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex());
}

QImage fit(const QImage &img)
{
    if (img.isNull()) return img;
    if (img.width() <= kSize && img.height() <= kSize) return img;
    return img.scaled(kSize, kSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

// Last time each thumbnail was asked for (by painting): jobs for items that
// scrolled away long ago are skipped and simply requested again later.
QMutex wantedMutex;
QHash<QString, qint64> wanted;

bool stillWanted(const QString &key)
{
    QMutexLocker l(&wantedMutex);
    return QDateTime::currentMSecsSinceEpoch() - wanted.value(key, 0) < 2500;
}

void lowerThreadPriority()
{
    static thread_local bool done = false;
    if (done) return;
    done = true;
    setpriority(PRIO_PROCESS, gettid(), 10);   // pool threads only make thumbnails
}

// The small JPEG preview most cameras and phones embed in the EXIF data:
// decodes in a few milliseconds, good enough until the real one is ready.
QImage exifPreview(const QString &path)
{
    try {
        auto image = Exiv2::ImageFactory::open(QFile::encodeName(path).toStdString());
        if (!image) return {};
        image->readMetadata();
        Exiv2::ExifData &ex = image->exifData();
        if (ex.empty()) return {};
        Exiv2::ExifThumbC thumb(ex);
        Exiv2::DataBuf buf = thumb.copy();
        if (buf.size() == 0) return {};
        QImage img = QImage::fromData(buf.c_data(), int(buf.size()));
        if (img.isNull() || std::max(img.width(), img.height()) < 96) return {};
        auto o = ex.findKey(Exiv2::ExifKey("Exif.Image.Orientation"));
        if (o != ex.end()) {
            QTransform t;
            switch (o->toInt64()) {
            case 3: t.rotate(180); break;
            case 6: t.rotate(90); break;
            case 8: t.rotate(270); break;
            default: break;
            }
            if (!t.isIdentity()) img = img.transformed(t);
        }
        return img;
    } catch (const std::exception &) {
        return {};
    }
}

QImage generate(const QString &path, const QString &mime)
{
    if (mime.startsWith("image/")) {
        QImageReader r(path);
        r.setAutoTransform(true);
        QSize s = r.size();
        if (s.isValid() && (s.width() > kSize || s.height() > kSize)) r.setScaledSize(s.scaled(kSize, kSize, Qt::KeepAspectRatio));
        return fit(r.read());
    }
    if (mime == "application/pdf") {
        auto doc = Poppler::Document::load(path);
        if (!doc || doc->isLocked() || doc->numPages() < 1) return {};
        doc->setRenderHint(Poppler::Document::Antialiasing);
        doc->setRenderHint(Poppler::Document::TextAntialiasing);
        auto page = doc->page(0);
        if (!page) return {};
        QSizeF pt = page->pageSizeF();
        double dpi = 72.0 * kSize / std::max(pt.width(), pt.height());
        return fit(page->renderToImage(dpi, dpi));
    }
    if (mime.startsWith("video/") || mime.startsWith("audio/")) {
        QTemporaryFile tmp(QDir::tempPath() + "/luna-thumb-XXXXXX.png");
        if (!tmp.open()) return {};
        tmp.close();
        QProcess p;
        p.start("ffmpegthumbnailer", {"-i", path, "-o", tmp.fileName(), "-s", QString::number(kSize), "-c", "png", "-t", "15%"});
        if (!p.waitForFinished(15000) || p.exitCode() != 0) {
            p.kill();
            return {};
        }
        return QImage(tmp.fileName());
    }
    return {};
}
} // namespace

void Thumbs::shutdown()
{
    pool().clear();
    pool().waitForDone(5000);
}

Thumbs &Thumbs::get()
{
    static Thumbs t;
    return t;
}

Thumbs::Thumbs() { m_mem.setMaxCost(96 * 1024); }   // ~96 MB of pixmaps

bool Thumbs::canThumb(const QString &mime, qint64 size)
{
    const Settings &s = Settings::get();
    if (mime.startsWith("image/")) return size <= qint64(s.thumbMaxMB) * 1024 * 1024 && mime != "image/x-xcf";
    if (mime == "application/pdf") return s.thumbPdf && size <= qint64(s.thumbMaxMB) * 1024 * 1024;
    if (mime.startsWith("video/")) return s.thumbVideos;
    if (mime == "audio/mpeg" || mime == "audio/flac" || mime == "audio/mp4" || mime == "audio/x-m4a") return s.thumbVideos;
    return false;
}

QPixmap Thumbs::thumbnail(const QString &path, qint64 mtime, const QString &mime)
{
    const QString key = path + '|' + QString::number(mtime);
    if (QPixmap *p = m_mem.object(key)) return *p;
    {
        QMutexLocker l(&wantedMutex);
        wanted.insert(key, QDateTime::currentMSecsSinceEpoch());
    }
    if (m_pending.contains(key) || m_failed.contains(key)) return m_preview.value(key);
    m_pending.insert(key);
    static std::atomic<int> priority{0};
    const QString hash = hashFor(path);
    QPointer<Thumbs> self(this);
    auto post = [self, path, mtime](const QImage &img, bool final) {
        auto *app = QCoreApplication::instance();
        if (!app) return;
        QMetaObject::invokeMethod(app, [self, path, mtime, img, final] {
            if (self) self->deliver(path, mtime, img, final);
        }, Qt::QueuedConnection);
    };
    pool().start([self, path, mtime, mime, hash, key, post] {
        lowerThreadPriority();
        if (!stillWanted(key)) {   // scrolled away: let a future paint ask again
            post(QImage(), false);
            return;
        }
        // 1) any freedesktop cache size already made by Luna, Nautilus…
        for (const char *dir : {"large", "x-large", "normal"}) {
            QImageReader r(cacheDir(dir) + '/' + hash + ".png");
            if (r.canRead() && r.text("Thumb::MTime").toLongLong() == mtime) {
                QImage img = r.read();
                if (!img.isNull()) {
                    post(img, true);
                    return;
                }
            }
        }
        const QString failed = cacheDir("fail/luna-1.0") + '/' + hash + ".png";
        if (QFile::exists(failed)) {
            post(QImage(), true);
            return;
        }
        // 2) instant preview from the EXIF thumbnail
        if (mime == "image/jpeg" || mime == "image/tiff" || mime.startsWith("image/x-") ) {
            QImage quick = exifPreview(path);
            if (!quick.isNull()) post(quick, false);
        }
        // 3) the real thumbnail, shown first and cached afterwards
        QImage img = generate(path, mime);
        post(img, true);
        QImage meta = img.isNull() ? QImage(1, 1, QImage::Format_ARGB32) : img;
        meta.setText("Thumb::URI", QString::fromLatin1(QUrl::fromLocalFile(path).toEncoded()));
        meta.setText("Thumb::MTime", QString::number(mtime));
        meta.setText("Software", "Luna");
        const QString dest = img.isNull() ? failed : cacheDir("large") + '/' + hash + ".png";
        QDir().mkpath(QFileInfo(dest).path());
        const QString tmp = dest + ".luna-tmp";
        QImageWriter w(tmp, "png");
        w.setQuality(40);   // light compression: fast to write, still small
        if (w.write(meta)) {
            QFile::setPermissions(tmp, QFile::ReadOwner | QFile::WriteOwner);
            QFile::rename(tmp, dest) || (QFile::remove(dest) && QFile::rename(tmp, dest));
        }
    }, priority++);
    return {};
}

void Thumbs::deliver(const QString &path, qint64 mtime, const QImage &img, bool final)
{
    const QString key = path + '|' + QString::number(mtime);
    if (!final) {
        if (img.isNull()) {   // skipped as stale: allow a new request
            m_pending.remove(key);
            return;
        }
        m_preview.insert(key, QPixmap::fromImage(img));
        emit ready(path);
        return;
    }
    m_pending.remove(key);
    m_preview.remove(key);
    {
        QMutexLocker l(&wantedMutex);   // done with it: keep the map small
        wanted.remove(key);
    }
    if (img.isNull()) {
        m_failed.insert(key);
        return;
    }
    auto *px = new QPixmap(QPixmap::fromImage(img));
    m_mem.insert(key, px, std::max(1, int(qint64(px->width()) * px->height() * 4 / 1024)));   // cost in KB
    emit ready(path);
}
