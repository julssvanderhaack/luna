// Ctrl+P: jump to any folder by typing bits of its path ("luna src"), from
// zoxide's frecency database plus the GTK bookmarks. Luna also tells zoxide
// about the folders it visits, so the shell's "z" learns from it too.
#pragma once
#include <QDialog>

class QLineEdit;
class QListWidget;

class JumpDialog : public QDialog {
    Q_OBJECT
public:
    explicit JumpDialog(QWidget *parent);
    QString chosen() const { return m_chosen; }

    static void recordVisit(const QString &dir);   // zoxide add, debounced and detached

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void fill(const QString &query);
    struct Entry {
        QString path;
        double score;
    };
    QList<Entry> m_entries;
    QLineEdit *m_edit;
    QListWidget *m_list;
    QString m_chosen;
};
