// Development hooks driven by LUNA_* environment variables: scripted
// screenshots (LUNA_SHOTS, LUNA_TOUR), benchmarks (LUNA_THUMBTEST,
// LUNA_SEARCHTEST, LUNA_LISTTEST) and end-to-end checks (LUNA_DNDTEST,
// LUNA_GPGTEST, LUNA_GITTEST, LUNA_PREVIEWTEST…). Some of them act on real
// files or keyrings, so they are compiled only with -DLUNA_TEST_HOOKS=ON and
// never in the normal or release builds.
#pragma once
#include <QElapsedTimer>
#include <QStringList>

class MainWindow;

namespace TestHooks {
void beforeWindow(QStringList &paths);   // may drop paths that are not tabs
void install(MainWindow *w, const QStringList &paths, QElapsedTimer startup);
}
