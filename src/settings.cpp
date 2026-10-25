#include "settings.h"
#include <algorithm>
#include <QCoreApplication>
#include <QSet>
#include <QSettings>
#include <QTimer>

Settings &Settings::get()
{
    static Settings s;
    return s;
}

int Settings::iconSizeFor(int zoom)
{
    static const int sizes[] = {32, 48, 64, 96, 128, 192};
    return sizes[std::clamp(zoom, 0, 5)];
}

int Settings::listIconSizeFor(int zoom)
{
    static const int sizes[] = {16, 22, 32, 48};
    return sizes[std::clamp(zoom, 0, 3)];
}

QStringList Settings::toolbarItems()
{
    return {"back", "forward", "up", "path", "search", "menu"};
}

QStringList Settings::normalizeToolbar(const QStringList &layout)
{
    const QStringList items = toolbarItems();
    QStringList out;
    QSet<QString> seen;
    for (QString t : layout) {
        if (t == "|") {
            if (!out.isEmpty() && out.last() != "|") out << t;
            continue;
        }
        const QString name = t.startsWith('-') ? t.mid(1) : t;
        if (!items.contains(name) || seen.contains(name)) continue;
        seen.insert(name);
        if (name == "path" || name == "menu") t = name;   // never hidden
        out << t;
    }
    for (const QString &name : items)   // anything missing goes at the end, hidden
        if (!seen.contains(name)) out << (name == "path" || name == "menu" ? name : '-' + name);
    while (!out.isEmpty() && out.last() == "|") out.removeLast();
    return out;
}

void Settings::changed()
{
    // Zooming with the wheel changes settings many times a second: write once.
    if (!m_savePending) {
        m_savePending = true;
        QTimer::singleShot(500, this, [this] {
            if (m_savePending) save();
        });
        static bool hooked = false;
        if (!hooked) {
            hooked = true;
            connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
                if (m_savePending) save();
            });
        }
    }
    emit updated();
}

void Settings::load()
{
    QSettings q("luna", "luna");
    viewMode = ViewMode(q.value("view/mode", int(viewMode)).toInt());
    iconZoom = q.value("view/iconZoom", iconZoom).toInt();
    listZoom = q.value("view/listZoom", listZoom).toInt();
    showHidden = q.value("view/showHidden", showHidden).toBool();
    foldersFirst = q.value("view/foldersFirst", foldersFirst).toBool();
    sortColumn = q.value("view/sortColumn", sortColumn).toInt();
    sortDescending = q.value("view/sortDescending", sortDescending).toBool();
    dateFormat = q.value("view/dateFormat", dateFormat).toInt();
    sizeIEC = q.value("view/sizeIEC", sizeIEC).toBool();
    showItemCounts = q.value("view/showItemCounts", showItemCounts).toBool();
    iconStyle = q.value("look/iconStyle", iconStyle).toInt();
    iconTheme = q.value("look/iconTheme", iconTheme).toString();
    useOmarchyColors = q.value("look/omarchyColors", useOmarchyColors).toBool();
    sidebarRight = q.value("sidebar/right", sidebarRight).toBool();
    sidebarVisible = q.value("sidebar/visible", sidebarVisible).toBool();
    showPlaces = q.value("sidebar/places", showPlaces).toBool();
    showBookmarks = q.value("sidebar/bookmarks", showBookmarks).toBool();
    showDevices = q.value("sidebar/devices", showDevices).toBool();
    singleClick = q.value("behavior/singleClick", singleClick).toBool();
    confirmTrash = q.value("behavior/confirmTrash", confirmTrash).toBool();
    confirmDelete = q.value("behavior/confirmDelete", confirmDelete).toBool();
    middleClickNewTab = q.value("behavior/middleClickNewTab", middleClickNewTab).toBool();
    smoothScroll = q.value("behavior/smoothScroll", smoothScroll).toBool();
    wheelSpeed = std::clamp(q.value("behavior/wheelSpeed", wheelSpeed).toInt(), 25, 500);
    touchSpeed = std::clamp(q.value("behavior/touchSpeed", touchSpeed).toInt(), 25, 500);
    restoreSession = q.value("behavior/restoreSession", restoreSession).toBool();
    typeToSearch = q.value("behavior/typeToSearch", typeToSearch).toBool();
    searchRecursiveDefault = q.value("behavior/searchRecursive", searchRecursiveDefault).toBool();
    executableText = q.value("behavior/executableText", executableText).toInt();
    thumbnails = q.value("thumbs/enabled", thumbnails).toBool();
    thumbMaxMB = q.value("thumbs/maxMB", thumbMaxMB).toInt();
    thumbVideos = q.value("thumbs/videos", thumbVideos).toBool();
    thumbPdf = q.value("thumbs/pdf", thumbPdf).toBool();
    terminal = q.value("terminal/command", terminal).toString();
    terminalHeight = q.value("terminal/height", terminalHeight).toInt();
    terminalClickInserts = q.value("terminal/clickInserts", terminalClickInserts).toBool();
    pathbarText = q.value("view/pathbarText", pathbarText).toBool();
    toolbarStyle = q.value("toolbar/style", toolbarStyle).toInt();
    toolbarIconSize = q.value("toolbar/iconSize", toolbarIconSize).toInt();
    tbForward = q.value("toolbar/forward", tbForward).toBool();
    tbUp = q.value("toolbar/up", tbUp).toBool();
    tbSearch = q.value("toolbar/search", tbSearch).toBool();
    toolbarLayout = q.value("toolbar/layout").toStringList();
    if (toolbarLayout.isEmpty()) {   // from the older per-button switches
        auto item = [](const char *name, bool on) { return (on ? QString() : QStringLiteral("-")) + name; };
        toolbarLayout = {"back", item("forward", tbForward), item("up", tbUp), "|", "path", "|", item("search", tbSearch), "menu"};
    }
    toolbarLayout = normalizeToolbar(toolbarLayout);
    accentColor = q.value("look/accent", accentColor).toString();
    viewFontPt = q.value("look/viewFontPt", viewFontPt).toDouble();
    sidebarIconSize = q.value("sidebar/iconSize", sidebarIconSize).toInt();
    sidebarFontPt = q.value("sidebar/fontPt", sidebarFontPt).toDouble();
    sidebarSpacing = q.value("sidebar/spacing", sidebarSpacing).toInt();
    sessionTabs = q.value("session/tabs").toStringList();
    startSplit = q.value("session/split", startSplit).toBool();
}

void Settings::save() const
{
    m_savePending = false;
    QSettings q("luna", "luna");
    q.setValue("view/mode", int(viewMode));
    q.setValue("view/iconZoom", iconZoom);
    q.setValue("view/listZoom", listZoom);
    q.setValue("view/showHidden", showHidden);
    q.setValue("view/foldersFirst", foldersFirst);
    q.setValue("view/sortColumn", sortColumn);
    q.setValue("view/sortDescending", sortDescending);
    q.setValue("view/dateFormat", dateFormat);
    q.setValue("view/sizeIEC", sizeIEC);
    q.setValue("view/showItemCounts", showItemCounts);
    q.setValue("look/iconStyle", iconStyle);
    q.setValue("look/iconTheme", iconTheme);
    q.setValue("look/omarchyColors", useOmarchyColors);
    q.setValue("sidebar/right", sidebarRight);
    q.setValue("sidebar/visible", sidebarVisible);
    q.setValue("sidebar/places", showPlaces);
    q.setValue("sidebar/bookmarks", showBookmarks);
    q.setValue("sidebar/devices", showDevices);
    q.setValue("behavior/singleClick", singleClick);
    q.setValue("behavior/confirmTrash", confirmTrash);
    q.setValue("behavior/confirmDelete", confirmDelete);
    q.setValue("behavior/middleClickNewTab", middleClickNewTab);
    q.setValue("behavior/smoothScroll", smoothScroll);
    q.setValue("behavior/wheelSpeed", wheelSpeed);
    q.setValue("behavior/touchSpeed", touchSpeed);
    q.remove("behavior/scrollSpeed");   // short-lived 3-step setting
    q.setValue("behavior/restoreSession", restoreSession);
    q.setValue("behavior/typeToSearch", typeToSearch);
    q.setValue("behavior/searchRecursive", searchRecursiveDefault);
    q.setValue("behavior/executableText", executableText);
    q.setValue("thumbs/enabled", thumbnails);
    q.setValue("thumbs/maxMB", thumbMaxMB);
    q.setValue("thumbs/videos", thumbVideos);
    q.setValue("thumbs/pdf", thumbPdf);
    q.setValue("terminal/command", terminal);
    q.setValue("terminal/height", terminalHeight);
    q.setValue("terminal/clickInserts", terminalClickInserts);
    q.setValue("view/pathbarText", pathbarText);
    q.setValue("toolbar/style", toolbarStyle);
    q.setValue("toolbar/iconSize", toolbarIconSize);
    q.setValue("toolbar/forward", tbForward);
    q.setValue("toolbar/up", tbUp);
    q.setValue("toolbar/search", tbSearch);
    for (const char *k : {"toolbar/view", "toolbar/split", "toolbar/sidebar"}) q.remove(k);   // these buttons live in the status bar now
    q.setValue("toolbar/layout", toolbarLayout);
    q.setValue("look/accent", accentColor);
    q.setValue("look/viewFontPt", viewFontPt);
    q.setValue("sidebar/iconSize", sidebarIconSize);
    q.setValue("sidebar/fontPt", sidebarFontPt);
    q.setValue("sidebar/spacing", sidebarSpacing);
    q.setValue("session/tabs", sessionTabs);
    q.setValue("session/split", startSplit);
}
