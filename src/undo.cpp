#include "undo.h"
#include "fileops.h"
#include "places.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace {
constexpr int kMaxSteps = 100;

QByteArray enc(const QString &p) { return QFile::encodeName(p); }
QString errText() { return QString::fromLocal8Bit(strerror(errno)); }

// What undo/redo itself does must not land on the stacks again.
struct Quiet {
    Quiet() { FileOps::get().setRecording(false); }
    ~Quiet() { FileOps::get().setRecording(true); }
};

QStringList befores(const Undo::Pairs &p)
{
    QStringList out;
    for (const auto &x : p) out << x.first;
    return out;
}

QStringList afters(const Undo::Pairs &p)
{
    QStringList out;
    for (const auto &x : p) out << x.second;
    return out;
}

Undo::Pairs swapped(const Undo::Pairs &p)
{
    Undo::Pairs out;
    for (const auto &x : p) out.append({x.second, x.first});
    return out;
}

// Moves and renames keep their stars.
void remapStars(const Undo::Op &op)
{
    if (op.type == Undo::Move || op.type == Undo::Rename) Starred::get().remap(op.pairs);
}
} // namespace

Undo &Undo::get()
{
    static Undo u;
    return u;
}

void Undo::record(Type type, const Pairs &pairs, bool dir)
{
    if (pairs.isEmpty()) return;
    const Op op{type, pairs, dir};
    remapStars(op);
    m_undo.append(op);
    if (m_undo.size() > kMaxSteps) m_undo.removeFirst();
    m_redo.clear();
    emit changed();
}

QString Undo::describe(const Op &op)
{
    const QString path = op.pairs.isEmpty() ? QString() : op.type == Trash || op.type == Copy || op.type == Move || op.type == Rename
                                                             ? op.pairs[0].first
                                                             : op.pairs[0].second;
    const QString what = op.pairs.size() == 1 ? QString::fromUtf8("«%1»").arg(QFileInfo(path).fileName())
                                              : QString::fromUtf8("%1 elementos").arg(op.pairs.size());
    switch (op.type) {
    case Copy: return QString::fromUtf8("copiar %1").arg(what);
    case Move: return QString::fromUtf8("mover %1").arg(what);
    case Link: return QString::fromUtf8("crear enlace a %1").arg(what);
    case Trash: return QString::fromUtf8("mover a la papelera %1").arg(what);
    case Restore: return QString::fromUtf8("restaurar %1").arg(what);
    case Rename: return QString::fromUtf8("renombrar %1").arg(what);
    case Create: return QString::fromUtf8("crear %1").arg(what);
    }
    return {};
}

QString Undo::undoText() const { return m_undo.isEmpty() ? QString::fromUtf8("Deshacer") : QString::fromUtf8("Deshacer ") + describe(m_undo.last()); }
QString Undo::redoText() const { return m_redo.isEmpty() ? QString::fromUtf8("Rehacer") : QString::fromUtf8("Rehacer ") + describe(m_redo.last()); }

void Undo::undo()
{
    if (!canUndo()) return;
    run(m_undo.takeLast(), true);
}

void Undo::redo()
{
    if (!canRedo()) return;
    run(m_redo.takeLast(), false);
}

void Undo::run(const Op &op, bool backwards)
{
    m_busy = true;
    emit changed();
    Quiet quiet;
    QStringList errors;
    auto job = [this, backwards](Job *j, auto makeResult) {
        connect(j, &Job::finished, this, [this, j, backwards, makeResult](const QStringList &errs) {
            finish(makeResult(j->pairs()), backwards, errs);
        });
    };
    auto trashAll = [&errors](const QStringList &paths) {
        Pairs done;
        errors << FileOps::get().trash(paths, &done);
        return done;
    };

    switch (op.type) {
    case Copy:
        if (backwards) {
            trashAll(afters(op.pairs));
            break;
        }
        job(FileOps::get().copyTo(befores(op.pairs), afters(op.pairs)), [](const Pairs &p) { return Op{Copy, p}; });
        return;
    case Move:
        if (backwards) job(FileOps::get().moveTo(afters(op.pairs), befores(op.pairs)), [](const Pairs &p) { return Op{Move, swapped(p)}; });
        else job(FileOps::get().moveTo(befores(op.pairs), afters(op.pairs)), [](const Pairs &p) { return Op{Move, p}; });
        return;
    case Link:
        for (const auto &x : op.pairs) {
            if (backwards) {
                if (QFileInfo(x.second).isSymLink() && ::unlink(enc(x.second).constData()) != 0)
                    errors << QString::fromUtf8("No se pudo quitar el enlace «%1»: %2").arg(QFileInfo(x.second).fileName(), errText());
            } else if (::symlink(enc(x.first).constData(), enc(x.second).constData()) != 0) {
                errors << QString::fromUtf8("No se pudo crear el enlace «%1»: %2").arg(QFileInfo(x.second).fileName(), errText());
            }
        }
        break;
    case Trash:
        if (backwards) {
            job(FileOps::get().restore(afters(op.pairs)), [](const Pairs &p) { return Op{Trash, swapped(p)}; });
            return;
        }
        finish(Op{Trash, trashAll(befores(op.pairs))}, false, errors);
        return;
    case Restore:
        if (backwards) {
            finish(Op{Restore, swapped(trashAll(afters(op.pairs)))}, true, errors);
            return;
        }
        job(FileOps::get().restore(befores(op.pairs)), [](const Pairs &p) { return Op{Restore, p}; });
        return;
    case Rename:
        for (const auto &x : op.pairs) {
            const QString &from = backwards ? x.second : x.first, &to = backwards ? x.first : x.second;
            if (QFileInfo::exists(to) || QFileInfo(to).isSymLink()) {
                errors << QString::fromUtf8("Ya existe «%1».").arg(QFileInfo(to).fileName());
                continue;
            }
            if (::rename(enc(from).constData(), enc(to).constData()) != 0)
                errors << QString::fromUtf8("No se pudo renombrar «%1»: %2").arg(QFileInfo(from).fileName(), errText());
        }
        break;
    case Create:
        if (backwards) {
            trashAll(afters(op.pairs));
            break;
        }
        for (const auto &x : op.pairs) {
            const bool ok = op.dir ? QDir().mkdir(x.second) : [&] {
                QFile f(x.second);
                return !f.exists() && f.open(QIODevice::WriteOnly);
            }();
            if (!ok) errors << QString::fromUtf8("No se pudo crear «%1».").arg(QFileInfo(x.second).fileName());
        }
        break;
    }
    finish(op, backwards, errors);
}

void Undo::finish(const Op &result, bool backwards, const QStringList &errors)
{
    m_busy = false;
    // What was undone can be redone (and the other way round), unless
    // nothing at all could be done.
    if (!result.pairs.isEmpty()) {
        remapStars(backwards ? Op{result.type, swapped(result.pairs), result.dir} : result);
        (backwards ? m_redo : m_undo).append(result);
    }
    QString text = (backwards ? QString::fromUtf8("Deshecho: ") : QString::fromUtf8("Rehecho: ")) + describe(result);
    if (!errors.isEmpty()) text += QString::fromUtf8(" (con errores: %1)").arg(errors.first());
    emit message(text);
    emit changed();
}
