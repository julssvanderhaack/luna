#include "places.h"
#include "fileops.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QXmlStreamReader>
#include <algorithm>

namespace {
constexpr int kRecentMax = 200;

QString settingsKeyRecent() { return QStringLiteral("recent/files"); }   // "msecs|path", newest first
}

bool Places::isVirtual(const QString &path) { return path == recent || path == starred; }

QString Places::displayName(const QString &path)
{
    if (path == recent) return QString::fromUtf8("Recientes");
    if (path == starred) return QString::fromUtf8("Destacados");
    if (path == FileOps::trashFiles()) return QString::fromUtf8("Papelera");
    if (path == QDir::homePath()) return QString::fromUtf8("Carpeta personal");
    const QString n = QFileInfo(path).fileName();
    return n.isEmpty() ? QStringLiteral("/") : n;
}

QStringList Places::contents(const QString &path)
{
    if (path == recent) return Recent::files();
    if (path == starred) return Starred::get().paths();
    return {};
}

// ------------------------------------------------------------------ Starred
Starred &Starred::get()
{
    static Starred s;
    return s;
}

Starred::Starred()
{
    m_order = QSettings("luna", "luna").value("starred/paths").toStringList();
    m_paths = QSet<QString>(m_order.begin(), m_order.end());
}

void Starred::save() { QSettings("luna", "luna").setValue("starred/paths", m_order); }

void Starred::add(const QStringList &paths)
{
    bool any = false;
    for (const QString &p : paths)
        if (!m_paths.contains(p)) {
            m_paths.insert(p);
            m_order.prepend(p);
            any = true;
        }
    if (!any) return;
    save();
    emit changed();
}

void Starred::remove(const QStringList &paths)
{
    bool any = false;
    for (const QString &p : paths)
        if (m_paths.remove(p)) {
            m_order.removeAll(p);
            any = true;
        }
    if (!any) return;
    save();
    emit changed();
}

void Starred::remap(const QList<QPair<QString, QString>> &moves)
{
    bool any = false;
    for (QString &p : m_order)
        for (const auto &m : moves) {
            // the item itself, or something starred inside a moved folder
            if (p == m.first) p = m.second;
            else if (p.startsWith(m.first + '/')) p = m.second + p.mid(m.first.size());
            else continue;
            any = true;
            break;
        }
    if (!any) return;
    m_order.removeDuplicates();
    m_paths = QSet<QString>(m_order.begin(), m_order.end());
    save();
    emit changed();
}

// ------------------------------------------------------------------- Recent
void Recent::add(const QStringList &files)
{
    QSettings q("luna", "luna");
    QStringList list = q.value(settingsKeyRecent()).toStringList();
    const QString now = QString::number(QDateTime::currentMSecsSinceEpoch());
    for (const QString &f : files) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const QString &e) { return e.section('|', 1) == f; }), list.end());
        list.prepend(now + '|' + f);
    }
    q.setValue(settingsKeyRecent(), list.mid(0, kRecentMax));
}

QStringList Recent::files()
{
    QList<QPair<qint64, QString>> all;
    for (const QString &e : QSettings("luna", "luna").value(settingsKeyRecent()).toStringList())
        all.append({e.section('|', 0, 0).toLongLong(), e.section('|', 1)});
    // What other applications opened (GTK's list; read only, never written).
    QFile f(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/recently-used.xbel");
    if (f.open(QIODevice::ReadOnly)) {
        auto stamp = [](QStringView s) {
            // 2026-10-21T10:00:00.123456Z: Qt wants at most milliseconds
            return s.size() >= 19 ? QDateTime::fromString(s.left(19).toString() + 'Z', Qt::ISODate).toMSecsSinceEpoch() : 0;
        };
        QXmlStreamReader x(&f);
        while (!x.atEnd()) {
            if (x.readNext() != QXmlStreamReader::StartElement || x.name() != QLatin1String("bookmark")) continue;
            const QXmlStreamAttributes a = x.attributes();
            const QUrl url(a.value("href").toString());
            if (!url.isLocalFile()) continue;
            const qint64 t = std::max({stamp(a.value("added")), stamp(a.value("modified")), stamp(a.value("visited"))});
            all.append({t, url.toLocalFile()});
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    QStringList out;
    QSet<QString> seen;
    for (const auto &e : all) {
        if (out.size() >= kRecentMax) break;
        if (seen.contains(e.second)) continue;
        seen.insert(e.second);
        QFileInfo fi(e.second);
        if (fi.exists() && !fi.isDir()) out << e.second;
    }
    return out;
}
