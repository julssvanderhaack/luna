// The GIO side of "Abrir como administrador". admin:// itself needs the
// password, so the operations run here on plain local paths: the same code,
// with GIO's local backend instead of gvfsd-admin.
#include "admin.h"
#include "fsmodel.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void write(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(data);
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

const FileEntry *find(const std::vector<FileEntry> &v, const QString &name)
{
    for (const FileEntry &e : v)
        if (e.name == name) return &e;
    return nullptr;
}
}

class TestAdmin : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    QString d;

private slots:
    void init()
    {
        d = m_tmp.path() + "/w";
        QDir(d).removeRecursively();
        QVERIFY(QDir().mkpath(d));
    }

    void paths()
    {
        QVERIFY(Admin::is("admin:///etc"));
        QVERIFY(!Admin::is("/etc"));
        QCOMPARE(Admin::toAdmin("/etc/ssl/"), QString("admin:///etc/ssl"));
        QCOMPARE(Admin::localPart("admin:///etc"), QString("/etc"));
        QCOMPARE(Admin::localPart("admin://"), QString("/"));
        QCOMPARE(Admin::localPart("/home"), QString("/home"));
        QCOMPARE(Admin::clean("admin:///etc//ssl/../x/"), QString("admin:///etc/x"));
        QCOMPARE(Admin::clean("/a//b/"), QString("/a/b"));
        QCOMPARE(Admin::parent("admin:///etc/fstab"), QString("admin:///etc"));
        QCOMPARE(Admin::parent("admin:///etc"), QString("admin:///"));
        QCOMPARE(Admin::fileName("admin:///etc/fstab"), QString("fstab"));
    }

    void urls()
    {
        const QString p = "admin:///root/mis cosas/ñandú #1";
        const QUrl u = Admin::url(p);
        QCOMPARE(u.scheme(), QString("admin"));
        QVERIFY(u.toEncoded().startsWith("admin:///root/mis%20cosas/"));
        QCOMPARE(Admin::fromUrl(u), p);
        QCOMPARE(Admin::fromUrl(QUrl::fromLocalFile("/tmp/x y")), QString("/tmp/x y"));
        QCOMPARE(Admin::fromUrl(Admin::url("/tmp/x y")), QString("/tmp/x y"));
        QVERIFY(Admin::fromUrl(QUrl("sftp://host/x")).isEmpty());
        QVERIFY(Admin::fromUrl(QUrl("admin://otro-host/x")).isEmpty());
    }

    void list()
    {
        write(d + "/a.txt", "12345");
        write(d + "/.oculto", "x");
        QVERIFY(QDir().mkpath(d + "/carpeta"));
        QCOMPARE(symlink("a.txt", QFile::encodeName(d + "/enlace").constData()), 0);
        QCOMPARE(symlink("carpeta", QFile::encodeName(d + "/a-carpeta").constData()), 0);
        QCOMPARE(symlink("nada", QFile::encodeName(d + "/roto").constData()), 0);
        std::vector<FileEntry> v;
        QString err;
        QVERIFY2(Admin::list(d, v, &err), qPrintable(err));
        QCOMPARE(int(v.size()), 6);
        QCOMPARE(find(v, "a.txt")->size, qint64(5));
        QCOMPARE(find(v, "a.txt")->mime, QString("text/plain"));
        QVERIFY(find(v, ".oculto")->hidden);
        QVERIFY(find(v, "carpeta")->isDir);
        QCOMPARE(find(v, "carpeta")->mime, QString("inode/directory"));
        QVERIFY(find(v, "enlace")->isLink && !find(v, "enlace")->broken);
        QCOMPARE(find(v, "enlace")->linkTarget, QString("a.txt"));
        QVERIFY(find(v, "a-carpeta")->isLink && find(v, "a-carpeta")->isDir);
        QVERIFY(find(v, "roto")->broken);
        // Same facts as the normal lister.
        std::vector<FileEntry> local;
        QVERIFY(FsModel::listDirectory(d, local, nullptr));
        for (const FileEntry &x : local) {
            const FileEntry *y = find(v, x.name);
            QVERIFY(y);
            QCOMPARE(y->mode, x.mode);
            QCOMPARE(y->mtime, x.mtime);
            QCOMPARE(y->isDir, x.isDir);
        }
        QVERIFY(!Admin::list(d + "/no-existe", v, &err));
        QVERIFY(!err.isEmpty());
    }

    void existsAndMake()
    {
        bool dir = false;
        QVERIFY(!Admin::exists(d + "/nuevo"));
        QString err;
        QVERIFY(Admin::makeDir(d + "/nuevo", &err));
        QVERIFY(Admin::exists(d + "/nuevo", &dir) && dir);
        QVERIFY(!Admin::makeDir(d + "/nuevo", &err));   // already there
        QVERIFY(Admin::makeFile(d + "/nuevo/vacio.txt", &err));
        QVERIFY(Admin::exists(d + "/nuevo/vacio.txt", &dir) && !dir);
        QCOMPARE(QFileInfo(d + "/nuevo/vacio.txt").size(), qint64(0));
        QVERIFY(!Admin::makeFile(d + "/nuevo/vacio.txt", &err));   // never overwrites
        QCOMPARE(symlink("nada", QFile::encodeName(d + "/roto").constData()), 0);
        QVERIFY(Admin::exists(d + "/roto"));   // a broken link is still there
    }

    void renameRules()
    {
        write(d + "/a", "x");
        QString err, made;
        QVERIFY(Admin::rename(d + "/a", "b c", &err, &made));
        QCOMPARE(made, d + "/b c");
        QCOMPARE(read(d + "/b c"), QByteArray("x"));
        QVERIFY(!Admin::rename(d + "/b c", "../fuera", &err));
        QVERIFY(!Admin::rename(d + "/b c", "..", &err));
        QVERIFY(!Admin::rename(d + "/b c", "", &err));
        QVERIFY(!QFileInfo::exists(m_tmp.path() + "/fuera"));
    }

    void copyFileKeepsTimeDropsSetuid()
    {
        write(d + "/prog", "#!/bin/sh\n");
        QCOMPARE(chmod(QFile::encodeName(d + "/prog").constData(), 06755), 0);
        struct timespec t[2] = {{1000000000, 0}, {1000000000, 0}};
        QCOMPARE(utimensat(AT_FDCWD, QFile::encodeName(d + "/prog").constData(), t, 0), 0);
        QStringList errors;
        QVERIFY2(Admin::copy(d + "/prog", d + "/copia", {}, &errors), qPrintable(errors.join('\n')));
        QCOMPARE(read(d + "/copia"), QByteArray("#!/bin/sh\n"));
        QCOMPARE(modeOf(d + "/copia"), 0755u);
        QCOMPARE(QFileInfo(d + "/copia").lastModified().toSecsSinceEpoch(), 1000000000);
        QCOMPARE(modeOf(d + "/prog"), 06755u);   // the original untouched
    }

    void copyTreeWithLinks()
    {
        QVERIFY(QDir().mkpath(d + "/src/sub"));
        write(d + "/src/sub/f", QByteArray(3 << 20, 'z'));
        write(d + "/src/.h", "h");
        QCOMPARE(chmod(QFile::encodeName(d + "/src/sub").constData(), 03775), 0);
        QCOMPARE(symlink("sub/f", QFile::encodeName(d + "/src/link").constData()), 0);
        QVERIFY(QDir().mkpath(m_tmp.path() + "/fuera"));
        write(m_tmp.path() + "/fuera/secreto", "s");
        QCOMPARE(symlink(QFile::encodeName(m_tmp.path() + "/fuera").constData(), QFile::encodeName(d + "/src/dirlink").constData()), 0);
        qint64 bytes = 0;
        Admin::CopyOptions o;
        o.progress = [&](qint64 n) { bytes += n; };
        QStringList errors;
        QVERIFY2(Admin::copy(d + "/src", d + "/dst", o, &errors), qPrintable(errors.join('\n')));
        QCOMPARE(QFileInfo(d + "/dst/sub/f").size(), qint64(3 << 20));
        QCOMPARE(read(d + "/dst/.h"), QByteArray("h"));
        QVERIFY(QFileInfo(d + "/dst/link").isSymLink());
        QVERIFY(QFileInfo(d + "/dst/dirlink").isSymLink());   // not a copy of what it points to
        QVERIFY(!QFileInfo::exists(d + "/dst/dirlink/secreto") || QFileInfo(d + "/dst/dirlink").isSymLink());
        QCOMPARE(modeOf(d + "/dst/sub"), 01775u);
        QVERIFY(bytes >= (3 << 20));
    }

    void mergeAsksPerItem()
    {
        QVERIFY(QDir().mkpath(d + "/src"));
        QVERIFY(QDir().mkpath(d + "/dst"));
        write(d + "/src/a", "nuevo");
        write(d + "/src/b", "b");
        write(d + "/dst/a", "viejo");
        QStringList asked;
        Admin::CopyOptions o;
        o.resolve = [&](const QString &, const QString &dst, bool &skip) {
            skip = QFileInfo::exists(dst);
            if (skip) asked << QFileInfo(dst).fileName();
            return dst;
        };
        QStringList errors;
        QVERIFY(!Admin::copy(d + "/src", d + "/dst", o, &errors));   // one item was left out
        QCOMPARE(asked, QStringList{"a"});
        QCOMPARE(read(d + "/dst/a"), QByteArray("viejo"));
        QCOMPARE(read(d + "/dst/b"), QByteArray("b"));
    }

    void moveAndCancel()
    {
        QVERIFY(QDir().mkpath(d + "/m/sub"));
        write(d + "/m/sub/x", "x");
        Admin::CopyOptions o;
        o.move = true;
        QStringList errors;
        QVERIFY(Admin::copy(d + "/m", d + "/m2", o, &errors));
        QVERIFY(!QFileInfo::exists(d + "/m"));
        QCOMPARE(read(d + "/m2/sub/x"), QByteArray("x"));

        std::atomic<bool> cancel{true};
        Admin::CopyOptions c;
        c.cancel = &cancel;
        QVERIFY(!Admin::copy(d + "/m2", d + "/m3", c, &errors));
        QVERIFY(!QFileInfo::exists(d + "/m3/sub/x"));
    }

    void removeDoesNotFollowLinks()
    {
        QVERIFY(QDir().mkpath(m_tmp.path() + "/fuera2"));
        write(m_tmp.path() + "/fuera2/importante", "no tocar");
        QVERIFY(QDir().mkpath(d + "/r/sub"));
        write(d + "/r/sub/f", "f");
        QCOMPARE(symlink(QFile::encodeName(m_tmp.path() + "/fuera2").constData(), QFile::encodeName(d + "/r/enlace").constData()), 0);
        QString err;
        QVERIFY2(Admin::remove(d + "/r", &err), qPrintable(err));
        QVERIFY(!QFileInfo::exists(d + "/r"));
        QCOMPARE(read(m_tmp.path() + "/fuera2/importante"), QByteArray("no tocar"));
        QVERIFY(!Admin::remove(d + "/no-existe", &err));
    }
};

QTEST_GUILESS_MAIN(TestAdmin)
#include "tst_admin.moc"
