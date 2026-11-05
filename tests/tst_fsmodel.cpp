// Listing a folder: hidden files, links (working and broken), odd names, and
// the model noticing a file that appears later.
#include "fsmodel.h"
#include "icons.h"
#include <QPainter>
#include <sys/stat.h>
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

    void mayRead()
    {
        if (geteuid() == 0) QSKIP("root reads everything");
        const quint32 me = geteuid(), other = me + 12345, nogroup = 0xfffffff0;
        QVERIFY(FsModel::mayRead(0644, me, nogroup, false));
        QVERIFY(!FsModel::mayRead(0244, me, nogroup, false));    // owner bits decide, even if others may
        QVERIFY(FsModel::mayRead(0604, other, nogroup, false));
        QVERIFY(!FsModel::mayRead(0640, other, nogroup, false));
        QVERIFY(FsModel::mayRead(0750, other, getegid(), true));   // my group
        QVERIFY(!FsModel::mayRead(0740, other, getegid(), true));  // a folder also needs x
        QVERIFY(!FsModel::mayRead(0700, other, nogroup, true));    // like /root
    }

    void unreadableGetsTheEmblem()
    {
        if (geteuid() == 0) QSKIP("root reads everything");
        QTemporaryDir t;
        QVERIFY(QDir().mkpath(t.path() + "/cerrada"));
        QVERIFY(QDir().mkpath(t.path() + "/abierta"));
        touch(t.path() + "/secreto.txt");
        QCOMPARE(chmod(QFile::encodeName(t.path() + "/cerrada").constData(), 0), 0);
        QCOMPARE(chmod(QFile::encodeName(t.path() + "/secreto.txt").constData(), 0200), 0);
        std::vector<FileEntry> v;
        QVERIFY(FsModel::listDirectory(t.path(), v, nullptr));
        QVERIFY(find(v, "cerrada")->unreadable);
        QVERIFY(find(v, "secreto.txt")->unreadable);
        QVERIFY(!find(v, "abierta")->unreadable);

        // The icon is the folder's with a red X at its lower right.
        const QIcon plain = Icons::get().forEntry(*find(v, "abierta"), t.path() + "/abierta");
        const QIcon locked = Icons::get().forEntry(*find(v, "cerrada"), t.path() + "/cerrada");
        const QImage a = plain.pixmap(64, 64).toImage(), b = locked.pixmap(64, 64).toImage();
        QCOMPARE(b.size(), QSize(64, 64));
        QCOMPARE(qAlpha(a.pixel(32, 20)), qAlpha(b.pixel(32, 20)));   // the folder itself untouched
        if (const QByteArray out = qgetenv("LUNA_EMBLEM_PNG"); !out.isEmpty()) {
            QImage sheet(3 * 72, 72, QImage::Format_ARGB32);
            sheet.fill(QColor("#1e1e2e"));
            QPainter p(&sheet);
            p.drawPixmap(4, 4, plain.pixmap(64, 64));
            p.drawPixmap(76, 4, locked.pixmap(64, 64));
            p.drawPixmap(148, 20, Icons::get().forEntry(*find(v, "secreto.txt"), {}).pixmap(32, 32));
            p.drawPixmap(188, 28, locked.pixmap(16, 16));
            p.end();
            sheet.save(QString::fromLocal8Bit(out));
        }
        const QColor c = b.pixelColor(50, 40);   // on the disc, off the X
        QVERIFY2(c.red() > 120 && c.red() > c.green() + 40, qPrintable(c.name()));
        chmod(QFile::encodeName(t.path() + "/cerrada").constData(), 0755);
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
