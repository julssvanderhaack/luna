// Nautilus-like location bar: breadcrumb buttons or an editable text field
// (toggle, remembered), plus "connect to server". Typing sftp://, ftp://,
// smb://… connects through GVFS.
#pragma once
#include <QWidget>

class QHBoxLayout;
class QLineEdit;
class QStackedWidget;
class QToolButton;

class PathBar : public QWidget {
    Q_OBJECT
public:
    explicit PathBar(QWidget *parent = nullptr);
    void setPath(const QString &path);
    void editPath();
    void setTextMode(bool text);

signals:
    void navigate(const QString &path);
    void connectRequested(const QString &uri);   // empty: open the dialog
    void folderMenuRequested(const QPoint &globalPos);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void rebuild();
    void scheduleRebuild();
    void submit();
    void showCrumbs();
    QString m_path;
    bool m_rebuildPending = false;
    int m_lastWidth = -1;
    bool m_textMode = false;
    QStackedWidget *m_stack;
    QWidget *m_crumbs;
    QHBoxLayout *m_crumbLayout;
    QLineEdit *m_edit;
    QToolButton *m_toggle, *m_connect;
};
