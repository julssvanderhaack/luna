#include "preview.h"
#include "i18n.h"
#include "apps.h"
#include "clip.h"
#include "archives.h"
#include "fsmodel.h"
#include "icons.h"
#include "office.h"
#include "properties.h"
#include "settings.h"
#include "theme.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QHBoxLayout>
#include <QTimer>
#include <QToolButton>
#include <QProcess>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <poppler-qt6.h>
#include <atomic>
#include <functional>

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

// Bumped on every file shown: an office conversion queued for a file the
// preview has left is skipped.
std::atomic<int> g_officeTicket{0};
bool officeWanted(int ticket) { return ticket == g_officeTicket.load(); }

// Folders first, then by the sort column, the way the panes sort.
class ArchiveItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool isDir = false;
    qint64 bytes = 0;
    bool operator<(const QTreeWidgetItem &o) const override
    {
        const auto &b = static_cast<const ArchiveItem &>(o);
        if (isDir != b.isDir) return isDir;
        const int col = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (col == 1 && bytes != b.bytes) return bytes < b.bytes;
        if (col == 2) return data(2, Qt::UserRole).toLongLong() < b.data(2, Qt::UserRole).toLongLong();
        return text(0).compare(b.text(0), Qt::CaseInsensitive) < 0;
    }
};

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
    setWindowTitle(i18n("Vista previa"));
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
    m_tree = new QTreeWidget;   // the mouse opens folders; the keys still walk the folder outside
    m_tree->setFocusPolicy(Qt::NoFocus);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({i18n("Nombre"), i18n("Tamaño"), i18n("Modificado")});
    m_tree->setUniformRowHeights(true);
    m_tree->setSortingEnabled(true);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) { openArchiveMember(it); });
    m_stack->addWidget(m_tree);
    v->addWidget(m_stack, 1);

    m_player = new QWidget;
    auto *pl = new QHBoxLayout(m_player);
    pl->setContentsMargins(0, 4, 0, 0);
    m_playBtn = new QToolButton;
    m_playBtn->setAutoRaise(true);
    m_playBtn->setIconSize(QSize(26, 26));   // the Nerd Font glyph fills about half of it
    m_playBtn->setFixedSize(32, 32);
    m_playBtn->setFocusPolicy(Qt::NoFocus);   // Space still closes, the arrows still walk
    connect(m_playBtn, &QToolButton::clicked, this, &Preview::playClip);
    m_playBar = new QProgressBar;
    m_playBar->setTextVisible(false);
    m_playBar->setFixedHeight(4);
    m_playBar->setRange(0, 1000);
    m_playTime = new QLabel;
    m_playTime->setProperty("role", "dim");
    pl->addWidget(m_playBtn);
    pl->addWidget(m_playBar, 1);
    pl->addWidget(m_playTime);
    m_player->hide();
    m_playTick = new QTimer(this);
    m_playTick->setInterval(250);   // only while a fragment plays
    connect(m_playTick, &QTimer::timeout, this, &Preview::updatePlayer);
    v->addWidget(m_player);
    v->addWidget(m_facts);

    m_hint = new QLabel(i18n("←/→ anterior y siguiente  ·  Intro abrir  ·  Espacio o Esc cerrar"));
    m_hint->setProperty("role", "dim");
    m_hint->setAlignment(Qt::AlignCenter);
    m_hint->setStyleSheet("font-size: 11px;");
    v->addWidget(m_hint);

    const QSize host = parent ? parent->window()->size() : QSize(1000, 700);
    resize(std::max(520, host.width() * 7 / 10), std::max(400, host.height() * 8 / 10));
    load(path);
}

namespace {
QString clock(double s)
{
    const int t = int(s + 0.5);
    return QString("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
}
}

void Preview::playClip()
{
    if (m_clip && m_clip->playing()) {
        m_clip->setPaused(!m_clip->paused());
        updatePlayer();
        return;
    }
    m_clip = std::make_unique<AudioClip>(m_path, 30);
    AudioClip *c = m_clip.get();
    connect(c, &AudioClip::started, this, [this](double start, double length) {
        m_playBar->setValue(0);
        m_playBtn->setToolTip(i18n("Pausa (P)"));
        m_player->setToolTip(i18n("Fragmento de %1 a %2").arg(clock(start), clock(start + length)));
        m_player->show();
        m_hint->setText(i18n("P pausa  ·  ←/→ anterior y siguiente  ·  Intro abrir  ·  Espacio o Esc cerrar"));
        m_playTick->start();
        updatePlayer();
    });
    connect(c, &AudioClip::finished, this, [this] {
        m_playTick->stop();
        updatePlayer();
    });
    connect(c, &AudioClip::failed, this, [this] {
        m_playTick->stop();
        m_player->hide();
    });
}

void Preview::updatePlayer()
{
    if (!m_clip) return;
    const bool on = m_clip->playing() && !m_clip->paused();
    m_playBtn->setIcon(Icons::get().ui(on ? "pause" : "play"));
    if (!m_clip->playing()) m_playBtn->setToolTip(i18n("Escuchar otra vez (P)"));
    else m_playBtn->setToolTip(on ? i18n("Pausa (P)") : i18n("Seguir (P)"));
    const double len = std::max(0.1, m_clip->length());
    const double pos = m_clip->playing() ? m_clip->position() : len;
    m_playBar->setValue(int(1000 * pos / len));
    // where in the song: "1:47 · fragmento 1:32–2:02"
    m_playTime->setText(i18n("%1 · fragmento %2–%3").arg(clock(m_clip->start() + pos), clock(m_clip->start()), clock(m_clip->start() + len)));
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
    ++g_officeTicket;
    m_path = path;
    m_pdfPath = path;
    m_image = QImage();
    m_picture->clear();
    m_facts->clear();
    m_text->clear();
    m_page = m_pages = 0;
    m_clip.reset();
    m_playTick->stop();
    m_player->hide();
    m_hint->setText(i18n("←/→ anterior y siguiente  ·  Intro abrir  ·  Espacio o Esc cerrar"));
    const QFileInfo fi(path);
    const QMimeType type = QMimeDatabase().mimeTypeForFile(fi);
    m_mime = type.name();
    m_title->setText(fi.fileName().isEmpty() ? path : fi.fileName());
    m_title->setToolTip(path);
    QStringList sub{type.comment()};
    if (fi.isFile()) sub << FsModel::formatSize(fi.size());
    if (fi.lastModified().isValid()) sub << i18n("modificado ") + FsModel::formatDate(fi.lastModified().toSecsSinceEpoch());
    m_sub->setText(sub.join(QString::fromUtf8("  ·  ")));
    m_stack->setCurrentIndex(0);
    auto bigIcon = [this](const QIcon &icon) { m_picture->setPixmap(icon.pixmap(QSize(160, 160), devicePixelRatioF())); };

    if (fi.isDir()) {
        bigIcon(Icons::get().forFolder(path));
        offThread<int>(this, &m_request, [path] { return int(QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).size()); },
                       [this](const int &n) { m_facts->setText(n == 1 ? i18n("1 elemento") : i18n("%1 elementos").arg(n)); });
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
        if (m_mime.startsWith("audio/") && AudioClip::available() && Settings::get().previewAudio) playClip();
        return;
    }
    if (fi.isFile() && Office::isOffice(m_mime)) {
        // the first page: the picture the document carries at once, then
        // LibreOffice's PDF (cached), which also turns pages
        bigIcon(Icons::get().forMime(m_mime));
        if (const QString pdf = Office::cachedPdf(path); !pdf.isEmpty()) {
            m_pdfPath = pdf;
            renderPdfPage();
            return;
        }
        offThread<QImage>(this, &m_request, [path] { return Office::embeddedThumbnail(path); }, [this](const QImage &img) {
            if (!img.isNull() && m_pdfPath == m_path) showImage(img);
        });
        if (!Office::haveLibreOffice()) {
            m_facts->setText(i18n("Instala LibreOffice para ver las páginas"));
            return;
        }
        m_facts->setText(i18n("Preparando las páginas con LibreOffice…"));
        const int ticket = g_officeTicket.load();
        struct Result {
            QString pdf, error;
        };
        offThread<Result>(this, &m_request, [path, ticket] {
            Result r;
            r.pdf = Office::toPdf(path, &r.error, officeWanted, ticket);
            return r;
        }, [this](const Result &r) {
            if (r.pdf.isEmpty()) {
                m_facts->setText(r.error.isEmpty() ? i18n("LibreOffice no pudo abrirlo") : r.error.left(200));
                return;
            }
            m_pdfPath = r.pdf;
            renderPdfPage();
        });
        return;
    }
    if (fi.isFile() && isArchive(type)) {
        // what is inside, as a tree, without extracting anything
        m_tree->clear();
        m_facts->setText(i18n("Leyendo el contenido…"));
        bigIcon(Icons::get().forMime(m_mime));
        offThread<Archive::Listing>(this, &m_request, [path] { return Archive::list(path, 50000); },
                                    [this](const Archive::Listing &l) { showArchive(l); });
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
                if (fi.size() > kTextBytes) text += i18n("\n\n… (solo los primeros 512 KB)");
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
    const QString path = m_pdfPath;
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
            m_facts->setText(i18n("No se puede mostrar (¿protegido?)"));
            return;
        }
        showImage(r.img);
        m_facts->setText(m_pages > 1 ? i18n("Página %1 de %2  ·  RePág/AvPág para pasar").arg(m_page + 1).arg(m_pages) : QString());
    });
}

void Preview::showArchive(const Archive::Listing &l)
{
    if (l.entries.isEmpty()) {
        m_facts->setText(l.encrypted ? i18n("Comprimido cifrado: hace falta la contraseña para ver su contenido")
                                     : i18n("No se puede leer el contenido.") + (l.error.isEmpty() ? QString() : "\n" + l.error));
        return;
    }
    m_tree->setUpdatesEnabled(false);
    m_tree->setSortingEnabled(false);
    m_tree->clear();
    QHash<QString, ArchiveItem *> dirs;
    QMimeDatabase db;
    Icons &ic = Icons::get();
    // the item of folder `dir`, made with its parents if needed (zips often list no folders)
    std::function<ArchiveItem *(const QString &)> folder = [&](const QString &dir) -> ArchiveItem * {
        if (dir.isEmpty()) return nullptr;
        if (auto *it = dirs.value(dir)) return it;
        ArchiveItem *parent = folder(dir.contains('/') ? dir.section('/', 0, -2) : QString());
        auto *it = parent ? new ArchiveItem(parent) : new ArchiveItem(m_tree);
        it->isDir = true;
        it->setText(0, dir.section('/', -1));
        it->setIcon(0, ic.forFolder(dir));
        it->setData(0, Qt::UserRole, dir);
        dirs.insert(dir, it);
        return it;
    };
    int files = 0;
    qint64 total = 0;
    for (const Archive::Entry &e : l.entries) {
        if (e.isDir) {
            if (ArchiveItem *it = folder(e.path); it && e.mtime) {
                it->setText(2, FsModel::formatDate(e.mtime));
                it->setData(2, Qt::UserRole, e.mtime);
            }
            continue;
        }
        ArchiveItem *parent = folder(e.path.contains('/') ? e.path.section('/', 0, -2) : QString());
        auto *it = parent ? new ArchiveItem(parent) : new ArchiveItem(m_tree);
        const QString name = e.path.section('/', -1);
        it->setText(0, e.encrypted ? name + QString::fromUtf8("  🔒") : name);
        it->setIcon(0, ic.forMime(db.mimeTypeForFile(name, QMimeDatabase::MatchExtension).name()));
        it->setData(0, Qt::UserRole, e.path);
        it->bytes = std::max<qint64>(0, e.size);
        if (e.size >= 0) it->setText(1, FsModel::formatSize(e.size));
        it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        if (e.mtime) {
            it->setText(2, FsModel::formatDate(e.mtime));
            it->setData(2, Qt::UserRole, e.mtime);
        }
        for (auto *p = parent; p; p = static_cast<ArchiveItem *>(p->parent())) p->bytes += it->bytes;
        total += it->bytes;
        ++files;
    }
    for (ArchiveItem *d : std::as_const(dirs)) {
        d->setText(1, FsModel::formatSize(d->bytes));
        d->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    }
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(0, Qt::AscendingOrder);
    if (m_tree->topLevelItemCount() == 1) m_tree->topLevelItem(0)->setExpanded(true);   // the usual single top folder
    m_tree->setUpdatesEnabled(true);
    m_stack->setCurrentIndex(3);
    QStringList facts{files == 1 ? i18n("1 archivo") : i18n("%1 archivos").arg(files),
                      dirs.size() == 1 ? i18n("1 carpeta") : i18n("%1 carpetas").arg(dirs.size()),
                      i18n("%1 sin comprimir").arg(FsModel::formatSize(total))};
    if (l.truncated) facts << i18n("solo los primeros %1").arg(l.entries.size());
    if (l.encrypted) facts << i18n("🔒 con contraseña");
    m_facts->setText(facts.join(QString::fromUtf8("  ·  ")) + "\n" + i18n("Doble clic abre un archivo sin extraer el resto"));
}

// Pulls one file out to a temporary folder and opens it.
void Preview::openArchiveMember(QTreeWidgetItem *item)
{
    auto *it = static_cast<ArchiveItem *>(item);
    if (!it || it->isDir) return;
    const QString archive = m_path, inner = it->data(0, Qt::UserRole).toString();
    const QString dir = QDir::tempPath() + "/luna-" + qEnvironmentVariable("USER") + "/" + QString::number(QDateTime::currentMSecsSinceEpoch(), 36);
    const QString dest = dir + '/' + inner.section('/', -1);
    m_facts->setText(i18n("Extrayendo «%1»…").arg(inner.section('/', -1)));
    struct Result {
        bool ok = false;
        QString error;
    };
    offThread<Result>(this, &m_request, [archive, inner, dir, dest] {
        Result r;
        QDir().mkpath(dir);
        r.ok = Archive::extract(archive, inner, dest, &r.error);
        return r;
    }, [this, dest](const Result &r) {
        if (!r.ok) {
            m_facts->setText(i18n("No se pudo extraer: %1").arg(r.error));
            return;
        }
        m_facts->setText(i18n("Abierto «%1» (copia temporal)").arg(QFileInfo(dest).fileName()));
        Apps::get().openDefault({dest});
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
        } else if (m_stack->currentIndex() == 1 || m_stack->currentIndex() == 3) {
            QScrollBar *bar = m_stack->currentIndex() == 1 ? m_text->verticalScrollBar() : m_tree->verticalScrollBar();
            bar->triggerAction(e->key() == Qt::Key_PageDown ? QAbstractSlider::SliderPageStepAdd : QAbstractSlider::SliderPageStepSub);
        }
        return;
    case Qt::Key_P:
        if (m_player->isVisible()) playClip();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        m_clip.reset();   // the app it opens in takes over
        Apps::get().openDefault({m_path});
        close();
        return;
    default:
        QDialog::keyPressEvent(e);
    }
}
