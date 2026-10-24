#include "theme.h"
#include "settings.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QHash>
#include <QPalette>
#include <QRegularExpression>
#include <QTextStream>

Theme &Theme::get()
{
    static Theme t;
    return t;
}

static QHash<QString, QString> readColors()
{
    QHash<QString, QString> c;
    for (const QString &p : {QDir::homePath() + "/.local/state/omarchy/current/theme/colors.toml",
                             QDir::homePath() + "/.config/omarchy/current/theme/colors.toml"}) {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) continue;
        static const QRegularExpression re("^\\s*([A-Za-z_]+)\\s*=\\s*\"([^\"]*)\"");
        QTextStream in(&f);
        while (!in.atEnd()) {
            auto m = re.match(in.readLine());
            if (m.hasMatch()) c.insert(m.captured(1), m.captured(2));
        }
        break;
    }
    return c;
}

void Theme::load()
{
    auto c = readColors();
    auto col = [&](const char *k, const QString &fallback) {
        QColor q(c.value(k, fallback));
        return q.isValid() ? q : QColor(fallback);
    };
    bg = col("background", "#1a1b26");
    bgDark = col("dark_background", bg.darker(125).name());
    bgLight = col("lighter_background", bg.lighter(140).name());
    fg = col("foreground", "#c0caf5");
    fgDim = col("dark_foreground", "#7a7f9a");
    accent = col("accent", "#7aa2f7");
    selection = col("selection", bgLight.name());
    muted = col("muted", bgLight.lighter(130).name());
    red = col("red", "#f7768e");
    yellow = col("yellow", "#e0af68");
    green = col("green", "#9ece6a");
    cyan = col("cyan", "#7dcfff");
    blue = col("blue", "#7aa2f7");
    magenta = col("magenta", "#bb9af7");
    orange = col("orange", "#ff9e64");
    // Terminal palette: the themes ship it complete for ghostty; colors.toml
    // lacks black/white, so it is only the fallback.
    const QColor base[8] = {bgDark, red, green, yellow, blue, magenta, cyan, fg};
    const char *bright[8] = {"", "bright_red", "bright_green", "bright_yellow", "bright_blue", "bright_magenta", "bright_cyan", ""};
    for (int i = 0; i < 8; ++i) {
        ansi[i] = base[i];
        ansi[i + 8] = i == 0 ? muted : i == 7 ? col("bright_foreground", fg.name()) : col(bright[i], base[i].name());
    }
    for (const QString &p : {QDir::homePath() + "/.local/state/omarchy/current/theme/ghostty.conf",
                             QDir::homePath() + "/.config/omarchy/current/theme/ghostty.conf"}) {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) continue;
        static const QRegularExpression re("^\\s*palette\\s*=\\s*(\\d+)\\s*=\\s*(#[0-9A-Fa-f]{6})");
        QTextStream in(&f);
        while (!in.atEnd())
            if (auto m = re.match(in.readLine()); m.hasMatch() && m.captured(1).toInt() < 16) ansi[m.captured(1).toInt()] = QColor(m.captured(2));
        break;
    }
    if (const QColor own(Settings::get().accentColor); own.isValid()) accent = own;   // the user's choice wins
    dark = bg.lightness() < 128;

    mono = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    const QStringList fams = QFontDatabase::families();
    for (const QString &f : fams)
        if (f.contains("Nerd Font") && !f.contains("Propo")) {
            nerd = f;
            if (f.contains("Mono")) break;
        }
}

void Theme::applyToApp() const
{
    QPalette pal;
    pal.setColor(QPalette::Window, bgDark);
    pal.setColor(QPalette::WindowText, fg);
    pal.setColor(QPalette::Base, bg);
    pal.setColor(QPalette::AlternateBase, bgLight);
    pal.setColor(QPalette::Text, fg);
    pal.setColor(QPalette::Button, bgLight);
    pal.setColor(QPalette::ButtonText, fg);
    pal.setColor(QPalette::Highlight, accent);
    pal.setColor(QPalette::HighlightedText, bg);
    pal.setColor(QPalette::PlaceholderText, fgDim);
    pal.setColor(QPalette::ToolTipBase, bgLight);
    pal.setColor(QPalette::ToolTipText, fg);
    pal.setColor(QPalette::Link, accent);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, fgDim);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, fgDim);
    QApplication::setPalette(pal);
    qApp->setStyleSheet(styleSheet());
}

QString Theme::styleSheet() const
{
    auto n = [](const QColor &c) { return c.name(QColor::HexArgb); };
    QColor hover = bgLight.lighter(dark ? 125 : 95);
    QColor accentText = accent.lightness() > 140 ? bg : fg;
    QString s = R"(
QWidget { background: %BGD%; color: %FG%; }
QToolTip { background: %BGL%; color: %FG%; border: 1px solid %MUTED%; padding: 4px; }
QLineEdit, QPlainTextEdit, QSpinBox, QComboBox {
  background: %BG%; border: 1px solid %MUTED%; border-radius: 3px; padding: 4px 7px;
  selection-background-color: %SEL%; selection-color: %FG%;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: %ACC%; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: %BGL%; border: 1px solid %MUTED%; selection-background-color: %SEL%; }
QPushButton, QToolButton { background: transparent; border: 1px solid transparent; border-radius: 3px; padding: 4px 8px; }
QPushButton { background: %BGL%; }
QPushButton:hover, QToolButton:hover { background: %HOVER%; }
QPushButton:pressed, QToolButton:pressed, QToolButton:checked { background: %SEL%; }
QPushButton:default, QPushButton[role="primary"] { background: %ACC%; color: %ACCT%; font-weight: bold; }
QToolButton::menu-indicator { image: none; width: 0; }
QToolButton[role="crumb"] { padding: 3px 5px; }
QToolButton[role="crumb"]:checked { background: transparent; color: %ACC%; font-weight: bold; }
QAbstractItemView {
  background: %BG%; border: none; outline: none;
  selection-background-color: %SELA%; selection-color: %FG%; alternate-background-color: %ALT%;
}
QAbstractItemView::item { border-radius: 2px; padding: 1px; }
QAbstractItemView::item:hover { background: %HOVERA%; }
QAbstractItemView::item:selected { background: %SELA%; color: %FG%; }
QAbstractItemView::item:selected:!active { background: %SELI%; }
QHeaderView::section { background: %BG%; color: %DIM%; border: none; border-bottom: 1px solid %MUTED%; padding: 4px 6px; }
QTabBar { font-family: "%UIFONT%"; }
QTabBar::tab { background: transparent; color: %DIM%; padding: 6px 14px; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: %FG%; border-bottom: 2px solid %ACC%; }
QTabBar::tab:hover { color: %FG%; }
QTabWidget::pane { border: none; }
QSplitter::handle { background: %BGD%; }
QScrollBar:vertical { background: transparent; width: 6px; margin: 1px; }
QScrollBar::handle:vertical { background: %MUTED%; border-radius: 1px; min-height: 28px; }
QScrollBar:horizontal { background: transparent; height: 6px; margin: 1px; }
QScrollBar::handle:horizontal { background: %MUTED%; border-radius: 1px; min-width: 28px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QMenu { background: %BGL%; border: 1px solid %MUTED%; padding: 4px; }
QMenu::item { padding: 5px 22px 5px 10px; border-radius: 2px; }
QMenu::item:selected { background: %SEL%; }
QMenu::separator { height: 1px; background: %MUTED%; margin: 4px 6px; }
QMenu::icon { padding-left: 6px; }
QLabel[role="dim"] { color: %DIM%; }
QLabel[role="section"] { color: %DIM%; font-weight: bold; }
QFrame[role="pane"] { border: none; }
QWidget#paneTitle { background: %BG%; border-bottom: 2px solid transparent; }
QWidget#paneTitle[active="true"] { border-bottom: 2px solid %ACC%; }
QWidget#pathBar { background: %BG%; border: 1px solid %MUTED%; border-radius: 3px; }
QWidget#pathBar QLineEdit { border: none; background: transparent; padding: 2px 4px; }
QWidget#pathBar QStackedWidget, QWidget#pathBar QStackedWidget > QWidget { background: transparent; }
QFrame#toolGroup { background: %BG%; border: 1px solid %MUTED%; border-radius: 3px; }
QFrame#toolGroup QToolButton { border-radius: 0; border: none; }
QSplitter#panes::handle { background: %MUTED%; width: 1px; }
QStatusBar { color: %DIM%; }
QStatusBar QLabel { color: %DIM%; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid %MUTED%; border-radius: 2px; background: %BG%; }
QCheckBox::indicator:checked { background: %ACC%; border-color: %ACC%; }
QProgressBar { background: %BG%; border: none; border-radius: 1px; height: 6px; text-align: center; }
QProgressBar::chunk { background: %ACC%; border-radius: 1px; }
QListWidget#settingsNav::item { padding: 8px 10px; }
)";
    QColor selA = accent;
    selA.setAlpha(70);
    QColor selI = accent;
    selI.setAlpha(40);
    QColor hovA = fg;
    hovA.setAlpha(18);
    QColor alt = bgLight;
    alt.setAlpha(60);
    s.replace("%BGD%", n(bgDark)).replace("%BGL%", n(bgLight)).replace("%BG%", n(bg));
    s.replace("%FG%", n(fg)).replace("%DIM%", n(fgDim)).replace("%MUTED%", n(muted));
    s.replace("%SELA%", n(selA)).replace("%SELI%", n(selI)).replace("%SEL%", n(selection));
    s.replace("%ACCT%", n(accentText)).replace("%ACC%", n(accent));
    s.replace("%UIFONT%", QFontDatabase::systemFont(QFontDatabase::GeneralFont).family());
    s.replace("%HOVERA%", n(hovA)).replace("%HOVER%", n(hover)).replace("%ALT%", n(alt));
    return s;
}
