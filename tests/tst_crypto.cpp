// Encrypting with a password and back, with gpg in a keyring made for the
// test (GNUPGHOME under the build tree): the user's keys are never touched.
#include "crypto.h"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace {
struct Outcome {
    bool ok = false;
    QString message, made;
};

template<typename Start>
Outcome wait(Start start)
{
    Outcome o;
    QEventLoop loop;
    start(&loop, [&](bool ok, const QString &message, const QString &made) {
        o = {ok, message, made};
        loop.quit();
    });
    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    loop.exec();
    return o;
}
}

class TestCrypto : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;

private slots:
    void initTestCase()
    {
        if (!Crypto::haveGpg()) QSKIP("gpg is not installed");
        const QString home = qEnvironmentVariable("GNUPGHOME");
        QVERIFY2(!home.isEmpty() && home.startsWith(QDir::homePath() + '/'), "GNUPGHOME must be the test's own");
        QVERIFY(QDir().mkpath(home));
        QFile::setPermissions(home, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    void cleanupTestCase() { QProcess::execute("gpgconf", {"--kill", "gpg-agent"}); }

    void fileRoundTrip()
    {
        const QString in = m_tmp.path() + "/secreto.txt";
        QFile f(in);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("lo que nadie debe leer");
        f.close();

        Crypto::EncryptJob job;
        job.paths = {in};
        job.out = in + ".gpg";
        job.passphrase = "correcta horse";
        Outcome o = wait([&](QObject *ctx, Crypto::Done done) { Crypto::encrypt(job, ctx, done); });
        QVERIFY2(o.ok, qPrintable(o.message));
        QVERIFY(QFile::exists(job.out));
        QVERIFY(Crypto::isEncrypted(job.out));
        QVERIFY(Crypto::needsPassphrase(job.out));
        QFile c(job.out);
        QVERIFY(c.open(QIODevice::ReadOnly));
        QVERIFY(!c.readAll().contains("nadie"));

        const QString out = m_tmp.path() + "/fuera";
        QVERIFY(QDir().mkpath(out));
        o = wait([&](QObject *ctx, Crypto::Done done) { Crypto::decrypt(job.out, out, "mala", ctx, done); });
        QVERIFY(!o.ok);
        QVERIFY(!QFile::exists(out + "/secreto.txt"));

        o = wait([&](QObject *ctx, Crypto::Done done) { Crypto::decrypt(job.out, out, "correcta horse", ctx, done); });
        QVERIFY2(o.ok, qPrintable(o.message));
        QFile p(out + "/secreto.txt");
        QVERIFY(p.open(QIODevice::ReadOnly));
        QCOMPARE(p.readAll(), QByteArray("lo que nadie debe leer"));
    }

    void folderRoundTrip()
    {
        const QString dir = m_tmp.path() + "/carpeta";
        QVERIFY(QDir().mkpath(dir + "/sub"));
        QFile f(dir + "/sub/nota");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("dentro");
        f.close();

        Crypto::EncryptJob job;
        job.paths = {dir};
        job.out = m_tmp.path() + "/carpeta.tar.gpg";
        job.passphrase = "otra";
        Outcome o = wait([&](QObject *ctx, Crypto::Done done) { Crypto::encrypt(job, ctx, done); });
        QVERIFY2(o.ok, qPrintable(o.message));

        const QString out = m_tmp.path() + "/vuelta";
        QVERIFY(QDir().mkpath(out));
        o = wait([&](QObject *ctx, Crypto::Done done) { Crypto::decrypt(job.out, out, "otra", ctx, done); });
        QVERIFY2(o.ok, qPrintable(o.message));
        QFile p(out + "/carpeta/sub/nota");
        QVERIFY(p.open(QIODevice::ReadOnly));
        QCOMPARE(p.readAll(), QByteArray("dentro"));
    }

    void passphraseNotInArgv()
    {
        // ps must never show it: start a slow encryption and look at gpg's command line.
        const QString in = m_tmp.path() + "/grande";
        QFile f(in);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(8 << 20, 'a'));
        f.close();
        Crypto::EncryptJob job;
        job.paths = {in};
        job.out = in + ".gpg";
        job.passphrase = "NO-DEBE-VERSE-1234";
        bool seen = false;
        int scans = 0;
        Outcome o = wait([&](QObject *ctx, Crypto::Done done) {
            auto *look = new QTimer(ctx);
            QObject::connect(look, &QTimer::timeout, ctx, [&] {
                ++scans;
                for (const QString &pid : QDir("/proc").entryList(QDir::Dirs)) {
                    QFile cmd("/proc/" + pid + "/cmdline");
                    if (cmd.open(QIODevice::ReadOnly) && cmd.readAll().contains("NO-DEBE-VERSE")) seen = true;
                }
            });
            look->start(2);
            Crypto::encrypt(job, ctx, done);
        });
        QVERIFY(o.ok);
        QVERIFY(scans > 0);
        QVERIFY(!seen);
    }
};

QTEST_GUILESS_MAIN(TestCrypto)
#include "tst_crypto.moc"
