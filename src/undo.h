// Undo/redo of file operations (Ctrl+Z / Ctrl+Shift+Z), like Nautilus.
// Every operation is kept as exact "before -> after" paths so it can be
// reversed. Undoing never deletes data for good: copies and new items go to
// the trash. Deleting for good cannot be undone.
#pragma once
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

class Undo : public QObject {
    Q_OBJECT
public:
    enum Type { Copy, Move, Link, Trash, Restore, Rename, Create };
    using Pairs = QList<QPair<QString, QString>>;   // before -> after (Create: "" -> new path)
    struct Op {
        Type type;
        Pairs pairs;
        bool dir = false;   // Create: a folder rather than a file
    };

    static Undo &get();
    void record(Type type, const Pairs &pairs, bool dir = false);
    bool canUndo() const { return !m_busy && !m_undo.isEmpty(); }
    bool canRedo() const { return !m_busy && !m_redo.isEmpty(); }
    QString undoText() const;   // "Deshacer mover «foto.jpg»"
    QString redoText() const;
    void undo();
    void redo();

signals:
    void changed();
    void message(const QString &text);

private:
    Undo() = default;
    void run(const Op &op, bool backwards);
    void finish(const Op &result, bool backwards, const QStringList &errors);
    static QString describe(const Op &op);

    QList<Op> m_undo, m_redo;
    bool m_busy = false;
};
