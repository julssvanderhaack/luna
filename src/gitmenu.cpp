#include "gitmenu.h"
#include "i18n.h"
#include "icons.h"
#include "settings.h"
#include "theme.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QHeaderView>
#include <QVBoxLayout>

namespace {
using Status = std::function<void(const QString &)>;

QProcessEnvironment gitEnv()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C");
    env.insert("GIT_TERMINAL_PROMPT", "0");   // fail instead of waiting for a terminal that isn't there
    env.insert("GIT_EDITOR", "true");
    return env;
}

// Blocking, for what the menu needs to know before it opens (fast commands).
QString gitNow(const QString &root, const QStringList &args, int *code = nullptr)
{
    QProcess p;
    p.setProcessEnvironment(gitEnv());
    p.start("git", QStringList{"-C", root} + args);
    if (!p.waitForFinished(3000)) {
        p.kill();
        if (code) *code = -1;
        return {};
    }
    if (code) *code = p.exitCode();
    return QString::fromUtf8(p.readAllStandardOutput());
}

// In the background; done(ok, stdout, stderr).
void gitRun(QWidget *parent, const QString &root, const QStringList &args, std::function<void(bool, const QString &, const QString &)> done)
{
    auto *p = new QProcess(parent);
    p->setProcessEnvironment(gitEnv());
    QObject::connect(p, &QProcess::finished, parent, [p, done](int code, QProcess::ExitStatus st) {
        const QString out = QString::fromUtf8(p->readAllStandardOutput()), err = QString::fromUtf8(p->readAllStandardError());
        p->deleteLater();
        done(st == QProcess::NormalExit && code == 0, out, err);
    });
    p->start("git", QStringList{"-C", root} + args);
}

QString firstLines(const QString &s, int n = 6)
{
    QStringList l = s.trimmed().split('\n');
    if (l.size() > n) l = l.mid(l.size() - n);
    return l.join('\n');
}

// Runs a git command and reports: status bar on success, a box on failure.
void act(QWidget *parent, const QString &root, const QStringList &args, const QString &okText, const Status &status)
{
    QPointer<QWidget> w(parent);
    status(i18n("git %1…").arg(args.value(0)));
    gitRun(parent, root, args, [w, okText, status, args](bool ok, const QString &out, const QString &err) {
        if (ok) {
            status(okText);
            return;
        }
        status(i18n("git %1 falló").arg(args.value(0)));
        if (w) QMessageBox::warning(w, "Git", firstLines(err.isEmpty() ? out : err, 12));
    });
}

// A diff, log or blame: monospace, coloured like a terminal.
QString colourDiff(const QString &text)
{
    const Theme &t = Theme::get();
    auto colour = [&](int i, const char *fallback) { return t.ansi[i].isValid() ? t.ansi[i].name() : QString(fallback); };
    const QString add = colour(2, "#4caf50"), del = colour(1, "#e53935"), hunk = colour(6, "#26a69a"), head = colour(3, "#fbc02d");
    QString html = "<pre style=\"font-family:'" + t.mono.toHtmlEscaped() + "'\">";
    for (const QString &l : text.split('\n')) {
        const QString e = l.toHtmlEscaped();
        if (l.startsWith("+++") || l.startsWith("---") || l.startsWith("diff ") || l.startsWith("commit ")) html += "<b style=\"color:" + head + "\">" + e + "</b>\n";
        else if (l.startsWith('+')) html += "<span style=\"color:" + add + "\">" + e + "</span>\n";
        else if (l.startsWith('-')) html += "<span style=\"color:" + del + "\">" + e + "</span>\n";
        else if (l.startsWith("@@")) html += "<span style=\"color:" + hunk + "\">" + e + "</span>\n";
        else html += e + '\n';
    }
    return html + "</pre>";
}

void showText(QWidget *parent, const QString &title, const QString &text)
{
    auto *d = new QDialog(parent);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(title);
    d->resize(900, 640);
    auto *v = new QVBoxLayout(d);
    auto *b = new QTextBrowser;
    b->setHtml(text.trimmed().isEmpty() ? "<p>" + i18n("No hay cambios.").toHtmlEscaped() + "</p>" : colourDiff(text));
    v->addWidget(b);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(bb, &QDialogButtonBox::rejected, d, &QDialog::close);
    v->addWidget(bb);
    d->show();
}

// Commits touching `rel` (or the whole repository); the selected one's
// changes below, and for one file "Restaurar esta versión".
void historyDialog(QWidget *parent, const QString &root, const QStringList &rel, const Status &status)
{
    const bool oneFile = rel.size() == 1 && QFileInfo(root + '/' + rel[0]).isFile();
    QStringList args{"log", "--date=short", "--format=%h%x09%ad%x09%an%x09%s", "-n", "500"};
    if (oneFile) args << "--follow";
    if (!rel.isEmpty()) args << "--" << rel;
    const QString log = gitNow(root, args);
    auto *d = new QDialog(parent);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(rel.isEmpty() ? i18n("Historial del repositorio") : i18n("Historial de %1").arg(rel.join(", ")));
    d->resize(980, 700);
    auto *v = new QVBoxLayout(d);
    auto *split = new QSplitter(Qt::Vertical);
    auto *list = new QTreeWidget;
    list->setHeaderLabels({i18n("Versión"), i18n("Fecha"), i18n("Autor"), i18n("Descripción")});
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->header()->setStretchLastSection(true);
    for (const QString &l : log.split('\n', Qt::SkipEmptyParts)) {
        const QStringList f = l.split('\t');
        new QTreeWidgetItem(list, {f.value(0), f.value(1), f.value(2), f.mid(3).join('\t')});
    }
    for (int c = 0; c < 3; ++c) list->resizeColumnToContents(c);
    auto *diff = new QTextBrowser;
    split->addWidget(list);
    split->addWidget(diff);
    split->setSizes({250, 450});
    v->addWidget(split, 1);
    auto *row = new QHBoxLayout;
    auto *restore = new QPushButton(i18n("Restaurar esta versión…"));
    restore->setVisible(oneFile);
    restore->setEnabled(false);
    row->addWidget(restore);
    row->addStretch();
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    row->addWidget(bb);
    v->addLayout(row);
    QObject::connect(bb, &QDialogButtonBox::rejected, d, &QDialog::close);
    QObject::connect(list, &QTreeWidget::currentItemChanged, d, [=](QTreeWidgetItem *it) {
        restore->setEnabled(it);
        if (!it) return;
        QStringList show{"show", "--stat", "--patch", "--format=commit %H%nAuthor: %an <%ae>%nDate:   %ad%n%n    %s%n", it->text(0)};
        if (!rel.isEmpty()) show << "--" << rel;
        diff->setHtml(colourDiff(gitNow(root, show)));
    });
    QObject::connect(restore, &QPushButton::clicked, d, [=] {
        QTreeWidgetItem *it = list->currentItem();
        if (!it) return;
        if (QMessageBox::question(d, i18n("Restaurar esta versión"),
                                  i18n("¿Volver «%1» a como estaba en %2 («%3»)? Los cambios sin confirmar del archivo se pierden.")
                                      .arg(rel[0], it->text(0), it->text(3))) != QMessageBox::Yes)
            return;
        act(d, root, {"restore", "--source=" + it->text(0), "--worktree", "--", rel[0]}, i18n("«%1» restaurado a %2").arg(rel[0], it->text(0)), status);
    });
    if (list->topLevelItemCount()) list->setCurrentItem(list->topLevelItem(0));
    d->show();
}

// Files to commit (checked = staged ones, or all when nothing is staged), a
// message and "amend".
void commitDialog(QWidget *parent, const QString &root, const Status &status)
{
    const QList<QByteArray> parts = gitNow(root, {"status", "--porcelain=v1", "-z", "--untracked-files=all"}).toUtf8().split('\0');
    struct Change {
        QString xy, path;
    };
    QList<Change> changes;
    bool anyStaged = false;
    for (int i = 0; i < parts.size(); ++i) {
        const QString e = QString::fromUtf8(parts[i]);
        if (e.size() < 4) continue;
        Change c{e.left(2), e.mid(3)};
        if (c.xy[0] == 'R' || c.xy[0] == 'C') ++i;
        anyStaged |= c.xy[0] != ' ' && c.xy[0] != '?';
        changes << c;
    }
    const QString lastMsg = gitNow(root, {"log", "-1", "--format=%B"}).trimmed();
    QDialog d(parent);
    d.setWindowTitle(i18n("Confirmar cambios"));
    d.resize(720, 600);
    auto *v = new QVBoxLayout(&d);
    v->addWidget(new QLabel(changes.isEmpty() ? i18n("No hay nada que confirmar.") : i18n("Archivos que entran en la confirmación:")));
    auto *list = new QListWidget;
    for (const Change &c : changes) {
        const QString what = c.xy == "??" ? i18n("nuevo") : c.xy.contains('D') ? i18n("borrado") : c.xy.contains('R') ? i18n("renombrado")
                                                                                                                       : i18n("modificado");
        auto *it = new QListWidgetItem(c.path + "   (" + what + ")", list);
        it->setData(Qt::UserRole, c.path);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(!anyStaged || (c.xy[0] != ' ' && c.xy[0] != '?') ? Qt::Checked : Qt::Unchecked);
    }
    v->addWidget(list, 1);
    v->addWidget(new QLabel(i18n("Mensaje:")));
    auto *msg = new QPlainTextEdit;
    msg->setFixedHeight(120);
    v->addWidget(msg);
    auto *amend = new QCheckBox(i18n("Corregir la última confirmación (amend)"));
    QObject::connect(amend, &QCheckBox::toggled, &d, [msg, lastMsg](bool on) {
        if (on && msg->toPlainText().trimmed().isEmpty()) msg->setPlainText(lastMsg);
    });
    v->addWidget(amend);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(i18n("Confirmar"));
    v->addWidget(bb);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, [&] {
        if (msg->toPlainText().trimmed().isEmpty()) {
            QMessageBox::warning(&d, d.windowTitle(), i18n("Escribe un mensaje que explique los cambios."));
            return;
        }
        d.accept();
    });
    msg->setFocus();
    if (d.exec() != QDialog::Accepted) return;
    QStringList files;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked) files << list->item(i)->data(Qt::UserRole).toString();
    if (files.isEmpty() && !amend->isChecked()) return;
    const QString message = msg->toPlainText().trimmed();
    const bool isAmend = amend->isChecked();
    QPointer<QWidget> w(parent);
    auto commit = [w, root, files, message, isAmend, status] {
        if (!w) return;
        QStringList args{"commit", "-m", message};
        if (isAmend) args << "--amend";
        if (!files.isEmpty()) args << "--" << files;   // only these, staged or not
        act(w, root, args, i18n("Cambios confirmados"), status);
    };
    if (files.isEmpty()) commit();
    else gitRun(parent, root, QStringList{"add", "-A", "--"} + files, [w, commit](bool ok, const QString &, const QString &err) {
        if (ok) commit();
        else if (w) QMessageBox::warning(w, "Git", firstLines(err));
    });
}

void openLazygit(const QString &dir)
{
    QStringList cmd = QProcess::splitCommand(Settings::get().terminal);
    if (cmd.isEmpty()) cmd = {"xdg-terminal-exec"};
    const QString prog = cmd.takeFirst();
    cmd << "lazygit";
    QProcess::startDetached(prog, cmd, dir);
}
}

namespace GitMenu {
bool haveGit()
{
    static const bool have = !QStandardPaths::findExecutable("git").isEmpty();
    return have;
}

void add(QMenu *menu, QWidget *parent, const QString &root, const QString &dir, const QStringList &paths, Status status)
{
    if (root.isEmpty() || !haveGit()) return;
    Icons &ic = Icons::get();
    QMenu *g = menu->addMenu(ic.ui("git"), "Git");
    const QDir r(root);
    QStringList rel;
    for (const QString &p : paths) rel << r.relativeFilePath(p);

    if (!rel.isEmpty()) {
        // what the clicked items are: X = index, Y = work tree (porcelain v1)
        bool untracked = false, worktree = false, staged = false, conflict = false;
        const QList<QByteArray> parts = gitNow(root, QStringList{"status", "--porcelain=v1", "-z", "--untracked-files=all", "--"} + rel).toUtf8().split('\0');
        for (int i = 0; i < parts.size(); ++i) {
            const QString e = QString::fromUtf8(parts[i]);
            if (e.size() < 4) continue;
            const QString xy = e.left(2);
            if (xy[0] == 'R' || xy[0] == 'C') ++i;
            if (xy == "??") untracked = true;
            else if (xy.contains('U') || xy == "AA" || xy == "DD") conflict = true;
            else {
                staged |= xy[0] != ' ';
                worktree |= xy[1] != ' ';
            }
        }
        int code = 0;
        gitNow(root, QStringList{"check-ignore", "-q", "--"} + rel, &code);
        const bool ignored = code == 0;
        const bool tracked = !untracked && !ignored;
        const QString what = rel.size() == 1 ? QString("«%1»").arg(QFileInfo(rel[0]).fileName()) : i18n("%1 elementos").arg(rel.size());

        if (worktree || staged)
            g->addAction(ic.ui("search"), i18n("Ver cambios"), parent, [=] {
                showText(parent, i18n("Cambios en %1").arg(what), gitNow(root, QStringList{"diff", "HEAD", "--"} + rel));
            });
        if (untracked || worktree || conflict)
            g->addAction(ic.ui("plus"), i18n("Añadir al índice (git add)"), parent,
                         [=] { act(parent, root, QStringList{"add", "-A", "--"} + rel, i18n("Añadido al índice: %1").arg(what), status); });
        if (staged)
            g->addAction(ic.ui("back"), i18n("Quitar del índice"), parent,
                         [=] { act(parent, root, QStringList{"restore", "--staged", "--"} + rel, i18n("Quitado del índice: %1").arg(what), status); });
        if ((worktree || staged) && !conflict)
            g->addAction(ic.ui("restore"), i18n("Descartar cambios (restaurar)…"), parent, [=] {
                if (QMessageBox::question(parent, i18n("Descartar cambios"),
                                          i18n("¿Volver %1 a la última versión confirmada? Los cambios sin confirmar se pierden.").arg(what)) != QMessageBox::Yes)
                    return;
                act(parent, root, QStringList{"restore", "--source=HEAD", "--staged", "--worktree", "--"} + rel, i18n("Restaurado: %1").arg(what), status);
            });
        if (conflict) {
            g->addAction(i18n("Resolver con mi versión"), parent, [=] {
                gitRun(parent, root, QStringList{"checkout", "--ours", "--"} + rel, [=](bool ok, const QString &, const QString &err) {
                    if (ok) act(parent, root, QStringList{"add", "--"} + rel, i18n("Conflicto resuelto con tu versión"), status);
                    else QMessageBox::warning(parent, "Git", firstLines(err));
                });
            });
            g->addAction(i18n("Resolver con la versión de ellos"), parent, [=] {
                gitRun(parent, root, QStringList{"checkout", "--theirs", "--"} + rel, [=](bool ok, const QString &, const QString &err) {
                    if (ok) act(parent, root, QStringList{"add", "--"} + rel, i18n("Conflicto resuelto con su versión"), status);
                    else QMessageBox::warning(parent, "Git", firstLines(err));
                });
            });
        }
        if (tracked) {
            g->addAction(ic.ui("history"), i18n("Historial…"), parent, [=] { historyDialog(parent, root, rel, status); });
            if (rel.size() == 1 && QFileInfo(paths[0]).isFile())
                g->addAction(ic.ui("info"), i18n("Quién cambió cada línea (blame)"), parent, [=] {
                    showText(parent, i18n("Autoría de «%1»").arg(rel[0]), gitNow(root, {"blame", "--date=short", "--", rel[0]}));
                });
        }
        if (untracked)
            g->addAction(ic.ui("hidden"), i18n("Ignorar (añadir a .gitignore)"), parent, [=] {
                QFile f(root + "/.gitignore");
                if (!f.open(QIODevice::ReadWrite | QIODevice::Text)) return;
                const QByteArray old = f.readAll();
                f.seek(f.size());
                if (!old.isEmpty() && !old.endsWith('\n')) f.write("\n");
                for (const QString &x : rel) f.write(("/" + x + (QFileInfo(root + '/' + x).isDir() ? "/" : "") + "\n").toUtf8());
                status(i18n("Añadido a .gitignore: %1").arg(what));
            });
        if (tracked && !worktree && !staged)
            g->addAction(ic.ui("cancel"), i18n("Dejar de seguir (sin borrarlo)"), parent, [=] {
                act(parent, root, QStringList{"rm", "-r", "--cached", "--quiet", "--"} + rel, i18n("Ya no se sigue: %1").arg(what), status);
            });
        g->addSeparator();
    }

    // the repository
    const QString branch = gitNow(root, {"branch", "--show-current"}).trimmed();
    g->addAction(ic.ui("check"), i18n("Confirmar cambios (commit)…"), parent, [=] { commitDialog(parent, root, status); });
    if (!gitNow(root, {"remote"}).trimmed().isEmpty()) {   // nothing to pull from or push to otherwise
        g->addAction(ic.ui("back"), i18n("Traer cambios (pull)"), parent, [=] { act(parent, root, {"pull", "--ff-only"}, i18n("Traídos los cambios"), status); });
        g->addAction(ic.ui("forward"), i18n("Enviar cambios (push)"), parent, [=] {
            QPointer<QWidget> w(parent);
            status(i18n("git push…"));
            gitRun(parent, root, {"push"}, [=](bool ok, const QString &, const QString &err) {
                if (ok) {
                    status(i18n("Cambios enviados"));
                    return;
                }
                if (err.contains("has no upstream branch") && !branch.isEmpty()) {   // first push of a new branch
                    act(w, root, {"push", "-u", "origin", branch}, i18n("Rama «%1» enviada").arg(branch), status);
                    return;
                }
                status(i18n("git push falló"));
                if (w) QMessageBox::warning(w, "Git", firstLines(err, 12));
            });
        });
        g->addAction(ic.ui("refresh"), i18n("Actualizar remotos (fetch)"), parent, [=] { act(parent, root, {"fetch", "--all", "--prune"}, i18n("Remotos actualizados"), status); });
    }

    QMenu *branches = g->addMenu(ic.ui("branch"), branch.isEmpty() ? i18n("Rama") : i18n("Rama: %1").arg(branch));
    for (const QString &b : gitNow(root, {"branch", "--format=%(refname:short)"}).split('\n', Qt::SkipEmptyParts)) {
        QAction *a = branches->addAction(b, parent, [=] { act(parent, root, {"switch", b}, i18n("Ahora en la rama «%1»").arg(b), status); });
        a->setCheckable(true);
        a->setChecked(b == branch);
    }
    branches->addSeparator();
    branches->addAction(i18n("Nueva rama…"), parent, [=] {
        bool ok = false;
        const QString name = QInputDialog::getText(parent, i18n("Nueva rama"), i18n("Nombre de la rama:"), QLineEdit::Normal, {}, &ok).trimmed();
        if (ok && !name.isEmpty()) act(parent, root, {"switch", "-c", name}, i18n("Creada la rama «%1»").arg(name), status);
    });

    g->addAction(i18n("Guardar cambios aparte (stash)"), parent,
                 [=] { act(parent, root, {"stash", "push", "--include-untracked", "-m", "Luna"}, i18n("Cambios guardados aparte"), status); });
    if (!gitNow(root, {"stash", "list", "-n", "1"}).trimmed().isEmpty())
        g->addAction(i18n("Recuperar lo guardado (stash pop)"), parent, [=] { act(parent, root, {"stash", "pop"}, i18n("Cambios recuperados"), status); });
    g->addSeparator();
    g->addAction(ic.ui("history"), i18n("Historial del repositorio…"), parent, [=] { historyDialog(parent, root, {}, status); });
    static const bool lazygit = !QStandardPaths::findExecutable("lazygit").isEmpty();
    if (lazygit) g->addAction(ic.ui("terminal"), i18n("Abrir lazygit"), parent, [dir] { openLazygit(dir); });
}

void addInit(QMenu *menu, QWidget *parent, const QString &dir, Status status)
{
    if (!haveGit()) return;
    menu->addAction(Icons::get().ui("git"), i18n("Crear repositorio git aquí"), parent,
                    [=] { act(parent, dir, {"init"}, i18n("Repositorio git creado"), status); });
}
}
