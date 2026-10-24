// Colours from the active Omarchy theme (colors.toml).
#pragma once
#include <QColor>
#include <QString>
#include <QVector>

struct Theme {
    QColor bg, bgDark, bgLight, fg, fgDim, accent, selection, muted;
    QColor red, yellow, green, cyan, blue, magenta, orange;
    QColor ansi[16];   // terminal palette (the theme's ghostty.conf, else colors.toml)
    bool dark = true;
    QString mono, nerd;   // monospace UI font, Nerd Font for glyph icons

    static Theme &get();
    void load();
    QString styleSheet() const;
    void applyToApp() const;   // palette and style sheet of the whole application
};
