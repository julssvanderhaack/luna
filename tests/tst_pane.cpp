// "Abrir como administrador" in a pane, without ever asking for the
// password: the session bus is cut off, so GVFS cannot start gvfsd-admin and
// admin:// fails the way a cancelled password does.
#include "admin.h"
#include "fsmodel.h"
#include "pane.h"
#include "places.h"
#include <QApplication>
#include <QDir>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <sys/stat.h>
#include <unistd.h>

namespace {
QPushButton *adminButton(Pane &p)
{
    for (QPushButton *b : p.findChildren<QPushButton *>())
        if (b->isVisible()) return b;
    return nullptr;
}
}

class TestPane : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;

private slots:
    void displayNames()
    {
        QCOMPARE(Places::displayName("admin:///etc"), QString("etc"));
        QVERIFY(Places::displayName("admin:///").startsWith('/'));
    }

    void lockedFolderOffersPassword()
    {
        if (geteuid() == 0) QSKIP("root reads every folder");
        const QString locked = m_tmp.path() + "/cerrada";
        QVERIFY(QDir().mkpath(locked));
        QCOMPARE(chmod(QFile::encodeName(locked).constData(), 0), 0);
        Pane p;
        p.resize(600, 400);
        p.show();
        p.setPath(locked);
        QTRY_VERIFY_WITH_TIMEOUT(adminButton(p) != nullptr, 5000);
        QCOMPARE(adminButton(p)->text(), QString("Abrir como administrador"));
        chmod(QFile::encodeName(locked).constData(), 0755);

        // A readable folder has no such button.
        p.setPath(m_tmp.path());
        QTRY_VERIFY_WITH_TIMEOUT(adminButton(p) == nullptr, 5000);
    }

    void adminFolderFailsGracefully()
    {
        Pane p;
        p.resize(600, 400);
        p.show();
        p.openAsAdmin("/etc");
        QCOMPARE(p.path(), QString("admin:///etc"));
        QVERIFY(p.isAdmin());
        QTRY_VERIFY_WITH_TIMEOUT(adminButton(p) != nullptr, 10000);   // no gvfsd: an error, and a way to retry
        QCOMPARE(adminButton(p)->text(), QString("Volver a intentarlo"));
        p.up();
        QCOMPARE(p.path(), QString("admin:///"));
        p.up();   // already at the top
        QCOMPARE(p.path(), QString("admin:///"));
        p.setPath(Admin::localPart(p.path()));
        QVERIFY(!p.isAdmin());
    }

    void cleanupTestCase() { FsModel::shutdown(); }
};

int main(int argc, char **argv)
{
    // Before GIO starts: no session bus, so no gvfsd and no polkit dialog.
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/luna-test-bus");
    qputenv("GIO_USE_VFS", "local");
    QApplication app(argc, argv);
    TestPane t;
    return QTest::qExec(&t, argc, argv);
}
#include "tst_pane.moc"
