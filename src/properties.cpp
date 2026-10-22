#include "properties.h"
#include "apps.h"
#include "fileops.h"
#include "fsmodel.h"
#include "icons.h"
#include "theme.h"
#include "thumbs.h"
#include <MediaInfo/MediaInfo.h>
#include <exiv2/exiv2.hpp>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QHash>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeDatabase>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QStorageInfo>
#include <QTabWidget>
#include <QThreadPool>
#include <QUrl>
#include <QVBoxLayout>
#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

QLabel *value(const QString &t)
{
    auto *l = new QLabel(t);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    l->setWordWrap(true);
    l->setOpenExternalLinks(true);
    return l;
}

QLabel *sectionLabel(const QString &t)
{
    auto *l = new QLabel(t);
    l->setProperty("role", "section");
    l->setContentsMargins(0, 10, 0, 2);
    return l;
}

QString fmtDuration(double ms)
{
    if (ms <= 0) return {};
    qint64 s = qint64(ms / 1000 + 0.5);
    qint64 h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    return h ? QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'))
             : QString("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
}

QString fmtBitrate(double bps)
{
    if (bps <= 0) return {};
    if (bps >= 1e6) return QString::number(bps / 1e6, 'f', 1) + " Mb/s";
    return QString::number(qRound(bps / 1000)) + " kb/s";
}

QString dateOf(const QDateTime &d)
{
    if (!d.isValid()) return QString::fromUtf8("desconocida");
    return QLocale(QLocale::Spanish, QLocale::Spain).toString(d, "dddd, d 'de' MMMM 'de' yyyy, HH:mm:ss");
}

using Section = QList<QPair<QString, QString>>;
using Sections = QList<QPair<QString, Section>>;

QString mi(MediaInfoLib::MediaInfo &m, MediaInfoLib::stream_t s, size_t n, const wchar_t *key)
{
    return QString::fromStdWString(m.Get(s, n, key)).trimmed();
}

Sections mediaSections(const QString &path)
{
    Sections out;
    MediaInfoLib::MediaInfo m;
    if (m.Open(path.toStdWString()) == 0) return out;
    using namespace MediaInfoLib;
    auto add = [](Section &s, const char *label, const QString &v) {
        if (!v.isEmpty()) s << qMakePair(QString::fromUtf8(label), v);
    };
    Section g;
    add(g, "Título", mi(m, Stream_General, 0, L"Title"));
    add(g, "Artista", mi(m, Stream_General, 0, L"Performer"));
    add(g, "Álbum", mi(m, Stream_General, 0, L"Album"));
    add(g, "Artista del álbum", mi(m, Stream_General, 0, L"Album/Performer"));
    QString track = mi(m, Stream_General, 0, L"Track/Position");
    if (!track.isEmpty() && !mi(m, Stream_General, 0, L"Track/Position_Total").isEmpty()) track += " de " + mi(m, Stream_General, 0, L"Track/Position_Total");
    add(g, "Pista", track);
    add(g, "Género", mi(m, Stream_General, 0, L"Genre"));
    if (m.Count_Get(Stream_Image) == 0) add(g, "Año", mi(m, Stream_General, 0, L"Recorded_Date"));
    add(g, "Compositor", mi(m, Stream_General, 0, L"Composer"));
    add(g, "Contenedor", mi(m, Stream_General, 0, L"Format"));
    add(g, "Duración", fmtDuration(mi(m, Stream_General, 0, L"Duration").toDouble()));
    add(g, "Tasa de bits total", fmtBitrate(mi(m, Stream_General, 0, L"OverallBitRate").toDouble()));
    if (m.Count_Get(Stream_Image) == 0) add(g, "Fecha de codificación", mi(m, Stream_General, 0, L"Encoded_Date"));
    add(g, "Aplicación", mi(m, Stream_General, 0, L"Encoded_Application"));
    add(g, "Comentario", mi(m, Stream_General, 0, L"Comment"));
    if (!g.isEmpty()) out << qMakePair(QString::fromUtf8("General"), g);

    for (size_t i = 0; i < m.Count_Get(Stream_Video); ++i) {
        Section v;
        add(v, "Códec", mi(m, Stream_Video, i, L"Format") + (mi(m, Stream_Video, i, L"Format_Profile").isEmpty() ? QString() : " (" + mi(m, Stream_Video, i, L"Format_Profile") + ")"));
        QString w = mi(m, Stream_Video, i, L"Width"), h = mi(m, Stream_Video, i, L"Height");
        if (!w.isEmpty()) add(v, "Resolución", w + QString::fromUtf8(" × ") + h);
        add(v, "Relación de aspecto", mi(m, Stream_Video, i, L"DisplayAspectRatio/String"));
        QString fps = mi(m, Stream_Video, i, L"FrameRate");
        if (!fps.isEmpty()) add(v, "Fotogramas por segundo", QString::number(fps.toDouble(), 'g', 5));
        add(v, "Tasa de bits", fmtBitrate(mi(m, Stream_Video, i, L"BitRate").toDouble()));
        add(v, "Profundidad de color", mi(m, Stream_Video, i, L"BitDepth").isEmpty() ? QString() : mi(m, Stream_Video, i, L"BitDepth") + " bits");
        add(v, "HDR", mi(m, Stream_Video, i, L"HDR_Format"));
        add(v, "Duración", fmtDuration(mi(m, Stream_Video, i, L"Duration").toDouble()));
        if (!v.isEmpty()) out << qMakePair(m.Count_Get(Stream_Video) > 1 ? QString::fromUtf8("Vídeo %1").arg(i + 1) : QString::fromUtf8("Vídeo"), v);
    }
    for (size_t i = 0; i < m.Count_Get(Stream_Audio); ++i) {
        Section a;
        add(a, "Códec", mi(m, Stream_Audio, i, L"Format") + (mi(m, Stream_Audio, i, L"Format_Profile").isEmpty() ? QString() : " " + mi(m, Stream_Audio, i, L"Format_Profile")));
        QString ch = mi(m, Stream_Audio, i, L"Channel(s)");
        add(a, "Canales", ch == "1" ? QString::fromUtf8("1 (mono)") : ch == "2" ? QString::fromUtf8("2 (estéreo)") : ch);
        QString sr = mi(m, Stream_Audio, i, L"SamplingRate");
        if (!sr.isEmpty()) add(a, "Frecuencia de muestreo", QString::number(sr.toDouble() / 1000, 'g', 4) + " kHz");
        QString br = fmtBitrate(mi(m, Stream_Audio, i, L"BitRate").toDouble());
        QString mode = mi(m, Stream_Audio, i, L"BitRate_Mode");
        if (!br.isEmpty()) add(a, "Tasa de bits", br + (mode == "VBR" ? QString::fromUtf8(" (variable)") : mode == "CBR" ? QString::fromUtf8(" (constante)") : QString()));
        add(a, "Profundidad", mi(m, Stream_Audio, i, L"BitDepth").isEmpty() ? QString() : mi(m, Stream_Audio, i, L"BitDepth") + " bits");
        add(a, "Idioma", mi(m, Stream_Audio, i, L"Language/String"));
        if (!a.isEmpty()) out << qMakePair(m.Count_Get(Stream_Audio) > 1 ? QString::fromUtf8("Audio %1").arg(i + 1) : QString::fromUtf8("Audio"), a);
    }
    if (size_t n = m.Count_Get(Stream_Text)) {
        Section t;
        QStringList langs;
        for (size_t i = 0; i < n; ++i) {
            QString l = mi(m, Stream_Text, i, L"Language/String");
            langs << (l.isEmpty() ? mi(m, Stream_Text, i, L"Format") : l);
        }
        add(t, "Pistas", langs.join(", "));
        out << qMakePair(QString::fromUtf8("Subtítulos"), t);
    }
    if (m.Count_Get(Stream_Image) && m.Count_Get(Stream_Video) == 0) {
        Section im;
        add(im, "Formato", mi(m, Stream_Image, 0, L"Format"));
        QString w = mi(m, Stream_Image, 0, L"Width"), h = mi(m, Stream_Image, 0, L"Height");
        if (!w.isEmpty()) add(im, "Dimensiones", w + QString::fromUtf8(" × ") + h + QString::fromUtf8(" píxeles"));
        add(im, "Profundidad de color", mi(m, Stream_Image, 0, L"BitDepth").isEmpty() ? QString() : mi(m, Stream_Image, 0, L"BitDepth") + " bits");
        add(im, "Espacio de color", mi(m, Stream_Image, 0, L"ColorSpace"));
        add(im, "Compresión", mi(m, Stream_Image, 0, L"Compression_Mode/String"));
        if (!im.isEmpty()) out << qMakePair(QString::fromUtf8("Imagen"), im);
    }
    m.Close();
    return out;
}

Sections exifSections(const QString &path)
{
    Sections out;
    try {
        auto image = Exiv2::ImageFactory::open(QFile::encodeName(path).toStdString());
        if (!image) return out;
        image->readMetadata();
        Exiv2::ExifData &ex = image->exifData();
        if (ex.empty()) return out;
        auto get = [&](const char *key) -> QString {
            auto it = ex.findKey(Exiv2::ExifKey(key));
            if (it == ex.end()) return {};
            return QString::fromStdString(it->print(&ex)).trimmed();
        };
        Section cam, shot, gps;
        auto add = [](Section &s, const char *label, const QString &v) {
            if (!v.isEmpty() && v != "(0)") s << qMakePair(QString::fromUtf8(label), v);
        };
        QString make = get("Exif.Image.Make"), model = get("Exif.Image.Model");
        if (!model.isEmpty() && !model.startsWith(make, Qt::CaseInsensitive)) model = make + " " + model;
        add(cam, "Cámara", model.isEmpty() ? make : model);
        add(cam, "Objetivo", get("Exif.Photo.LensModel"));
        add(cam, "Software", get("Exif.Image.Software"));
        QDateTime taken = QDateTime::fromString(get("Exif.Photo.DateTimeOriginal"), "yyyy:MM:dd HH:mm:ss");
        add(shot, "Fecha de la foto", taken.isValid() ? QLocale(QLocale::Spanish, QLocale::Spain).toString(taken, "d 'de' MMMM 'de' yyyy, HH:mm:ss") : get("Exif.Photo.DateTimeOriginal"));
        add(shot, "Exposición", get("Exif.Photo.ExposureTime"));
        add(shot, "Apertura", get("Exif.Photo.FNumber"));
        add(shot, "ISO", get("Exif.Photo.ISOSpeedRatings"));
        add(shot, "Distancia focal", get("Exif.Photo.FocalLength"));
        add(shot, "Focal equivalente 35 mm", get("Exif.Photo.FocalLengthIn35mmFilm"));
        add(shot, "Flash", get("Exif.Photo.Flash"));
        add(shot, "Balance de blancos", get("Exif.Photo.WhiteBalance"));
        static const QHash<QString, QString> orient = {
            {"top, left", QString::fromUtf8("normal")}, {"top, right", QString::fromUtf8("volteada en horizontal")},
            {"bottom, right", QString::fromUtf8("girada 180°")}, {"bottom, left", QString::fromUtf8("volteada en vertical")},
            {"left, top", QString::fromUtf8("traspuesta")}, {"right, top", QString::fromUtf8("girada 90° (vertical)")},
            {"right, bottom", QString::fromUtf8("transversa")}, {"left, bottom", QString::fromUtf8("girada 270° (vertical)")}};
        const QString o = get("Exif.Image.Orientation");
        add(shot, "Orientación", orient.value(o, o));
        auto coord = [&](const char *key, const char *refKey) -> double {
            auto it = ex.findKey(Exiv2::ExifKey(key));
            if (it == ex.end() || it->count() < 3) return NAN;
            double v = it->toFloat(0) + it->toFloat(1) / 60.0 + it->toFloat(2) / 3600.0;
            auto r = ex.findKey(Exiv2::ExifKey(refKey));
            if (r != ex.end()) {
                std::string ref = r->toString();
                if (ref == "S" || ref == "W") v = -v;
            }
            return v;
        };
        double lat = coord("Exif.GPSInfo.GPSLatitude", "Exif.GPSInfo.GPSLatitudeRef");
        double lon = coord("Exif.GPSInfo.GPSLongitude", "Exif.GPSInfo.GPSLongitudeRef");
        if (std::isfinite(lat) && std::isfinite(lon)) {
            const QString ll = QString("%1, %2").arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6);
            add(gps, "Coordenadas", QString("<a href=\"https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=16/%1/%2\">%3</a>")
                                        .arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6).arg(ll));
            add(gps, "Altitud", get("Exif.GPSInfo.GPSAltitude"));
        }
        if (!cam.isEmpty()) out << qMakePair(QString::fromUtf8("Cámara"), cam);
        if (!shot.isEmpty()) out << qMakePair(QString::fromUtf8("Toma"), shot);
        if (!gps.isEmpty()) out << qMakePair(QString::fromUtf8("Ubicación"), gps);
    } catch (const std::exception &) {
    }
    return out;
}

} // namespace

PropertiesDialog::PropertiesDialog(const QStringList &paths, QWidget *parent) : QDialog(parent), m_paths(paths)
{
    setAttribute(Qt::WA_DeleteOnClose);
    m_single = paths.size() == 1;
    m_isDir = m_single && QFileInfo(paths[0]).isDir();
    setWindowTitle(m_single ? QString::fromUtf8("Propiedades de «%1»").arg(QFileInfo(paths[0]).fileName())
                            : QString::fromUtf8("Propiedades de %1 elementos").arg(paths.size()));
    resize(500, 600);
    auto *v = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;
    tabs->addTab(generalTab(), "General");
    tabs->addTab(permissionsTab(), "Permisos");
    if (m_single && !m_isDir) {
        const QString mime = QMimeDatabase().mimeTypeForFile(paths[0]).name();
        if (mime.startsWith("image/") || mime.startsWith("video/") || mime.startsWith("audio/"))
            tabs->addTab(metadataTab(), "Metadatos");
        tabs->addTab(openWithTab(), "Abrir con");
    }
    v->addWidget(tabs);
    auto *close = new QPushButton("Cerrar");
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    auto *row = new QHBoxLayout;
    row->addStretch();
    row->addWidget(close);
    v->addLayout(row);
}

QWidget *PropertiesDialog::generalTab()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *top = new QHBoxLayout;
    auto *icon = new QLabel;
    const QString first = m_paths[0];
    bool ok;
    FileEntry e = FsModel::statPath(first, &ok);
    QIcon ic = e.isDir ? Icons::get().forFolder(first) : Icons::get().forEntry(e, first);
    QPixmap px;
    if (m_single && !e.isDir && Thumbs::canThumb(e.mime, e.size)) px = Thumbs::get().thumbnail(first, e.mtime, e.mime);
    icon->setPixmap(px.isNull() ? ic.pixmap(64, 64) : px.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    top->addWidget(icon);
    auto *nameCol = new QVBoxLayout;
    if (m_single) {
        auto *name = new QLineEdit(QFileInfo(first).fileName());
        QFont f = name->font();
        f.setPointSizeF(f.pointSizeF() * 1.15);
        name->setFont(f);
        connect(name, &QLineEdit::editingFinished, this, [this, name] {
            const QString n = name->text().trimmed();
            QFileInfo fi(m_paths[0]);
            if (n.isEmpty() || n == fi.fileName() || n.contains('/')) return;
            const QString np = fi.absolutePath() + '/' + n;
            if (!QFileInfo::exists(np) && QFile::rename(m_paths[0], np)) {
                m_paths[0] = np;
                setWindowTitle(QString::fromUtf8("Propiedades de «%1»").arg(n));
            } else {
                name->setText(fi.fileName());
            }
        });
        nameCol->addWidget(name);
        nameCol->addWidget(value(FsModel::mimeComment(e.mime) + "  ·  " + e.mime));
    } else {
        nameCol->addWidget(value(QString::fromUtf8("<b>%1 elementos</b>").arg(m_paths.size())));
    }
    top->addLayout(nameCol, 1);
    v->addLayout(top);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setHorizontalSpacing(14);
    m_size = value(QString::fromUtf8("calculando…"));
    form->addRow(QString::fromUtf8("Tamaño:"), m_size);
    // Recursive size in the background
    QPointer<QLabel> sizeLabel = m_size;
    const QStringList paths = m_paths;
    QThreadPool::globalInstance()->start([sizeLabel, paths] {
        qint64 bytes = 0;
        int files = 0, dirs = 0;
        for (const QString &p : paths) {
            QFileInfo fi(p);
            if (fi.isDir() && !fi.isSymLink()) {
                ++dirs;
                QDirIterator it(p, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    it.next();
                    QFileInfo f = it.fileInfo();
                    if (f.isDir() && !f.isSymLink()) ++dirs;
                    else {
                        ++files;
                        if (!f.isSymLink()) bytes += f.size();
                    }
                    if (!sizeLabel) return;
                }
            } else {
                ++files;
                bytes += fi.isSymLink() ? 0 : fi.size();
            }
        }
        QString t = FsModel::formatSize(bytes) + QString::fromUtf8("  (%1 bytes)").arg(QLocale(QLocale::Spanish).toString(bytes));
        if (dirs) t += QString::fromUtf8("\n%1 archivos, %2 carpetas").arg(files).arg(dirs - (paths.size() == 1 ? 1 : 0));
        QMetaObject::invokeMethod(qApp, [sizeLabel, t] {
            if (sizeLabel) sizeLabel->setText(t);
        });
    });
    if (m_single) {
        QFileInfo fi(first);
        auto *loc = value(QString("<a href=\"%1\">%2</a>").arg(QUrl::fromLocalFile(fi.absolutePath()).toString(), fi.absolutePath().toHtmlEscaped()));
        loc->setOpenExternalLinks(false);
        connect(loc, &QLabel::linkActivated, this, [](const QString &u) { QDesktopServices::openUrl(QUrl(u)); });
        form->addRow(QString::fromUtf8("Ubicación:"), loc);
        if (fi.isSymLink()) form->addRow(QString::fromUtf8("Enlace a:"), value(fi.symLinkTarget()));
        if (first.startsWith(FileOps::trashFiles())) form->addRow(QString::fromUtf8("Ubicación original:"), value(FileOps::originalPath(first)));
        form->addRow(QString::fromUtf8("Creado:"), value(dateOf(fi.birthTime())));
        form->addRow(QString::fromUtf8("Modificado:"), value(dateOf(fi.lastModified())));
        form->addRow(QString::fromUtf8("Último acceso:"), value(dateOf(fi.lastRead())));
        if (!fi.isDir() && QString(e.mime).startsWith("image/")) {
            QImageReader r(first);
            QSize s = r.size();
            if (s.isValid()) form->addRow(QString::fromUtf8("Dimensiones:"), value(QString::fromUtf8("%1 × %2 píxeles").arg(s.width()).arg(s.height())));
        }
    }
    QStorageInfo si(QFileInfo(first).absolutePath());
    if (si.isValid())
        form->addRow(QString::fromUtf8("Espacio libre:"),
                     value(QString::fromUtf8("%1 de %2 en %3").arg(FsModel::formatSize(si.bytesAvailable()), FsModel::formatSize(si.bytesTotal()),
                                                                    si.displayName())));
    v->addLayout(form);
    v->addStretch();
    return w;
}

QWidget *PropertiesDialog::permissionsTab()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    bool ok;
    FileEntry e = FsModel::statPath(m_paths[0], &ok);
    const bool owner = e.uid == getuid() || getuid() == 0;
    auto *form = new QFormLayout;
    form->addRow("Propietario:", value(FsModel::userName(e.uid)));
    m_group = new QComboBox;
    // Groups the user belongs to (the only ones chgrp allows)
    gid_t groups[128];
    int n = getgroups(128, groups);
    QList<gid_t> gl(groups, groups + std::max(0, n));
    if (!gl.contains(e.gid)) gl.prepend(e.gid);
    for (gid_t g : gl) m_group->addItem(FsModel::groupName(g), uint(g));
    m_group->setCurrentIndex(m_group->findData(uint(e.gid)));
    m_group->setEnabled(owner);
    connect(m_group, &QComboBox::currentIndexChanged, this, [this] {
        if (m_updating) return;
        gid_t g = m_group->currentData().toUInt();
        for (const QString &p : m_paths) chown(QFile::encodeName(p).constData(), uid_t(-1), g);
    });
    form->addRow("Grupo:", m_group);
    v->addLayout(form);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(18);
    const bool dir = m_isDir;
    grid->addWidget(new QLabel("<b>Lectura</b>"), 0, 1, Qt::AlignCenter);
    grid->addWidget(new QLabel("<b>Escritura</b>"), 0, 2, Qt::AlignCenter);
    grid->addWidget(new QLabel(dir ? "<b>Acceso</b>" : QString::fromUtf8("<b>Ejecución</b>")), 0, 3, Qt::AlignCenter);
    const char *who[] = {"Propietario", "Grupo", "Otros"};
    for (int r = 0; r < 3; ++r) {
        grid->addWidget(new QLabel(QString::fromUtf8(who[r])), r + 1, 0);
        for (int c = 0; c < 3; ++c) {
            auto *cb = new QCheckBox;
            cb->setEnabled(owner);
            m_bits[r][c] = cb;
            const quint32 bit = 0400u >> (r * 3 + c);
            connect(cb, &QCheckBox::toggled, this, [this, bit](bool on) {
                if (!m_updating) applyMode(on ? bit : 0, on ? 0 : bit);
            });
            grid->addWidget(cb, r + 1, c + 1, Qt::AlignCenter);
        }
    }
    v->addSpacing(8);
    v->addLayout(grid);

    if (!dir) {
        m_exec = new QCheckBox(QString::fromUtf8("Permitir ejecutar el archivo como un programa"));
        m_exec->setEnabled(owner);
        connect(m_exec, &QCheckBox::toggled, this, [this](bool on) {
            if (m_updating) return;
            // x wherever r is set (like chmod +x respecting umask-ish reading rights)
            bool ok;
            quint32 mode = FsModel::statPath(m_paths[0], &ok).mode;
            quint32 x = ((mode & 0444) >> 2) | 0100;
            applyMode(on ? x : 0, on ? 0 : 0111);
        });
        v->addWidget(m_exec);
    }
    v->addWidget(sectionLabel("Especiales"));
    m_suid = new QCheckBox("Establecer UID (setuid)");
    m_sgid = new QCheckBox("Establecer GID (setgid)");
    m_sticky = new QCheckBox(QString::fromUtf8("Bit pegajoso (sticky): solo el dueño puede borrar"));
    for (auto [cb, bit] : {std::pair{m_suid, quint32(S_ISUID)}, std::pair{m_sgid, quint32(S_ISGID)}, std::pair{m_sticky, quint32(S_ISVTX)}}) {
        cb->setEnabled(owner);
        connect(cb, &QCheckBox::toggled, this, [this, bit = bit](bool on) {
            if (!m_updating) applyMode(on ? bit : 0, on ? 0 : bit);
        });
        v->addWidget(cb);
    }
    auto *octRow = new QHBoxLayout;
    octRow->addWidget(new QLabel("Valor octal:"));
    m_octal = new QLineEdit;
    m_octal->setMaximumWidth(80);
    m_octal->setFont(QFont(Theme::get().mono));
    m_octal->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-7]{3,4}"), m_octal));
    m_octal->setEnabled(owner);
    connect(m_octal, &QLineEdit::editingFinished, this, [this] {
        bool ok;
        quint32 m = m_octal->text().toUInt(&ok, 8);
        if (ok) applyMode(m, 07777 & ~m);
    });
    octRow->addWidget(m_octal);
    octRow->addStretch();
    v->addSpacing(6);
    v->addLayout(octRow);
    if (dir) {
        m_recursive = new QCheckBox(QString::fromUtf8("Aplicar también a todo lo que contiene\n(a los archivos no se les añade ejecución)"));
        m_recursive->setEnabled(owner);
        v->addWidget(m_recursive);
    }
    if (!owner) {
        auto *l = new QLabel(QString::fromUtf8("Solo el propietario puede cambiar los permisos."));
        l->setProperty("role", "dim");
        v->addWidget(l);
    }
    v->addStretch();
    loadPermissions();
    return w;
}

void PropertiesDialog::loadPermissions()
{
    bool ok;
    quint32 mode = FsModel::statPath(m_paths[0], &ok).mode;
    m_updating = true;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m_bits[r][c]->setChecked(mode & (0400u >> (r * 3 + c)));
    if (m_exec) m_exec->setChecked(mode & 0100);
    m_suid->setChecked(mode & S_ISUID);
    m_sgid->setChecked(mode & S_ISGID);
    m_sticky->setChecked(mode & S_ISVTX);
    m_octal->setText(QString("%1").arg(mode & 07777, 4, 8, QChar('0')));
    m_updating = false;
}

void PropertiesDialog::applyMode(quint32 set, quint32 clear)
{
    auto apply = [&](const QString &p, bool isFile) {
        struct stat st;
        if (lstat(QFile::encodeName(p).constData(), &st) != 0 || S_ISLNK(st.st_mode)) return;
        quint32 s = set, c = clear;
        if (isFile) s &= ~quint32(0111);   // never make contained files executable
        chmod(QFile::encodeName(p).constData(), ((st.st_mode & 07777) | s) & ~c);
    };
    for (const QString &p : m_paths) {
        apply(p, false);
        if (m_recursive && m_recursive->isChecked() && QFileInfo(p).isDir()) {
            QDirIterator it(p, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                apply(it.filePath(), !it.fileInfo().isDir());
            }
        }
    }
    loadPermissions();
}

QWidget *PropertiesDialog::metadataTab()
{
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *wait = new QLabel(QString::fromUtf8("Leyendo metadatos…"));
    wait->setProperty("role", "dim");
    v->addWidget(wait);
    v->addStretch();
    scroll->setWidget(w);
    QPointer<QWidget> target = w;
    const QString path = m_paths[0];
    const bool image = QMimeDatabase().mimeTypeForFile(path).name().startsWith("image/");
    QThreadPool::globalInstance()->start([target, path, image] {
        Sections s = mediaSections(path);
        if (image) s += exifSections(path);
        QMetaObject::invokeMethod(qApp, [target, s] {
            if (!target) return;
            auto *v = static_cast<QVBoxLayout *>(target->layout());
            while (QLayoutItem *it = v->takeAt(0)) {
                delete it->widget();
                delete it;
            }
            if (s.isEmpty()) {
                auto *l = new QLabel(QString::fromUtf8("Este archivo no tiene metadatos legibles."));
                l->setProperty("role", "dim");
                v->addWidget(l);
            }
            for (const auto &sec : s) {
                v->addWidget(sectionLabel(sec.first));
                auto *form = new QFormLayout;
                form->setLabelAlignment(Qt::AlignRight);
                form->setHorizontalSpacing(14);
                for (const auto &kv : sec.second) form->addRow(kv.first + ":", value(kv.second));
                v->addLayout(form);
            }
            v->addStretch();
        });
    });
    return scroll;
}

QWidget *PropertiesDialog::openWithTab()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    const QString mime = QMimeDatabase().mimeTypeForFile(m_paths[0]).name();
    auto *info = new QLabel(QString::fromUtf8("Aplicación predeterminada para «%1» (%2):").arg(FsModel::mimeComment(mime), mime));
    info->setWordWrap(true);
    v->addWidget(info);
    auto *list = new QListWidget;
    list->setIconSize(QSize(26, 26));
    const QString def = Apps::get().defaultFor(mime);
    for (const AppInfo *a : Apps::get().forMime(mime)) {
        auto *it = new QListWidgetItem(Icons::get().app(a->icon), a->name + (a->id == def ? QString::fromUtf8("  ✓ predeterminada") : QString()));
        it->setData(Qt::UserRole, a->id);
        list->addItem(it);
        if (a->id == def) list->setCurrentItem(it);
    }
    v->addWidget(list, 1);
    auto *row = new QHBoxLayout;
    auto *other = new QPushButton(QString::fromUtf8("Otra aplicación…"));
    auto *set = new QPushButton("Establecer como predeterminada");
    set->setProperty("role", "primary");
    row->addWidget(other);
    row->addStretch();
    row->addWidget(set);
    v->addLayout(row);
    auto refill = [list, mime] {
        const QString d = Apps::get().defaultFor(mime);
        for (int i = 0; i < list->count(); ++i) {
            auto *it = list->item(i);
            const AppInfo *a = Apps::get().byId(it->data(Qt::UserRole).toString());
            if (a) it->setText(a->name + (a->id == d ? QString::fromUtf8("  ✓ predeterminada") : QString()));
        }
    };
    connect(set, &QPushButton::clicked, this, [list, mime, refill] {
        if (auto *it = list->currentItem()) {
            Apps::get().setDefault(mime, it->data(Qt::UserRole).toString());
            refill();
        }
    });
    connect(other, &QPushButton::clicked, this, [this, list, mime] {
        OpenWithDialog d(m_paths, this);
        if (d.exec() != QDialog::Accepted) return;
        Apps::get().setDefault(mime, d.chosen());
        const AppInfo *a = Apps::get().byId(d.chosen());
        if (!a) return;
        bool found = false;
        for (int i = 0; i < list->count(); ++i) found |= list->item(i)->data(Qt::UserRole).toString() == a->id;
        if (!found) {
            auto *it = new QListWidgetItem(Icons::get().app(a->icon), a->name);
            it->setData(Qt::UserRole, a->id);
            list->addItem(it);
        }
        for (int i = 0; i < list->count(); ++i) {
            auto *it = list->item(i);
            const AppInfo *x = Apps::get().byId(it->data(Qt::UserRole).toString());
            if (x) it->setText(x->name + (x->id == a->id ? QString::fromUtf8("  ✓ predeterminada") : QString()));
        }
    });
    return w;
}
