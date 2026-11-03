// Reading inside archives, and the private folder a member opened from the
// preview is extracted to.
#include "archives.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>

namespace {
struct Member {
    QByteArray path, data;
    bool dir = false;
};

// Writes an archive with libarchive itself: format "zip", "gnutar", "7zip"…
bool makeArchive(const QString &file, const char *format, const char *filter, const QList<Member> &members)
{
    archive *a = archive_write_new();
    archive_write_set_format_by_name(a, format);
    if (filter) archive_write_add_filter_by_name(a, filter);
    if (archive_write_open_filename(a, QFile::encodeName(file).constData()) != ARCHIVE_OK) return false;
    for (const Member &m : members) {
        archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, m.path.constData());
        archive_entry_set_filetype(e, m.dir ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(e, m.dir ? 0755 : 0644);
        archive_entry_set_size(e, m.data.size());
        archive_entry_set_mtime(e, 1700000000, 0);
        archive_write_header(a, e);
        if (!m.dir) archive_write_data(a, m.data.constData(), size_t(m.data.size()));
        archive_entry_free(e);
    }
    const bool ok = archive_write_close(a) == ARCHIVE_OK;
    archive_write_free(a);
    return ok;
}

const QList<Member> sample = {
    {"docs/", {}, true},
    {"docs/léeme.txt", "hola desde dentro"},
    {"./raiz.bin", QByteArray(100000, '\x7f')},
    {"../fuera.txt", "intento de escapar"},
};
}

class TestArchives : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;

private slots:
    void listAndRead_data()
    {
        QTest::addColumn<QString>("format");
        QTest::addColumn<QString>("filter");
        QTest::newRow("zip") << "zip" << "";
        QTest::newRow("tar.gz") << "gnutar" << "gzip";
        QTest::newRow("tar.xz") << "pax" << "xz";
        QTest::newRow("7z") << "7zip" << "";
    }

    void listAndRead()
    {
        QFETCH(QString, format);
        QFETCH(QString, filter);
        const QString file = m_tmp.path() + "/a." + QTest::currentDataTag();
        QVERIFY(makeArchive(file, format.toLatin1().constData(), filter.isEmpty() ? nullptr : filter.toLatin1().constData(), sample));

        const Archive::Listing l = Archive::list(file);
        QVERIFY2(l.error.isEmpty(), qPrintable(l.error));
        QStringList paths;
        for (const Archive::Entry &e : l.entries) paths << e.path;
        QVERIFY(paths.contains("docs"));
        QVERIFY(paths.contains("docs/léeme.txt"));
        QVERIFY(paths.contains("raiz.bin"));   // "./" dropped
        for (const Archive::Entry &e : l.entries)
            if (e.path == "docs") QVERIFY(e.isDir);

        QCOMPARE(Archive::read(file, "docs/léeme.txt"), QByteArray("hola desde dentro"));
        QCOMPARE(Archive::read(file, "raiz.bin", 1000).size() >= 1000, true);   // stops near the cap
        QVERIFY(Archive::read(file, "no/esta").isEmpty());

        const QString out = m_tmp.path() + "/extraido";
        QFile::remove(out);
        QVERIFY(Archive::extract(file, "raiz.bin", out));
        QCOMPARE(QFileInfo(out).size(), qint64(100000));
        QString err;
        QVERIFY(!Archive::extract(file, "no/esta", out, &err));
        QVERIFY(!err.isEmpty());
    }

    void capTruncates()
    {
        QList<Member> many;
        for (int i = 0; i < 50; ++i) many.append({QByteArray("f") + QByteArray::number(i), "x"});
        const QString file = m_tmp.path() + "/many.zip";
        QVERIFY(makeArchive(file, "zip", nullptr, many));
        const Archive::Listing l = Archive::list(file, 10);
        QCOMPARE(l.entries.size(), 10);
        QVERIFY(l.truncated);
    }

    void notAnArchive()
    {
        const QString file = m_tmp.path() + "/nada.zip";
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("esto no es un zip");
        f.close();
        const Archive::Listing l = Archive::list(file);
        QVERIFY(l.entries.isEmpty());
        QVERIFY(!l.error.isEmpty());
    }

    void privateDirIsPrivateAndRandom()
    {
        const QString a = Archive::privateDir(), b = Archive::privateDir();
        QVERIFY(!a.isEmpty());
        QVERIFY(a != b);
        QVERIFY(a.startsWith(QDir::homePath()));   // the test's own HOME, under ~/.cache/luna/members
        for (const QString &d : {a, b, QFileInfo(a).absolutePath(), QFileInfo(QFileInfo(a).absolutePath()).absolutePath()}) {
            struct stat st;
            QCOMPARE(lstat(QFile::encodeName(d).constData(), &st), 0);
            QVERIFY(S_ISDIR(st.st_mode));
            QCOMPARE(st.st_mode & 0777, 0700u);
        }
    }

    void memberFileName()
    {
        QCOMPARE(Archive::memberFileName("docs/léeme.txt"), QString("léeme.txt"));
        QCOMPARE(Archive::memberFileName("plano"), QString("plano"));
        QCOMPARE(Archive::memberFileName(""), QString("archivo"));
        QCOMPARE(Archive::memberFileName("a/.."), QString("archivo"));
        QCOMPARE(Archive::memberFileName(".."), QString("archivo"));
        QVERIFY(!Archive::memberFileName("../../etc/passwd").contains('/'));
    }
};

QTEST_GUILESS_MAIN(TestArchives)
#include "tst_archives.moc"
