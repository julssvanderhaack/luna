#include "settingsdialog.h"
#include "clip.h"
#include "i18n.h"
#include "icons.h"
#include "mainwindow.h"
#include "settings.h"
#include "theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>

namespace {
QCheckBox *check(const QString &text, bool &field)
{
    auto *c = new QCheckBox(text);
    c->setChecked(field);
    QObject::connect(c, &QCheckBox::toggled, c, [&field](bool on) {
        field = on;
        Settings::get().changed();
    });
    return c;
}

QComboBox *combo(const QStringList &items, int &field)
{
    auto *c = new QComboBox;
    c->addItems(items);
    c->setCurrentIndex(field);
    QObject::connect(c, &QComboBox::currentIndexChanged, c, [&field](int i) {
        field = i;
        Settings::get().changed();
    });
    return c;
}

// A speed in percent, applied while the slider moves.
QWidget *speed(int &field)
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    auto *sl = new QSlider(Qt::Horizontal);
    sl->setRange(1, 20);   // x 25 %: 25 % .. 500 %
    sl->setPageStep(2);
    sl->setValue(qRound(field / 25.0));
    auto *label = new QLabel;
    label->setMinimumWidth(label->fontMetrics().horizontalAdvance("500 %") + 4);
    auto show = [label](int pct) { label->setText(QString("%1 %").arg(pct)); };
    show(field);
    QObject::connect(sl, &QSlider::valueChanged, sl, [&field, show](int v) {
        field = v * 25;
        show(field);
        Settings::get().changed();
    });
    h->addWidget(sl, 1);
    h->addWidget(label);
    return w;
}

QLabel *note(const QString &t)
{
    auto *l = new QLabel(t);
    l->setProperty("role", "dim");
    l->setWordWrap(true);
    return l;
}

// A point size where 0 means "automatic" (shown as `autoText`).
QSpinBox *pointSize(double &field, const QString &autoText)
{
    auto *b = new QSpinBox;
    b->setRange(0, 24);
    b->setSpecialValueText(autoText);
    b->setSuffix(" pt");
    b->setValue(field > 0 ? qRound(field) : 0);
    QObject::connect(b, &QSpinBox::valueChanged, b, [&field](int v) {
        field = v < 6 ? 0 : v;   // 1..5 pt would be unreadable
        Settings::get().changed();
    });
    return b;
}

// Header buttons, left to right: drag to reorder, tick to show, and group
// separators to split them into groups (drawn joined with "Iconos juntos").
QWidget *toolbarEditor()
{
    struct Info { const char *name, *glyph; };
    static const QHash<QString, Info> info = {
        {"back", {I18N_NOOP("Atrás"), "back"}}, {"forward", {I18N_NOOP("Adelante"), "forward"}}, {"up", {I18N_NOOP("Carpeta superior"), "up"}},
        {"path", {I18N_NOOP("Barra de ruta"), "crumbs"}}, {"search", {I18N_NOOP("Buscar"), "search"}}, {"menu", {I18N_NOOP("Menú"), "menu"}},
    };
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    auto *list = new QListWidget;
    list->setDragDropMode(QAbstractItemView::InternalMove);
    list->setDefaultDropAction(Qt::MoveAction);
    list->setMinimumHeight(300);
    h->addWidget(list, 1);
    auto *buttons = new QVBoxLayout;
    h->addLayout(buttons);

    auto separator = [] {
        auto *it = new QListWidgetItem(i18n("────  nuevo grupo  ────"));
        it->setData(Qt::UserRole, QStringLiteral("|"));
        it->setTextAlignment(Qt::AlignCenter);
        it->setForeground(Theme::get().fgDim);
        it->setFlags(it->flags() & ~Qt::ItemIsUserCheckable);   // checkable by default
        it->setToolTip(i18n("Lo que hay debajo va en otro grupo"));
        return it;
    };
    auto fill = [=](const QStringList &layout) {
        const QSignalBlocker b(list);
        list->clear();
        for (const QString &t : layout) {
            if (t == "|") {
                list->addItem(separator());
                continue;
            }
            const QString name = t.startsWith('-') ? t.mid(1) : t;
            const Info i = info.value(name);
            auto *it = new QListWidgetItem(Icons::get().ui(i.glyph), i18n(i.name));
            it->setData(Qt::UserRole, name);
            if (name == "path" || name == "menu") {
                it->setFlags(it->flags() & ~Qt::ItemIsUserCheckable);
                it->setToolTip(i18n("Siempre visible; se puede mover"));
            } else {
                it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
                it->setCheckState(t.startsWith('-') ? Qt::Unchecked : Qt::Checked);
            }
            list->addItem(it);
        }
    };
    auto save = [=] {
        QStringList out;
        for (int r = 0; r < list->count(); ++r) {
            const QListWidgetItem *it = list->item(r);
            const QString t = it->data(Qt::UserRole).toString();
            // only items with a real check box can be hidden
            const bool hidden = it->data(Qt::CheckStateRole).isValid() && it->checkState() == Qt::Unchecked;
            out << (hidden ? '-' + t : t);
        }
        const QStringList tidy = Settings::normalizeToolbar(out);
        Settings::get().toolbarLayout = tidy;
        Settings::get().changed();
        if (tidy != out) {   // e.g. two separators in a row
            const int row = list->currentRow();
            fill(tidy);
            list->setCurrentRow(std::min(row, list->count() - 1));
        }
    };
    fill(Settings::get().toolbarLayout);
    QObject::connect(list->model(), &QAbstractItemModel::rowsMoved, list, [save] { save(); });
    QObject::connect(list, &QListWidget::itemChanged, list, [save] { save(); });

    auto button = [&](const QString &text, auto f) {
        auto *b = new QPushButton(text);
        QObject::connect(b, &QPushButton::clicked, list, f);
        buttons->addWidget(b);
        return b;
    };
    // Taking an item out and back in passes through states that are not a
    // valid layout: keep the list quiet and save once at the end.
    auto shift = [=](int by) {
        const int r = list->currentRow(), to = r + by;
        if (r < 0 || to < 0 || to >= list->count()) return;
        {
            const QSignalBlocker b(list);
            list->insertItem(to, list->takeItem(r));
            list->setCurrentRow(to);
        }
        save();
    };
    button(i18n("Subir"), [=] { shift(-1); });
    button(i18n("Bajar"), [=] { shift(1); });
    buttons->addSpacing(10);
    button(i18n("Separar grupo"), [=] {
        const int r = list->currentRow();
        {
            const QSignalBlocker b(list);
            list->insertItem(r < 0 ? list->count() : r + 1, separator());
        }
        save();
    });
    button(i18n("Quitar separación"), [=] {
        QListWidgetItem *it = list->currentItem();
        if (!it || it->data(Qt::UserRole).toString() != "|") return;
        delete it;
        save();
    });
    buttons->addSpacing(10);
    button(i18n("Restablecer"), [=] {
        fill(Settings::normalizeToolbar({"back", "forward", "up", "|", "path", "|", "search", "menu"}));
        save();
    });
    buttons->addStretch();
    return w;
}
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(i18n("Ajustes de Luna"));
    resize(780, 620);
    auto *h = new QHBoxLayout(this);
    m_nav = new QListWidget;
    m_nav->setObjectName("settingsNav");
    m_nav->setFixedWidth(190);
    m_nav->setIconSize(QSize(18, 18));
    m_pages = new QStackedWidget;
    h->addWidget(m_nav);
    h->addWidget(m_pages, 1);
    connect(m_nav, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    Settings &s = Settings::get();

    // ---- Vista
    QFormLayout *f = page(i18n("Vista"), "icons");
    auto *mode = new QComboBox;
    mode->addItems({i18n("Iconos"), i18n("Compacta"), i18n("Lista"), i18n("Lista detallada"), i18n("Árbol"), i18n("Columnas")});
    mode->setCurrentIndex(int(s.viewMode));
    connect(mode, &QComboBox::currentIndexChanged, this, [](int i) {
        Settings::get().viewMode = ViewMode(i);
        Settings::get().changed();
    });
    f->addRow(i18n("Vista predeterminada:"), mode);
    f->addRow(i18n("Tamaño de los iconos:"), combo({"32 px", "48 px", "64 px", "96 px", "128 px", "192 px"}, s.iconZoom));
    f->addRow(i18n("Tamaño en listas:"), combo({i18n("Muy pequeño"), i18n("Pequeño"), i18n("Mediano"), i18n("Grande")}, s.listZoom));
    f->addRow(i18n("Ordenar por:"), combo({i18n("Nombre"), i18n("Tamaño"), i18n("Tipo"), i18n("Fecha de modificación"), i18n("Permisos"), i18n("Propietario")}, s.sortColumn));
    f->addRow("", check(i18n("Orden descendente"), s.sortDescending));
    f->addRow("", check(i18n("Carpetas antes que archivos"), s.foldersFirst));
    f->addRow("", check(i18n("Mostrar archivos ocultos"), s.showHidden));
    f->addRow(i18n("Fechas:"), combo({i18n("Relativas (hoy, ayer…)"), i18n("Fecha y hora"), "ISO (2026-10-19 15:30)"}, s.dateFormat));
    auto *units = new QComboBox;
    units->addItems({i18n("Decimales (kB, MB, GB)"), i18n("Binarias (KiB, MiB, GiB)")});
    units->setCurrentIndex(s.sizeIEC ? 1 : 0);
    connect(units, &QComboBox::currentIndexChanged, this, [](int i) {
        Settings::get().sizeIEC = i == 1;
        Settings::get().changed();
    });
    f->addRow(i18n("Unidades de tamaño:"), units);
    f->addRow("", check(i18n("Mostrar cuántos elementos tiene cada carpeta"), s.showItemCounts));

    // ---- Apariencia
    f = page(i18n("Apariencia"), "star");
    auto *lang = new QComboBox;
    lang->addItem(i18n("Del sistema (%1)").arg(I18n::nativeName(I18n::systemLanguage())), QString());
    for (const QString &code : I18n::available()) lang->addItem(I18n::nativeName(code), code);
    lang->setCurrentIndex(std::max(0, lang->findData(s.language)));
    auto *restartRow = new QWidget;
    auto *rh = new QHBoxLayout(restartRow);
    rh->setContentsMargins(0, 0, 0, 0);
    rh->addWidget(note(i18n("El idioma cambia al reiniciar Luna.")), 1);
    auto *restart = new QPushButton(i18n("Reiniciar ahora"));
    rh->addWidget(restart);
    f->addRow(i18n("Idioma:"), lang);
    f->addRow("", restartRow);
    auto showRestart = [f, restartRow] {   // only once the row is in the form: no stray window
        const QString want = Settings::get().language;
        f->setRowVisible(restartRow, (I18n::available().contains(want) ? want : I18n::systemLanguage()) != I18n::current());
    };
    showRestart();
    connect(lang, &QComboBox::currentIndexChanged, this, [lang, showRestart](int) {
        Settings::get().language = lang->currentData().toString();
        Settings::get().changed();
        showRestart();
    });
    connect(restart, &QPushButton::clicked, this, [this] { restartLuna(); });
    auto *style = new QComboBox;
    style->addItems({i18n("Iconos del tema GTK"), i18n("Glifos de Omarchy (Nerd Font, colores del tema)")});
    style->setCurrentIndex(s.iconStyle);
    connect(style, &QComboBox::currentIndexChanged, this, [](int i) {
        Settings::get().iconStyle = i;
        Settings::get().changed();
    });
    f->addRow(i18n("Estilo de iconos:"), style);
    auto *theme = new QComboBox;
    theme->addItem(i18n("Seguir al sistema (%1)").arg(Icons::get().gtkTheme()), QString());
    for (const QString &t : Icons::installedThemes()) theme->addItem(t, t);
    theme->setCurrentIndex(std::max(0, theme->findData(s.iconTheme)));
    connect(theme, &QComboBox::currentIndexChanged, this, [theme](int) {
        Settings::get().iconTheme = theme->currentData().toString();
        Settings::get().changed();
    });
    f->addRow(i18n("Tema de iconos GTK:"), theme);
    auto *accentRow = new QWidget;
    auto *ah = new QHBoxLayout(accentRow);
    ah->setContentsMargins(0, 0, 0, 0);
    auto *pick = new QPushButton;
    auto *reset = new QPushButton(i18n("Usar el del tema"));
    auto showAccent = [pick, reset] {
        const QColor c = Theme::get().accent;
        pick->setText(Settings::get().accentColor.isEmpty() ? i18n("Del tema (%1)").arg(c.name()) : c.name());
        QPixmap swatch(16, 16);
        swatch.fill(c);
        pick->setIcon(QIcon(swatch));
        reset->setEnabled(!Settings::get().accentColor.isEmpty());
    };
    showAccent();
    connect(pick, &QPushButton::clicked, this, [this, showAccent] {
        const QColor c = QColorDialog::getColor(Theme::get().accent, this, i18n("Color de acento"));
        if (!c.isValid()) return;
        Settings::get().accentColor = c.name();
        Settings::get().changed();
        showAccent();
    });
    connect(reset, &QPushButton::clicked, this, [showAccent] {
        Settings::get().accentColor.clear();
        Settings::get().changed();
        showAccent();
    });
    ah->addWidget(pick, 1);
    ah->addWidget(reset);
    f->addRow(i18n("Color de acento:"), accentRow);
    f->addRow(i18n("Texto de las vistas:"), pointSize(s.viewFontPt, i18n("Como el sistema")));
    f->addRow("", note(i18n("«Seguir al sistema» usa el tema que pone Omarchy al cambiar de tema (gsettings). "
                                         "Los demás colores de la ventana salen del tema activo de Omarchy.")));

    // ---- Barra superior
    f = page(i18n("Barra superior"), "menu");
    f->addRow(i18n("Estilo:"), combo({i18n("Solo iconos"), i18n("Iconos y texto"), i18n("Iconos juntos (agrupados)")}, s.toolbarStyle));
    f->addRow(i18n("Tamaño de los iconos:"), combo({i18n("Pequeños"), i18n("Medianos"), i18n("Grandes")}, s.toolbarIconSize));
    f->addRow(toolbarEditor());
    f->addRow(note(i18n("De arriba abajo = de izquierda a derecha. Arrastra para mover, marca para mostrar y usa las "
                                     "separaciones para formar grupos (con el estilo «Iconos juntos» cada grupo va unido). "
                                     "Clic derecho sobre la barra para cambios rápidos.")));

    // ---- Barra lateral
    f = page(i18n("Barra lateral"), "sidebar");
    auto *side = new QComboBox;
    side->addItems({i18n("Izquierda"), i18n("Derecha")});
    side->setCurrentIndex(s.sidebarRight ? 1 : 0);
    connect(side, &QComboBox::currentIndexChanged, this, [](int i) {
        Settings::get().sidebarRight = i == 1;
        Settings::get().changed();
    });
    f->addRow(i18n("Posición:"), side);
    f->addRow("", check(i18n("Mostrar la barra lateral (F9)"), s.sidebarVisible));
    f->addRow(i18n("Tamaño del texto:"), pointSize(s.sidebarFontPt, i18n("Automático (algo menor)")));
    static const int iconSizes[] = {12, 14, 16, 18, 20, 24, 32};
    auto *sideIcons = new QComboBox;
    for (int px : iconSizes) sideIcons->addItem(QString("%1 px").arg(px), px);
    sideIcons->setCurrentIndex(std::max(0, sideIcons->findData(s.sidebarIconSize)));
    connect(sideIcons, &QComboBox::currentIndexChanged, this, [sideIcons](int) {
        Settings::get().sidebarIconSize = sideIcons->currentData().toInt();
        Settings::get().changed();
    });
    f->addRow(i18n("Tamaño de los iconos:"), sideIcons);
    f->addRow(i18n("Espaciado:"), combo({i18n("Compacto"), i18n("Normal"), i18n("Amplio")}, s.sidebarSpacing));
    f->addRow("", check(i18n("Lugares"), s.showPlaces));
    f->addRow("", check(i18n("Marcadores"), s.showBookmarks));
    f->addRow("", check(i18n("Dispositivos"), s.showDevices));
    f->addRow("", note(i18n("Los marcadores son los de GTK (~/.config/gtk-3.0/bookmarks): los mismos que ve Nautilus y la barra. "
                                         "Arrastra carpetas a la barra para añadirlas y arrástralos para reordenarlos.")));

    // ---- Comportamiento
    f = page(i18n("Comportamiento"), "settings");
    f->addRow("", check(i18n("Abrir con un solo clic"), s.singleClick));
    f->addRow("", check(i18n("Clic central abre carpetas en pestaña nueva"), s.middleClickNewTab));
    f->addRow("", check(i18n("Desplazamiento suave con la rueda del ratón"), s.smoothScroll));
    f->addRow(i18n("Velocidad de la rueda:"), speed(s.wheelSpeed));
    f->addRow(i18n("Velocidad del touchpad:"), speed(s.touchSpeed));
    f->addRow("", check(i18n("Escribir filtra la carpeta (si no, salta al nombre)"), s.typeToSearch));
    f->addRow("", check(i18n("Al buscar, incluir subcarpetas por defecto"), s.searchRecursiveDefault));
    f->addRow("", check(i18n("Preguntar antes de mover a la papelera"), s.confirmTrash));
    f->addRow("", check(i18n("Preguntar antes de eliminar definitivamente"), s.confirmDelete));
    f->addRow("", check(i18n("Recordar las pestañas al cerrar"), s.restoreSession));
    if (AudioClip::available())
        f->addRow("", check(i18n("En la vista previa de una canción, escuchar 30 segundos de la mitad"), s.previewAudio));
    f->addRow(i18n("Programas y scripts:"), combo({i18n("Preguntar qué hacer"), i18n("Abrirlos como texto"), i18n("Ejecutarlos")}, s.executableText));

    // ---- Miniaturas
    f = page(i18n("Miniaturas"), "icons");
    f->addRow("", check(i18n("Mostrar miniaturas"), s.thumbnails));
    f->addRow("", check(i18n("De vídeos y carátulas de música (ffmpegthumbnailer)"), s.thumbVideos));
    f->addRow("", check(i18n("De documentos PDF"), s.thumbPdf));
    auto *mb = new QSpinBox;
    mb->setRange(1, 2000);
    mb->setSuffix(" MB");
    mb->setValue(s.thumbMaxMB);
    connect(mb, &QSpinBox::valueChanged, this, [](int v) {
        Settings::get().thumbMaxMB = v;
        Settings::get().changed();
    });
    f->addRow(i18n("Tamaño máximo de imagen:"), mb);
    f->addRow("", note(i18n("Las miniaturas se guardan en ~/.cache/thumbnails (compartidas con Nautilus) y se generan con "
                                         "dos hilos como máximo para no cargar la CPU.")));

    // ---- Terminal
    f = page("Terminal", "terminal");
    auto *term = new QLineEdit(s.terminal);
    connect(term, &QLineEdit::editingFinished, this, [term] {
        Settings::get().terminal = term->text().trimmed();
        Settings::get().changed();
    });
    f->addRow(i18n("Orden:"), term);
    f->addRow("", note(i18n("Terminal aparte, en la carpeta actual (Mayús+F4). Por defecto xdg-terminal-exec, la terminal de Omarchy.")));
    f->addRow("", check(i18n("Con el terminal integrado abierto (F4), un clic en un archivo escribe su nombre"), s.terminalClickInserts));
    f->addRow("", note(i18n("Solo mientras el terminal espera órdenes. Arrastrar archivos al terminal escribe sus rutas completas.")));

    m_nav->setCurrentRow(0);
}

// Starts a new Luna on the same folders and closes this one: the texts are
// built once, so a new language needs a fresh process.
void SettingsDialog::restartLuna()
{
    QStringList args;
    if (!Settings::get().restoreSession)   // otherwise closing the windows saves the session (split view included)
        if (auto *mw = qobject_cast<MainWindow *>(parentWidget() ? parentWidget()->window() : nullptr)) args = mw->tabPaths();
    accept();
    // after exec() has returned: the window that opened this dialog goes away
    QTimer::singleShot(0, qApp, [args] {
        for (QWidget *w : QApplication::topLevelWidgets())
            if (qobject_cast<MainWindow *>(w)) w->close();
        Settings::get().save();
        if (QProcess::startDetached(QCoreApplication::applicationFilePath(), args)) qApp->quit();
    });
}

void SettingsDialog::showPage(const QString &title)
{
    for (int i = 0; i < m_nav->count(); ++i)
        if (m_nav->item(i)->text() == title) m_nav->setCurrentRow(i);
}

QFormLayout *SettingsDialog::page(const QString &title, const QString &glyph)
{
    m_nav->addItem(new QListWidgetItem(Icons::get().ui(glyph), title));
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *t = new QLabel("<h2>" + title + "</h2>");
    v->addWidget(t);
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setVerticalSpacing(10);
    v->addLayout(form);
    v->addStretch();
    m_pages->addWidget(w);
    return form;
}
