#pragma once
#include <QLocale>
#include <QString>
#include <QStringList>

// Luna is written in Spanish: the source strings are the Spanish text and
// every other language is a Qt Linguist .ts file in i18n/ (embedded, read at
// start-up, so building needs no Qt tools). i18n/update.py collects the
// strings marked below into those files.

// Translated text. Takes the Spanish source as UTF-8.
QString i18n(const char *source);
// Marks a string for i18n/update.py without translating it (arrays of
// const char *): translate it where it is shown with i18n(ptr).
#define I18N_NOOP(s) s

namespace I18n {
// Languages Luna has: "es" (built in) and one per i18n/luna_<code>.ts.
QStringList available();
// Name of a language in itself ("Español", "English (United States)").
QString nativeName(const QString &code);
// The language picked for this system's UI languages ("" = system).
QString systemLanguage();
// Installs the translators for `setting` ("" follows the system). Call once,
// before any window is built: the strings are not retranslated live.
void install(const QString &setting);
// The language in use and its locale (dates, numbers, sorting).
QString current();
QLocale locale();
}
