#pragma once
#include <QMainWindow>

class Pane;
class PathBar;
class Sidebar;
class QFrame;
class QHBoxLayout;
class QLabel;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QToolButton;
class QVBoxLayout;
class Job;

// One tab: one or two panes side by side.
class TabPage : public QWidget {
    Q_OBJECT
public:
    explicit TabPage(const QString &path, QWidget *parent = nullptr);
    Pane *active() const { return m_active; }
    Pane *other() const;
    Pane *first() const { return m_a; }
    bool isSplit() const { return m_b != nullptr; }
    void setSplit(bool on);
    void setActive(Pane *p);

signals:
    void paneCreated(Pane *p);
    void activeChanged(Pane *p);

private:
    QSplitter *m_split;
    Pane *m_a, *m_b = nullptr, *m_active;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QStringList &paths = {});
    Pane *pane() const;
    TabPage *page() const;
    int newTab(const QString &path, bool activate = true);
    // Mounts a sftp:// smb:// … address (asks for it when empty) and opens it
    void connectServer(const QString &uri);

protected:
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void wirePane(Pane *p);
    void closeTab(int index);
    void syncHeader();
    void showTerminal(bool show);
    void layoutSidebar();
    void applySettings();
    void trackJob(Job *job);
    void addShortcuts();
    void showShortcuts();
    void showAbout();
    void syncViewButton();
    void applyHeaderStyle();
    void rebuildHeader();
    void headerMenu(const QPoint &globalPos);
    bool narrow() const;
    QToolButton *toolButton(const QString &glyph, const QString &tip);

    QTabBar *m_tabBar;
    QStackedWidget *m_pages;
    Sidebar *m_sidebar = nullptr;
    QSplitter *m_main;
    QSplitter *m_center = nullptr;   // the pages above, the terminal below
    class Terminal *m_term = nullptr;   // created the first time it is shown
    QToolButton *m_termBtn = nullptr;
    QToolButton *m_back = nullptr, *m_forward = nullptr, *m_up = nullptr, *m_split = nullptr, *m_sideBtn = nullptr, *m_viewBtn = nullptr,
                *m_search = nullptr, *m_menuBtn = nullptr;
    QList<QFrame *> m_groups;        // header button groups, see rebuildHeader()
    QHBoxLayout *m_header = nullptr;
    QStringList m_headerLayout;      // layout the header was last built from
    bool m_sidebarForced = false;   // shown with F9 although the window is narrow
    QList<QAction *> m_viewActions;
    PathBar *m_pathbar;
    QLabel *m_status;
    QWidget *m_jobs;
    QVBoxLayout *m_jobsLayout;
};
