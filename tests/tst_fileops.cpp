// Copy, move and delete jobs on a scratch folder: what lands on disk, the
// permissions it gets, and that links are never followed.
#include "fileops.h"
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void write(const QString &path, const QByteArray &data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

QByteArray read(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
}

unsigned modeOf(const QString &path)
{
    struct stat st;
    return lstat(QFile::encodeName(path).constData(), &st) == 0 ? st.st_mode & 07777 : 0;
}

struct Result {
    QStringList errors, conflicts;
    QList<QPair<QString, QString>> pairs;
};

// Runs a job to the end; conflicts get `answer` (asked from the worker thread,
// so the reply is direct, as MainWindow's blocking dialog would give).
Result run(Job::Kind kind, const QStringList &srcs, const QString &dest, int answer = Job::Cancel, const QStringList &targets = {})
{
    Result r;
    QMutex lock;
    auto *job = new Job(kind, srcs, dest);
    job->setTargets(targets);
    QObject::connect(job, &Job::conflict, job, [&](const QString &, const QString &dst, int *a, bool *) {
        QMutexLocker l(&lock);
        r.conflicts << dst;
        *a = answer;
    }, Qt::DirectConnection);
    QEventLoop loop;
    QObject::connect(job, &Job::finished, job, [&](const QStringList &errors) {
        r.errors = errors;
        r.pairs = job->pairs();
        QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    job->start();
    loop.exec();
    return r;
}
}

class TestFileOps : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp{QDir::homePath() + "/scratch-XXXXXX"};   // same disk as the trash
    QString src, dst;

private slots:
    void init()
    {
        QVERIFY(m_tmp.isValid());
        src = m_tmp.path() + "/src";
        dst = m_tmp.path() + "/dst";
        QDir(src).removeRecursively();
        QDir(dst).removeRecursively();
        QVERIFY(QDir().mkpath(src));
        QVERIFY(QDir().mkpath(dst));
    }

    void copyKeepsContentsAndTime()
    {
        write(src + "/a.txt", "hola");
        struct timespec t[2] = {{1000000000, 0}, {1000000000, 0}};
        QCOMPARE(utimensat(AT_FDCWD, QFile::encodeName(src + "/a.txt").constData(), t, 0), 0);
        const Result r = run(Job::Copy, {src + "/a.txt"}, dst);
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join('\n')));
        QCOMPARE(read(dst + "/a.txt"), QByteArray("hola"));
        QCOMPARE(QFileInfo(dst + "/a.txt").lastModified().toSecsSinceEpoch(), 1000000000);
        QCOMPARE(read(src + "/a.txt"), QByteArray("hola"));
    }

    void copyTreeWithHiddenAndUnicode()
    {
        QVERIFY(QDir().mkpath(src + "/carpeta/ñandú"));
        write(src + "/carpeta/.oculto", "x");
        write(src + "/carpeta/ñandú/canción.flac", QByteArray(3 << 20, 'z'));   // more than one copy chunk
        const Result r = run(Job::Copy, {src + "/carpeta"}, dst);
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(read(dst + "/carpeta/.oculto"), QByteArray("x"));
        QCOMPARE(QFileInfo(dst + "/carpeta/ñandú/canción.flac").size(), qint64(3 << 20));
    }

    void symlinkIsCopiedAsLink()
    {
        write(src + "/target", "t");
        QCOMPARE(symlink("target", QFile::encodeName(src + "/link").constData()), 0);
        QCOMPARE(symlink("/nowhere/at/all", QFile::encodeName(src + "/broken").constData()), 0);
        const Result r = run(Job::Copy, {src + "/link", src + "/broken"}, dst);
        QVERIFY(r.errors.isEmpty());
        QVERIFY(QFileInfo(dst + "/link").isSymLink());
        QCOMPARE(QFileInfo(dst + "/link").symLinkTarget(), dst + "/target");   // relative, as it was
        QVERIFY(QFileInfo(dst + "/broken").isSymLink());
    }

    void setuidAndSetgidAreDropped()
    {
        write(src + "/prog", "#!");
        QCOMPARE(chmod(QFile::encodeName(src + "/prog").constData(), 06755), 0);
        QVERIFY(QDir().mkpath(src + "/shared"));
        QCOMPARE(chmod(QFile::encodeName(src + "/shared").constData(), 03775), 0);
        const mode_t old = umask(022);
        const Result r = run(Job::Copy, {src + "/prog", src + "/shared"}, dst);
        umask(old);
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(modeOf(dst + "/prog"), 0755u);
        QCOMPARE(modeOf(dst + "/shared"), 01775u);   // sticky stays, setgid goes
    }

    void copiedModeTable()
    {
        QCOMPARE(FileOps::copiedMode(0104755, false), 0755u);
        QCOMPARE(FileOps::copiedMode(0102755, false), 0755u);
        QCOMPARE(FileOps::copiedMode(041777, true), 01777u);
        QCOMPARE(FileOps::copiedMode(042775, true), 0775u);
    }

    void replaceOverSymlinkLeavesItsTargetAlone()
    {
        // dst/a.txt is a link to a file outside: replacing must swap the link,
        // never write through it.
        write(src + "/a.txt", "nuevo");
        write(m_tmp.path() + "/victima", "intacto");
        QCOMPARE(symlink(QFile::encodeName(m_tmp.path() + "/victima").constData(), QFile::encodeName(dst + "/a.txt").constData()), 0);
        const Result r = run(Job::Copy, {src + "/a.txt"}, dst, Job::Replace);
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(r.conflicts.size(), 1);
        QVERIFY(!QFileInfo(dst + "/a.txt").isSymLink());
        QCOMPARE(read(dst + "/a.txt"), QByteArray("nuevo"));
        QCOMPARE(read(m_tmp.path() + "/victima"), QByteArray("intacto"));
    }

    void keepBothAndSkip()
    {
        write(src + "/a.txt", "nuevo");
        write(dst + "/a.txt", "viejo");
        Result r = run(Job::Copy, {src + "/a.txt"}, dst, Job::Skip);
        QCOMPARE(read(dst + "/a.txt"), QByteArray("viejo"));
        r = run(Job::Copy, {src + "/a.txt"}, dst, Job::KeepBoth);
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(r.pairs.size(), 1);
        const QString made = r.pairs.first().second;
        QVERIFY(made != dst + "/a.txt");
        QCOMPARE(read(made), QByteArray("nuevo"));
        QCOMPARE(read(dst + "/a.txt"), QByteArray("viejo"));
    }

    void copyIntoSameFolderMakesUniqueName()
    {
        write(src + "/a.txt", "x");
        const Result r = run(Job::Copy, {src + "/a.txt"}, src);
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(QDir(src).entryList(QDir::Files).size(), 2);
        QCOMPARE(FileOps::uniqueName(src, "nuevo.txt"), QString("nuevo.txt"));
        QVERIFY(FileOps::uniqueName(src, "a.txt") != "a.txt");
    }

    void noCopyIntoItself()
    {
        QVERIFY(QDir().mkpath(src + "/d/sub"));
        const Result r = run(Job::Copy, {src + "/d"}, src + "/d/sub");
        QCOMPARE(r.errors.size(), 1);
        QVERIFY(!QFileInfo::exists(src + "/d/sub/d"));
    }

    void moveRenames()
    {
        write(src + "/a.txt", "m");
        const Result r = run(Job::Move, {src + "/a.txt"}, dst);
        QVERIFY(r.errors.isEmpty());
        QVERIFY(!QFileInfo::exists(src + "/a.txt"));
        QCOMPARE(read(dst + "/a.txt"), QByteArray("m"));
    }

    void deleteDoesNotFollowLinks()
    {
        QVERIFY(QDir().mkpath(m_tmp.path() + "/fuera"));
        write(m_tmp.path() + "/fuera/importante", "no tocar");
        QVERIFY(QDir().mkpath(src + "/d"));
        QCOMPARE(symlink(QFile::encodeName(m_tmp.path() + "/fuera").constData(), QFile::encodeName(src + "/d/enlace").constData()), 0);
        const Result r = run(Job::Delete, {src + "/d"}, {});
        QVERIFY(r.errors.isEmpty());
        QVERIFY(!QFileInfo::exists(src + "/d"));
        QCOMPARE(read(m_tmp.path() + "/fuera/importante"), QByteArray("no tocar"));
    }

    void trashAndRestore()
    {
        write(src + "/basura.txt", "b");
        QList<QPair<QString, QString>> done;
        const QStringList errors = FileOps::get().trash({src + "/basura.txt"}, &done);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        QCOMPARE(done.size(), 1);
        QVERIFY(!QFileInfo::exists(src + "/basura.txt"));
        QVERIFY(done.first().second.startsWith(QDir::homePath()));   // the test's own HOME
        QCOMPARE(FileOps::originalPath(done.first().second), src + "/basura.txt");
        const Result r = run(Job::Restore, {done.first().second}, {});
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(read(src + "/basura.txt"), QByteArray("b"));
    }
};

QTEST_GUILESS_MAIN(TestFileOps)
#include "tst_fileops.moc"
