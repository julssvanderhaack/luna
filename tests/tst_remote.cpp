// Passwords typed in a remote address never reach the recent list.
#include "remote.h"
#include <QSettings>
#include <QTest>

class TestRemote : public QObject {
    Q_OBJECT

private slots:
    void withoutPassword_data()
    {
        QTest::addColumn<QString>("in");
        QTest::addColumn<QString>("out");
        QTest::newRow("sftp") << "sftp://ana:secreto@host/x" << "sftp://ana@host/x";
        QTest::newRow("port") << "ftp://ana:s3cr%40to@host:2121/" << "ftp://ana@host:2121/";
        QTest::newRow("no password") << "sftp://ana@host/x" << "sftp://ana@host/x";
        QTest::newRow("no user") << "smb://servidor/compartida" << "smb://servidor/compartida";
        QTest::newRow("empty") << "" << "";
    }

    void withoutPassword()
    {
        QFETCH(QString, in);
        QFETCH(QString, out);
        QCOMPARE(Remote::withoutPassword(in), out);
    }

    void recentDropsStoredPasswords()
    {
        // An older Luna may have saved one: reading the list cleans it on disk too.
        QSettings("luna", "luna").setValue("remote/recent", QStringList{"sftp://ana:secreto@host/x", "sftp://ana@host/x", "smb://nas/fotos"});
        const QStringList r = Remote::get().recent();
        QCOMPARE(r, (QStringList{"sftp://ana@host/x", "smb://nas/fotos"}));
        QSettings q("luna", "luna");
        q.sync();
        for (const QString &s : q.value("remote/recent").toStringList()) QVERIFY(!s.contains("secreto"));
    }
};

QTEST_GUILESS_MAIN(TestRemote)
#include "tst_remote.moc"
