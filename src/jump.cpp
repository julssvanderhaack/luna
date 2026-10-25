#include "jump.h"
#include "fsmodel.h"
#include "icons.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace {
bool haveZoxide() { return !QStandardPaths::findExecutable("zoxide").isEmpty(); }

QString tilde(const QString &p)
{
    const QString home = QDir::homePath();
    return p == home ? "~" : p.startsWith(home + '/') ? "~" + p.mid(home.size()) : p;
}
}

void JumpDialog::recordVisit(const QString &dir)
{
    // Only real local folders, and only once the user stops on one: browsing
    // through ten folders in a row should not spawn ten processes.
    static QTimer *timer = nullptr;
    static QString pending, last;
    if (!dir.startsWith('/') || dir.startsWith(QString("/run/user/")) || !haveZoxide()) return;
    if (!timer) {
        timer = new QTimer(qApp);
        timer->setSingleShot(true);
        timer->setInterval(1500);
        QObject::connect(timer, &QTimer::timeout, [] {
            if (pending == last) return;
            last = pending;
            QProcess::startDetached("nice", {"-n", "19", "zoxide", "add", "--", pending});
        });
    }
    pending = dir;
    timer->start();
}

JumpDialog::JumpDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Ir a una carpeta");
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(12, 12, 12, 10);
    m_edit = new QLineEdit;
    m_edit->setPlaceholderText(QString::fromUtf8("Escribe partes de la ruta, p. ej. «luna src»"));
    m_edit->setClearButtonEnabled(true);
    m_edit->installEventFilter(this);
    m_list = new QListWidget;
    m_list->setUniformItemSizes(true);
    m_list->setFocusPolicy(Qt::NoFocus);
    auto *hint = new QLabel(haveZoxide() ? QString::fromUtf8("Carpetas frecuentes de zoxide y tus marcadores  ·  Intro abre  ·  Esc cierra")
                                         : QString::fromUtf8("Tus marcadores (instala zoxide para recordar las carpetas que visitas)"));
    hint->setProperty("role", "dim");
    hint->setStyleSheet("font-size: 11px;");
    v->addWidget(m_edit);
    v->addWidget(m_list, 1);
    v->addWidget(hint);
    resize(std::max(560, parent->window()->width() / 2), 420);

    // zoxide's frecency list ("  44.0 /path"), then the GTK bookmarks
    QSet<QString> seen;
    if (haveZoxide()) {
        QProcess p;
        p.start("zoxide", {"query", "--list", "--score"});
        p.waitForFinished(1500);
        for (const QByteArray &line : p.readAllStandardOutput().split('\n')) {
            const QString l = QString::fromUtf8(line).trimmed();
            const int sp = l.indexOf(' ');
            if (sp < 0) continue;
            const QString path = l.mid(sp + 1).trimmed();
            if (seen.contains(path) || !QFileInfo(path).isDir()) continue;
            seen.insert(path);
            m_entries.append({path, l.left(sp).toDouble()});
        }
    }
    QStringList bases{QDir::homePath()};   // home and the bookmarks, looked inside below
    QFile bm(QDir::homePath() + "/.config/gtk-3.0/bookmarks");
    if (bm.open(QIODevice::ReadOnly))
        for (const QByteArray &line : bm.readAll().split('\n')) {
            const QUrl url(QString::fromUtf8(line.split(' ').value(0)));
            if (!url.isLocalFile()) continue;
            const QString path = QDir::cleanPath(url.toLocalFile());
            if (!QFileInfo(path).isDir()) continue;
            bases << path;
            if (seen.contains(path)) continue;
            seen.insert(path);
            m_entries.append({path, 1.0});
        }
    // ...and the folders right inside home and each bookmark, so a folder is
    // found before zoxide has seen it (a few readdirs, once per Ctrl+P)
    for (const QString &base : bases)
        for (const QFileInfo &fi : QDir(base).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (!seen.contains(fi.absoluteFilePath())) {
                seen.insert(fi.absoluteFilePath());
                m_entries.append({fi.absoluteFilePath(), 0.5});
            }
    std::stable_sort(m_entries.begin(), m_entries.end(), [](const Entry &a, const Entry &b) { return a.score > b.score; });

    connect(m_edit, &QLineEdit::textChanged, this, &JumpDialog::fill);
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) {
        m_chosen = it->data(Qt::UserRole).toString();
        accept();
    });
    fill({});
}

// Every word must appear in the path, in any order ("luna src" finds
// ~/src/luna); a folder named exactly like a word comes first, then one
// whose name holds a word, then by frecency.
void JumpDialog::fill(const QString &query)
{
    m_list->clear();
    const QStringList words = foldName(query).split(' ', Qt::SkipEmptyParts);
    QList<QPair<int, const Entry *>> hits;   // (rank, entry)
    for (const Entry &e : m_entries) {
        const QString hay = foldName(e.path);
        if (!std::all_of(words.cbegin(), words.cend(), [&](const QString &w) { return hay.contains(w); })) continue;
        const QString name = foldName(QFileInfo(e.path).fileName());
        auto any = [&](auto test) { return std::any_of(words.cbegin(), words.cend(), test); };
        const int rank = words.isEmpty() || any([&](const QString &w) { return name == w; }) ? 0   // the folder is called that
                         : any([&](const QString &w) { return name.contains(w); })     ? 1
                                                                                      : 2;
        hits.append({rank, &e});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &h : hits.mid(0, 60)) {
        auto *it = new QListWidgetItem(Icons::get().forFolder(h.second->path), tilde(h.second->path));
        it->setData(Qt::UserRole, h.second->path);
        m_list->addItem(it);
    }
    if (hits.isEmpty() && !words.isEmpty()) {
        auto *none = new QListWidgetItem(QString::fromUtf8("Sin coincidencias"));
        none->setFlags(Qt::NoItemFlags);
        m_list->addItem(none);
        return;
    }
    m_list->setCurrentRow(0);
}

bool JumpDialog::eventFilter(QObject *o, QEvent *e)
{
    // the arrows move in the list while the cursor stays in the search box
    if (o == m_edit && e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        const int row = m_list->currentRow();
        switch (k->key()) {
        case Qt::Key_Down: m_list->setCurrentRow(std::min(row + 1, m_list->count() - 1)); return true;
        case Qt::Key_Up: m_list->setCurrentRow(std::max(row - 1, 0)); return true;
        case Qt::Key_PageDown: m_list->setCurrentRow(std::min(row + 10, m_list->count() - 1)); return true;
        case Qt::Key_PageUp: m_list->setCurrentRow(std::max(row - 10, 0)); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (QListWidgetItem *it = m_list->currentItem()) {
                m_chosen = it->data(Qt::UserRole).toString();
                accept();
            }
            return true;
        default: break;
        }
    }
    return QDialog::eventFilter(o, e);
}
