// Listing a folder: hidden files, links (working and broken), odd names, and
// the model noticing a file that appears later.
#include "fsmodel.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <unistd.h>

namespace {
void touch(const QString &path, const QByteArray &data = "x")
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(data);
}

const FileEntry *find(const std::vector<FileEntry> &v, const QString &name)
{
    for (const FileEntry &e : v)
        if (e.name == name) return &e;
    return nullptr;
}

QStringList names(const FsModel &m)
{
    QStringList out;
    for (int r = 0; r < m.rowCount(); ++r) out << m.index(r, 0).data(FsModel::PathRole).toString().section('/', -1);
    return out;
}
}

class TestFsModel : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;

private slots:
    void initTestCase()
    {
        const QString d = m_tmp.path();
        touch(d + "/normal.txt", "12345");
        touch(d + "/.oculto");
        touch(d + "/con espacios y ñ — «comillas».txt");
        touch(d + "/salto\nde línea");
        QVERIFY(QDir().mkpath(d + "/carpeta"));
        touch(d + "/carpeta/dentro");
        QCOMPARE(symlink("normal.txt", QFile::encodeName(d + "/enlace").constData()), 0);
        QCOMPARE(symlink("no-existe", QFile::encodeName(d + "/roto").constData()), 0);
    }

    void listDirectory()
    {
        std::vector<FileEntry> v;
        QString err;
        QVERIFY(FsModel::listDirectory(m_tmp.path(), v, &err));
        QVERIFY(err.isEmpty());
        QCOMPARE(int(v.size()), 7);

        const FileEntry *n = find(v, "normal.txt");
        QVERIFY(n);
        QCOMPARE(n->size, qint64(5));
        QVERIFY(!n->isDir && !n->isLink && !n->hidden);
        QVERIFY(find(v, ".oculto")->hidden);
        QVERIFY(find(v, "carpeta")->isDir);
        QVERIFY(find(v, "salto\nde línea"));
        QVERIFY(find(v, "con espacios y ñ — «comillas».txt"));

        const FileEntry *l = find(v, "enlace");
        QVERIFY(l->isLink && !l->broken);
        QCOMPARE(l->linkTarget, QString("normal.txt"));
        QVERIFY(find(v, "roto")->isLink && find(v, "roto")->broken);
    }

    void missingFolder()
    {
        std::vector<FileEntry> v;
        QString err;
        QVERIFY(!FsModel::listDirectory(m_tmp.path() + "/no-existe", v, &err));
        QVERIFY(!err.isEmpty());
    }

    void modelLoadsAndWatches()
    {
        FsModel m;
        QSignalSpy loaded(&m, &FsModel::rootLoaded);
        m.setRoot(m_tmp.path());
        QVERIFY(loaded.wait(10000));
        QTRY_COMPARE_WITH_TIMEOUT(m.rowCount(), 7, 5000);
        QVERIFY(names(m).contains("carpeta"));

        touch(m_tmp.path() + "/nuevo.txt");
        QTRY_VERIFY_WITH_TIMEOUT(names(m).contains("nuevo.txt"), 10000);
        QFile::remove(m_tmp.path() + "/nuevo.txt");
        QTRY_VERIFY_WITH_TIMEOUT(!names(m).contains("nuevo.txt"), 10000);
    }

    void permString()
    {
        QCOMPARE(FsModel::permString(0100644, false, false), QString("-rw-r--r--"));
        QCOMPARE(FsModel::permString(040755, true, false), QString("drwxr-xr-x"));
    }

    void cleanupTestCase() { FsModel::shutdown(); }
};

QTEST_MAIN(TestFsModel)
#include "tst_fsmodel.moc"
