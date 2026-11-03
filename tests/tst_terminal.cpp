// Paths sent to the docked terminal's shell are quoted so the shell reads
// them as one word and runs nothing inside them.
#include "terminal.h"
#include <QProcess>
#include <QTest>

class TestTerminal : public QObject {
    Q_OBJECT

private slots:
    void shellQuote_data()
    {
        QTest::addColumn<QString>("path");
        QTest::newRow("plain") << "/home/ana/docs";
        QTest::newRow("spaces") << "/home/ana/mis cosas";
        QTest::newRow("quote") << "/tmp/it's";
        QTest::newRow("command") << "/tmp/$(touch pwned)`id`";
        QTest::newRow("newline") << "/tmp/a\nrm -rf b";
        QTest::newRow("glob") << "/tmp/*;?[x]";
        QTest::newRow("dash") << "-rf";
        QTest::newRow("unicode") << "/tmp/ñandú «x»";
    }

    void shellQuote()
    {
        QFETCH(QString, path);
        // sh must give back exactly the same bytes, as a single argument.
        QProcess sh;
        sh.start("sh", {"-c", "printf '%s' " + Terminal::shellQuote(path)});
        QVERIFY(sh.waitForFinished());
        QCOMPARE(QString::fromUtf8(sh.readAllStandardOutput()), path);
    }

    void plainStaysReadable() { QCOMPARE(Terminal::shellQuote("/home/ana/docs"), QString("/home/ana/docs")); }
};

QTEST_GUILESS_MAIN(TestTerminal)
#include "tst_terminal.moc"
