#include "preview.h"
#include "apps.h"
#include "fsmodel.h"
#include "icons.h"
#include "properties.h"
#include "theme.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QVBoxLayout>
#include <poppler-qt6.h>

namespace {
Preview *g_open = nullptr;
constexpr qint64 kTextBytes = 512 * 1024;

bool isText(const QMimeType &t)
{
    if (t.inherits("text/plain")) return true;
    static const QStringList more = {"application/json", "application/xml", "application/javascript", "application/x-shellscript",
                                     "application/toml", "application/x-yaml", "application/x-desktop", "application/sql"};
    for (const QString &m : more)
        if (t.inherits(m)) return true;
    return false;
}

bool isArchive(const QMimeType &t)
{
    static const QStringList kinds = {"application/zip", "application/x-tar", "application/x-compressed-tar", "application/x-xz-compressed-tar",
                                      "application/x-zstd-compressed-tar", "application/x-bzip2-compressed-tar", "application/x-bzip-compressed-tar",
                                      "application/x-7z-compressed", "application/vnd.rar", "application/x-rar", "application/x-cd-image",
                                      "application/java-archive", "application/vnd.debian.binary-package", "application/x-rpm"};
    for (const QString &k : kinds)
        if (t.inherits(k)) return true;
    return false;
}

// What an archive holds, one path per line (bsdtar reads zip, tar, 7z, rar,
// iso…; 7z is the fallback). Capped so a huge archive stays quick.
QString archiveListing(const QString &path)
{
    constexpr int kMax = 5000;
    QStringList names;
    QProcess p;
    p.start("bsdtar", {"-tf", path});
    p.waitForFinished(20000);
    if (p.exitCode() == 0) {
        names = QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
    } else {
        p.start("7z", {"l", "-ba", "-slt", path});
        p.waitForFinished(20000);
        for (const QString &l : QString::fromUtf8(p.readAllStandardOutput()).split('\n'))
            if (l.startsWith("Path = ")) names << l.mid(7);
    }
    if (names.isEmpty()) return QString::fromUtf8("No se puede leer el contenido.");
    const int total = int(names.size());
    QString head = total == 1 ? QString::fromUtf8("1 elemento\n\n") : QString::fromUtf8("%1 elementos\n\n").arg(total);
    if (total > kMax) names = names.mid(0, kMax) << QString::fromUtf8("… y %1 más").arg(total - kMax);
    return head + names.join('\n');
}

// Runs fn on a pool thread and hands its result to done() on the GUI thread,
// unless the window is gone or has moved on to another file.
template <typename T>
void offThread(Preview *w, const int *request, std::function<T()> fn, std::function<void(const T &)> done)
{
    QPointer<Preview> self(w);
    const int req = *request;
    QThreadPool::globalInstance()->start([self, req, request, fn, done] {
        T result = fn();
        QMetaObject::invokeMethod(qApp, [self, req, request, result, done] {
            if (self && *request == req) done(result);
        });
    });
}
}

bool Preview::isOpen() { return g_open; }

Preview::Preview(const QString &path, std::function<QString(int)> step, QWidget *parent) : QDialog(parent), m_step(std::move(step))
{
    g_open = this;
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle("Vista previa");
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(14, 12, 14, 10);
    v->setSpacing(4);
    m_title = new QLabel;
    m_title->setStyleSheet("font-weight: bold; font-size: 15px;");
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // long names must not widen the window
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_sub = new QLabel;
    m_sub->setProperty("role", "dim");
    m_sub->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    v->addWidget(m_title);
    v->addWidget(m_sub);

    m_stack = new QStackedWidget;
    auto *picPage = new QWidget;
    auto *pv = new QVBoxLayout(picPage);
    pv->setContentsMargins(0, 6, 0, 0);
    m_picture = new QLabel;
    m_picture->setAlignment(Qt::AlignCenter);
    m_picture->setMinimumSize(1, 1);
    m_picture->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_facts = new QLabel;
    m_facts->setAlignment(Qt::AlignCenter);
    m_facts->setProperty("role", "dim");
    m_facts->setWordWrap(true);
    pv->addWidget(m_picture, 1);
    pv->addWidget(m_facts);
    m_text = new QPlainTextEdit;
    m_text->setReadOnly(true);
    m_text->setFocusPolicy(Qt::NoFocus);   // the keys walk the folder, the wheel scrolls
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setFont(QFont(Theme::get().mono));
    m_rich = new QTextBrowser;   // Markdown, formatted
    m_rich->setFocusPolicy(Qt::NoFocus);
    m_rich->setOpenExternalLinks(true);
    m_stack->addWidget(picPage);
    m_stack->addWidget(m_text);
    m_stack->addWidget(m_rich);
    v->addWidget(m_stack, 1);

    m_hint = new QLabel(QString::fromUtf8("←/→ anterior y siguiente  ·  Intro abrir  ·  Espacio o Esc cerrar"));
    m_hint->setProperty("role", "dim");
    m_hint->setAlignment(Qt::AlignCenter);
    m_hint->setStyleSheet("font-size: 11px;");
    v->addWidget(m_hint);

    const QSize host = parent ? parent->window()->size() : QSize(1000, 700);
    resize(std::max(520, host.width() * 7 / 10), std::max(400, host.height() * 8 / 10));
    load(path);
}

Preview::~Preview()
{
    if (g_open == this) g_open = nullptr;
}

QSize Preview::imageTarget() const
{
    const qreal dpr = devicePixelRatioF();
    return QSize(std::max(64, width() - 28), std::max(64, height() - 120)) * dpr;
}

void Preview::load(const QString &path)
{
    ++m_request;
    m_path = path;
    m_image = QImage();
    m_picture->clear();
    m_facts->clear();
    m_text->clear();
    m_page = m_pages = 0;
    const QFileInfo fi(path);
    const QMimeType type = QMimeDatabase().mimeTypeForFile(fi);
    m_mime = type.name();
    m_title->setText(fi.fileName().isEmpty() ? path : fi.fileName());
    m_title->setToolTip(path);
    QStringList sub{type.comment()};
    if (fi.isFile()) sub << FsModel::formatSize(fi.size());
    if (fi.lastModified().isValid()) sub << QString::fromUtf8("modificado ") + FsModel::formatDate(fi.lastModified().toSecsSinceEpoch());
    m_sub->setText(sub.join(QString::fromUtf8("  ·  ")));
    m_stack->setCurrentIndex(0);
    auto bigIcon = [this](const QIcon &icon) { m_picture->setPixmap(icon.pixmap(QSize(160, 160), devicePixelRatioF())); };

    if (fi.isDir()) {
        bigIcon(Icons::get().forFolder(path));
        offThread<int>(this, &m_request, [path] { return int(QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).size()); },
                       [this](const int &n) { m_facts->setText(n == 1 ? QString::fromUtf8("1 elemento") : QString::fromUtf8("%1 elementos").arg(n)); });
        return;
    }
    if (m_mime.startsWith("image/") && QImageReader(path).canRead()) {
        const QSize target = imageTarget();
        offThread<QImage>(this, &m_request, [path, target] {
            QImageReader r(path);
            r.setAutoTransform(true);
            const QSize s = r.size();
            if (s.isValid() && (s.width() > target.width() || s.height() > target.height())) r.setScaledSize(s.scaled(target, Qt::KeepAspectRatio));
            QImage img = r.read();
            if (s.isValid()) img.setText("luna-size", QString::fromUtf8("%1 × %2").arg(s.width()).arg(s.height()));
            return img;
        }, [this](const QImage &img) {
            showImage(img);
            if (!img.text("luna-size").isEmpty()) m_facts->setText(img.text("luna-size"));
        });
        return;
    }
    if (m_mime == "application/pdf") {
        renderPdfPage();
        return;
    }
    if (m_mime.startsWith("video/") || m_mime.startsWith("audio/")) {
        bigIcon(Icons::get().forMime(m_mime));
        const int px = std::max(imageTarget().width(), imageTarget().height());
        offThread<QImage>(this, &m_request, [path, px] {
            // a frame of the video, or the song's embedded cover
            QTemporaryFile tmp(QDir::tempPath() + "/luna-preview-XXXXXX.png");
            if (!tmp.open()) return QImage();
            tmp.close();
            QProcess p;
            p.start("ffmpegthumbnailer", {"-i", path, "-o", tmp.fileName(), "-s", QString::number(std::min(px, 1600)), "-c", "png", "-t", "15%"});
            if (!p.waitForFinished(15000) || p.exitCode() != 0) {
                p.kill();
                return QImage();
            }
            return QImage(tmp.fileName());
        }, [this](const QImage &img) {
            if (!img.isNull()) showImage(img);
        });
        offThread<QList<QPair<QString, QString>>>(this, &m_request, [path] { return mediaSummary(path); },
                                                   [this](const QList<QPair<QString, QString>> &f) { showFacts(f); });
        return;
    }
    if (fi.isFile() && isArchive(type)) {
        // the list of what is inside, without extracting anything
        bigIcon(Icons::get().forMime(m_mime));
        m_text->setPlainText(QString::fromUtf8("Leyendo el contenido…"));
        m_stack->setCurrentIndex(1);
        offThread<QString>(this, &m_request, [path] { return archiveListing(path); }, [this](const QString &t) { m_text->setPlainText(t); });
        return;
    }
    if (fi.isFile() && type.inherits("text/markdown") && fi.size() <= kTextBytes) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            m_rich->setMarkdown(QString::fromUtf8(f.readAll()));
            m_stack->setCurrentIndex(2);
            return;
        }
    }
    if (fi.isFile() && isText(type)) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray data = f.read(kTextBytes);
            if (!data.contains('\0')) {
                QString text = QString::fromUtf8(data);
                if (fi.size() > kTextBytes) text += QString::fromUtf8("\n\n… (solo los primeros 512 KB)");
                m_text->setPlainText(text);
                m_stack->setCurrentIndex(1);
                return;
            }
        }
    }
    bigIcon(Icons::get().forMime(m_mime));
}

void Preview::showImage(const QImage &img)
{
    m_image = img;
    if (img.isNull()) return;
    const QSize box = m_picture->size() * devicePixelRatioF();
    QImage fitted = img.width() > box.width() || img.height() > box.height() ? img.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation) : img;
    QPixmap pm = QPixmap::fromImage(fitted);
    pm.setDevicePixelRatio(devicePixelRatioF());
    m_picture->setPixmap(pm);
}

void Preview::renderPdfPage()
{
    ++m_request;
    const QString path = m_path;
    const int page = m_page;
    const QSize target = imageTarget();
    struct Result {
        QImage img;
        int pages = 0;
    };
    offThread<Result>(this, &m_request, [path, page, target] {
        Result r;
        auto doc = Poppler::Document::load(path);
        if (!doc || doc->isLocked() || doc->numPages() < 1) return r;
        r.pages = doc->numPages();
        doc->setRenderHint(Poppler::Document::Antialiasing);
        doc->setRenderHint(Poppler::Document::TextAntialiasing);
        auto p = doc->page(std::clamp(page, 0, r.pages - 1));
        if (!p) return r;
        const QSizeF pt = p->pageSizeF();
        const double dpi = 72.0 * std::min(target.width() / pt.width(), target.height() / pt.height());
        r.img = p->renderToImage(dpi, dpi);
        return r;
    }, [this](const Result &r) {
        m_pages = r.pages;
        if (r.img.isNull()) {
            m_picture->setPixmap(Icons::get().forMime(m_mime).pixmap(QSize(160, 160), devicePixelRatioF()));
            m_facts->setText(QString::fromUtf8("No se puede mostrar (¿protegido?)"));
            return;
        }
        showImage(r.img);
        m_facts->setText(m_pages > 1 ? QString::fromUtf8("Página %1 de %2  ·  RePág/AvPág para pasar").arg(m_page + 1).arg(m_pages) : QString());
    });
}

void Preview::showFacts(const QList<QPair<QString, QString>> &facts)
{
    QStringList parts;
    for (const auto &kv : facts) parts << QString("%1: <b>%2</b>").arg(kv.first.toHtmlEscaped(), kv.second.toHtmlEscaped());
    m_facts->setText(parts.join(QString::fromUtf8("  ·  ")));
}

void Preview::resizeEvent(QResizeEvent *e)
{
    QDialog::resizeEvent(e);
    if (!m_image.isNull()) showImage(m_image);
}

void Preview::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Space:
    case Qt::Key_Escape:
        close();
        return;
    case Qt::Key_Left:
    case Qt::Key_Up:
    case Qt::Key_Right:
    case Qt::Key_Down: {
        const QString next = m_step ? m_step(e->key() == Qt::Key_Left || e->key() == Qt::Key_Up ? -1 : 1) : QString();
        if (!next.isEmpty()) load(next);
        return;
    }
    case Qt::Key_PageDown:
    case Qt::Key_PageUp:
        if (m_pages > 1) {
            const int page = std::clamp(m_page + (e->key() == Qt::Key_PageDown ? 1 : -1), 0, m_pages - 1);
            if (page != m_page) {
                m_page = page;
                renderPdfPage();
            }
        } else if (m_stack->currentIndex() == 1) {
            m_text->verticalScrollBar()->triggerAction(e->key() == Qt::Key_PageDown ? QAbstractSlider::SliderPageStepAdd : QAbstractSlider::SliderPageStepSub);
        }
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        Apps::get().openDefault({m_path});
        close();
        return;
    default:
        QDialog::keyPressEvent(e);
    }
}
