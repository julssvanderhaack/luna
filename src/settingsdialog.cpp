#include "settingsdialog.h"
#include "icons.h"
#include "settings.h"
#include "theme.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>

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
        {"back", {"Atrás", "back"}}, {"forward", {"Adelante", "forward"}}, {"up", {"Carpeta superior", "up"}},
        {"path", {"Barra de ruta", "crumbs"}}, {"search", {"Buscar", "search"}}, {"menu", {"Menú", "menu"}},
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
        auto *it = new QListWidgetItem(QString::fromUtf8("────  nuevo grupo  ────"));
        it->setData(Qt::UserRole, QStringLiteral("|"));
        it->setTextAlignment(Qt::AlignCenter);
        it->setForeground(Theme::get().fgDim);
        it->setFlags(it->flags() & ~Qt::ItemIsUserCheckable);   // checkable by default
        it->setToolTip(QString::fromUtf8("Lo que hay debajo va en otro grupo"));
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
            auto *it = new QListWidgetItem(Icons::get().ui(i.glyph), QString::fromUtf8(i.name));
            it->setData(Qt::UserRole, name);
            if (name == "path" || name == "menu") {
                it->setFlags(it->flags() & ~Qt::ItemIsUserCheckable);
                it->setToolTip(QString::fromUtf8("Siempre visible; se puede mover"));
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
    button(QString::fromUtf8("Subir"), [=] { shift(-1); });
    button(QString::fromUtf8("Bajar"), [=] { shift(1); });
    buttons->addSpacing(10);
    button(QString::fromUtf8("Separar grupo"), [=] {
        const int r = list->currentRow();
        {
            const QSignalBlocker b(list);
            list->insertItem(r < 0 ? list->count() : r + 1, separator());
        }
        save();
    });
    button(QString::fromUtf8("Quitar separación"), [=] {
        QListWidgetItem *it = list->currentItem();
        if (!it || it->data(Qt::UserRole).toString() != "|") return;
        delete it;
        save();
    });
    buttons->addSpacing(10);
    button("Restablecer", [=] {
        fill(Settings::normalizeToolbar({"back", "forward", "up", "|", "path", "|", "search", "menu"}));
        save();
    });
    buttons->addStretch();
    return w;
}
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Ajustes de Luna");
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
    QFormLayout *f = page("Vista", "icons");
    auto *mode = new QComboBox;
    mode->addItems({"Iconos", "Compacta", "Lista", "Lista detallada", QString::fromUtf8("Árbol"), "Columnas"});
    mode->setCurrentIndex(int(s.viewMode));
    connect(mode, &QComboBox::currentIndexChanged, this, [](int i) {
        Settings::get().viewMode = ViewMode(i);
        Settings::get().changed();
    });
    f->addRow("Vista predeterminada:", mode);
    f->addRow(QString::fromUtf8("Tamaño de los iconos:"), combo({"32 px", "48 px", "64 px", "96 px", "128 px", "192 px"}, s.iconZoom));
    f->addRow(QString::fromUtf8("Tamaño en listas:"), combo({"Muy pequeño", QString::fromUtf8("Pequeño"), "Mediano", "Grande"}, s.listZoom));
    f->addRow("Ordenar por:", combo({"Nombre", QString::fromUtf8("Tamaño"), "Tipo", QString::fromUtf8("Fecha de modificación"), "Permisos", "Propietario"}, s.sortColumn));
    f->addRow("", check("Orden descendente", s.sortDescending));
    f->addRow("", check("Carpetas antes que archivos", s.foldersFirst));
    f->addRow("", check("Mostrar archivos ocultos", s.showHidden));
    f->addRow("Fechas:", combo({"Relativas (hoy, ayer…)", "Fecha y hora", "ISO (2026-10-19 15:30)"}, s.dateFormat));
    auto *units = new QComboBox;
    units->addItems({QString::fromUtf8("Decimales (kB, MB, GB)"), QString::fromUtf8("Binarias (KiB, MiB, GiB)")});
    units->setCurrentIndex(s.sizeIEC ? 1 : 0);
    connect(units, &QComboBox::currentIndexChanged, this, [](int i) {
        Settings::get().sizeIEC = i == 1;
        Settings::get().changed();
    });
    f->addRow(QString::fromUtf8("Unidades de tamaño:"), units);
    f->addRow("", check(QString::fromUtf8("Mostrar cuántos elementos tiene cada carpeta"), s.showItemCounts));

    // ---- Apariencia
    f = page("Apariencia", "star");
    auto *style = new QComboBox;
    style->addItems({QString::fromUtf8("Iconos del tema GTK"), QString::fromUtf8("Glifos de Omarchy (Nerd Font, colores del tema)")});
    style->setCurrentIndex(s.iconStyle);
    connect(style, &QComboBox::currentIndexChanged, this, [](int i) {
        Settings::get().iconStyle = i;
        Settings::get().changed();
    });
    f->addRow("Estilo de iconos:", style);
    auto *theme = new QComboBox;
    theme->addItem(QString::fromUtf8("Seguir al sistema (%1)").arg(Icons::get().gtkTheme()), QString());
    for (const QString &t : Icons::installedThemes()) theme->addItem(t, t);
    theme->setCurrentIndex(std::max(0, theme->findData(s.iconTheme)));
    connect(theme, &QComboBox::currentIndexChanged, this, [theme](int) {
        Settings::get().iconTheme = theme->currentData().toString();
        Settings::get().changed();
    });
    f->addRow("Tema de iconos GTK:", theme);
    auto *accentRow = new QWidget;
    auto *ah = new QHBoxLayout(accentRow);
    ah->setContentsMargins(0, 0, 0, 0);
    auto *pick = new QPushButton;
    auto *reset = new QPushButton(QString::fromUtf8("Usar el del tema"));
    auto showAccent = [pick, reset] {
        const QColor c = Theme::get().accent;
        pick->setText(Settings::get().accentColor.isEmpty() ? QString::fromUtf8("Del tema (%1)").arg(c.name()) : c.name());
        QPixmap swatch(16, 16);
        swatch.fill(c);
        pick->setIcon(QIcon(swatch));
        reset->setEnabled(!Settings::get().accentColor.isEmpty());
    };
    showAccent();
    connect(pick, &QPushButton::clicked, this, [this, showAccent] {
        const QColor c = QColorDialog::getColor(Theme::get().accent, this, QString::fromUtf8("Color de acento"));
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
    f->addRow("Color de acento:", accentRow);
    f->addRow(QString::fromUtf8("Texto de las vistas:"), pointSize(s.viewFontPt, QString::fromUtf8("Como el sistema")));
    f->addRow("", note(QString::fromUtf8("«Seguir al sistema» usa el tema que pone Omarchy al cambiar de tema (gsettings). "
                                         "Los demás colores de la ventana salen del tema activo de Omarchy.")));

    // ---- Barra superior
    f = page("Barra superior", "menu");
    f->addRow("Estilo:", combo({"Solo iconos", "Iconos y texto", "Iconos juntos (agrupados)"}, s.toolbarStyle));
    f->addRow(QString::fromUtf8("Tamaño de los iconos:"), combo({QString::fromUtf8("Pequeños"), "Medianos", "Grandes"}, s.toolbarIconSize));
    f->addRow(toolbarEditor());
    f->addRow(note(QString::fromUtf8("De arriba abajo = de izquierda a derecha. Arrastra para mover, marca para mostrar y usa las "
                                     "separaciones para formar grupos (con el estilo «Iconos juntos» cada grupo va unido). "
                                     "Clic derecho sobre la barra para cambios rápidos.")));

    // ---- Barra lateral
    f = page("Barra lateral", "sidebar");
    auto *side = new QComboBox;
    side->addItems({"Izquierda", "Derecha"});
    side->setCurrentIndex(s.sidebarRight ? 1 : 0);
    connect(side, &QComboBox::currentIndexChanged, this, [](int i) {
        Settings::get().sidebarRight = i == 1;
        Settings::get().changed();
    });
    f->addRow(QString::fromUtf8("Posición:"), side);
    f->addRow("", check("Mostrar la barra lateral (F9)", s.sidebarVisible));
    f->addRow(QString::fromUtf8("Tamaño del texto:"), pointSize(s.sidebarFontPt, QString::fromUtf8("Automático (algo menor)")));
    static const int iconSizes[] = {12, 14, 16, 18, 20, 24, 32};
    auto *sideIcons = new QComboBox;
    for (int px : iconSizes) sideIcons->addItem(QString("%1 px").arg(px), px);
    sideIcons->setCurrentIndex(std::max(0, sideIcons->findData(s.sidebarIconSize)));
    connect(sideIcons, &QComboBox::currentIndexChanged, this, [sideIcons](int) {
        Settings::get().sidebarIconSize = sideIcons->currentData().toInt();
        Settings::get().changed();
    });
    f->addRow(QString::fromUtf8("Tamaño de los iconos:"), sideIcons);
    f->addRow("Espaciado:", combo({"Compacto", "Normal", "Amplio"}, s.sidebarSpacing));
    f->addRow("", check("Lugares", s.showPlaces));
    f->addRow("", check("Marcadores", s.showBookmarks));
    f->addRow("", check("Dispositivos", s.showDevices));
    f->addRow("", note(QString::fromUtf8("Los marcadores son los de GTK (~/.config/gtk-3.0/bookmarks): los mismos que ve Nautilus y la barra. "
                                         "Arrastra carpetas a la barra para añadirlas y arrástralos para reordenarlos.")));

    // ---- Comportamiento
    f = page("Comportamiento", "settings");
    f->addRow("", check("Abrir con un solo clic", s.singleClick));
    f->addRow("", check(QString::fromUtf8("Clic central abre carpetas en pestaña nueva"), s.middleClickNewTab));
    f->addRow("", check(QString::fromUtf8("Desplazamiento suave con la rueda del ratón"), s.smoothScroll));
    f->addRow(QString::fromUtf8("Velocidad de la rueda:"), speed(s.wheelSpeed));
    f->addRow("Velocidad del touchpad:", speed(s.touchSpeed));
    f->addRow("", check(QString::fromUtf8("Escribir filtra la carpeta (si no, salta al nombre)"), s.typeToSearch));
    f->addRow("", check(QString::fromUtf8("Al buscar, incluir subcarpetas por defecto"), s.searchRecursiveDefault));
    f->addRow("", check(QString::fromUtf8("Preguntar antes de mover a la papelera"), s.confirmTrash));
    f->addRow("", check(QString::fromUtf8("Preguntar antes de eliminar definitivamente"), s.confirmDelete));
    f->addRow("", check(QString::fromUtf8("Recordar las pestañas al cerrar"), s.restoreSession));
    f->addRow("Programas y scripts:", combo({QString::fromUtf8("Preguntar qué hacer"), "Abrirlos como texto", "Ejecutarlos"}, s.executableText));

    // ---- Miniaturas
    f = page("Miniaturas", "icons");
    f->addRow("", check(QString::fromUtf8("Mostrar miniaturas"), s.thumbnails));
    f->addRow("", check(QString::fromUtf8("De vídeos y carátulas de música (ffmpegthumbnailer)"), s.thumbVideos));
    f->addRow("", check("De documentos PDF", s.thumbPdf));
    auto *mb = new QSpinBox;
    mb->setRange(1, 2000);
    mb->setSuffix(" MB");
    mb->setValue(s.thumbMaxMB);
    connect(mb, &QSpinBox::valueChanged, this, [](int v) {
        Settings::get().thumbMaxMB = v;
        Settings::get().changed();
    });
    f->addRow(QString::fromUtf8("Tamaño máximo de imagen:"), mb);
    f->addRow("", note(QString::fromUtf8("Las miniaturas se guardan en ~/.cache/thumbnails (compartidas con Nautilus) y se generan con "
                                         "dos hilos como máximo para no cargar la CPU.")));

    // ---- Terminal
    f = page("Terminal", "terminal");
    auto *term = new QLineEdit(s.terminal);
    connect(term, &QLineEdit::editingFinished, this, [term] {
        Settings::get().terminal = term->text().trimmed();
        Settings::get().changed();
    });
    f->addRow("Orden:", term);
    f->addRow("", note(QString::fromUtf8("Terminal aparte, en la carpeta actual (Mayús+F4). Por defecto xdg-terminal-exec, la terminal de Omarchy.")));
    f->addRow("", check(QString::fromUtf8("Con el terminal integrado abierto (F4), un clic en un archivo escribe su nombre"), s.terminalClickInserts));
    f->addRow("", note(QString::fromUtf8("Solo mientras el terminal espera órdenes. Arrastrar archivos al terminal escribe sus rutas completas.")));

    m_nav->setCurrentRow(0);
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
