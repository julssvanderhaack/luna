// The "Git" submenu of the right-click menus: what fits the state of the
// clicked items (stage, restore, diff, history, blame, ignore…) and the
// repository (commit, pull, push, branches, stash, lazygit). Commands run in
// the background; the pane's watcher on .git/index repaints the marks.
#pragma once
#include <QString>
#include <QStringList>
#include <functional>

class QMenu;
class QWidget;

namespace GitMenu {
bool haveGit();
// paths empty = the folder's own menu. status() gets a line for the status bar.
void add(QMenu *menu, QWidget *parent, const QString &root, const QString &dir, const QStringList &paths,
         std::function<void(const QString &)> status);
// Outside a repository: "Crear repositorio git aquí".
void addInit(QMenu *menu, QWidget *parent, const QString &dir, std::function<void(const QString &)> status);
}
