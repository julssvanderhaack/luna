#include "i18n.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QLibraryInfo>
#include <QTranslator>
#include <QXmlStreamReader>

namespace {
QString g_current = QStringLiteral("es");

// Reads a .ts straight into a hash: one context, no plurals, so this is all
// QTranslator would do with the compiled .qm.
class TsTranslator : public QTranslator {
public:
    bool loadTs(const QString &file)
    {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) return false;
        QXmlStreamReader x(&f);
        QString source, translation;
        bool unfinished = false;
        while (!x.atEnd()) {
            x.readNext();
            if (x.isStartElement()) {
                if (x.name() == QLatin1String("message")) {
                    source.clear();
                    translation.clear();
                } else if (x.name() == QLatin1String("source")) {
                    source = x.readElementText();
                } else if (x.name() == QLatin1String("translation")) {
                    const QStringView type = x.attributes().value(QLatin1String("type"));
                    unfinished = type == QLatin1String("unfinished") || type == QLatin1String("obsolete") || type == QLatin1String("vanished");
                    translation = x.readElementText();
                }
            } else if (x.isEndElement() && x.name() == QLatin1String("message")) {
                if (!unfinished && !source.isEmpty() && !translation.isEmpty()) m_map.insert(source, translation);
            }
        }
        return !x.hasError() && !m_map.isEmpty();
    }
    QString translate(const char *, const char *source, const char *, int) const override
    {
        return m_map.value(QString::fromUtf8(source));
    }
    bool isEmpty() const override { return m_map.isEmpty(); }

private:
    QHash<QString, QString> m_map;
};

QString tsFile(const QString &code) { return QStringLiteral(":/i18n/luna_%1.ts").arg(code); }
}

QString i18n(const char *source)
{
    return QCoreApplication::translate("Luna", source);
}

namespace I18n {
QStringList available()
{
    QStringList out{QStringLiteral("es")};
    for (const QString &f : QDir(QStringLiteral(":/i18n")).entryList({QStringLiteral("luna_*.ts")}, QDir::Files, QDir::Name))
        out << f.mid(5, f.size() - 8);
    return out;
}

QString nativeName(const QString &code)
{
    // Qt names a bare "es" after its default country ("español de España")
    static const QHash<QString, QString> known = {{QStringLiteral("es"), QStringLiteral("Español")},
                                                  {QStringLiteral("en_US"), QStringLiteral("English (US)")}};
    if (auto it = known.constFind(code); it != known.constEnd()) return *it;
    const QLocale l(code);
    QString lang = l.nativeLanguageName();   // "American English" already says where
    if (code.contains('_') && lang == QLocale(code.section('_', 0, 0)).nativeLanguageName())
        lang += QStringLiteral(" (%1)").arg(l.nativeTerritoryName());
    if (!lang.isEmpty()) lang[0] = lang[0].toUpper();
    return lang;
}

QString systemLanguage()
{
    const QStringList have = available();
    // uiLanguages honours LANGUAGE, LC_ALL, LC_MESSAGES and LANG, best first.
    for (QString ui : QLocale::system().uiLanguages()) {
        ui.replace('-', '_');
        if (have.contains(ui)) return ui;
        const QString base = ui.section('_', 0, 0);
        if (have.contains(base)) return base;
        for (const QString &h : have)
            if (h.section('_', 0, 0) == base) return h;   // en_GB → en_US
    }
    return QStringLiteral("en_US");   // a language Luna lacks: English is the closest to everyone
}

void install(const QString &setting)
{
    const QStringList have = available();
    g_current = have.contains(setting) ? setting : systemLanguage();
    const QLocale loc(g_current);
    QLocale::setDefault(loc);

    // Qt's own texts (standard buttons, line-edit menu…), from qt6-translations.
    auto *qt = new QTranslator(qApp);
    if (qt->load(loc, QStringLiteral("qtbase"), QStringLiteral("_"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(qt);
    else
        delete qt;

    if (g_current == QLatin1String("es")) return;
    auto *t = new TsTranslator;
    t->setParent(qApp);
    if (t->loadTs(tsFile(g_current)))
        QCoreApplication::installTranslator(t);
    else
        delete t;
}

QString current() { return g_current; }

QLocale locale() { return QLocale(g_current); }
}
