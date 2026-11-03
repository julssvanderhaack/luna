#include "fsmodel.h"
#include "i18n.h"
#include "icons.h"
#include "mainwindow.h"
#include "settings.h"
#include "smoothscroll.h"
#include "theme.h"
#include "thumbs.h"
#ifdef LUNA_TEST_HOOKS
#include "testhooks.h"
#endif
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QPainter>
#include <QPixmapCache>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

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
    I18n::install(Settings::get().language);   // before any text is built
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
    app.installEventFilter(new SmoothScroll(&app));   // before any object filter (Ctrl+wheel zoom passes through)
#ifdef LUNA_TEST_HOOKS
    const QStringList given = paths;   // the hooks see them all (LUNA_SHOTS: a photo second)
    TestHooks::beforeWindow(paths);
#endif
    auto *w = new MainWindow(paths);
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->show();
    for (const QString &uri : remotes) QTimer::singleShot(0, w, [w, uri] { w->connectServer(uri); });
#ifdef LUNA_TEST_HOOKS
    TestHooks::install(w, given, startup);
#endif
    const int rc = app.exec();
    FsModel::shutdown();
    Thumbs::shutdown();
    QThreadPool::globalInstance()->clear();
    QThreadPool::globalInstance()->waitForDone(3000);
    return rc;
}
