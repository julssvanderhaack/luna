#include "testhooks.h"
#include "crypto.h"
#include "fsmodel.h"
#include "gitmenu.h"
#include <QMenu>
#include <QToolButton>
#include <QRadioButton>
#include <QKeyEvent>
#include "i18n.h"
#include "icons.h"
#include "thumbs.h"
#include <QThreadPool>
#include "mainwindow.h"
#include "pane.h"
#include "preview.h"
#include "properties.h"
#include "remote.h"
#include "settingsdialog.h"
#include "smoothscroll.h"
#include <QElapsedTimer>
#include <QTimer>
#include "settings.h"
#include "theme.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QFile>
#include <QFileSystemWatcher>
#include <QPalette>
#include <QPixmapCache>
#include <QPainter>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QAbstractItemView>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QMouseEvent>
#include <QTabWidget>
#include <QUrl>
#include <QStackedWidget>
#include <memory>
#include <functional>
#include <QWheelEvent>
#include <QScrollBar>
#include <QColumnView>
#include <QMimeData>
#include <QDragEnterEvent>

namespace TestHooks {

void beforeWindow(QStringList &paths)
{
    struct MetaCallLogger : QObject {
        bool eventFilter(QObject *o, QEvent *e) override
        {
            if (e->type() == QEvent::MetaCall) {
                QWidget *pw = o->isWidgetType() ? static_cast<QWidget *>(o)->parentWidget() : nullptr;
                fprintf(stderr, "metacall -> %s '%s' parent=%s\n", o->metaObject()->className(), qPrintable(o->objectName()),
                        pw ? pw->metaObject()->className() : "-");
                fflush(stderr);
            }
            return false;
        }
    };
    if (qEnvironmentVariableIsSet("LUNA_DEBUG_META")) qApp->installEventFilter(new MetaCallLogger);
    // LUNA_SHOTS takes a photo for Propiedades as second argument: not a tab
    if (qEnvironmentVariableIsSet("LUNA_SHOTS")) paths = paths.mid(0, 1);
}

void install(MainWindow *w, const QStringList &paths, QElapsedTimer startup)
{
    // Thumbnail benchmark: LUNA_THUMBTEST=1 luna <folder>
    if (qEnvironmentVariableIsSet("LUNA_THUMBTEST")) {
        auto *t = new QElapsedTimer;
        t->start();
        auto *count = new int(0);
        QObject::connect(&Thumbs::get(), &Thumbs::ready, w, [t, count](const QString &) {
            ++*count;
            if (*count == 1 || *count == 10 || *count == 30) fprintf(stderr, "miniatura %d lista a los %lld ms\n", *count, qlonglong(t->elapsed()));
        });
        QTimer::singleShot(15000, w, [count] { fprintf(stderr, "total en 15 s: %d entregas\n", *count); qApp->quit(); });
    }
    // Search benchmark: LUNA_SEARCHTEST=<pattern> luna <folder>
    if (qEnvironmentVariableIsSet("LUNA_SEARCHTEST")) {
        QTimer::singleShot(800, w, [w] {
            auto *m = w->pane()->findChild<FsModel *>();
            auto *timer = new QElapsedTimer;
            timer->start();
            QObject::connect(m, &FsModel::searchProgress, w, [timer](int n, bool done) {
                if (!done) return;
                fprintf(stderr, "RESULT %d coincidencias en %lld ms\n", n, qlonglong(timer->elapsed()));
                qApp->quit();
            });
            m->startSearch(qEnvironmentVariable("LUNA_SEARCHTEST"));
        });
    }
    // Listing benchmark: LUNA_LISTTEST=1 luna <folder> (QT_QPA_PLATFORM=offscreen works)
    if (qEnvironmentVariableIsSet("LUNA_LISTTEST")) {
        auto *t = new QElapsedTimer;
        t->start();
        fprintf(stderr, "ventana creada a los %lld ms del arranque\n", qlonglong(startup.elapsed()));
        auto *m = w->pane()->findChild<FsModel *>();
        QObject::connect(m, &FsModel::rootLoaded, w, [w, t] {
            fprintf(stderr, "listado entregado a los %lld ms\n", qlonglong(t->elapsed()));
            QTimer::singleShot(0, w, [w, t] {
                w->repaint();
                fprintf(stderr, "vista pintada a los %lld ms\n", qlonglong(t->elapsed()));
                auto *proxy = w->pane()->findChild<SortProxy *>();
                auto step = [w](const char *what, const std::function<void()> &f) {
                    QElapsedTimer s;
                    s.start();
                    f();
                    w->repaint();
                    fprintf(stderr, "%s: %lld ms\n", what, qlonglong(s.elapsed()));
                };
                step("ordenar por tamaño", [proxy] { proxy->sort(FsModel::Size); });
                step("ordenar por tipo", [proxy] { proxy->sort(FsModel::Type); });
                step("ordenar por nombre", [proxy] { proxy->sort(FsModel::Name); });
                step("filtrar «12»", [proxy] { proxy->setQuickFilter("12"); });
                step("quitar filtro", [proxy] { proxy->setQuickFilter({}); });
                step("seleccionar todo", [w] { w->pane()->selectAll(); });
                step("vista detallada", [w] { w->pane()->setViewMode(ViewMode::Details); });
                step("vista iconos", [w] { w->pane()->setViewMode(ViewMode::Icons); });
                QTimer::singleShot(1500, w, [w] {
                    auto *m = w->pane()->findChild<FsModel *>();
                    int scripts = 0, counted = 0;
                    for (int r = 0; r < m->rowCount(); ++r) {
                        const FileEntry *e = m->entry(m->index(r, 0));
                        scripts += !e->isDir && !e->name.contains(QLatin1Char('.')) && e->mime.endsWith(QLatin1String("shellscript"));
                        counted += e->isDir && e->size >= 0;
                    }
                    fprintf(stderr, "scripts detectados por contenido: %d, carpetas con recuento: %d\n", scripts, counted);
                    qApp->quit();
                });
            });
        });
    }
    // Move test: LUNA_MOVETEST=1 luna <dir with a.txt b.txt destino/>
    if (qEnvironmentVariableIsSet("LUNA_MOVETEST")) {
        QTimer::singleShot(1200, w, [w] {
            Pane *p = w->pane();
            QAbstractItemView *v = nullptr;
            for (auto *x : p->findChildren<QAbstractItemView *>())
                if (x->isVisible() && !qobject_cast<QHeaderView *>(x)) v = x;
            auto *m = v->model();
            for (int r = 0; r < m->rowCount(); ++r) {
                const QString n = m->index(r, 0).data().toString();
                if (n == "a.txt" || n == "b.txt") v->selectionModel()->select(m->index(r, 0), QItemSelectionModel::Select);
            }
            p->transferSelection(true, p->path() + "/destino");
        });
        QTimer::singleShot(3500, w, [] { qApp->quit(); });
    }
    // Right-click test: LUNA_MENUTEST=<dir> luna <folder>
    if (qEnvironmentVariableIsSet("LUNA_MENUTEST")) {
        const QString dir = qEnvironmentVariable("LUNA_MENUTEST");
        auto clickAt = [w](bool onItem) {
            QAbstractItemView *v = nullptr;
            for (auto *x : w->pane()->findChildren<QAbstractItemView *>())
                if (x->isVisible() && !qobject_cast<QHeaderView *>(x) && x->model() && x->model()->columnCount() > 1) v = x;
            QRect r0 = v->visualRect(v->model()->index(3, 0)), r1 = v->visualRect(v->model()->index(4, 0));
            QPoint gap((r0.right() + r1.left()) / 2, r0.bottom() - 2);   // between two icons, near their text
            QPoint pos = onItem ? r1.center() : gap;
            // Like a real right click: press, then the context menu event
            QMouseEvent press(QEvent::MouseButtonPress, pos, v->viewport()->mapToGlobal(pos), Qt::RightButton, Qt::RightButton, Qt::NoModifier);
            QApplication::sendEvent(v->viewport(), &press);
            QContextMenuEvent ev(QContextMenuEvent::Mouse, pos, v->viewport()->mapToGlobal(pos));
            QApplication::sendEvent(v->viewport(), &ev);
        };
        auto grabMenu = [dir](const QString &name) {
            QTimer::singleShot(400, [dir, name] {
                if (QWidget *m = QApplication::activePopupWidget()) {
                    m->grab().save(dir + "/" + name + ".png");
                    m->close();
                }
            });
        };
        QTimer::singleShot(1500, w, [=] { grabMenu("menu-archivo"); clickAt(true); });
        QTimer::singleShot(3000, w, [=] { grabMenu("menu-fondo"); clickAt(false); });
        QTimer::singleShot(4500, w, [=] { w->pane()->setViewMode(ViewMode::Details); });
        QTimer::singleShot(5500, w, [=] { grabMenu("menu-archivo-lista"); clickAt(true); });
        QTimer::singleShot(6500, w, [=] { grabMenu("menu-fondo-lista"); clickAt(false); });
        QTimer::singleShot(8000, w, [=] { qApp->quit(); });
    }
    // Split drag & drop check: LUNA_DNDTEST=<dirB> luna <dirA>. In every view
    // mode drags the first file of pane A and drops it on pane B, once on empty
    // space and once on top of a file; prints where the file ended up.
    if (qEnvironmentVariableIsSet("LUNA_DNDTEST")) {
        const QString dirB = qEnvironmentVariable("LUNA_DNDTEST");
        auto viewOf = [](Pane *p) {
            auto *v = qobject_cast<QAbstractItemView *>(p->findChild<QStackedWidget *>()->currentWidget());
            if (auto *cv = qobject_cast<QColumnView *>(v))
                for (auto *c : cv->findChildren<QAbstractItemView *>()) if (c->isVisible()) v = c;
            return v;
        };
        auto drop = [=](QAbstractItemView *from, QAbstractItemView *to, QPoint at) {
            QModelIndex src;
            for (int r = 0; r < from->model()->rowCount(from->rootIndex()); ++r) {
                QModelIndex i = from->model()->index(r, 0, from->rootIndex());
                if (!i.data(FsModel::IsDirRole).toBool()) { src = i; break; }
            }
            if (!src.isValid()) return QString("sin fichero en A");
            const QString name = src.data().toString();
            QMimeData *md = from->model()->mimeData({src});
            QWidget *vp = to->viewport();
            QDragEnterEvent en(at, Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, md, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(vp, &en);
            QDragMoveEvent mv(at, Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, md, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(vp, &mv);
            QString r = name + QString(" enter=%1 move=%2(%3)").arg(en.isAccepted()).arg(mv.isAccepted()).arg(int(mv.dropAction()));
            if (mv.isAccepted()) {
                QDropEvent dr(at, Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, md, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(vp, &dr);
            }
            delete md;
            return r;
        };
        QTimer::singleShot(500, w, [=] {
            w->page()->setSplit(true);
            Pane *b = w->page()->active(), *a = w->page()->other();
            b->setPath(dirB);
            auto run = std::make_shared<std::function<void(int)>>();
            *run = [=](int mode) {
                if (mode > int(ViewMode::Columns)) { qApp->quit(); return; }
                a->setViewMode(ViewMode(mode));
                b->setViewMode(ViewMode(mode));
                QTimer::singleShot(700, w, [=] {
                    QAbstractItemView *va = viewOf(a), *vb = viewOf(b);
                    const QPoint empty(vb->viewport()->width() - 5, vb->viewport()->height() - 5);
                    fprintf(stderr, "modo %d hueco: %s\n", mode, qPrintable(drop(va, vb, empty)));
                    QTimer::singleShot(700, w, [=] {
                        QModelIndex f;
                        for (int r = 0; r < vb->model()->rowCount(vb->rootIndex()); ++r) {
                            QModelIndex i = vb->model()->index(r, 0, vb->rootIndex());
                            if (!i.data(FsModel::IsDirRole).toBool()) { f = i; break; }
                        }
                        const QPoint on = f.isValid() ? vb->visualRect(f).center() : empty;
                        fprintf(stderr, "modo %d encima de %s: %s\n", mode, f.isValid() ? qPrintable(f.data().toString()) : "-",
                                qPrintable(drop(va, vb, on)));
                        QTimer::singleShot(700, w, [=] {
                            // same pane: first file of B onto its folder "sub"
                            QModelIndex dir;
                            for (int r = 0; r < vb->model()->rowCount(vb->rootIndex()); ++r) {
                                QModelIndex i = vb->model()->index(r, 0, vb->rootIndex());
                                if (i.data(FsModel::IsDirRole).toBool()) { dir = i; break; }
                            }
                            if (dir.isValid() && mode != int(ViewMode::Columns))
                                fprintf(stderr, "modo %d mismo panel sobre %s: %s\n", mode, qPrintable(dir.data().toString()),
                                        qPrintable(drop(vb, vb, vb->visualRect(dir).center())));
                            QTimer::singleShot(700, w, [=] { (*run)(mode + 1); });
                        });
                    });
                });
            };
            (*run)(0);
        });
    }
    // Wheel animation check: LUNA_SCROLLTEST=1 luna <big folder> prints the
    // scroll bar every ~20 ms after one notch, a quick burst of 4 and a
    // touchpad swipe of 5 x 10 px.
    if (qEnvironmentVariableIsSet("LUNA_SCROLLTEST")) {
        auto notch = [](QAbstractScrollArea *a) {
            QWidget *vp = a->viewport();
            const QPointF p(vp->width() / 2, vp->height() / 2);
            QWheelEvent ev(p, vp->mapToGlobal(p), {}, {0, -120}, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(vp, &ev);
        };
        auto swipe = [](QAbstractScrollArea *a) {   // touchpad: pixels plus the angle Qt derives from them
            QWidget *vp = a->viewport();
            const QPointF p(vp->width() / 2, vp->height() / 2);
            QWheelEvent ev(p, vp->mapToGlobal(p), {0, -10}, {0, -80}, Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
            QApplication::sendEvent(vp, &ev);
        };
        auto run = std::make_shared<std::function<void(int)>>();
        *run = [=](int mode) {
            if (mode > int(ViewMode::Columns)) { qApp->quit(); return; }
            w->pane()->setViewMode(ViewMode(mode));
            QTimer::singleShot(600, w, [=] {
                auto *v = qobject_cast<QAbstractItemView *>(w->pane()->findChild<QStackedWidget *>()->currentWidget());
                if (auto *cv = qobject_cast<QColumnView *>(v))
                    for (auto *c : cv->findChildren<QAbstractItemView *>()) if (c->isVisible()) v = c;
                QScrollBar *bar = v->verticalScrollBar()->maximum() > 0 ? v->verticalScrollBar() : v->horizontalScrollBar();
                auto samples = std::make_shared<QStringList>();
                auto timer = new QElapsedTimer;
                timer->start();
                QObject::connect(bar, &QScrollBar::valueChanged, w, [=](int val) { *samples << QString("%1ms=%2").arg(timer->elapsed()).arg(val); });
                *samples << QString("start=%1 max=%2 step=%3").arg(bar->value()).arg(bar->maximum()).arg(bar->singleStep());
                notch(v);
                QTimer::singleShot(300, w, [=] {
                    *samples << "| burst";
                    for (int i = 0; i < 4; ++i) QTimer::singleShot(i * 40, w, [=] { notch(v); });
                });
                QTimer::singleShot(800, w, [=] {
                    const int before = bar->value() == bar->maximum() ? 0 : bar->value();
                    bar->setValue(before);
                    for (int i = 0; i < 5; ++i) swipe(v);
                    *samples << QString("| touchpad 5x10px: %1 -> %2").arg(before).arg(bar->value());
                });
                QTimer::singleShot(900, w, [=] {
                    QObject::disconnect(bar, &QScrollBar::valueChanged, w, nullptr);
                    printf("%s (%s): %lld cambios\n  %s\n", v->metaObject()->className(), qPrintable(QString::number(mode)),
                           qlonglong(samples->size()), qPrintable(samples->join(' ')));
                    fflush(stdout);
                    delete timer;
                    (*run)(mode + 1);
                });
            });
        };
        QTimer::singleShot(1500, w, [=] { (*run)(0); });
    }
    // GnuPG check: GNUPGHOME=<empty dir> LUNA_GPGTEST=<work dir with a folder
    // "proyecto"> luna — makes a key, encrypts and decrypts both ways.
    if (qEnvironmentVariableIsSet("LUNA_GPGTEST")) {
        const QString d = qEnvironmentVariable("LUNA_GPGTEST");
        auto log = [](const char *step, bool ok, const QString &msg) { fprintf(stderr, "%s: %s  %s\n", step, ok ? "OK" : "FALLO", qPrintable(msg)); };
        QTimer::singleShot(300, w, [=] {
            Crypto::createKey("Prueba Luna", "prueba@luna.test", "", w, [=](bool ok, const QString &msg, const QString &) {
                log("crear clave", ok, msg);
                const QList<Crypto::Key> ks = Crypto::keys();
                for (const auto &k : ks) fprintf(stderr, "  clave %s secret=%d valid=%d %s\n", qPrintable(k.uid), k.secret, k.valid, qPrintable(k.shortId));
                Crypto::EncryptJob sym;
                sym.paths = {d + "/proyecto"};
                sym.out = d + "/proyecto.tar.gpg";
                sym.passphrase = "secreto ñ";
                Crypto::encrypt(sym, w, [=](bool ok, const QString &msg, const QString &) {
                    log("cifrar carpeta con contraseña", ok, msg);
                    fprintf(stderr, "  pide contraseña: %d\n", Crypto::needsPassphrase(sym.out));
                    QDir().mkpath(d + "/out");
                    Crypto::decrypt(sym.out, d + "/out", "mala", w, [=](bool ok, const QString &msg, const QString &) {
                        log("descifrar con contraseña mala (debe fallar)", !ok, msg);
                        Crypto::decrypt(sym.out, d + "/out", "secreto ñ", w, [=](bool ok, const QString &msg, const QString &made) {
                            log("descifrar carpeta", ok, msg + " → " + made);
                            Crypto::EncryptJob pub;
                            pub.paths = {d + "/proyecto/LEEME.txt"};
                            pub.out = d + "/LEEME.txt.gpg";
                            pub.symmetric = false;
                            for (const auto &k : ks) pub.recipients << k.fingerprint;
                            if (!ks.isEmpty()) pub.signKey = ks[0].fingerprint;
                            Crypto::encrypt(pub, w, [=](bool ok, const QString &msg, const QString &) {
                                log("cifrar archivo para la clave y firmar", ok, msg);
                                fprintf(stderr, "  pide contraseña: %d\n", Crypto::needsPassphrase(pub.out));
                                Crypto::decrypt(pub.out, d + "/out", "", w, [=](bool ok, const QString &msg, const QString &made) {
                                    log("descifrar archivo", ok, msg + " → " + made);
                                    qApp->quit();
                                });
                            });
                        });
                    });
                });
            });
        });
    }
    // Git menu check: LUNA_GITTEST=<repo> LUNA_GITFILES=a,b,… luna — prints the
    // right-click "Git" submenu for each file (and the folder), then runs the
    // action LUNA_GITDO=<file>|<text> and prints `git status`.
    if (qEnvironmentVariableIsSet("LUNA_GITTEST")) {
        const QString repo = qEnvironmentVariable("LUNA_GITTEST");
        QTimer::singleShot(300, w, [=] {
            auto status = [](const QString &s) { fprintf(stderr, "  [estado] %s\n", qPrintable(s)); };
            std::function<void(QMenu *, int)> dump = [&](QMenu *m, int depth) {
                for (QAction *a : m->actions()) {
                    if (a->isSeparator()) continue;
                    fprintf(stderr, "%*s%s%s\n", depth * 2, "", qPrintable(a->text()), a->isChecked() ? "  ✓" : "");
                    if (a->menu()) dump(a->menu(), depth + 1);
                }
            };
            for (const QString &f : qEnvironmentVariable("LUNA_GITFILES").split(',', Qt::SkipEmptyParts)) {
                QMenu m;
                GitMenu::add(&m, w, repo, repo, {repo + '/' + f}, status);
                fprintf(stderr, "== %s\n", qPrintable(f));
                dump(&m, 1);
            }
            QMenu folder;
            GitMenu::add(&folder, w, repo, repo, {}, status);
            fprintf(stderr, "== (carpeta)\n");
            dump(&folder, 1);
            const QStringList todo = qEnvironmentVariable("LUNA_GITDO").split('|');
            if (todo.size() == 2) {
                QMenu m;
                GitMenu::add(&m, w, repo, repo, {repo + '/' + todo[0]}, status);
                for (QAction *a : m.actions().first()->menu()->actions())
                    if (a->text() == todo[1]) {
                        fprintf(stderr, "== ejecuto «%s» en %s\n", qPrintable(todo[1]), qPrintable(todo[0]));
                        a->trigger();
                    }
            }
            QTimer::singleShot(1500, w, [] {
                int n = 0;
                if (qEnvironmentVariableIsSet("LUNA_GITSHOT"))
                    for (QWidget *t : QApplication::topLevelWidgets())
                        if (t->isVisible() && t->inherits("QDialog")) t->grab().save(qEnvironmentVariable("LUNA_GITSHOT") + QString("/git%1.png").arg(n++));
                qApp->quit();
            });
        });
    }
    // Quick look check: LUNA_PREVIEWTEST=<dir> luna <folder> <file>… opens the
    // Space preview of each file, waits LUNA_PREVIEWWAIT ms (default 4000)
    // and saves <dir>/preview-<n>.png.
    if (qEnvironmentVariableIsSet("LUNA_PREVIEWTEST")) {
        const QString dir = qEnvironmentVariable("LUNA_PREVIEWTEST");
        const int wait = qEnvironmentVariableIntValue("LUNA_PREVIEWWAIT") ? qEnvironmentVariableIntValue("LUNA_PREVIEWWAIT") : 4000;
        const QStringList files = QCoreApplication::arguments().mid(2);
        auto run = std::make_shared<std::function<void(int)>>();
        *run = [=](int i) {
            if (i >= files.size()) { qApp->quit(); return; }
            auto *pv = new Preview(QFileInfo(files[i]).absoluteFilePath(), {}, w);
            pv->show();
            QTimer::singleShot(wait, w, [=] {
                pv->grab().save(QString("%1/preview-%2.png").arg(dir).arg(i));
                pv->close();
                (*run)(i + 1);
            });
        };
        QTimer::singleShot(500, w, [run] { (*run)(0); });
    }
    // Scripted screenshots for development: LUNA_SHOTS=<dir> luna <folder> [photo]
    if (qEnvironmentVariableIsSet("LUNA_SHOT_SIZE")) {
        const QStringList wh = qEnvironmentVariable("LUNA_SHOT_SIZE").split('x');
        w->resize(wh.value(0).toInt(), wh.value(1).toInt());
    }
    if (qEnvironmentVariableIsSet("LUNA_SHOTS")) {
        const QString dir = qEnvironmentVariable("LUNA_SHOTS");
        const QString photo = paths.value(1);
        auto shot = [dir](QWidget *x, const QString &n) { x->grab().save(dir + "/" + n + ".png"); };
        QTimer::singleShot(1500, w, [=] { shot(w, "1-iconos"); w->pane()->setViewMode(ViewMode::Details); });
        QTimer::singleShot(2300, w, [=] { shot(w, "2-detallada"); w->pane()->setViewMode(ViewMode::Tree); });
        QTimer::singleShot(3000, w, [=] { shot(w, "3-arbol"); w->pane()->setViewMode(ViewMode::Columns); });
        QTimer::singleShot(3700, w, [=] { shot(w, "4-columnas"); w->pane()->setViewMode(ViewMode::Compact); });
        QTimer::singleShot(4400, w, [=] {
            shot(w, "5-compacta");
            w->page()->setSplit(true);
        });
        QTimer::singleShot(4900, w, [=] {
            shot(w, "5b-dividida");
            w->page()->setSplit(false);
            w->pane()->setViewMode(ViewMode::Icons);
            QMetaObject::invokeMethod(w, [w] { w->findChild<QWidget *>(); });
        });
        QTimer::singleShot(5200, w, [=] {
            if (!photo.isEmpty()) {
                auto *d = new PropertiesDialog({photo}, w);
                d->show();
                QTimer::singleShot(1500, d, [=] {
                    shot(d, "6-propiedades");
                    if (auto *tabs = d->findChild<QTabWidget *>()) {
                        tabs->setCurrentIndex(1);
                        QTimer::singleShot(300, d, [=] {
                            shot(d, "7-permisos");
                            tabs->setCurrentIndex(2);
                            QTimer::singleShot(1200, d, [=] { shot(d, "8-metadatos"); d->close(); });
                        });
                    }
                });
            }
        });
        QTimer::singleShot(9000, w, [=] {
            auto *s = new SettingsDialog(w);
            s->show();
            QTimer::singleShot(500, s, [=] {
                shot(s, "9-ajustes");
                s->showPage(i18n("Comportamiento"));
                shot(s, "9b-comportamiento");
                s->showPage(i18n("Apariencia"));
                shot(s, "9c-apariencia");
                s->close();
                auto *c = new ConnectDialog(w);
                c->show();
                QTimer::singleShot(400, c, [=] { shot(c, "10-conectar"); c->close(); qApp->quit(); });
            });
        });
    }
    // README screenshots of the newer features: LUNA_TOUR=<dir> with
    // LUNA_TOUR_DOC (Office file), LUNA_TOUR_ZIP (archive), LUNA_TOUR_GIT (a
    // changed file in a repository), LUNA_TOUR_CRYPT (what to encrypt) and
    // LUNA_TOUR_TERM (a command for the docked terminal). Saves the windows
    // and geom.txt (name x y w h) so they can be put over the main window.
    if (qEnvironmentVariableIsSet("LUNA_TOUR")) {
        const QString dir = qEnvironmentVariable("LUNA_TOUR");
        auto geom = std::make_shared<QFile>(dir + "/geom.txt");
        if (!geom->open(QIODevice::WriteOnly | QIODevice::Text)) qWarning("LUNA_TOUR: cannot write %s", qPrintable(geom->fileName()));
        auto shot = [dir, geom](QWidget *x, const QString &n) {
            x->grab().save(dir + "/" + n + ".png");
            const QRect g = x->isWindow() ? x->geometry() : QRect(x->mapTo(x->window(), QPoint()), x->size());
            geom->write(QString("%1 %2 %3 %4 %5\n").arg(n).arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height()).toUtf8());
            geom->flush();
        };
        auto steps = std::make_shared<QList<std::function<void(std::function<void()>)>>>();
        auto preview = [=](const QString &file, const QString &name, int wait) {
            steps->append([=](std::function<void()> next) {
                if (file.isEmpty()) return next();
                w->pane()->setPath(QFileInfo(file).absolutePath(), true, {file});
                QTimer::singleShot(1500, w, [=] {
                    shot(w, name + "-fondo");
                    auto *pv = new Preview(file, {}, w);
                    pv->show();
                    QTimer::singleShot(wait, w, [=] {
                        shot(pv, name);
                        pv->close();
                        next();
                    });
                });
            });
        };
        preview(qEnvironmentVariable("LUNA_TOUR_DOC"), "word", 9000);
        preview(qEnvironmentVariable("LUNA_TOUR_ZIP"), "zip", 2000);
        const QString gitFile = qEnvironmentVariable("LUNA_TOUR_GIT");
        if (!gitFile.isEmpty())
            steps->append([=](std::function<void()> next) {
                w->pane()->setPath(QFileInfo(gitFile).absolutePath(), true, {gitFile});
                QTimer::singleShot(2000, w, [=] {
                    QAbstractItemView *v = nullptr;
                    for (auto *x : w->pane()->findChildren<QAbstractItemView *>())
                        if (x->isVisible()) v = x;
                    if (!v) return next();
                    QModelIndex at;
                    for (int r = 0; r < v->model()->rowCount(v->rootIndex()); ++r) {
                        const QModelIndex ix = v->model()->index(r, 0, v->rootIndex());
                        if (ix.data().toString() == QFileInfo(gitFile).fileName()) at = ix;
                    }
                    const QPoint pos = v->visualRect(at).center() + QPoint(40, 0);
                    QTimer::singleShot(500, w, [=] {
                        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                        if (!menu) return;
                        for (QAction *a : menu->actions())
                            if (a->text() == "Git") menu->setActiveAction(a);
                        QTimer::singleShot(600, w, [=] {
                            shot(w, "git-fondo");
                            shot(menu, "menu");
                            if (QAction *a = menu->activeAction(); a && a->menu()) shot(a->menu(), "git");
                            menu->close();
                            QTimer::singleShot(300, w, next);
                        });
                    });
                    emit v->customContextMenuRequested(pos);
                });
            });
        const QString crypt = qEnvironmentVariable("LUNA_TOUR_CRYPT");
        if (!crypt.isEmpty())
            steps->append([=](std::function<void()> next) {
                w->pane()->setPath(QFileInfo(crypt).absolutePath(), true, {crypt});
                QTimer::singleShot(1200, w, [=] {
                    shot(w, "cifrar-fondo");
                    QTimer::singleShot(800, w, [=] {
                        auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                        if (!d) return;
                        for (auto *r : d->findChildren<QRadioButton *>())   // the keys are listed when this is picked
                            if (r->text() == i18n("Para estas personas (con sus claves públicas)")) r->click();
                        QTimer::singleShot(1500, d, [=] {
                            shot(d, "cifrar");
                            d->reject();
                        });
                    });
                    Crypto::encryptDialog(w, {crypt}, QFileInfo(crypt).absolutePath(), [](bool, const QString &, const QString &) {});
                    QTimer::singleShot(300, w, next);
                });
            });
        const QString term = qEnvironmentVariable("LUNA_TOUR_TERM");
        if (!term.isEmpty())
            steps->append([=](std::function<void()> next) {
                if (!gitFile.isEmpty()) w->pane()->setPath(QFileInfo(gitFile).absolutePath());
                for (auto *b : w->findChildren<QToolButton *>())
                    if (b->toolTip() == i18n("Terminal (F4)") && !b->isChecked()) b->click();
                QTimer::singleShot(2500, w, [=] {
                    for (auto *t : w->findChildren<QWidget *>())
                        if (t->inherits("Terminal")) {
                            for (const QChar c : term) {
                                QKeyEvent k(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
                                QCoreApplication::sendEvent(t, &k);
                            }
                            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
                            QCoreApplication::sendEvent(t, &enter);
                        }
                    QTimer::singleShot(2000, w, [=] {
                        shot(w, "terminal");
                        next();
                    });
                });
            });
        auto run = std::make_shared<std::function<void(int)>>();
        *run = [steps, run](int i) {
            if (i >= steps->size()) return qApp->quit();
            (*steps)[i]([run, i] { QTimer::singleShot(0, qApp, [run, i] { (*run)(i + 1); }); });
        };
        QTimer::singleShot(1500, w, [run] { (*run)(0); });
    }
}

}
