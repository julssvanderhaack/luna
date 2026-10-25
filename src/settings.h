// User preferences, persisted with QSettings (~/.config/luna/luna.conf).
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

enum class ViewMode { Icons, Compact, List, Details, Tree, Columns };

class Settings : public QObject {
    Q_OBJECT
public:
    static Settings &get();
    void load();
    void save() const;
    void changed();   // applies at once, writes to disk a moment later

    // Vista
    ViewMode viewMode = ViewMode::Icons;
    int iconZoom = 2;              // 0..5, see iconSizeFor()
    int listZoom = 1;              // 0..3
    bool showHidden = false;
    bool foldersFirst = true;
    int sortColumn = 0;            // FsModel::Column
    bool sortDescending = false;
    int dateFormat = 0;            // 0 relativo, 1 local corto, 2 ISO
    bool sizeIEC = false;          // KiB vs kB
    bool showItemCounts = true;    // folder size column shows number of items
    // Apariencia
    int iconStyle = 0;             // 0 iconos GTK, 1 glifos Omarchy
    QString iconTheme;             // empty = follow GTK (gsettings)
    bool useOmarchyColors = true;
    QString accentColor;           // empty = the Omarchy theme's accent
    double viewFontPt = 0;         // file views; 0 = system size
    // Barra lateral
    int sidebarIconSize = 16;      // px
    double sidebarFontPt = 0;      // 0 = one point below the system size
    int sidebarSpacing = 1;        // 0 compacto, 1 normal, 2 amplio
    bool sidebarRight = true;
    bool sidebarVisible = true;
    bool showPlaces = true, showBookmarks = true, showDevices = true;
    // Comportamiento
    bool singleClick = false;
    bool confirmTrash = false;
    bool confirmDelete = true;
    bool middleClickNewTab = true;
    bool smoothScroll = true;      // animate mouse-wheel steps
    int wheelSpeed = 100;          // % of Qt's 3 lines per notch
    int touchSpeed = 250;          // % of the finger's movement
    bool restoreSession = true;
    bool typeToSearch = true;      // typing filters (false: jumps to name)
    bool searchRecursiveDefault = false;
    int executableText = 0;        // 0 preguntar, 1 abrir, 2 ejecutar
    // Miniaturas
    bool thumbnails = true;
    int thumbMaxMB = 50;
    bool thumbVideos = true;
    bool thumbPdf = true;
    // Barra superior
    int toolbarStyle = 0;          // 0 solo iconos, 1 iconos y texto, 2 iconos juntos
    int toolbarIconSize = 1;       // 0 pequeño, 1 mediano, 2 grande
    bool tbForward = true, tbUp = true, tbSearch = true;   // pre-layout, migrated
    // Order of the header items, left to right: "|" starts a new group and a
    // leading "-" hides an item but keeps its place. "path" and "menu" are
    // always shown. See toolbarItems().
    QStringList toolbarLayout;
    static QStringList toolbarItems();                                // every item, in default order
    static QStringList normalizeToolbar(const QStringList &layout);  // valid, complete, tidy
    // Barra de ruta: false = botones (migas), true = texto editable
    bool pathbarText = false;
    // Terminal
    QString terminal = "xdg-terminal-exec";
    int terminalHeight = 200;      // px, the docked terminal (F4)
    bool terminalClickInserts = true;   // a click on a file types its name in the docked terminal
    // Sesión
    QStringList sessionTabs;
    bool startSplit = false;

    static int iconSizeFor(int zoom);
    static int listIconSizeFor(int zoom);

signals:
    void updated();

private:
    mutable bool m_savePending = false;
};
