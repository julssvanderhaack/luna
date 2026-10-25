#include "fsmodel.h"
#include "icons.h"
#include "thumbs.h"
#include <QThreadPool>
#include "mainwindow.h"
#include "pane.h"
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

namespace {
// Fusion without the stock icons on dialog buttons (red/green blobs).
class LunaStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    int styleHint(StyleHint h, const QStyleOption *o, const QWidget *w, QStyleHintReturn *r) const override
    {
        if (h == SH_DialogButtonBox_ButtonsHaveIcons) return 0;
        return QProxyStyle::styleHint(h, o, w, r);
    }
    // Tree view expanders: a thin chevron in the text colour. (A style sheet
    // rule on QTreeView::branch would take over and draw no arrow at all.)
    void drawPrimitive(PrimitiveElement pe, const QStyleOption *o, QPainter *p, const QWidget *w) const override
    {
        if (pe != PE_IndicatorBranch) return QProxyStyle::drawPrimitive(pe, o, p, w);
        if (!(o->state & State_Children)) return;   // no guide lines, like Nautilus
        const QPointF c = QRectF(o->rect).center();
        const qreal r = 3.5;
        QPolygonF chevron;
        if (o->state & State_Open) chevron << QPointF(c.x() - r, c.y() - r / 2) << QPointF(c.x(), c.y() + r / 2) << QPointF(c.x() + r, c.y() - r / 2);
        else chevron << QPointF(c.x() - r / 2, c.y() - r) << QPointF(c.x() + r / 2, c.y()) << QPointF(c.x() - r / 2, c.y() + r);
        QColor col = o->palette.color(QPalette::Text);
        if (!(o->state & State_MouseOver)) col.setAlphaF(0.6);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(col, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->drawPolyline(chevron);
        p->restore();
    }
};
}

int main(int argc, char **argv)
{
    QElapsedTimer startup;
    startup.start();
    QApplication app(argc, argv);
    QApplication::setApplicationName("luna");
    QApplication::setApplicationDisplayName("Luna");
    QGuiApplication::setDesktopFileName("luna");
    QApplication::setWindowIcon(QIcon::fromTheme("luna", QIcon(":/luna.svg")));

    // Scaled thumbnails and icons live in QPixmapCache; the default 10 MB
    // holds a screenful at most, so scrolling a photo folder rescaled them.
    QPixmapCache::setCacheLimit(64 * 1024);

    Settings::get().load();   // first: the theme takes the accent colour from it
    Theme &th = Theme::get();
    th.load();
    Icons::get().reload();

    app.setStyle(new LunaStyle(QStyleFactory::create("Fusion")));
    th.applyToApp();

    // Follow Omarchy's theme live. omarchy-theme-set replaces current/theme
    // and then rewrites current/theme.name; inotify tells us, nothing polls.
    const QString themeState = QDir::homePath() + "/.local/state/omarchy/current";
    auto *themeWatch = new QFileSystemWatcher(&app);
    auto watchTheme = [themeWatch, themeState] {
        for (const QString &p : {themeState, themeState + "/theme.name"})
            if (QFileInfo::exists(p) && !themeWatch->files().contains(p) && !themeWatch->directories().contains(p)) themeWatch->addPath(p);
    };
    watchTheme();
    auto *themeSettle = new QTimer(&app);   // the theme is copied file by file: wait for the dust
    themeSettle->setSingleShot(true);
    themeSettle->setInterval(600);
    QObject::connect(themeWatch, &QFileSystemWatcher::directoryChanged, themeSettle, qOverload<>(&QTimer::start));
    QObject::connect(themeWatch, &QFileSystemWatcher::fileChanged, themeSettle, qOverload<>(&QTimer::start));
    // The same folder changes when Omarchy rotates the wallpaper: reload only
    // for a different theme (name or colours), not for a new background.
    auto themeStamp = [themeState] {
        QFile name(themeState + "/theme.name");
        const QByteArray n = name.open(QIODevice::ReadOnly) ? name.readAll() : QByteArray();
        return n + QByteArray::number(QFileInfo(themeState + "/theme/colors.toml").lastModified().toMSecsSinceEpoch());
    };
    QObject::connect(themeSettle, &QTimer::timeout, &app, [watchTheme, themeStamp, last = themeStamp()]() mutable {
        watchTheme();   // theme.name may have been replaced
        const QByteArray now = themeStamp();
        if (now == last) return;
        last = now;
        Theme::get().load();
        Theme::get().applyToApp();
        Icons::get().reload();
        emit Settings::get().updated();   // every window repaints (MainWindow::applySettings)
    });

    QStringList paths, remotes;
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLocal8Bit(argv[i]);
        if (a.startsWith('-')) continue;
        if (a.startsWith("file://")) a = QUrl(a).toLocalFile();
        else if (a.contains("://")) {   // sftp://, smb://… mounted once the window is up
            remotes << a;
            continue;
        }
        paths << QFileInfo(a).absoluteFilePath();
    }
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
    app.installEventFilter(new SmoothScroll(&app));   // before any object filter (Ctrl+wheel zoom passes through)
    if (qEnvironmentVariableIsSet("LUNA_DEBUG_META")) app.installEventFilter(new MetaCallLogger);
    auto *w = new MainWindow(paths);
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->show();
    for (const QString &uri : remotes) QTimer::singleShot(0, w, [w, uri] { w->connectServer(uri); });
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
                s->showPage("Comportamiento");
                shot(s, "9b-comportamiento");
                s->close();
                auto *c = new ConnectDialog(w);
                c->show();
                QTimer::singleShot(400, c, [=] { shot(c, "10-conectar"); c->close(); qApp->quit(); });
            });
        });
    }
    const int rc = app.exec();
    FsModel::shutdown();
    Thumbs::shutdown();
    QThreadPool::globalInstance()->clear();
    QThreadPool::globalInstance()->waitForDone(3000);
    return rc;
}
