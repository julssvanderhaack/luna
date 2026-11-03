// The English translation: complete, with every %N of the Spanish source, and
// actually used once installed.
#include "i18n.h"
#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTest>
#include <QXmlStreamReader>

namespace {
QSet<QString> placeholders(const QString &s)
{
    QSet<QString> out;
    static const QRegularExpression re("%[1-9]");
    for (auto it = re.globalMatch(s); it.hasNext();) out << it.next().captured();
    return out;
}
}

class TestI18n : public QObject {
    Q_OBJECT

private slots:
    void tsIsComplete()
    {
        QFile f(LUNA_TS);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QXmlStreamReader x(&f);
        QString source, translation;
        bool unfinished = false;
        int messages = 0;
        QStringList problems;
        while (!x.atEnd()) {
            x.readNext();
            if (x.isStartElement() && x.name() == u"source") source = x.readElementText();
            else if (x.isStartElement() && x.name() == u"translation") {
                unfinished = x.attributes().value("type") == u"unfinished";
                translation = x.readElementText();
            } else if (x.isEndElement() && x.name() == u"message") {
                ++messages;
                if (unfinished || translation.isEmpty()) problems << "untranslated: " + source;
                else if (placeholders(source) != placeholders(translation)) problems << "placeholders differ: " + source + " → " + translation;
                source.clear();
                translation.clear();
                unfinished = false;
            }
        }
        QVERIFY2(!x.hasError(), qPrintable(x.errorString()));
        QVERIFY(messages > 500);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void installEnglish()
    {
        QVERIFY(I18n::available().contains("en_US"));
        QCOMPARE(i18n("Abrir"), QString("Abrir"));   // before: the Spanish source
        I18n::install("en_US");
        QCOMPARE(I18n::current(), QString("en_US"));
        QCOMPARE(i18n("Abrir"), QString("Open"));
        QCOMPARE(i18n("Elige la aplicación para abrir «%1»").arg("x"), QString("Choose the application to open “x”"));
    }
};

QTEST_GUILESS_MAIN(TestI18n)
#include "tst_i18n.moc"
