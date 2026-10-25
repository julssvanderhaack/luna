#include "apps.h"
#include "icons.h"
#include "settings.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeDatabase>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextStream>
#include <QVBoxLayout>

namespace {

QStringList dataDirs()
{
    QStringList dirs;
    dirs << QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QString env = qEnvironmentVariable("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
    dirs << env.split(':', Qt::SkipEmptyParts);
    dirs << QDir::homePath() + "/.local/share/flatpak/exports/share" << "/var/lib/flatpak/exports/share";
    dirs.removeDuplicates();
    return dirs;
}

// Minimal desktop-entry parser: [Desktop Entry] keys, localized Name/Comment.
bool parseDesktop(const QString &file, AppInfo &a)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QTextStream in(&f);
    bool inEntry = false;
    QString name, nameEs, nameEsEs, comment, commentEs, type;
    bool noDisplay = false;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.startsWith('[')) {
            if (inEntry) break;
            inEntry = line == "[Desktop Entry]";
            continue;
        }
        if (!inEntry || line.startsWith('#')) continue;
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        QString k = line.left(eq).trimmed(), v = line.mid(eq + 1).trimmed();
        if (k == "Name") name = v;
        else if (k == "Name[es]") nameEs = v;
        else if (k == "Name[es_ES]") nameEsEs = v;
        else if (k == "Comment") comment = v;
        else if (k == "Comment[es]") commentEs = v;
        else if (k == "Exec") a.exec = v;
        else if (k == "Icon") a.icon = v;
        else if (k == "Type") type = v;
        else if (k == "Terminal") a.terminal = v == "true";
        else if (k == "NoDisplay") noDisplay = v == "true";
        else if (k == "Hidden") a.hidden = a.hidden || v == "true";
        else if (k == "MimeType") a.mimes = v.split(';', Qt::SkipEmptyParts);
    }
    if (type != "Application" || a.exec.isEmpty()) return false;
    a.name = !nameEsEs.isEmpty() ? nameEsEs : !nameEs.isEmpty() ? nameEs : name;
    a.comment = !commentEs.isEmpty() ? commentEs : comment;
    if (noDisplay && a.mimes.isEmpty()) a.hidden = true;
    a.path = file;
    return true;
}

QStringList mimeChain(const QString &mime)
{
    QStringList chain{mime};
    QMimeDatabase db;
    QMimeType t = db.mimeTypeForName(mime);
    if (t.isValid()) {
        for (const QString &a : t.aliases()) chain << a;
        for (const QString &p : t.allAncestors()) chain << p;
    }
    chain.removeDuplicates();
    return chain;
}

} // namespace

Apps &Apps::get()
{
    static Apps a;
    if (!a.m_loaded) a.load();
    return a;
}

void Apps::load()
{
    m_loaded = true;
    m_apps.clear();
    m_byMime.clear();
    // Higher-priority directories first: the first id found wins.
    for (const QString &base : dataDirs()) {
        const QString dir = base + "/applications";
        QDirIterator it(dir, {"*.desktop"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next();
            QString id = file.mid(dir.size() + 1).replace('/', '-');
            if (m_apps.contains(id)) continue;
            AppInfo a;
            a.id = id;
            if (!parseDesktop(file, a)) continue;
            m_apps.insert(id, a);
        }
    }
    for (const AppInfo &a : m_apps)
        if (!a.hidden)
            for (const QString &m : a.mimes) m_byMime[m] << a.id;
    readMimeApps();
}

void Apps::readMimeApps()
{
    m_defaults.clear();
    m_added.clear();
    m_removed.clear();
    QStringList files;
    files << QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/mimeapps.list";
    for (const QString &d : qEnvironmentVariable("XDG_CONFIG_DIRS", "/etc/xdg").split(':', Qt::SkipEmptyParts)) files << d + "/mimeapps.list";
    for (const QString &d : dataDirs()) files << d + "/applications/mimeapps.list";
    for (const QString &file : files) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) continue;
        QTextStream in(&f);
        QString section;
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.startsWith('[')) {
                section = line;
                continue;
            }
            int eq = line.indexOf('=');
            if (eq < 0) continue;
            QString mime = line.left(eq).trimmed();
            QStringList ids = line.mid(eq + 1).split(';', Qt::SkipEmptyParts);
            auto &target = section == "[Default Applications]" ? m_defaults
                           : section == "[Added Associations]"  ? m_added
                           : section == "[Removed Associations]" ? m_removed
                                                                  : m_added;
            if (section != "[Default Applications]" && section != "[Added Associations]" && section != "[Removed Associations]") continue;
            for (const QString &id : ids)
                if (!target[mime].contains(id)) target[mime] << id;   // earlier files take precedence
        }
    }
}

const AppInfo *Apps::byId(const QString &id) const
{
    auto it = m_apps.find(id);
    return it == m_apps.end() ? nullptr : &*it;
}

QString Apps::defaultFor(const QString &mime) const
{
    for (const QString &m : mimeChain(mime)) {
        for (const QString &id : m_defaults.value(m))
            if (m_apps.contains(id)) return id;
    }
    auto rec = forMime(mime);
    return rec.isEmpty() ? QString() : rec.first()->id;
}

QList<const AppInfo *> Apps::forMime(const QString &mime) const
{
    QStringList ids;
    const QStringList chain = mimeChain(mime);
    for (const QString &m : chain)
        for (const QString &id : m_defaults.value(m)) ids << id;
    for (const QString &m : chain)
        for (const QString &id : m_added.value(m)) ids << id;
    for (const QString &m : chain)
        for (const QString &id : m_byMime.value(m)) ids << id;
    QStringList removed;
    for (const QString &m : chain) removed << m_removed.value(m);
    ids.removeDuplicates();
    QList<const AppInfo *> out;
    for (const QString &id : ids)
        if (!removed.contains(id))
            if (const AppInfo *a = byId(id)) out << a;
    return out;
}

QList<const AppInfo *> Apps::all() const
{
    QList<const AppInfo *> out;
    for (const AppInfo &a : m_apps)
        if (!a.hidden) out << &a;
    std::sort(out.begin(), out.end(), [](auto *a, auto *b) { return a->name.compare(b->name, Qt::CaseInsensitive) < 0; });
    return out;
}

void Apps::setDefault(const QString &mime, const QString &appId)
{
    QProcess::execute("xdg-mime", {"default", appId, mime});
    readMimeApps();
}

// Hyprland places the window itself: hl.exec_cmd(cmd, rules) on the Lua
// config (0.56+, Omarchy's), where hyprctl dispatch takes Lua, or
// "[workspace N silent] cmd" on the classic one.
static void launchOnWorkspace(const QString &bin, const QStringList &args, int workspace)
{
    QStringList words{bin};
    for (const QString &a : args) {
        QString q = a;
        q.replace('\'', R"('\'')");
        words << '\'' + q + '\'';
    }
    const QString cmd = words.join(' ');
    QString lua = cmd;
    lua.replace('\\', R"(\\)").replace('"', R"(\")").replace('\n', R"(\n)");
    QProcess p;
    const QString ws = QString::number(workspace);   // arg(a, b) at once: a file named "%2" stays intact
    p.start("hyprctl", {"dispatch", QString(R"(hl.dsp.exec_cmd("%1", { workspace = "%2 silent" }))").arg(lua, ws)});
    p.waitForFinished(2000);
    if (p.readAllStandardOutput().trimmed() == "ok") return;
    QProcess::execute("hyprctl", {"dispatch", "exec", QString("[workspace %1 silent] %2").arg(ws, cmd)});
}

bool Apps::launch(const QString &appId, const QStringList &paths, int workspace)
{
    const AppInfo *a = byId(appId);
    if (!a) return false;
    // Apps that take a single file (%f/%u) get one launch per file.
    const bool single = !a->exec.contains("%F") && !a->exec.contains("%U");
    const QString bin = QStandardPaths::findExecutable("uwsm-app").isEmpty() ? "gtk-launch" : "uwsm-app";
    auto run = [&](const QStringList &files) {
        QStringList args;
        if (bin == "uwsm-app") args << "--";
        args << appId << files;
        if (workspace > 0) return launchOnWorkspace(bin, args, workspace);
        QProcess::startDetached(bin, args, QDir::homePath());
    };
    if (single && paths.size() > 1) {
        for (const QString &p : paths) run({p});
    } else {
        run(paths);
    }
    return true;
}

QStringList Apps::openDefault(const QStringList &paths, int workspace)
{
    QMimeDatabase db;
    QHash<QString, QStringList> groups;
    QStringList none;
    for (const QString &p : paths) {
        const QString mime = db.mimeTypeForFile(p).name();
        const QString id = defaultFor(mime);
        if (id.isEmpty()) none << p;
        else groups[id] << p;
    }
    for (auto it = groups.begin(); it != groups.end(); ++it) launch(it.key(), it.value(), workspace);
    return none;
}

void Apps::runTerminal(const QString &dir)
{
    QString cmd = Settings::get().terminal.trimmed();
    if (cmd.isEmpty()) cmd = "xdg-terminal-exec";
    QStringList parts = QProcess::splitCommand(cmd);
    const QString prog = parts.takeFirst();
    QStringList args;
    if (!QStandardPaths::findExecutable("uwsm-app").isEmpty()) {
        args << "--" << prog << parts;
        QProcess::startDetached("uwsm-app", args, dir);
    } else {
        QProcess::startDetached(prog, parts, dir);
    }
}

// ----------------------------------------------------------- OpenWithDialog
OpenWithDialog::OpenWithDialog(const QStringList &paths, QWidget *parent) : QDialog(parent), m_paths(paths)
{
    setWindowTitle(QString::fromUtf8("Abrir con…"));
    resize(440, 520);
    m_mime = QMimeDatabase().mimeTypeForFile(paths.value(0)).name();
    auto *v = new QVBoxLayout(this);
    auto *title = new QLabel(paths.size() == 1 ? QString::fromUtf8("Elige la aplicación para abrir «%1»").arg(QFileInfo(paths[0]).fileName())
                                               : QString::fromUtf8("Elige la aplicación para abrir %1 archivos").arg(paths.size()));
    title->setWordWrap(true);
    v->addWidget(title);
    m_search = new QLineEdit;
    m_search->setPlaceholderText(QString::fromUtf8("Buscar aplicación…"));
    v->addWidget(m_search);
    m_list = new QListWidget;
    m_list->setIconSize(QSize(28, 28));
    v->addWidget(m_list, 1);
    m_default = new QCheckBox(QString::fromUtf8("Usar siempre para «%1»").arg(QMimeDatabase().mimeTypeForName(m_mime).comment()));
    v->addWidget(m_default);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Open)->setText("Abrir");
    bb->button(QDialogButtonBox::Cancel)->setText("Cancelar");
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        if (auto *it = m_list->currentItem()) {
            m_chosen = it->data(Qt::UserRole).toString();
            if (!m_chosen.isEmpty()) accept();
        }
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        m_chosen = it->data(Qt::UserRole).toString();
        if (!m_chosen.isEmpty()) accept();
    });
    connect(m_search, &QLineEdit::textChanged, this, &OpenWithDialog::fill);
    fill({});
}

bool OpenWithDialog::makeDefault() const { return m_default->isChecked(); }

void OpenWithDialog::fill(const QString &filter)
{
    m_list->clear();
    Apps &apps = Apps::get();
    auto rec = apps.forMime(m_mime);
    const QString def = apps.defaultFor(m_mime);
    auto addHeader = [&](const QString &t) {
        auto *h = new QListWidgetItem(t);
        h->setFlags(Qt::NoItemFlags);
        QFont f = h->font();
        f.setBold(true);
        h->setFont(f);
        m_list->addItem(h);
    };
    QSet<QString> shown;
    auto add = [&](const AppInfo *a) {
        if (!filter.isEmpty() && !a->name.contains(filter, Qt::CaseInsensitive) && !a->id.contains(filter, Qt::CaseInsensitive)) return;
        auto *it = new QListWidgetItem(Icons::get().app(a->icon), a->id == def ? a->name + QString::fromUtf8("  (predeterminada)") : a->name);
        it->setData(Qt::UserRole, a->id);
        it->setToolTip(a->comment);
        m_list->addItem(it);
        shown.insert(a->id);
    };
    if (!rec.isEmpty()) {
        addHeader(QString::fromUtf8("Recomendadas"));
        for (auto *a : rec) add(a);
    }
    addHeader(QString::fromUtf8("Todas las aplicaciones"));
    for (auto *a : apps.all())
        if (!shown.contains(a->id)) add(a);
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->flags() & Qt::ItemIsSelectable) {
            m_list->setCurrentRow(i);
            break;
        }
}
