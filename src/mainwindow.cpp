#include "mainwindow.h"
#include "apps.h"
#include "fileops.h"
#include "icons.h"
#include "pane.h"
#include "pathbar.h"
#include "remote.h"
#include <QActionGroup>
#include "properties.h"
#include "settings.h"
#include "settingsdialog.h"
#include "sidebar.h"
#include "theme.h"
#include "thumbs.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QContextMenuEvent>
#include <QCursor>
#include <QFrame>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QToolButton>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#ifdef LUNA_TERMINAL
#include "terminal.h"
#endif
#include "fsmodel.h"
#include "jump.h"
#include "places.h"
#include "undo.h"

// ------------------------------------------------------------------ TabPage
TabPage::TabPage(const QString &path, QWidget *parent) : QWidget(parent)
{
    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    m_split = new QSplitter(Qt::Horizontal);
    m_split->setObjectName("panes");
    m_split->setHandleWidth(1);
    m_split->setChildrenCollapsible(false);
    l->addWidget(m_split);
    m_a = new Pane;
    m_split->addWidget(m_a);
    m_active = m_a;
    m_a->setPath(path);
}

Pane *TabPage::other() const { return m_active == m_a ? m_b : m_a; }

void TabPage::setActive(Pane *p)
{
    if (!p) return;
    m_active = p;
    m_a->setActive(isSplit() && p == m_a);
    if (m_b) m_b->setActive(p == m_b);
    emit activeChanged(p);
}

void TabPage::setSplit(bool on)
{
    if (on == isSplit()) return;
    if (on) {
        m_b = new Pane;
        m_split->addWidget(m_b);
        m_b->setViewMode(m_a->viewMode());
        m_b->setPath(m_a->path());
        m_a->setSplitMode(true);
        m_b->setSplitMode(true);
        m_a->setOtherPane(m_b);
        m_b->setOtherPane(m_a);
        emit paneCreated(m_b);
        m_split->setSizes({1, 1});
        setActive(m_b);
        m_b->focusView();
    } else {
        Pane *keep = m_active;
        Pane *drop = keep == m_a ? m_b : m_a;
        drop->deleteLater();
        m_a = keep;
        m_b = nullptr;
        m_a->setSplitMode(false);
        m_a->setOtherPane(nullptr);
        setActive(m_a);
    }
}

// --------------------------------------------------------------- MainWindow
MainWindow::MainWindow(const QStringList &paths)
{
    m_themeGen = Theme::get().generation;   // the theme it starts with needs no refresh
    setWindowIcon(QIcon::fromTheme("luna", QIcon(":/luna.svg")));
    resize(1100, 700);
    auto *central = new QWidget;
    auto *v = new QVBoxLayout(central);
    v->setContentsMargins(6, 4, 6, 0);
    v->setSpacing(2);

    // header
    auto *header = new QHBoxLayout;
    header->setSpacing(2);
    m_back = toolButton("back", QString::fromUtf8("Atrás (Alt+←)"));
    m_forward = toolButton("forward", QString::fromUtf8("Adelante (Alt+→)"));
    m_up = toolButton("up", QString::fromUtf8("Carpeta superior (Alt+↑)"));
    connect(m_back, &QToolButton::clicked, this, [this] { pane()->back(); });
    connect(m_forward, &QToolButton::clicked, this, [this] { pane()->forward(); });
    connect(m_up, &QToolButton::clicked, this, [this] { pane()->up(); });
    m_pathbar = new PathBar;
    connect(m_pathbar, &PathBar::navigate, this, [this](const QString &p) {
        pane()->setPath(p);
        pane()->focusView();
    });
    connect(m_pathbar, &PathBar::connectRequested, this, &MainWindow::connectServer);
    connect(m_pathbar, &PathBar::folderMenuRequested, this, [this](const QPoint &p) { pane()->showFolderMenu(p); });
    m_search = toolButton("search", "Buscar (Ctrl+F)");
    connect(m_search, &QToolButton::clicked, this, [this] {
        if (pane()->filterVisible()) pane()->closeFilter();
        else pane()->showFilter();
    });

    // View mode: one dropdown with names
    m_viewBtn = new QToolButton;
    m_viewBtn->setPopupMode(QToolButton::InstantPopup);
    m_viewBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_viewBtn->setIconSize(QSize(20, 20));
    m_viewBtn->setFocusPolicy(Qt::NoFocus);
    m_viewBtn->setFixedHeight(36);
    m_viewBtn->setToolTip("Modo de vista");
    auto *viewMenu = new QMenu(m_viewBtn);
    auto *group = new QActionGroup(viewMenu);
    const char *glyphs[] = {"icons", "compact", "list", "details", "tree", "columns"};
    const char *names[] = {"Iconos", "Compacta", "Lista", "Lista detallada", "Árbol", "Columnas"};
    for (int i = 0; i < 6; ++i) {
        QAction *a = viewMenu->addAction(Icons::get().ui(glyphs[i]), QString::fromUtf8(names[i]));
        a->setShortcut(QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_1 + i)));
        a->setShortcutContext(Qt::WidgetShortcut);   // the window-wide shortcut lives in addShortcuts()
        a->setCheckable(true);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, i] {
            pane()->setViewMode(ViewMode(i));
            Settings::get().viewMode = ViewMode(i);
            Settings::get().save();
            syncHeader();
        });
        m_viewActions << a;
    }
    viewMenu->addSeparator();
    viewMenu->addAction(Icons::get().ui("zoomin"), QString::fromUtf8("Iconos más grandes"), this, [this] { pane()->zoom(1); })->setShortcut(QKeySequence("Ctrl++"));
    viewMenu->addAction(Icons::get().ui("zoomout"), QString::fromUtf8("Iconos más pequeños"), this, [this] { pane()->zoom(-1); })->setShortcut(QKeySequence("Ctrl+-"));
    auto *hid = viewMenu->addAction(Icons::get().ui("hidden"), "Mostrar archivos ocultos", this, [] {
        Settings::get().showHidden = !Settings::get().showHidden;
        Settings::get().changed();
    });
    hid->setShortcut(QKeySequence("Ctrl+H"));
    hid->setCheckable(true);
    connect(viewMenu, &QMenu::aboutToShow, hid, [hid] { hid->setChecked(Settings::get().showHidden); });
    m_viewBtn->setMenu(viewMenu);
    m_split = toolButton("split", "Vista dividida (F3)");
    m_split->setCheckable(true);
    connect(m_split, &QToolButton::clicked, this, [this](bool on) {
        page()->setSplit(on);
        syncHeader();
    });
    m_sideBtn = toolButton("sidebar", "Barra lateral (F9)");
    m_sideBtn->setCheckable(true);
    connect(m_sideBtn, &QToolButton::clicked, this, [this](bool on) {
        if (narrow()) {   // narrow window: show/hide just for now
            m_sidebarForced = on;
            layoutSidebar();
            return;
        }
        Settings::get().sidebarVisible = on;
        Settings::get().changed();
    });
    auto *menuBtn = toolButton("menu", "Menú");
    m_menuBtn = menuBtn;
    menuBtn->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(menuBtn);
    menu->addAction(Icons::get().ui("newtab"), "Ventana nueva", QKeySequence("Ctrl+N"), this, [] { (new MainWindow({QDir::homePath()}))->show(); });
    menu->addAction(Icons::get().ui("newtab"), QString::fromUtf8("Pestaña nueva"), QKeySequence("Ctrl+T"), this, [this] { newTab(pane()->path()); });
    menu->addAction(Icons::get().ui("newfolder"), "Nueva carpeta", QKeySequence("Ctrl+Shift+N"), this, [this] { pane()->newFolder(); });
    menu->addSeparator();
    auto *undo = menu->addAction(Icons::get().ui("restore"), "Deshacer", QKeySequence::Undo, &Undo::get(), &Undo::undo);
    auto *redo = menu->addAction(Icons::get().ui("restore"), "Rehacer", QKeySequence("Ctrl+Shift+Z"), &Undo::get(), &Undo::redo);
    connect(menu, &QMenu::aboutToShow, undo, [undo, redo] {
        undo->setText(Undo::get().undoText());
        undo->setEnabled(Undo::get().canUndo());
        redo->setText(Undo::get().redoText());
        redo->setEnabled(Undo::get().canRedo());
    });
    menu->addSeparator();
    auto *hidden = menu->addAction(Icons::get().ui("hidden"), "Mostrar archivos ocultos", QKeySequence("Ctrl+H"), this, [] {
        Settings::get().showHidden = !Settings::get().showHidden;
        Settings::get().changed();
    });
    hidden->setCheckable(true);
    connect(menu, &QMenu::aboutToShow, hidden, [hidden] { hidden->setChecked(Settings::get().showHidden); });
    menu->addAction(Icons::get().ui("trash"), "Ir a la papelera", this, [this] { pane()->setPath(FileOps::trashFiles()); });
    menu->addAction(Icons::get().ui("refresh"), "Recientes", this, [this] { pane()->setPath(Places::recent); });
    menu->addAction(Icons::get().ui("star"), "Destacados", this, [this] { pane()->setPath(Places::starred); });
    menu->addAction(Icons::get().ui("search"), QString::fromUtf8("Ir a una carpeta…"), QKeySequence("Ctrl+P"), this, &MainWindow::jump);
    menu->addAction(Icons::get().ui("network"), QString::fromUtf8("Conectar a un servidor…"), this, [this] { connectServer({}); });
    menu->addSeparator();
    menu->addAction(Icons::get().ui("icons"), "Barra superior…", this, [this] { headerMenu(QCursor::pos()); });
    menu->addAction(Icons::get().ui("settings"), "Ajustes", QKeySequence("Ctrl+,"), this, [this] { SettingsDialog(this).exec(); });
    menu->addAction(Icons::get().ui("info"), "Atajos de teclado", this, &MainWindow::showShortcuts);
    menu->addAction(Icons::get().ui("star"), "Acerca de Luna", this, &MainWindow::showAbout);
    menuBtn->setMenu(menu);
    // Shortcuts shown in these menus are only labels: the real ones are the
    // window-wide QShortcuts in addShortcuts(). Two live bindings for the
    // same keys would make Qt treat them as ambiguous and fire neither.
    std::function<void(QMenu *)> labelsOnly = [&labelsOnly](QMenu *m) {
        for (QAction *a : m->actions()) {
            a->setShortcutContext(Qt::WidgetShortcut);
            if (a->menu()) labelsOnly(a->menu());
        }
    };
    labelsOnly(menu);
    labelsOnly(m_viewBtn->menu());
    for (QToolButton *b : {m_back, m_forward, m_up, m_search, menuBtn}) b->installEventFilter(this);
    m_header = header;
    v->addLayout(header);
    rebuildHeader();

    m_tabBar = new QTabBar;
    m_tabBar->setMovable(true);
    m_tabBar->setExpanding(false);
    m_tabBar->setDocumentMode(true);
    m_tabBar->setElideMode(Qt::ElideMiddle);
    m_tabBar->hide();
    v->addWidget(m_tabBar);

    m_main = new QSplitter(Qt::Horizontal);
    m_main->setChildrenCollapsible(false);
    m_pages = new QStackedWidget;
    m_sidebar = new Sidebar;
    m_center = new QSplitter(Qt::Vertical);
    m_center->setChildrenCollapsible(false);
    m_center->addWidget(m_pages);
#ifdef LUNA_TERMINAL
    connect(m_center, &QSplitter::splitterMoved, this, [this] {
        if (m_term && m_term->isVisible()) {
            Settings::get().terminalHeight = m_term->height();
            Settings::get().changed();
        }
    });
#endif
    m_main->addWidget(m_center);
    m_main->addWidget(m_sidebar);
    v->addWidget(m_main, 1);

    m_jobs = new QWidget;
    m_jobsLayout = new QVBoxLayout(m_jobs);
    m_jobsLayout->setContentsMargins(4, 2, 4, 2);
    m_jobs->hide();
    v->addWidget(m_jobs);
    setCentralWidget(central);

    m_termBtn = toolButton("terminal", "Terminal (F4)");
    m_termBtn->setCheckable(true);
    connect(m_termBtn, &QToolButton::clicked, this, &MainWindow::showTerminal);
#ifndef LUNA_TERMINAL
    m_termBtn->hide();   // built without libvterm
#endif
    // Sidebar, split, view mode and terminal always sit in the bottom-left
    // corner; the folder's figures go to the right.
    for (QToolButton *b : {m_sideBtn, m_split, m_viewBtn, m_termBtn}) {
        b->setIconSize(QSize(16, 16));
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
        b->setFixedSize(24, 22);
        b->setStyleSheet("padding: 2px;");
        statusBar()->addWidget(b);
    }
    m_status = new QLabel;
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // a long message must not widen the window
    m_status->setContentsMargins(0, 0, 6, 0);
    statusBar()->addWidget(m_status, 1);
    statusBar()->setSizeGripEnabled(false);

    connect(m_tabBar, &QTabBar::currentChanged, this, [this](int i) {
        if (i < 0) return;
        m_pages->setCurrentWidget(m_tabBar->tabData(i).value<QWidget *>());
        syncHeader();
    });
    connect(m_tabBar, &QTabBar::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabBar, &QTabBar::tabBarClicked, this, [this](int i) {
        if (i >= 0 && (QApplication::mouseButtons() & Qt::MiddleButton)) closeTab(i);
    });
    connect(m_sidebar, &Sidebar::open, this, [this](const QString &p) { pane()->setPath(p); pane()->focusView(); });
    connect(m_sidebar, &Sidebar::openInNewTab, this, [this](const QString &p) { newTab(p); });
    connect(m_sidebar, &Sidebar::openInOtherPane, this, [this](const QString &p) {
        page()->setSplit(true);
        page()->other() ? page()->other()->setPath(p) : pane()->setPath(p);
        syncHeader();
    });
    connect(m_sidebar, &Sidebar::dropOnPlace, this, [this](const QList<QUrl> &urls, const QString &dest) {
        QStringList srcs;
        for (const QUrl &u : urls)
            if (u.isLocalFile()) srcs << u.toLocalFile();
        if (srcs.isEmpty() || dest == Places::recent) return;
        if (dest == Places::starred) {   // dropping on Destacados stars, like Nautilus
            Starred::get().add(srcs);
            return;
        }
        if (dest == FileOps::trashFiles()) {
            FileOps::get().trash(srcs);
            return;
        }
        if (QApplication::keyboardModifiers() & Qt::ControlModifier || !FileOps::sameDevice(srcs[0], dest)) FileOps::get().copy(srcs, dest);
        else FileOps::get().move(srcs, dest);
    });
    connect(&FileOps::get(), &FileOps::jobStarted, this, &MainWindow::trackJob);
    connect(&Undo::get(), &Undo::message, this, [this](const QString &t) { m_status->setText(t); });
    connect(m_sidebar, &Sidebar::connectRequested, this, &MainWindow::connectServer);
    connect(&Remote::get(), &Remote::mountsChanged, m_sidebar, &Sidebar::reload);
    connect(&Settings::get(), &Settings::updated, this, &MainWindow::applySettings);

    addShortcuts();
    QStringList start = paths;
    const Settings &s = Settings::get();
    if (start.isEmpty() && s.restoreSession) start = s.sessionTabs;
    if (start.isEmpty()) start = {QDir::homePath()};
    for (const QString &p : start) newTab(p, false);
    m_tabBar->setCurrentIndex(0);
    if (paths.isEmpty() && s.restoreSession && s.startSplit) page()->setSplit(true);
    layoutSidebar();
    applyHeaderStyle();
    syncHeader();
    pane()->focusView();
}

QToolButton *MainWindow::toolButton(const QString &glyph, const QString &tip)
{
    auto *b = new QToolButton;
    b->setIcon(Icons::get().ui(glyph));
    b->setIconSize(QSize(20, 20));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setFixedSize(38, 36);
    return b;
}

TabPage *MainWindow::page() const { return qobject_cast<TabPage *>(m_pages->currentWidget()); }
Pane *MainWindow::pane() const { return page() ? page()->active() : nullptr; }

int MainWindow::newTab(const QString &path, bool activate)
{
    auto *tp = new TabPage(path);
    m_pages->addWidget(tp);
    connect(tp, &TabPage::paneCreated, this, &MainWindow::wirePane);
    connect(tp, &TabPage::activeChanged, this, [this, tp](Pane *) {
        if (tp == page()) syncHeader();
    });
    wirePane(tp->first());
    const QString p = tp->first()->path();
    int i = m_tabBar->addTab(Places::displayName(p));
    m_tabBar->setTabData(i, QVariant::fromValue<QWidget *>(tp));
    m_tabBar->setTabToolTip(i, p);
    auto *close = new QToolButton;
    close->setIcon(Icons::get().ui("close"));
    close->setIconSize(QSize(12, 12));
    close->setAutoRaise(true);
    close->setFixedSize(22, 22);
    close->setToolTip(QString::fromUtf8("Cerrar pestaña (Ctrl+W)"));
    close->setFocusPolicy(Qt::NoFocus);
    connect(close, &QToolButton::clicked, this, [this, close] {
        for (int k = 0; k < m_tabBar->count(); ++k)
            if (m_tabBar->tabButton(k, QTabBar::RightSide) == close) closeTab(k);
    });
    m_tabBar->setTabButton(i, QTabBar::RightSide, close);
    m_tabBar->setVisible(m_tabBar->count() > 1);
    if (activate) {
        m_tabBar->setCurrentIndex(i);
        tp->active()->focusView();
    }
    return i;
}

void MainWindow::closeTab(int index)
{
    if (m_tabBar->count() <= 1) {
        close();
        return;
    }
    QWidget *w = m_tabBar->tabData(index).value<QWidget *>();
    m_tabBar->removeTab(index);
    m_pages->removeWidget(w);
    w->deleteLater();
    m_tabBar->setVisible(m_tabBar->count() > 1);
    syncHeader();
}

void MainWindow::wirePane(Pane *p)
{
    auto *tp = qobject_cast<TabPage *>(p->parentWidget() ? p->parentWidget()->parentWidget() : nullptr);
    connect(p, &Pane::activated, this, [tp](Pane *x) {
        if (tp && tp->active() != x) tp->setActive(x);
    });
    connect(p, &Pane::pathChanged, this, [this, p, tp](const QString &path) {
        JumpDialog::recordVisit(path);
        for (int i = 0; i < m_tabBar->count(); ++i)
            if (m_tabBar->tabData(i).value<QWidget *>() == tp && tp->active() == p) {
                m_tabBar->setTabText(i, Places::displayName(path));
                m_tabBar->setTabToolTip(i, path);
            }
        if (pane() == p) syncHeader();
    });
    connect(p, &Pane::historyChanged, this, [this, p] {
        if (pane() == p) syncHeader();
    });
#ifdef LUNA_TERMINAL
    // With the terminal open, clicking a file types its name at the prompt
    // (just the name where the shell already is, else the whole path).
    connect(p, &Pane::fileClicked, this, [this](const QString &path) {
        if (!m_term || !m_term->isVisible() || !Settings::get().terminalClickInserts || path.isEmpty()) return;
        const QFileInfo fi(path);
        const QString word = fi.absolutePath() == m_term->directory() ? fi.fileName() : path;
        m_term->insertText(Terminal::shellQuote(word) + ' ', true);
    });
#endif
    connect(p, &Pane::status, this, [this, p](const QString &t) {
        if (pane() == p) m_status->setText(t);
    });
    connect(p, &Pane::openInNewTab, this, [this](const QString &path) { newTab(path, false); });
    connect(p, &Pane::openInOtherPane, this, [this](const QString &path) {
        page()->setSplit(true);
        if (Pane *o = page()->other()) o->setPath(path);
        syncHeader();
    });
    connect(p, &Pane::bookmarkRequested, this, [this](const QString &path) {
        m_sidebar->addBookmark(path);
        m_status->setText(QString::fromUtf8("Añadido a marcadores: %1").arg(QFileInfo(path).fileName()));
    });
    connect(p, &Pane::propertiesRequested, this, [this](const QStringList &paths) { (new PropertiesDialog(paths, this))->show(); });
}

void MainWindow::jump()
{
    JumpDialog d(this);
    if (d.exec() == QDialog::Accepted && !d.chosen().isEmpty()) {
        pane()->setPath(d.chosen());
        pane()->focusView();
    }
}

void MainWindow::showTerminal(bool show)
{
#ifdef LUNA_TERMINAL
    if (show && !m_term) {
        m_term = new Terminal(pane()->path());
        m_center->addWidget(m_term);
        m_center->setStretchFactor(0, 1);
        m_center->setStretchFactor(1, 0);
        connect(m_term, &Terminal::finished, this, [this] {   // "exit": close the panel, a new shell next time
            m_term->deleteLater();
            m_term = nullptr;
            m_termBtn->setChecked(false);
            pane()->focusView();
        });
    }
    if (m_term) {
        m_term->setVisible(show);
        if (show) {
            const int h = std::clamp(Settings::get().terminalHeight, 80, std::max(80, m_center->height() - 120));
            m_center->setSizes({m_center->height() - h, h});
            m_term->setDirectory(pane()->path());
            m_term->setFocus();
        } else {
            pane()->focusView();
        }
    }
    m_termBtn->setChecked(show && m_term);
#else
    Q_UNUSED(show);
#endif
}

void MainWindow::syncHeader()
{
    Pane *p = pane();
    if (!p) return;
    m_back->setEnabled(p->canBack());
    m_forward->setEnabled(p->canForward());
    m_up->setEnabled(p->path() != "/");
    syncViewButton();
    m_pathbar->setPath(p->path());
    m_split->setChecked(page()->isSplit());
    m_sideBtn->setChecked(m_sidebar->isVisible());
    m_sidebar->setCurrentPath(p->path());
#ifdef LUNA_TERMINAL
    if (m_term && m_term->isVisible()) m_term->setDirectory(p->path());
#endif
    setWindowTitle(Places::displayName(p->path()) + QString::fromUtf8(" — Luna"));
}

bool MainWindow::narrow() const { return width() < 720; }

void MainWindow::layoutSidebar()
{
    const Settings &s = Settings::get();
    // In a narrow window the sidebar steps aside unless asked for (F9).
    const bool show = narrow() ? m_sidebarForced : s.sidebarVisible;
    const bool wasVisible = m_sidebar->isVisible();
    m_sidebar->setVisible(show);
    m_sidebar->setMaximumWidth(300);
    const int sideIndex = m_main->indexOf(m_sidebar);
    const int want = s.sidebarRight ? 1 : 0;
    const bool moved = sideIndex != want;
    if (moved) m_main->insertWidget(want, m_sidebar);
    m_main->setStretchFactor(m_main->indexOf(m_center), 1);
    m_main->setStretchFactor(m_main->indexOf(m_sidebar), 0);
    if (show && (!wasVisible || moved)) {
        const int sw = narrow() ? std::min(200, width() / 2) : 220;
        m_main->setSizes(s.sidebarRight ? QList<int>{width() - sw, sw} : QList<int>{sw, width() - sw});
    }
    if (m_sideBtn) m_sideBtn->setChecked(show);
}

// Lays the header out from Settings::toolbarLayout: groups of buttons
// split by "|", with the path bar taking the free room wherever it is.
void MainWindow::rebuildHeader()
{
    const QStringList layout = Settings::get().toolbarLayout;
    if (layout == m_headerLayout) return;
    m_headerLayout = layout;
    const QHash<QString, QWidget *> items = {{"back", m_back}, {"forward", m_forward}, {"up", m_up}, {"path", m_pathbar}, {"search", m_search}, {"menu", m_menuBtn}};
    for (QWidget *w : items) {   // park them while the groups are rebuilt
        w->hide();
        w->setParent(this);
    }
    qDeleteAll(m_groups);
    m_groups.clear();
    while (QLayoutItem *it = m_header->takeAt(0)) delete it;
    QFrame *group = nullptr;
    for (const QString &t : layout) {
        if (t == "|") {
            group = nullptr;
            continue;
        }
        QWidget *w = items.value(t);
        if (!w) continue;   // hidden ("-name")
        if (w == m_pathbar) {
            group = nullptr;
            if (m_header->count()) m_header->addSpacing(4);
            m_header->addWidget(w, 1);
            w->show();
            continue;
        }
        if (!group) {
            if (m_header->count()) m_header->addSpacing(m_header->itemAt(m_header->count() - 1)->widget() == m_pathbar ? 6 : 8);
            group = new QFrame;
            auto *l = new QHBoxLayout(group);
            l->setContentsMargins(0, 0, 0, 0);
            group->installEventFilter(this);   // right click: header menu
            m_header->addWidget(group);
            m_groups << group;
        }
        group->layout()->addWidget(w);
        w->show();
    }
}

void MainWindow::applyHeaderStyle()
{
    rebuildHeader();
    const Settings &s = Settings::get();
    static const int sizes[] = {16, 20, 24};
    const int icon = sizes[std::clamp(s.toolbarIconSize, 0, 2)];
    const int h = icon + 16;
    const int style = s.toolbarStyle;
    const bool text = style == 1 && !narrow();
    const bool grouped = style == 2;
    struct B { QToolButton *b; const char *label; };
    const B buttons[] = {{m_back, "Atrás"}, {m_forward, "Adelante"}, {m_up, "Subir"}, {m_search, "Buscar"}, {m_menuBtn, "Menú"}};
    for (const B &x : buttons) {
        x.b->setIconSize(QSize(icon, icon));
        x.b->setToolButtonStyle(text ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
        x.b->setText(QString::fromUtf8(x.label));
        if (text) {
            x.b->setMinimumSize(0, h);
            x.b->setMaximumSize(QWIDGETSIZE_MAX, h);
        } else {
            const int w = grouped ? h : h + 2;
            x.b->setFixedSize(w, grouped ? h - 2 : h);
        }
    }
    // What goes first when the window is narrow
    const bool tiny = width() < 560;
    m_forward->setVisible(s.toolbarLayout.contains("forward") && !tiny);
    for (QFrame *g : m_groups) {
        g->setObjectName(grouped ? "toolGroup" : "");
        g->layout()->setSpacing(grouped ? 0 : 2);
        g->style()->unpolish(g);
        g->style()->polish(g);
        for (QToolButton *b : g->findChildren<QToolButton *>()) {
            b->style()->unpolish(b);
            b->style()->polish(b);
        }
    }
    syncViewButton();
}

void MainWindow::headerMenu(const QPoint &globalPos)
{
    Settings &s = Settings::get();
    QMenu m(this);
    m.addSection(QString::fromUtf8("Estilo de la barra"));
    const char *styles[] = {"Solo iconos", "Iconos y texto", "Iconos juntos"};
    for (int i = 0; i < 3; ++i) {
        QAction *a = m.addAction(QString::fromUtf8(styles[i]), this, [i] {
            Settings::get().toolbarStyle = i;
            Settings::get().changed();
        });
        a->setCheckable(true);
        a->setChecked(s.toolbarStyle == i);
    }
    m.addSection(QString::fromUtf8("Tamaño de los iconos"));
    const char *sizes[] = {"Pequeños", "Medianos", "Grandes"};
    for (int i = 0; i < 3; ++i) {
        QAction *a = m.addAction(QString::fromUtf8(sizes[i]), this, [i] {
            Settings::get().toolbarIconSize = i;
            Settings::get().changed();
        });
        a->setCheckable(true);
        a->setChecked(s.toolbarIconSize == i);
    }
    m.addSection("Botones");
    struct T { const char *item, *name; };
    for (T t : {T{"back", "Atrás"}, T{"forward", "Adelante"}, T{"up", "Carpeta superior"}, T{"search", "Buscar"}}) {
        const QString item = QString::fromLatin1(t.item);
        QAction *a = m.addAction(QString::fromUtf8(t.name), this, [item] {
            QStringList &l = Settings::get().toolbarLayout;
            const int i = std::max(l.indexOf(item), l.indexOf('-' + item));
            if (i >= 0) l[i] = l[i].startsWith('-') ? item : '-' + item;   // keeps its place
            Settings::get().changed();
        });
        a->setCheckable(true);
        a->setChecked(s.toolbarLayout.contains(item));
    }
    m.addSeparator();
    m.addAction(Icons::get().ui("settings"), QString::fromUtf8("Ordenar y agrupar botones…"), this, [this] {
        SettingsDialog d(this);
        d.showPage("Barra superior");
        d.exec();
    });
    m.exec(globalPos);
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    if (!m_menuBtn || !m_sidebar) return;   // still being constructed
    static int lastClass = -1;
    const int cls = width() < 560 ? 0 : narrow() ? 1 : 2;
    if (cls == lastClass) return;
    lastClass = cls;
    if (cls == 2) m_sidebarForced = false;
    layoutSidebar();
    applyHeaderStyle();
}

bool MainWindow::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() == QEvent::ContextMenu) {
        headerMenu(static_cast<QContextMenuEvent *>(e)->globalPos());
        return true;
    }
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::applySettings()
{
    static int lastStyle = -1, lastCounts = -1;
    static QString lastTheme = "\x01", lastAccent;
    const Settings &s = Settings::get();
    const bool accentChanged = lastAccent != s.accentColor;
    if (accentChanged) {   // recolour everything: palette, style sheet, glyph icons
        lastAccent = s.accentColor;
        Theme::get().load();
        Theme::get().applyToApp();
    }
    const bool iconsChanged = lastStyle != s.iconStyle || lastTheme != s.iconTheme || accentChanged;
    if (iconsChanged) Icons::get().reload();
    // A new Omarchy theme (main.cpp watches it) already reloaded colours and
    // icons for the whole app; each window still repaints its own parts.
    const bool themeChanged = m_themeGen != Theme::get().generation;
    m_themeGen = Theme::get().generation;
    for (Pane *p : findChildren<Pane *>()) {
        p->applySettings();
        if (iconsChanged || themeChanged || (lastCounts >= 0 && lastCounts != int(s.showItemCounts))) p->refresh();
    }
#ifdef LUNA_TERMINAL
    if (m_term && themeChanged) m_term->applyTheme();
#endif
    lastStyle = s.iconStyle;
    lastTheme = s.iconTheme;
    lastCounts = s.showItemCounts;
    // Rebuilding the sidebar stats every mount and runs lsblk: only when
    // something it shows changed.
    static QList<int> lastSidebar;
    const QList<int> sidebar = {s.showPlaces, s.showBookmarks, s.showDevices};
    if (iconsChanged || themeChanged || sidebar != lastSidebar) m_sidebar->reload();
    lastSidebar = sidebar;
    m_sidebar->applySizes();
    layoutSidebar();
    applyHeaderStyle();
    syncHeader();
}

void MainWindow::trackJob(Job *job)
{
    auto *row = new QWidget;
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto *label = new QLabel(job->title());
    label->setMinimumWidth(220);
    auto *bar = new QProgressBar;
    bar->setRange(0, 1000);
    bar->setTextVisible(false);
    bar->setFixedHeight(6);
    auto *detail = new QLabel;
    detail->setProperty("role", "dim");
    auto *cancel = new QToolButton;
    cancel->setIcon(Icons::get().ui("close"));
    cancel->setToolTip("Cancelar");
    h->addWidget(label);
    h->addWidget(bar, 1);
    h->addWidget(detail);
    h->addWidget(cancel);
    m_jobsLayout->addWidget(row);
    m_jobs->show();
    QPointer<Job> pj = job;
    connect(cancel, &QToolButton::clicked, this, [pj] {
        if (pj) pj->cancel();
    });
    auto started = std::make_shared<qint64>(QDateTime::currentMSecsSinceEpoch());
    connect(job, &Job::progress, row, [bar, detail, started](qint64 done, qint64 total, int df, int tf, const QString &) {
        if (total > 0) bar->setValue(int(1000.0 * done / total));
        else if (tf > 0) bar->setValue(int(1000.0 * df / tf));
        const double secs = (QDateTime::currentMSecsSinceEpoch() - *started) / 1000.0;
        QString t = QString::fromUtf8("%1 de %2").arg(df).arg(tf);
        if (total > 0 && secs > 0.5) t += QString::fromUtf8("  ·  %1/s").arg(FsModel::formatSize(qint64(done / secs)));
        detail->setText(t);
    });
    connect(job, &Job::conflict, this, [this](const QString &src, const QString &dst, int *answer, bool *all) {
        QFileInfo s(src), d(dst);
        QMessageBox box(this);
        box.setWindowTitle(QString::fromUtf8("Ya existe «%1»").arg(d.fileName()));
        box.setText(QString::fromUtf8("En el destino ya hay un elemento llamado «%1».").arg(d.fileName()));
        QLocale es(QLocale::Spanish, QLocale::Spain);
        box.setInformativeText(QString::fromUtf8("Nuevo: %1, %2\nExistente: %3, %4")
                                   .arg(FsModel::formatSize(s.size()), es.toString(s.lastModified(), "d MMM yyyy HH:mm"),
                                        FsModel::formatSize(d.size()), es.toString(d.lastModified(), "d MMM yyyy HH:mm")));
        auto *rep = box.addButton("Reemplazar", QMessageBox::DestructiveRole);
        auto *skip = box.addButton("Omitir", QMessageBox::RejectRole);
        auto *both = box.addButton("Mantener ambos", QMessageBox::AcceptRole);
        box.addButton("Cancelar", QMessageBox::RejectRole);
        auto *cb = new QCheckBox(QString::fromUtf8("Hacer lo mismo con los demás conflictos"));
        box.setCheckBox(cb);
        box.setDefaultButton(both);
        box.exec();
        *answer = box.clickedButton() == rep ? Job::Replace : box.clickedButton() == skip ? Job::Skip : box.clickedButton() == both ? Job::KeepBoth : Job::Cancel;
        *all = cb->isChecked();
    }, Qt::BlockingQueuedConnection);
    connect(job, &Job::finished, this, [this, row](const QStringList &errors) {
        row->deleteLater();
        QTimer::singleShot(0, this, [this] { m_jobs->setVisible(m_jobsLayout->count() > 0); });
        if (!errors.isEmpty() && !(errors.size() == 1 && errors[0] == QString::fromUtf8("Cancelado."))) {
            auto *box = new QMessageBox(QMessageBox::Warning, QString::fromUtf8("Hubo problemas"),
                                        errors.mid(0, 12).join('\n') + (errors.size() > 12 ? QString::fromUtf8("\n… y %1 más").arg(errors.size() - 12) : QString()),
                                        QMessageBox::Ok, this);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->show();
        }
        m_sidebar->reload();
        for (Pane *p : findChildren<Pane *>()) p->refresh();
    });
}

void MainWindow::addShortcuts()
{
    auto sc = [this](const QKeySequence &k, std::function<void()> f) {
        auto *s = new QShortcut(k, this);
        connect(s, &QShortcut::activated, this, f);
    };
    sc(QKeySequence("Alt+Left"), [this] { pane()->back(); });
    sc(QKeySequence("Alt+Right"), [this] { pane()->forward(); });
    sc(QKeySequence("Alt+Up"), [this] { pane()->up(); });
    sc(QKeySequence("Alt+Home"), [this] { pane()->setPath(QDir::homePath()); });
    sc(QKeySequence("Ctrl+T"), [this] { newTab(pane()->path()); });
    sc(QKeySequence("Ctrl+W"), [this] { closeTab(m_tabBar->currentIndex()); });
    sc(QKeySequence("Ctrl+Tab"), [this] { m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + 1) % m_tabBar->count()); });
    sc(QKeySequence("Ctrl+Shift+Tab"), [this] { m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + m_tabBar->count() - 1) % m_tabBar->count()); });
    sc(QKeySequence("Ctrl+PgDown"), [this] { m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + 1) % m_tabBar->count()); });
    sc(QKeySequence("Ctrl+PgUp"), [this] { m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + m_tabBar->count() - 1) % m_tabBar->count()); });
    sc(QKeySequence("Ctrl+N"), [] { (new MainWindow({QDir::homePath()}))->show(); });
    sc(QKeySequence("F5"), [this] { pane()->refresh(); });
    sc(QKeySequence("Ctrl+R"), [this] { pane()->refresh(); });
    sc(QKeySequence("Ctrl+L"), [this] { m_pathbar->editPath(); });
    sc(QKeySequence("Ctrl+P"), [this] { jump(); });
    sc(QKeySequence("Ctrl+F"), [this] { pane()->showFilter(); });
    sc(QKeySequence("Ctrl+H"), [] {
        Settings::get().showHidden = !Settings::get().showHidden;
        Settings::get().changed();
    });
    for (int i = 0; i < 6; ++i)
        sc(QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_1 + i)), [this, i] {
            pane()->setViewMode(ViewMode(i));
            Settings::get().viewMode = ViewMode(i);
            Settings::get().save();
            syncHeader();
        });
    sc(QKeySequence("Ctrl++"), [this] { pane()->zoom(1); });
    sc(QKeySequence("Ctrl+="), [this] { pane()->zoom(1); });
    sc(QKeySequence("Ctrl+-"), [this] { pane()->zoom(-1); });
    sc(QKeySequence("F2"), [this] { pane()->renameSelected(); });
    sc(QKeySequence::Delete, [this] { pane()->trashSelection(); });
    sc(QKeySequence("Shift+Del"), [this] { pane()->deleteSelection(); });
    sc(QKeySequence::Copy, [this] { pane()->copySelection(); });
    sc(QKeySequence::Cut, [this] { pane()->cutSelection(); });
    sc(QKeySequence::Paste, [this] { pane()->paste(); });
    sc(QKeySequence("Ctrl+A"), [this] { pane()->selectAll(); });
    sc(QKeySequence::Undo, [] { Undo::get().undo(); });
    sc(QKeySequence("Ctrl+Shift+Z"), [] { Undo::get().redo(); });
    sc(QKeySequence("Ctrl+Y"), [] { Undo::get().redo(); });
    sc(QKeySequence("Ctrl+Shift+I"), [this] { pane()->invertSelection(); });
    sc(QKeySequence("Ctrl+Shift+N"), [this] { pane()->newFolder(); });
    sc(QKeySequence("Ctrl+Shift+C"), [this] { pane()->copyPath(); });
    sc(QKeySequence("Alt+Return"), [this] {
        QStringList s = pane()->selectedPaths();
        (new PropertiesDialog(s.isEmpty() ? QStringList{pane()->path()} : s, this))->show();
    });
#ifdef LUNA_TERMINAL
    sc(QKeySequence("F4"), [this] { showTerminal(!(m_term && m_term->isVisible())); });
#endif
    sc(QKeySequence("Shift+F4"), [this] { pane()->openTerminal(); });
    sc(QKeySequence("F3"), [this] {
        page()->setSplit(!page()->isSplit());
        syncHeader();
    });
    sc(QKeySequence("F6"), [this] {
        if (Pane *o = page()->other()) {
            page()->setActive(o);
            o->focusView();
        }
    });
    sc(QKeySequence("F9"), [this] {
        if (narrow()) {
            m_sidebarForced = !m_sidebar->isVisible();
            layoutSidebar();
            return;
        }
        Settings::get().sidebarVisible = !Settings::get().sidebarVisible;
        Settings::get().changed();
    });
    sc(QKeySequence("Ctrl+D"), [this] {
        if (!pane()->isVirtual()) m_sidebar->addBookmark(pane()->path());
    });
    sc(QKeySequence("Ctrl+,"), [this] { SettingsDialog(this).exec(); });
    sc(QKeySequence("Ctrl+Q"), [] { qApp->closeAllWindows(); });
}

void MainWindow::syncViewButton()
{
    static const char *glyphs[] = {"icons", "compact", "list", "details", "tree", "columns"};
    static const char *names[] = {"Iconos", "Compacta", "Lista", "Lista detallada", "Árbol", "Columnas"};
    const int m = int(pane()->viewMode());
    m_viewBtn->setIcon(Icons::get().ui(glyphs[m]));
    m_viewBtn->setText(QString::fromUtf8(names[m]) + QString::fromUtf8("  ▾"));
    m_viewBtn->setToolTip(QString::fromUtf8("Modo de vista: ") + QString::fromUtf8(names[m]));
    for (int i = 0; i < m_viewActions.size(); ++i) m_viewActions[i]->setChecked(i == m);
}

void MainWindow::connectServer(const QString &uri0)
{
    QString uri = uri0;
    if (uri.isEmpty()) {
        ConnectDialog d(this);
        if (d.exec() != QDialog::Accepted) return;
        uri = d.uri();
    }
    m_status->setText(QString::fromUtf8("Conectando con %1…").arg(QUrl(uri).host()));
    QPointer<MainWindow> self(this);
    Remote::get().connectTo(uri, this, [self](const QString &path) {
        if (!self) return;
        if (path.isEmpty()) {
            self->m_status->clear();
            return;
        }
        self->pane()->setPath(path);
        self->pane()->focusView();
    });
}

void MainWindow::showShortcuts()
{
    const char *rows[][2] = {
        {"Alt+← / Alt+→ / Alt+↑", "Atrás / adelante / carpeta superior"}, {"Retroceso", "Carpeta superior"},
        {"Ctrl+L", "Escribir una ruta"}, {"Ctrl+P", "Ir a una carpeta (zoxide y marcadores)"}, {"Ctrl+F o escribir", "Filtrar; Intro busca en subcarpetas"},
        {"Ctrl+T / Ctrl+W", "Abrir / cerrar pestaña"}, {"Ctrl+Tab", "Siguiente pestaña"},
        {"F3 / F6", "Vista dividida / cambiar de panel"}, {"F9", "Barra lateral"},
        {"Ctrl+1 … Ctrl+6", "Iconos, compacta, lista, detallada, árbol, columnas"}, {"Ctrl + rueda, Ctrl+/−", "Tamaño de los iconos"},
        {"Ctrl+H", "Mostrar ocultos"}, {"F2", "Renombrar (en lote si hay varios)"},
        {"Supr / Mayús+Supr", "Papelera / eliminar definitivamente"}, {"Ctrl+C / X / V", "Copiar / cortar / pegar"},
        {"Ctrl+Mayús+N", "Nueva carpeta"}, {"Alt+Intro", "Propiedades"}, {"F4", "Terminal integrado"}, {"Mayús+F4", "Terminal aparte"},
        {"Ctrl+Z / Ctrl+Mayús+Z", "Deshacer / rehacer"},
        {"Ctrl+D", "Añadir a marcadores"}, {"Ctrl+,", "Ajustes"}, {"Arrastrar + Ctrl / Mayús / ambos", "Copiar / mover / enlazar"},
    };
    QString html = "<table cellspacing=6>";
    for (auto &r : rows) html += QString("<tr><td><b>%1</b></td><td>%2</td></tr>").arg(QString::fromUtf8(r[0]), QString::fromUtf8(r[1]));
    html += "</table>";
    QMessageBox::information(this, "Atajos de teclado", html);
}

void MainWindow::showAbout()
{
    QMessageBox box(this);
    box.setWindowTitle("Acerca de Luna");
    box.setIconPixmap(QIcon::fromTheme("luna", QIcon(":/luna.svg")).pixmap(96, 96));
    box.setText(QString::fromUtf8("<h2>Luna</h2>Explorador de archivos para Omarchy.<br>Llamado así por Luna, la gata siamesa. 🐾"));
    box.setInformativeText(QString::fromUtf8("C++ y Qt 6 · metadatos con libmediainfo y exiv2 · miniaturas con ffmpegthumbnailer y poppler"));
    box.exec();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    Settings &s = Settings::get();
    QStringList tabs;
    for (int i = 0; i < m_tabBar->count(); ++i)
        if (auto *tp = qobject_cast<TabPage *>(m_tabBar->tabData(i).value<QWidget *>())) tabs << tp->first()->path();
    s.sessionTabs = tabs;
    s.startSplit = page() && page()->isSplit();
    s.save();
    e->accept();
}
