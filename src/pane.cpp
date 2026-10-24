#include "pane.h"
#include "apps.h"
#include "fileops.h"
#include "fsmodel.h"
#include "icons.h"
#include "places.h"
#include "theme.h"
#include "undo.h"
#include <utility>
#include <QApplication>
#include <QClipboard>
#include <QColumnView>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QSettings>
#include <QStandardPaths>
#include <QCursor>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QPainter>
#include <QSet>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QStorageInfo>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <sys/stat.h>

namespace {
// Icon view items: icon on top, name wrapped to three lines (elided only on
// the last one, like Nautilus) and a rounded accent highlight.
class IconDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    int iconSize = 64;
    QSize grid;
    struct Wrapped {
        QStringList lines;
        int width = 0;   // widest line
    };
    // Laying out text is the costly part, and the view asks for every item's
    // size on each relayout: remember the result per name.
    const Wrapped &wrapped(const QString &name, const QFont &font, int tw) const
    {
        if (tw != m_cacheWidth || font != m_cacheFont || m_cache.size() > 50000) {
            m_cache.clear();
            m_cacheWidth = tw;
            m_cacheFont = font;
        }
        auto it = m_cache.find(name);
        if (it == m_cache.end()) {
            Wrapped w;
            w.lines = wrapLines(name, font, tw);
            QFontMetrics fm(font);
            for (const QString &l : w.lines) w.width = std::max(w.width, fm.horizontalAdvance(l));
            it = m_cache.insert(name, w);
        }
        return *it;
    }
    // The item is only as big as what is drawn, so the gaps between icons
    // count as empty space (background menu, rubber-band selection).
    static QStringList wrapLines(const QString &name, const QFont &font, int tw)
    {
        QFontMetrics fm(font);
        QStringList lines;
        QTextLayout layout(name, font);
        QTextOption to(Qt::AlignHCenter);
        to.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(to);
        layout.beginLayout();
        while (lines.size() < 3) {
            QTextLine l = layout.createLine();
            if (!l.isValid()) break;
            l.setLineWidth(tw);
            if (lines.size() == 2) {
                lines << fm.elidedText(name.mid(l.textStart()), Qt::ElideMiddle, tw);
                break;
            }
            lines << name.mid(l.textStart(), l.textLength()).trimmed();
        }
        layout.endLayout();
        return lines;
    }
    QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &i) const override
    {
        const Wrapped &wr = wrapped(i.data(Qt::DisplayRole).toString(), o.font, grid.width() - 14);
        const int w = std::max(iconSize, wr.width);
        return QSize(std::min(w + 14, grid.width() - 6), iconSize + 14 + int(wr.lines.size()) * QFontMetrics(o.font).height());
    }
    QRect textRect(const QRect &r) const { return QRect(r.left() + 4, r.top() + iconSize + 10, r.width() - 8, r.height() - iconSize - 12); }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override
    {
        const Theme &th = Theme::get();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        QRect r = o.rect.adjusted(2, 2, -2, -2);
        const bool sel = o.state & QStyle::State_Selected, hover = o.state & QStyle::State_MouseOver;
        QFontMetrics fm(o.font);
        const int tw = r.width() - 8;
        const QStringList lines = wrapped(i.data(Qt::DisplayRole).toString(), o.font, std::max(tw, grid.width() - 14)).lines;
        const int textH = lines.size() * fm.height();
        QRect bg(r.left(), r.top(), r.width(), iconSize + 14 + textH);
        if (sel || hover) {
            QColor c = th.accent;
            c.setAlpha(sel ? 75 : 28);
            p->setPen(Qt::NoPen);
            p->setBrush(c);
            p->drawRoundedRect(bg, 3, 3);
        }
        QIcon icon = i.data(Qt::DecorationRole).value<QIcon>();
        QRect ir(r.left() + (r.width() - iconSize) / 2, r.top() + 6, iconSize, iconSize);
        QSize actual = icon.actualSize(QSize(iconSize, iconSize));
        QPixmap px = icon.pixmap(actual.isValid() ? actual : QSize(iconSize, iconSize), p->device()->devicePixelRatioF());
        if (!px.isNull()) {
            QSizeF s = px.deviceIndependentSize();
            s.scale(iconSize, iconSize, Qt::KeepAspectRatio);
            QRectF target(ir.left() + (iconSize - s.width()) / 2, ir.bottom() - s.height(), s.width(), s.height());
            p->drawPixmap(target.toRect(), px);
        }
        QVariant fgv = i.data(Qt::ForegroundRole);
        p->setPen(fgv.isValid() ? fgv.value<QColor>() : th.fg);
        if (sel) p->setPen(th.fg);
        p->setFont(o.font);
        int y = r.top() + iconSize + 10;
        for (const QString &l : lines) {
            p->drawText(QRect(r.left() + 4, y, tw, fm.height()), Qt::AlignHCenter | Qt::AlignVCenter, l);
            y += fm.height();
        }
        p->restore();
    }
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &o, const QModelIndex &) const override
    {
        editor->setGeometry(textRect(o.rect).adjusted(-2, -2, 2, 0).intersected(o.rect.adjusted(0, 0, 0, 20)));
    }

private:
    mutable QHash<QString, Wrapped> m_cache;
    mutable int m_cacheWidth = -1;
    mutable QFont m_cacheFont;
};
} // namespace

Pane::Pane(QWidget *parent) : QFrame(parent)
{
    setProperty("role", "pane");
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);

    // Slim per-pane header, only shown in split view
    m_title = new QWidget;
    m_title->setObjectName("paneTitle");
    m_title->setAttribute(Qt::WA_StyledBackground);
    auto *th = new QHBoxLayout(m_title);
    th->setContentsMargins(8, 3, 8, 3);
    m_titleLabel = new QLabel;
    m_titleLabel->setTextFormat(Qt::RichText);
    // A rich-text label is as wide as its whole text at least: a long path
    // made each split pane demand ~500 px and pushed the window wider than
    // its tile, cutting the header's menu button off. It shrinks freely and
    // updateTitle() elides the text instead.
    m_titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    th->addWidget(m_titleLabel, 1);
    m_title->installEventFilter(this);
    m_title->hide();
    v->addWidget(m_title);

    m_trashBar = new QWidget;
    auto *tb = new QHBoxLayout(m_trashBar);
    tb->setContentsMargins(8, 2, 4, 2);
    auto *tl = new QLabel(QString::fromUtf8("Papelera: los elementos se pueden restaurar a su ubicación original."));
    tl->setProperty("role", "dim");
    tb->addWidget(tl, 1);
    auto *empty = new QPushButton(Icons::get().ui("trash"), "Vaciar papelera");
    connect(empty, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "Vaciar papelera", QString::fromUtf8("¿Eliminar definitivamente todo lo que hay en la papelera?"),
                                  QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
            FileOps::get().emptyTrash();
    });
    tb->addWidget(empty);
    m_trashBar->hide();
    v->addWidget(m_trashBar);

    m_model = new FsModel(this);
    m_proxy = new SortProxy(this);
    m_proxy->setSourceModel(m_model);
    m_selection = new QItemSelectionModel(m_proxy, this);

    m_stack = new QStackedWidget;
    m_icons = new QListView;
    m_icons->setViewMode(QListView::IconMode);
    m_icons->setMovement(QListView::Static);
    m_icons->setResizeMode(QListView::Adjust);
    m_icons->setWordWrap(true);
    m_icons->setUniformItemSizes(false);
    m_icons->setLayoutMode(QListView::Batched);
    m_icons->setBatchSize(400);
    m_icons->setSpacing(4);
    m_icons->setTextElideMode(Qt::ElideMiddle);
    m_icons->setItemDelegate(new IconDelegate(m_icons));
    m_compact = new QListView;
    m_compact->setViewMode(QListView::ListMode);
    m_compact->setFlow(QListView::TopToBottom);
    m_compact->setWrapping(true);
    m_compact->setResizeMode(QListView::Adjust);
    m_compact->setUniformItemSizes(true);
    m_compact->setSpacing(1);
    m_compact->setTextElideMode(Qt::ElideMiddle);
    auto makeTree = [](bool tree) {
        auto *t = new QTreeView;
        t->setRootIsDecorated(tree);
        t->setItemsExpandable(tree);
        t->setUniformRowHeights(true);
        t->setSortingEnabled(true);
        t->setAlternatingRowColors(false);
        t->setAllColumnsShowFocus(true);
        t->setExpandsOnDoubleClick(false);
        t->header()->setStretchLastSection(false);
        t->header()->setSectionsMovable(true);
        t->setTextElideMode(Qt::ElideMiddle);
        return t;
    };
    m_list = makeTree(false);
    m_details = makeTree(false);
    m_tree = makeTree(true);
    m_columns = new QColumnView;
    m_columns->setResizeGripsVisible(true);
    for (QAbstractItemView *w : {static_cast<QAbstractItemView *>(m_icons), static_cast<QAbstractItemView *>(m_compact),
                                 static_cast<QAbstractItemView *>(m_list), static_cast<QAbstractItemView *>(m_details),
                                 static_cast<QAbstractItemView *>(m_tree), static_cast<QAbstractItemView *>(m_columns)}) {
        setupView(w);
        m_stack->addWidget(w);
    }
    for (QTreeView *t : {m_list, m_details, m_tree}) {
        t->header()->setSectionResizeMode(FsModel::Name, QHeaderView::Stretch);
        t->setColumnWidth(FsModel::Size, 130);
        t->setColumnWidth(FsModel::Type, 150);
        t->setColumnWidth(FsModel::Modified, 140);
        t->setColumnWidth(FsModel::Permissions, 110);
        t->setColumnWidth(FsModel::Owner, 100);
        connect(t->header(), &QHeaderView::sortIndicatorChanged, this, [](int col, Qt::SortOrder o) {
            Settings &s = Settings::get();
            if (s.sortColumn == col && s.sortDescending == (o == Qt::DescendingOrder)) return;
            s.sortColumn = col;
            s.sortDescending = o == Qt::DescendingOrder;
            s.changed();
        });
    }
    for (int c : {FsModel::Type, FsModel::Permissions, FsModel::Owner}) m_list->hideColumn(c);
    for (int c : {FsModel::Type, FsModel::Permissions, FsModel::Owner}) m_tree->hideColumn(c);

    auto *body = new QWidget;
    auto *bl = new QVBoxLayout(body);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->addWidget(m_stack);
    v->addWidget(body, 1);
    m_overlay = new QLabel(body);
    m_overlay->setAlignment(Qt::AlignCenter);
    m_overlay->setProperty("role", "dim");
    m_overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_overlay->setStyleSheet("background: transparent; font-size: 15px;");
    m_overlay->hide();
    body->installEventFilter(this);

    m_filterBar = new QWidget;
    auto *fb = new QHBoxLayout(m_filterBar);
    fb->setContentsMargins(0, 2, 0, 0);
    auto *fi = new QLabel;
    fi->setPixmap(Icons::get().ui("search").pixmap(16, 16));
    fb->addWidget(fi);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(QString::fromUtf8("Filtrar por nombre (Intro busca en subcarpetas)"));
    m_filter->installEventFilter(this);
    fb->addWidget(m_filter, 1);
    m_recursive = new QToolButton;
    m_recursive->setText(QString::fromUtf8("Subcarpetas"));
    m_recursive->setCheckable(true);
    m_recursive->setChecked(Settings::get().searchRecursiveDefault);
    m_recursive->setToolTip(QString::fromUtf8("Buscar también dentro de las subcarpetas"));
    fb->addWidget(m_recursive);
    auto *fc = new QToolButton;
    fc->setIcon(Icons::get().ui("close"));
    connect(fc, &QToolButton::clicked, this, &Pane::closeFilter);
    fb->addWidget(fc);
    m_filterBar->hide();
    v->addWidget(m_filterBar);
    auto *searchTimer = new QTimer(this);
    searchTimer->setSingleShot(true);
    searchTimer->setInterval(350);
    connect(searchTimer, &QTimer::timeout, this, [this] {
        const QString t = m_filter->text().trimmed();
        if (recursiveSearch() && t.size() >= 2) m_model->startSearch(t);
    });
    connect(m_filter, &QLineEdit::textChanged, this, [this, searchTimer](const QString &t) {
        if (recursiveSearch()) {
            searchTimer->start();
        } else {
            m_proxy->setQuickFilter(t);
            updateStatus();
            updateOverlay();
        }
    });
    connect(m_filter, &QLineEdit::returnPressed, this, [this, searchTimer] {
        if (isVirtual()) return;   // a list of files, not a folder tree
        if (!m_recursive->isChecked()) m_recursive->setChecked(true);
        searchTimer->stop();
        if (!m_filter->text().trimmed().isEmpty()) m_model->startSearch(m_filter->text().trimmed());
        m_proxy->setQuickFilter({});
    });
    connect(m_recursive, &QToolButton::toggled, this, [this](bool on) {
        if (isVirtual()) return;
        if (on && !m_filter->text().trimmed().isEmpty()) {
            m_proxy->setQuickFilter({});
            m_model->startSearch(m_filter->text().trimmed());
        } else if (!on && m_model->searching()) {
            setPath(m_path, false);
            m_proxy->setQuickFilter(m_filter->text());
        }
    });

    connect(m_model, &FsModel::loadingChanged, this, [this](bool) {
        updateStatus();
        updateOverlay();
    });
    connect(m_model, &FsModel::errorOccurred, this, [this](const QString &msg) {
        emit status(msg);
        updateOverlay();
    });
    connect(m_model, &FsModel::searchProgress, this, [this](int n, bool done) {
        emit status(done ? QString::fromUtf8("%1 resultados").arg(n) : QString::fromUtf8("Buscando… %1 resultados").arg(n));
        updateOverlay();
    });
    connect(m_model, &FsModel::dropRequested, this, [this](const QList<QUrl> &urls, const QString &dest, Qt::DropAction) { handleDrop(urls, dest); });
    connect(m_model, &QAbstractItemModel::rowsInserted, this, [this] {
        if (!m_pendingSelect.isEmpty()) selectPaths(m_pendingSelect, m_pendingEdit);
        updateOverlay();
    });
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, &Pane::updateOverlay);
    connect(m_selection, &QItemSelectionModel::selectionChanged, this, &Pane::updateStatus);
    connect(m_proxy, &QAbstractItemModel::layoutChanged, this, &Pane::updateStatus);
    connect(&Starred::get(), &Starred::changed, this, [this] {
        if (m_path == Places::starred) refresh();
    });

    applySettings();
    setViewMode(Settings::get().viewMode);
}

void Pane::setupView(QAbstractItemView *v)
{
    v->setModel(m_proxy);
    v->setSelectionModel(m_selection);
    v->setSelectionMode(QAbstractItemView::ExtendedSelection);
    v->setEditTriggers(QAbstractItemView::EditKeyPressed);
    v->setDragEnabled(true);
    v->setAcceptDrops(true);
    v->setDropIndicatorShown(true);
    v->setDragDropMode(QAbstractItemView::DragDrop);
    v->setDefaultDropAction(Qt::MoveAction);
    v->setContextMenuPolicy(Qt::CustomContextMenu);
    v->setMouseTracking(true);
    // Pixel steps: touchpads glide and SmoothScroll can animate the wheel.
    v->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    v->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    v->installEventFilter(this);
    v->viewport()->installEventFilter(this);
    connect(v, &QWidget::customContextMenuRequested, this, [this, v](const QPoint &pos) {
        QModelIndex ix = v->indexAt(pos);
        if (qEnvironmentVariableIsSet("LUNA_DEBUG")) {
            const QPoint cur = v->viewport()->mapFromGlobal(QCursor::pos());
            qWarning("menu: view=%s pos=(%d,%d) cursor=(%d,%d) index=%s current=%s selected=%lld",
                     v->metaObject()->className(), pos.x(), pos.y(), cur.x(), cur.y(),
                     ix.isValid() ? qPrintable(ix.data().toString()) : "-",
                     m_selection->currentIndex().isValid() ? qPrintable(m_selection->currentIndex().data().toString()) : "-",
                     qlonglong(selectedItems().size()));
        }
        if (ix.isValid() && !m_selection->isSelected(ix)) m_selection->select(ix, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        if (!ix.isValid()) m_selection->clearSelection();
        if (qEnvironmentVariableIsSet("LUNA_DEBUG"))
            qWarning("menu: after select: items=%lld paths=%lld", qlonglong(selectedItems().size()), qlonglong(selectedPaths().size()));
        contextMenu(v->viewport()->mapToGlobal(pos), ix.isValid());
    });
    // In the tree, clicking a folder opens it in place (expands/collapses)
    // instead of entering it; Enter still enters it.
    auto toggleInTree = [this](const QModelIndex &ix) {
        if (m_mode != ViewMode::Tree || !ix.isValid() || !ix.data(FsModel::IsDirRole).toBool()) return false;
        const QModelIndex row = ix.sibling(ix.row(), 0);
        m_tree->setExpanded(row, !m_tree->isExpanded(row));
        return true;
    };
    connect(v, &QAbstractItemView::doubleClicked, this, [this, toggleInTree](const QModelIndex &ix) {
        // QTreeView emits doubleClicked and then activated for the same click
        if (!Settings::get().singleClick && toggleInTree(ix)) m_skipActivate = true;
    });
    connect(v, &QAbstractItemView::activated, this, [this](const QModelIndex &ix) {
        if (std::exchange(m_skipActivate, false)) return;
        if (!Settings::get().singleClick) activateIndex(ix);
    });
    connect(v, &QAbstractItemView::clicked, this, [this, toggleInTree](const QModelIndex &ix) {
        if (Settings::get().singleClick && !(QApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) && !toggleInTree(ix))
            activateIndex(ix);
    });
}

QAbstractItemView *Pane::view() const { return qobject_cast<QAbstractItemView *>(m_stack->currentWidget()); }

void Pane::focusView() { view()->setFocus(); }

QModelIndex Pane::sourceIndex(const QModelIndex &p) const { return m_proxy->mapToSource(p); }

QStringList Pane::pathsFor(const QModelIndexList &rows) const
{
    QStringList out;
    for (const QModelIndex &i : rows) out << m_model->filePath(sourceIndex(i));
    return out;
}

// One column-0 index per selected item. selectedRows() is not enough: the
// icon views select a single cell, not the whole six-column row.
QModelIndexList Pane::selectedItems() const
{
    // Straight from the selection ranges: selectedIndexes() would expand a
    // select-all into one index per cell, and persistent indexes are costly.
    QModelIndexList out;
    const QItemSelection selection = m_selection->selection();
    const bool overlap = selection.size() > 1;   // ranges may share rows
    QSet<std::pair<quintptr, int>> seen;        // (parent, row)
    for (const QItemSelectionRange &r : selection) {
        const QModelIndex parent = r.parent();
        const quintptr pid = parent.isValid() ? quintptr(parent.internalPointer()) + 1 : 0;
        for (int row = r.top(); row <= r.bottom(); ++row) {
            if (overlap) {
                if (seen.contains({pid, row})) continue;
                seen.insert({pid, row});
            }
            out << m_proxy->index(row, 0, parent);
        }
    }
    return out;
}

QStringList Pane::selectedPaths() const { return pathsFor(selectedItems()); }

bool Pane::isTrash() const { return m_path == FileOps::trashFiles(); }

bool Pane::isVirtual() const { return Places::isVirtual(m_path); }

bool Pane::recursiveSearch() const { return m_recursive->isChecked() && !isVirtual(); }

void Pane::setActive(bool a)
{
    m_title->setProperty("active", a);
    m_title->style()->unpolish(m_title);
    m_title->style()->polish(m_title);
}

void Pane::applySettings()
{
    const Settings &s = Settings::get();
    // Re-sorting a big folder is the expensive part: only when it matters
    // (zooming, for instance, also lands here).
    const QList<int> sortKey = {s.sortColumn, s.sortDescending, s.foldersFirst, s.showHidden};
    if (sortKey != m_sortKey) {
        m_sortKey = sortKey;
        m_proxy->sort(s.sortColumn, s.sortDescending ? Qt::DescendingOrder : Qt::AscendingOrder);
        for (QTreeView *t : {m_list, m_details, m_tree}) {
            t->header()->blockSignals(true);
            t->header()->setSortIndicator(s.sortColumn, s.sortDescending ? Qt::DescendingOrder : Qt::AscendingOrder);
            t->header()->blockSignals(false);
        }
        m_proxy->reapply();
    }
    applyIconSizes();
    for (QAbstractItemView *v : findChildren<QAbstractItemView *>()) v->viewport()->update();
    updateStatus();
}

// Narrow panes: hide the least useful columns so the name stays readable.
void Pane::adjustColumns(QTreeView *t)
{
    QList<int> optional;
    if (t == m_details) optional = {FsModel::Owner, FsModel::Permissions, FsModel::Type, FsModel::Modified, FsModel::Size};
    else optional = {FsModel::Modified, FsModel::Size};
    for (int c : optional) t->setColumnHidden(c, false);
    auto nameRoom = [&] {
        int used = 0;
        for (int c = 1; c < FsModel::ColumnCount; ++c)
            if (!t->isColumnHidden(c)) used += t->columnWidth(c);
        return t->viewport()->width() - used;
    };
    for (int c : optional) {
        if (nameRoom() >= 200) break;
        t->setColumnHidden(c, true);
    }
}

void Pane::applyIconSizes()
{
    const Settings &s = Settings::get();
    QFont vf = QApplication::font(m_icons);
    for (QAbstractItemView *v : {static_cast<QAbstractItemView *>(m_icons), static_cast<QAbstractItemView *>(m_compact),
                                 static_cast<QAbstractItemView *>(m_list), static_cast<QAbstractItemView *>(m_details),
                                 static_cast<QAbstractItemView *>(m_tree), static_cast<QAbstractItemView *>(m_columns)}) {
        QFont f = QApplication::font(v);   // each view class keeps the platform theme's font
        if (s.viewFontPt > 0) f.setPointSizeF(s.viewFontPt);
        if (v == m_icons) vf = f;
        if (v->font() != f) v->setFont(f);
    }
    const int is = Settings::iconSizeFor(s.iconZoom);
    m_icons->setIconSize(QSize(is, is));
    const int gw = std::max(is + 60, 128), gh = is + QFontMetrics(vf).height() * 3 + 22;
    m_icons->setGridSize(QSize(gw, gh));
    if (auto *d = dynamic_cast<IconDelegate *>(m_icons->itemDelegate())) {
        d->iconSize = is;
        d->grid = QSize(gw, gh);
    }
    const int ls = Settings::listIconSizeFor(s.listZoom);
    for (QAbstractItemView *v : {static_cast<QAbstractItemView *>(m_compact), static_cast<QAbstractItemView *>(m_list),
                                 static_cast<QAbstractItemView *>(m_details), static_cast<QAbstractItemView *>(m_tree),
                                 static_cast<QAbstractItemView *>(m_columns)})
        v->setIconSize(QSize(ls, ls));
    m_compact->setGridSize(QSize(260, ls + 8));
}

void Pane::setViewMode(ViewMode m)
{
    m_mode = m;
    QWidget *w = nullptr;
    switch (m) {
    case ViewMode::Icons: w = m_icons; break;
    case ViewMode::Compact: w = m_compact; break;
    case ViewMode::List: w = m_list; break;
    case ViewMode::Details: w = m_details; break;
    case ViewMode::Tree: w = m_tree; break;
    case ViewMode::Columns: w = m_columns; break;
    }
    const bool hadFocus = view() && view()->hasFocus();
    m_stack->setCurrentWidget(w);
    if (hadFocus) w->setFocus();
    QModelIndex cur = m_selection->currentIndex();
    if (cur.isValid()) view()->scrollTo(cur);
    emit historyChanged();   // lets the window refresh its view selector
}

void Pane::zoom(int delta)
{
    Settings &s = Settings::get();
    if (m_mode == ViewMode::Icons) s.iconZoom = std::clamp(s.iconZoom + delta, 0, 5);
    else s.listZoom = std::clamp(s.listZoom + delta, 0, 3);
    s.changed();
}

void Pane::setPath(const QString &path0, bool record, const QStringList &select)
{
    QString path = Places::isVirtual(path0) ? path0 : QDir::cleanPath(path0);
    if (path.isEmpty()) return;
    QFileInfo fi(path);
    QStringList sel = select;
    if (!Places::isVirtual(path) && fi.exists() && !fi.isDir()) {   // a file: open its folder and select it
        sel = {path};
        path = fi.absolutePath();
    }
    if (record && !m_path.isEmpty() && path != m_path) {
        m_back << m_path;
        m_forward.clear();
    }
    closeFilter();
    m_path = path;
    updateTitle();
    m_trashBar->setVisible(isTrash());
    m_recursive->setVisible(!isVirtual());
    if (isVirtual()) m_model->setPaths(path, Places::contents(path));
    else m_model->setRoot(path);
    m_pendingSelect = sel;
    m_pendingEdit = false;
    m_columns->setRootIndex({});
    // Free space comes from statvfs, which can stall on a dead network mount.
    m_freeBytes = -1;
    QPointer<Pane> self(this);
    if (!isVirtual())
        QThreadPool::globalInstance()->start([self, path] {
            QStorageInfo si(path);
            const qint64 free = si.isValid() ? si.bytesAvailable() : -1;
            QMetaObject::invokeMethod(qApp, [self, path, free] {
                if (!self || self->m_path != path) return;
                self->m_freeBytes = free;
                self->updateStatus();
            }, Qt::QueuedConnection);
        });
    emit pathChanged(path);
    emit historyChanged();
    updateStatus();
}

void Pane::back()
{
    if (m_back.isEmpty()) return;
    const QString prev = m_path;
    m_forward << m_path;
    setPath(m_back.takeLast(), false, {prev});
}

void Pane::forward()
{
    if (m_forward.isEmpty()) return;
    m_back << m_path;
    setPath(m_forward.takeLast(), false);
}

void Pane::up()
{
    if (m_path == "/") return;
    const QString cur = m_path;
    QString parent = isTrash() || isVirtual() ? QDir::homePath() : QFileInfo(m_path).absolutePath();
    setPath(parent, true, {cur});
}

void Pane::refresh()
{
    if (isVirtual()) {
        const QStringList keep = selectedPaths();
        m_model->setPaths(m_path, Places::contents(m_path));
        m_pendingSelect = keep;
        return;
    }
    m_model->refresh();
}

// "<b>Name</b>  /full/path", elided to the label's width: the path first
// (in the middle), then the name.
void Pane::updateTitle()
{
    const QString name = Places::displayName(m_path);
    const QString path = isVirtual() ? QString() : m_path;
    m_titleLabel->setToolTip(path);
    const int avail = m_titleLabel->contentsRect().width();
    QFont bold = m_titleLabel->font();
    bold.setBold(true);
    const QFontMetrics fb(bold), fm(m_titleLabel->font());
    const QString shownName = fb.elidedText(name, Qt::ElideRight, std::max(avail, 0));
    const int rest = avail - fb.horizontalAdvance(shownName) - 2 * fm.horizontalAdvance(' ');
    const QString shownPath = rest > fm.horizontalAdvance(QString::fromUtf8("/…/")) ? fm.elidedText(path, Qt::ElideMiddle, rest) : QString();
    m_titleLabel->setText(QString("<b>%1</b>&nbsp;&nbsp;<span style=\"color:%2\">%3</span>")
                              .arg(shownName.toHtmlEscaped(), Theme::get().fgDim.name(), shownPath.toHtmlEscaped()));
}

void Pane::setSplitMode(bool split) { m_title->setVisible(split); }

// ----------------------------------------------------------------- opening
void Pane::activateIndex(const QModelIndex &ix, bool newTab)
{
    if (!ix.isValid()) return;
    const QModelIndex s = sourceIndex(ix.sibling(ix.row(), 0));
    const FileEntry *e = m_model->entry(s);
    if (!e) return;
    const QString path = m_model->filePath(s);
    if (e->isDir) {
        if (m_mode == ViewMode::Columns && !newTab) return;   // columns browse in place
        if (newTab) emit openInNewTab(path);
        else setPath(path);
        return;
    }
    if (isTrash()) {
        emit propertiesRequested({path});
        return;
    }
    QStringList files;
    for (const QString &p : selectedPaths())
        if (!QFileInfo(p).isDir()) files << p;
    if (!files.contains(path)) files = {path};
    openFiles(files);
}

void Pane::openFiles(const QStringList &paths)
{
    QStringList toOpen;
    QMimeDatabase db;
    for (const QString &p : paths) {
        QFileInfo fi(p);
        const QString mime = db.mimeTypeForFile(p).name();
        const bool exec = fi.isExecutable() && (mime == "application/x-executable" || mime == "application/x-pie-executable" ||
                                                 mime == "application/x-shellscript" || mime == "text/x-python3" ||
                                                 mime == "text/x-python" || mime == "application/x-sharedlib");
        if (!exec) {
            toOpen << p;
            continue;
        }
        int choice = Settings::get().executableText;   // 0 ask, 1 open, 2 run
        if (choice == 0) {
            QMessageBox box(this);
            box.setWindowTitle(QString::fromUtf8("Archivo ejecutable"));
            box.setText(QString::fromUtf8("«%1» es un programa. ¿Qué quieres hacer?").arg(fi.fileName()));
            auto *run = box.addButton("Ejecutar", QMessageBox::AcceptRole);
            auto *term = box.addButton("Ejecutar en terminal", QMessageBox::AcceptRole);
            auto *open = box.addButton("Abrir", QMessageBox::AcceptRole);
            box.addButton("Cancelar", QMessageBox::RejectRole);
            box.exec();
            if (box.clickedButton() == run) choice = 2;
            else if (box.clickedButton() == open) choice = 1;
            else if (box.clickedButton() == term) {
                QStringList cmd = QProcess::splitCommand(Settings::get().terminal);
                if (cmd.isEmpty()) cmd = {"xdg-terminal-exec"};
                QString prog = cmd.takeFirst();
                cmd << p;
                QProcess::startDetached(prog, cmd, fi.absolutePath());
                continue;
            } else continue;
        }
        if (choice == 2) QProcess::startDetached(p, {}, fi.absolutePath());
        else toOpen << p;
    }
    if (toOpen.isEmpty()) return;
    Recent::add(toOpen);
    const QStringList none = Apps::get().openDefault(toOpen);
    if (!none.isEmpty()) {
        OpenWithDialog d(none, this);
        if (d.exec() == QDialog::Accepted) {
            if (d.makeDefault()) Apps::get().setDefault(db.mimeTypeForFile(none[0]).name(), d.chosen());
            Apps::get().launch(d.chosen(), none);
        }
    }
}

void Pane::openSelection(bool newTab)
{
    QModelIndexList rows = selectedItems();
    QStringList files;
    for (const QModelIndex &ix : rows) {
        QModelIndex s = sourceIndex(ix);
        const FileEntry *e = m_model->entry(s);
        if (!e) continue;
        if (e->isDir) {
            if (newTab || rows.size() > 1) emit openInNewTab(m_model->filePath(s));
            else setPath(m_model->filePath(s));
        } else {
            files << m_model->filePath(s);
        }
    }
    if (!files.isEmpty()) openFiles(files);
}

void Pane::openWithDialog()
{
    QStringList paths = selectedPaths();
    if (paths.isEmpty()) return;
    OpenWithDialog d(paths, this);
    if (d.exec() != QDialog::Accepted) return;
    if (d.makeDefault()) Apps::get().setDefault(QMimeDatabase().mimeTypeForFile(paths[0]).name(), d.chosen());
    Recent::add(paths);
    Apps::get().launch(d.chosen(), paths);
}

QMenu *Pane::openWithMenu(const QStringList &paths, QWidget *parent)
{
    auto *m = new QMenu(QString::fromUtf8("Abrir con"), parent);
    m->setIcon(Icons::get().ui("open"));
    const QString mime = QMimeDatabase().mimeTypeForFile(paths.value(0)).name();
    const QString def = Apps::get().defaultFor(mime);
    for (const AppInfo *a : Apps::get().forMime(mime)) {
        QString label = a->name + (a->id == def ? QString::fromUtf8("  (predeterminada)") : QString());
        const QString id = a->id;
        m->addAction(Icons::get().app(a->icon), label, this, [paths, id] {
            Recent::add(paths);
            Apps::get().launch(id, paths);
        });
    }
    if (!m->isEmpty()) m->addSeparator();
    m->addAction(QString::fromUtf8("Otra aplicación…"), this, &Pane::openWithDialog);
    return m;
}

// ------------------------------------------------- "Mover a" / "Copiar a"
QMenu *Pane::transferMenu(bool move, QWidget *parent)
{
    auto *m = new QMenu(move ? QString::fromUtf8("Mover a") : QString::fromUtf8("Copiar a"), parent);
    m->setIcon(Icons::get().ui(move ? "cut" : "copy"));
    const QStringList sel = selectedPaths();
    QSet<QString> used;
    auto add = [&](const QIcon &icon, const QString &label, const QString &dest) {
        if (dest.isEmpty() || used.contains(dest) || dest == m_path || !QFileInfo(dest).isDir()) return;
        for (const QString &s : sel)
            if (dest == s || dest.startsWith(s + '/')) return;   // never into itself
        used.insert(dest);
        m->addAction(icon, label, this, [this, move, dest] { transferSelection(move, dest); })->setToolTip(dest);
    };
    if (m_other && m_other->path() != m_path) {
        add(Icons::get().ui("split"), QString::fromUtf8("Otro panel: %1").arg(QFileInfo(m_other->path()).fileName()), m_other->path());
        m->addSeparator();
    }
    // Recent destinations
    const QStringList recent = QSettings("luna", "luna").value("transfer/recent").toStringList();
    int before = m->actions().size();
    for (const QString &r : recent.mid(0, 5)) add(Icons::get().forFolder(r), QFileInfo(r).fileName(), r);
    if (m->actions().size() > before) m->addSeparator();
    // Places
    const QString home = QDir::homePath();
    add(Icons::get().place("home"), QString::fromUtf8("Carpeta personal"), home);
    struct P { QStandardPaths::StandardLocation l; const char *name; };
    for (P p : {P{QStandardPaths::DocumentsLocation, "Documentos"}, P{QStandardPaths::DownloadLocation, "Descargas"},
                P{QStandardPaths::MusicLocation, "Música"}, P{QStandardPaths::PicturesLocation, "Imágenes"},
                P{QStandardPaths::MoviesLocation, "Vídeos"}, P{QStandardPaths::DesktopLocation, "Escritorio"}}) {
        const QString d = QStandardPaths::writableLocation(p.l);
        if (d != home) add(Icons::get().forFolder(d), QString::fromUtf8(p.name), d);
    }
    // GTK bookmarks (local ones)
    QFile f(QDir::homePath() + "/.config/gtk-3.0/bookmarks");
    if (f.open(QIODevice::ReadOnly)) {
        before = m->actions().size();
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (!line.startsWith("file://")) continue;
            const int sp = line.indexOf(' ');
            const QString path = QUrl(sp < 0 ? line : line.left(sp)).toLocalFile();
            const QString label = sp < 0 ? QFileInfo(path).fileName() : line.mid(sp + 1);
            if (m->actions().size() == before) m->addSeparator();
            add(Icons::get().forFolder(path), label, path);
        }
    }
    m->addSeparator();
    m->addAction(Icons::get().ui("open"), QString::fromUtf8("Elegir carpeta…"), this, [this, move] { transferSelection(move); });
    return m;
}

void Pane::transferSelection(bool move, const QString &dest0)
{
    const QStringList sel = selectedPaths();
    if (sel.isEmpty()) return;
    QString dest = dest0;
    if (dest.isEmpty()) {
        QFileDialog dlg(this, move ? QString::fromUtf8("Mover %1 elemento(s) a…").arg(sel.size()) : QString::fromUtf8("Copiar %1 elemento(s) a…").arg(sel.size()),
                        m_path);
        dlg.setFileMode(QFileDialog::Directory);
        dlg.setOptions(QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
        dlg.setLabelText(QFileDialog::Accept, move ? QString::fromUtf8("Mover aquí") : QString::fromUtf8("Copiar aquí"));
        dlg.setLabelText(QFileDialog::Reject, "Cancelar");
        dlg.setLabelText(QFileDialog::LookIn, QString::fromUtf8("Buscar en:"));
        dlg.setLabelText(QFileDialog::FileName, "Carpeta:");
        dlg.setSidebarUrls({QUrl::fromLocalFile(QDir::homePath()), QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)),
                            QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)),
                            QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation))});
        if (dlg.exec() != QDialog::Accepted || dlg.selectedFiles().isEmpty()) return;
        dest = dlg.selectedFiles().first();
    }
    for (const QString &s : sel)
        if (dest == s || dest.startsWith(s + '/')) {
            QMessageBox::warning(this, move ? "Mover" : "Copiar", QString::fromUtf8("No se puede meter «%1» dentro de sí misma.").arg(QFileInfo(s).fileName()));
            return;
        }
    QSettings q("luna", "luna");
    QStringList recent = q.value("transfer/recent").toStringList();
    recent.removeAll(dest);
    recent.prepend(dest);
    q.setValue("transfer/recent", recent.mid(0, 8));
    if (move) FileOps::get().move(sel, dest);
    else FileOps::get().copy(sel, dest);
    emit status(QString::fromUtf8("%1 %2 elemento(s) a %3").arg(move ? "Moviendo" : "Copiando").arg(sel.size()).arg(QFileInfo(dest).fileName()));
}

// ------------------------------------------------------------------ actions
void Pane::selectPaths(const QStringList &paths, bool edit)
{
    QItemSelection sel;
    QModelIndex first;
    QStringList missing;
    for (const QString &p : paths) {
        QModelIndex s = m_model->indexForPath(p);
        if (!s.isValid()) {
            missing << p;
            continue;
        }
        QModelIndex px = m_proxy->mapFromSource(s);
        if (!px.isValid()) continue;
        sel.select(px, px.sibling(px.row(), FsModel::ColumnCount - 1));
        if (!first.isValid()) first = px;
    }
    m_pendingSelect = missing;
    if (missing.isEmpty()) m_pendingEdit = false;
    if (!first.isValid()) return;
    m_selection->select(sel, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_selection->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
    view()->scrollTo(first);
    if (edit && missing.isEmpty()) {
        view()->setFocus();
        view()->edit(first);
    }
}

void Pane::renameSelected()
{
    QModelIndexList rows = selectedItems();
    if (rows.isEmpty() || isTrash()) return;
    if (rows.size() == 1) {
        m_selection->setCurrentIndex(rows[0], QItemSelectionModel::NoUpdate);
        view()->edit(rows[0]);
        return;
    }
    // Batch rename: a pattern with # for the counter
    bool ok;
    QString pattern = QInputDialog::getText(this, QString::fromUtf8("Renombrar %1 elementos").arg(rows.size()),
                                            QString::fromUtf8("Nuevo nombre (# = número, se conserva la extensión):"), QLineEdit::Normal,
                                            QString::fromUtf8("Archivo #"), &ok);
    if (!ok || pattern.trimmed().isEmpty()) return;
    QStringList paths = selectedPaths();
    std::sort(paths.begin(), paths.end(), [](const QString &a, const QString &b) { return QString::localeAwareCompare(a, b) < 0; });
    const int width = QString::number(paths.size()).size();
    int i = 1;
    QStringList errors;
    Undo::Pairs renamed;
    for (const QString &p : paths) {
        QFileInfo fi(p);
        QString name = pattern;
        name.replace('#', QString("%1").arg(i++, width, 10, QChar('0')));
        if (!fi.isDir() && !fi.suffix().isEmpty()) name += "." + fi.suffix();
        const QString to = fi.absolutePath() + '/' + name;
        if (!QFile::rename(p, to)) errors << fi.fileName();
        else renamed.append({p, to});
    }
    Undo::get().record(Undo::Rename, renamed);
    if (!errors.isEmpty()) QMessageBox::warning(this, "Renombrar", QString::fromUtf8("No se pudieron renombrar:\n") + errors.join('\n'));
    m_model->refresh();
}

void Pane::newFolder()
{
    if (isTrash() || m_model->searching()) return;
    const QString name = FileOps::uniqueName(m_path, QString::fromUtf8("Nueva carpeta"));
    if (!QDir(m_path).mkdir(name)) {
        emit status(QString::fromUtf8("No se pudo crear la carpeta (¿sin permisos?)."));
        return;
    }
    m_pendingSelect = {m_path + '/' + name};
    m_pendingEdit = true;
    Undo::get().record(Undo::Create, {{QString(), m_path + '/' + name}}, true);
    m_model->refresh();
}

void Pane::newFile()
{
    if (isTrash() || m_model->searching()) return;
    const QString name = FileOps::uniqueName(m_path, QString::fromUtf8("Archivo nuevo.txt"));
    QFile f(m_path + '/' + name);
    if (!f.open(QIODevice::WriteOnly)) {
        emit status(QString::fromUtf8("No se pudo crear el archivo (¿sin permisos?)."));
        return;
    }
    f.close();
    m_pendingSelect = {f.fileName()};
    m_pendingEdit = true;
    Undo::get().record(Undo::Create, {{QString(), f.fileName()}});
    m_model->refresh();
}

void Pane::copySelection()
{
    QStringList p = selectedPaths();
    if (p.isEmpty()) return;
    FileOps::toClipboard(p, false);
    emit status(QString::fromUtf8("%1 elemento(s) copiados al portapapeles").arg(p.size()));
}

void Pane::cutSelection()
{
    QStringList p = selectedPaths();
    if (p.isEmpty() || isTrash()) return;
    FileOps::toClipboard(p, true);
    emit status(QString::fromUtf8("%1 elemento(s) cortados").arg(p.size()));
}

void Pane::paste()
{
    if (isTrash() || isVirtual()) return;
    bool cut = false;
    QStringList srcs = FileOps::fromClipboard(&cut);
    if (srcs.isEmpty()) return;
    const QString dest = m_path;
    Job *j = cut ? FileOps::get().move(srcs, dest) : FileOps::get().copy(srcs, dest);
    QPointer<Pane> self(this);
    connect(j, &Job::finished, this, [self, j](const QStringList &) {
        if (self) self->m_pendingSelect = j->createdPaths();
    });
    if (cut) QApplication::clipboard()->clear();
}

void Pane::trashSelection()
{
    QStringList p = selectedPaths();
    if (p.isEmpty()) return;
    if (isTrash()) {
        deleteSelection();
        return;
    }
    if (Settings::get().confirmTrash &&
        QMessageBox::question(this, "Papelera", QString::fromUtf8("¿Mover %1 elemento(s) a la papelera?").arg(p.size())) != QMessageBox::Yes)
        return;
    QStringList errors = FileOps::get().trash(p);
    if (!errors.isEmpty()) {
        if (QMessageBox::question(this, "Papelera", errors.join('\n') + QString::fromUtf8("\n\n¿Eliminarlos definitivamente?")) == QMessageBox::Yes)
            deleteSelection();
    } else {
        emit status(QString::fromUtf8("%1 elemento(s) movidos a la papelera").arg(p.size()));
    }
    m_model->refresh();
}

void Pane::deleteSelection()
{
    QStringList p = selectedPaths();
    if (p.isEmpty()) return;
    if (Settings::get().confirmDelete &&
        QMessageBox::warning(this, "Eliminar definitivamente",
                             p.size() == 1 ? QString::fromUtf8("¿Eliminar definitivamente «%1»? No se puede deshacer.").arg(QFileInfo(p[0]).fileName())
                                           : QString::fromUtf8("¿Eliminar definitivamente %1 elementos? No se puede deshacer.").arg(p.size()),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    FileOps::get().deleteForever(p);
    if (isTrash())
        for (const QString &x : p) QFile::remove(FileOps::trashInfo() + '/' + QFileInfo(x).fileName() + ".trashinfo");
}

void Pane::restoreSelection()
{
    QStringList p = selectedPaths();
    if (!p.isEmpty() && isTrash()) FileOps::get().restore(p);
}

void Pane::selectAll() { view()->selectAll(); }

void Pane::invertSelection()
{
    QItemSelection all(m_proxy->index(0, 0), m_proxy->index(m_proxy->rowCount() - 1, FsModel::ColumnCount - 1));
    m_selection->select(all, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
}

void Pane::openTerminal()
{
    QString dir = m_path;
    QStringList sel = selectedPaths();
    if (sel.size() == 1 && QFileInfo(sel[0]).isDir()) dir = sel[0];
    else if (isVirtual()) dir = sel.isEmpty() ? QDir::homePath() : QFileInfo(sel[0]).absolutePath();
    Apps::get().runTerminal(dir);
}

void Pane::copyPath()
{
    QStringList p = selectedPaths();
    if (p.isEmpty()) p = {m_path};
    QApplication::clipboard()->setText(p.join('\n'));
    emit status(QString::fromUtf8("Ruta copiada"));
}

void Pane::createLink()
{
    QStringList p = selectedPaths();
    if (!p.isEmpty() && !isVirtual()) FileOps::get().link(p, m_path);
}

void Pane::compressSelection()
{
    QStringList p = selectedPaths();
    if (p.isEmpty() || isVirtual()) return;
    QString base = p.size() == 1 ? QFileInfo(p[0]).completeBaseName() : QFileInfo(m_path).fileName();
    if (base.isEmpty()) base = "archivo";
    QStringList formats = {".zip", ".tar.gz", ".tar.xz", ".tar.zst", ".7z"};
    bool ok;
    QString fmt = QInputDialog::getItem(this, "Comprimir", QString::fromUtf8("Formato de «%1»:").arg(base), formats, 0, false, &ok);
    if (!ok) return;
    const QString out = m_path + '/' + FileOps::uniqueName(m_path, base + fmt);
    QStringList rel;
    for (const QString &x : p) rel << QFileInfo(x).fileName();
    auto *proc = new QProcess(this);
    proc->setWorkingDirectory(m_path);
    connect(proc, &QProcess::finished, this, [this, proc, out](int code) {
        proc->deleteLater();
        emit status(code == 0 ? QString::fromUtf8("Creado «%1»").arg(QFileInfo(out).fileName()) : QString::fromUtf8("Error al comprimir"));
        if (code == 0) {
            m_pendingSelect = {out};
            m_model->refresh();
        }
    });
    emit status(QString::fromUtf8("Comprimiendo…"));
    if (fmt == ".7z") proc->start("7z", QStringList{"a", out, "--"} + rel);
    else proc->start("bsdtar", QStringList{"-a", "-cf", out, "--"} + rel);
}

void Pane::extractSelection()
{
    if (isVirtual()) return;
    for (const QString &p : selectedPaths()) {
        QFileInfo fi(p);
        QString base = fi.fileName();
        for (const QString &ext : {".tar.gz", ".tar.xz", ".tar.zst", ".tar.bz2", ".tgz", ".zip", ".7z", ".rar", ".tar"})
            if (base.endsWith(ext, Qt::CaseInsensitive)) {
                base.chop(ext.size());
                break;
            }
        const QString dir = m_path + '/' + FileOps::uniqueName(m_path, base);
        QDir().mkpath(dir);
        auto *proc = new QProcess(this);
        connect(proc, &QProcess::finished, this, [this, proc, dir](int code) {
            proc->deleteLater();
            emit status(code == 0 ? QString::fromUtf8("Extraído en «%1»").arg(QFileInfo(dir).fileName()) : QString::fromUtf8("Error al extraer"));
            m_pendingSelect = {dir};
            m_model->refresh();
        });
        emit status(QString::fromUtf8("Extrayendo…"));
        if (p.endsWith(".7z", Qt::CaseInsensitive) || p.endsWith(".rar", Qt::CaseInsensitive)) proc->start("7z", {"x", "-o" + dir, p});
        else proc->start("bsdtar", {"-xf", p, "-C", dir});
    }
}

void Pane::handleDrop(const QList<QUrl> &urls, const QString &dest)
{
    QStringList srcs;
    for (const QUrl &u : urls)
        if (u.isLocalFile()) srcs << u.toLocalFile();
    if (srcs.isEmpty() || dest.startsWith(FileOps::trashFiles())) {
        if (!srcs.isEmpty() && dest.startsWith(FileOps::trashFiles())) {
            FileOps::get().trash(srcs);
            m_model->refresh();
        }
        return;
    }
    const auto mods = QApplication::keyboardModifiers();
    if ((mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) FileOps::get().link(srcs, dest);
    else if (mods & Qt::ControlModifier) FileOps::get().copy(srcs, dest);
    else if (mods & Qt::ShiftModifier) FileOps::get().move(srcs, dest);
    else if (FileOps::sameDevice(srcs[0], dest)) FileOps::get().move(srcs, dest);
    else FileOps::get().copy(srcs, dest);
}

// --------------------------------------------------------------- filtering
void Pane::showFilter(const QString &initial)
{
    m_filterBar->show();
    m_filter->setFocus();
    if (!initial.isEmpty()) m_filter->setText(m_filter->text() + initial);
}

bool Pane::filterVisible() const { return m_filterBar && m_filterBar->isVisible(); }

void Pane::closeFilter()
{
    if (!m_filterBar->isVisible() && m_proxy->quickFilter().isEmpty()) return;
    const bool wasSearch = m_model->searching();
    m_filterBar->hide();
    m_filter->blockSignals(true);
    m_filter->clear();
    m_filter->blockSignals(false);
    m_proxy->setQuickFilter({});
    m_recursive->setChecked(Settings::get().searchRecursiveDefault);
    if (isVirtual()) m_proxy->setQuickFilter({});
    else if (wasSearch) m_model->setRoot(m_path);
    focusView();
}

// ------------------------------------------------------------------ status
void Pane::updateStatus()
{
    const int total = m_proxy->rowCount();
    const QModelIndexList sel = m_model->isLoading() ? QModelIndexList() : selectedItems();
    QString s;
    if (m_model->isLoading()) {
        s = QString::fromUtf8("Cargando…");
    } else if (sel.isEmpty()) {
        s = total == 1 ? QString::fromUtf8("1 elemento") : QString::fromUtf8("%1 elementos").arg(total);
    } else {
        qint64 bytes = 0;
        const FileEntry *one = nullptr;
        for (const QModelIndex &ix : sel) {
            const FileEntry *e = m_model->entry(sourceIndex(ix));
            if (!e) continue;
            one = e;
            if (!e->isDir) bytes += e->size;
        }
        if (sel.size() == 1 && one) s = QString::fromUtf8("«%1» seleccionado").arg(one->name);
        else s = QString::fromUtf8("%1 de %2 seleccionados").arg(sel.size()).arg(total);
        if (bytes > 0) s += " (" + FsModel::formatSize(bytes) + ")";
    }
    if (m_freeBytes >= 0) s += QString::fromUtf8("  ·  %1 libres").arg(FsModel::formatSize(m_freeBytes));
    emit status(s);
}

void Pane::updateOverlay()
{
    QString text;
    if (!m_model->error().isEmpty()) text = m_model->error();
    else if (!m_model->isLoading() && m_proxy->rowCount() == 0)
        text = m_model->searching() && !isVirtual() ? QString::fromUtf8("Sin resultados")
               : !m_proxy->quickFilter().isEmpty() ? QString::fromUtf8("Nada coincide con «%1»").arg(m_proxy->quickFilter())
               : m_path == Places::recent  ? QString::fromUtf8("No has abierto archivos últimamente")
               : m_path == Places::starred ? QString::fromUtf8("Nada destacado todavía: clic derecho → Destacar")
               : isTrash() ? QString::fromUtf8("La papelera está vacía")
                           : QString::fromUtf8("Carpeta vacía");
    m_overlay->setText(text);
    m_overlay->setVisible(!text.isEmpty());
    m_overlay->setGeometry(m_overlay->parentWidget()->rect());
    m_overlay->raise();
}

// ------------------------------------------------------------ context menu
void Pane::showFolderMenu(const QPoint &globalPos)
{
    m_selection->clearSelection();
    contextMenu(globalPos, false);
}

void Pane::contextMenu(const QPoint &pos, bool onItem)
{
    QMenu m(this);
    Icons &ic = Icons::get();
    QStringList sel = selectedPaths();
    if (onItem && !sel.isEmpty()) {
        bool allDirs = true, anyDir = false, archive = false;
        QMimeDatabase db;
        for (const QString &p : sel) {
            bool d = QFileInfo(p).isDir();
            allDirs &= d;
            anyDir |= d;
            const QString mt = db.mimeTypeForFile(p, QMimeDatabase::MatchExtension).name();
            archive |= mt.contains("zip") || mt.contains("tar") || mt.contains("compressed") || mt.contains("7z") || mt.contains("rar");
        }
        if (isTrash()) {
            m.addAction(ic.ui("restore"), "Restaurar", this, &Pane::restoreSelection);
            m.addAction(ic.ui("trash"), "Eliminar definitivamente", this, &Pane::deleteSelection);
            m.addSeparator();
            m.addAction(ic.ui("info"), "Propiedades", QKeySequence("Alt+Return"), this, [this, sel] { emit propertiesRequested(sel); });
            m.exec(pos);
            return;
        }
        if (allDirs) {
            m.addAction(ic.ui("open"), "Abrir", this, [this] { openSelection(); });
            m.addAction(ic.ui("newtab"), QString::fromUtf8("Abrir en pestaña nueva"), this, [this] { openSelection(true); });
            if (sel.size() == 1) m.addAction(ic.ui("split"), "Abrir en el otro panel", this, [this, sel] { emit openInOtherPane(sel[0]); });
            m.addAction(ic.ui("terminal"), "Abrir terminal aquí", this, &Pane::openTerminal);
        } else if (!anyDir) {
            const QString mime = db.mimeTypeForFile(sel[0]).name();
            const AppInfo *def = Apps::get().byId(Apps::get().defaultFor(mime));
            m.addAction(def ? Icons::get().app(def->icon) : ic.ui("open"), def ? QString::fromUtf8("Abrir con %1").arg(def->name) : "Abrir", this,
                        [this, sel] { openFiles(sel); });
            m.addMenu(openWithMenu(sel, &m));
        }
        m.addSeparator();
        m.addAction(ic.ui("cut"), "Cortar", QKeySequence::Cut, this, &Pane::cutSelection);
        m.addAction(ic.ui("copy"), "Copiar", QKeySequence::Copy, this, &Pane::copySelection);
        if (sel.size() == 1 && allDirs)
            m.addAction(ic.ui("paste"), "Pegar dentro", this, [sel] {
                bool cut;
                QStringList s = FileOps::fromClipboard(&cut);
                if (s.isEmpty()) return;
                if (cut) FileOps::get().move(s, sel[0]);
                else FileOps::get().copy(s, sel[0]);
            })->setEnabled(!FileOps::fromClipboard(nullptr).isEmpty());
        m.addAction(ic.ui("copy"), "Copiar ruta", this, &Pane::copyPath);
        if (isVirtual() && sel.size() == 1) {
            const QString item = sel[0];
            m.addAction(ic.ui("open"), QString::fromUtf8("Abrir la carpeta que lo contiene"), this,
                        [this, item] { setPath(QFileInfo(item).absolutePath(), true, {item}); });
        }
        bool allStarred = true;
        for (const QString &x : sel) allStarred &= Starred::get().contains(x);
        if (allStarred) m.addAction(ic.ui("star"), "Quitar de destacados", this, [sel] { Starred::get().remove(sel); });
        else m.addAction(ic.ui("star"), "Destacar", this, [sel] { Starred::get().add(sel); });
        m.addSeparator();
        m.addMenu(transferMenu(true, &m));
        m.addMenu(transferMenu(false, &m));
        m.addSeparator();
        m.addAction(ic.ui("edit"), sel.size() == 1 ? QString::fromUtf8("Renombrar…") : QString::fromUtf8("Renombrar en lote…"), QKeySequence("F2"), this,
                    &Pane::renameSelected);
        m.addAction(ic.ui("trash"), "Mover a la papelera", QKeySequence::Delete, this, &Pane::trashSelection);
        m.addAction(ic.ui("cancel"), "Eliminar definitivamente", QKeySequence("Shift+Del"), this, &Pane::deleteSelection);
        m.addSeparator();
        if (!isVirtual()) {   // these create things in the current folder
            if (archive) m.addAction(ic.ui("open"), QString::fromUtf8("Extraer aquí"), this, &Pane::extractSelection);
            m.addAction(ic.ui("copy"), QString::fromUtf8("Comprimir…"), this, &Pane::compressSelection);
            m.addAction(ic.ui("star"), "Crear enlace", this, &Pane::createLink);
        }
        if (sel.size() == 1 && allDirs) m.addAction(ic.ui("star"), QString::fromUtf8("Añadir a marcadores"), this, [this, sel] { emit bookmarkRequested(sel[0]); });
        m.addSeparator();
        m.addAction(ic.ui("info"), "Propiedades", QKeySequence("Alt+Return"), this, [this, sel] { emit propertiesRequested(sel); });
    } else {
        if (isTrash()) {
            m.addAction(ic.ui("trash"), "Vaciar papelera", this, [] { FileOps::get().emptyTrash(); });
            m.exec(pos);
            return;
        }
        Undo &u = Undo::get();
        m.addAction(ic.ui("restore"), u.undoText(), QKeySequence::Undo, &u, &Undo::undo)->setEnabled(u.canUndo());
        m.addAction(ic.ui("restore"), u.redoText(), QKeySequence("Ctrl+Shift+Z"), &u, &Undo::redo)->setEnabled(u.canRedo());
        m.addSeparator();
        if (!isVirtual()) {
            m.addAction(ic.ui("newfolder"), "Nueva carpeta", QKeySequence("Ctrl+Shift+N"), this, &Pane::newFolder);
            m.addAction(ic.ui("edit"), QString::fromUtf8("Nuevo archivo vacío"), this, &Pane::newFile);
            m.addAction(ic.ui("paste"), "Pegar", QKeySequence::Paste, this, &Pane::paste)->setEnabled(!FileOps::fromClipboard(nullptr).isEmpty());
            m.addSeparator();
            m.addAction(ic.ui("terminal"), "Abrir terminal aquí", QKeySequence("F4"), this, &Pane::openTerminal);
            m.addAction(ic.ui("copy"), "Copiar ruta de la carpeta", this, &Pane::copyPath);
        }
        m.addAction(QString::fromUtf8("Seleccionar todo"), QKeySequence::SelectAll, this, &Pane::selectAll);
        m.addSeparator();
        auto *viewMenu = m.addMenu(ic.ui("icons"), "Vista");
        const char *names[] = {"Iconos", "Compacta", "Lista", "Lista detallada", "Árbol", "Columnas"};
        const char *glyphs[] = {"icons", "compact", "list", "details", "tree", "columns"};
        for (int i = 0; i < 6; ++i) {
            QAction *a = viewMenu->addAction(ic.ui(glyphs[i]), QString::fromUtf8(names[i]), this, [this, i] {
                setViewMode(ViewMode(i));
                Settings::get().viewMode = ViewMode(i);
                Settings::get().changed();
            });
            a->setCheckable(true);
            a->setChecked(int(m_mode) == i);
        }
        auto *sortMenu = m.addMenu(ic.ui("sort"), "Ordenar por");
        const char *cols[] = {"Nombre", "Tamaño", "Tipo", "Fecha de modificación", "Permisos", "Propietario"};
        for (int i = 0; i < 6; ++i) {
            QAction *a = sortMenu->addAction(QString::fromUtf8(cols[i]), this, [i] {
                Settings::get().sortColumn = i;
                Settings::get().changed();
            });
            a->setCheckable(true);
            a->setChecked(Settings::get().sortColumn == i);
        }
        sortMenu->addSeparator();
        QAction *desc = sortMenu->addAction("Descendente", this, [] {
            Settings::get().sortDescending = !Settings::get().sortDescending;
            Settings::get().changed();
        });
        desc->setCheckable(true);
        desc->setChecked(Settings::get().sortDescending);
        QAction *ff = sortMenu->addAction("Carpetas primero", this, [] {
            Settings::get().foldersFirst = !Settings::get().foldersFirst;
            Settings::get().changed();
        });
        ff->setCheckable(true);
        ff->setChecked(Settings::get().foldersFirst);
        QAction *hid = m.addAction(ic.ui("hidden"), "Mostrar archivos ocultos", QKeySequence("Ctrl+H"), this, [] {
            Settings::get().showHidden = !Settings::get().showHidden;
            Settings::get().changed();
        });
        hid->setCheckable(true);
        hid->setChecked(Settings::get().showHidden);
        if (!isVirtual()) {
            m.addSeparator();
            m.addAction(ic.ui("star"), QString::fromUtf8("Añadir carpeta a marcadores"), QKeySequence("Ctrl+D"), this,
                        [this] { emit bookmarkRequested(m_path); });
            m.addAction(ic.ui("info"), "Propiedades de la carpeta", this, [this] { emit propertiesRequested({m_path}); });
        }
    }
    m.exec(pos);
}

// ------------------------------------------------------------------ events
bool Pane::eventFilter(QObject *o, QEvent *e)
{
    if (!m_overlay || !m_filter) return QFrame::eventFilter(o, e);   // still constructing
    if (o == m_filter && e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Escape) {
            closeFilter();
            return true;
        }
        if (k->key() == Qt::Key_Down) {
            focusView();
            if (!m_selection->hasSelection() && m_proxy->rowCount() > 0)
                m_selection->setCurrentIndex(m_proxy->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            return true;
        }
    }
    if (o == m_overlay->parentWidget() && e->type() == QEvent::Resize) updateOverlay();
    if (e->type() == QEvent::Resize)
        for (QTreeView *t : {m_list, m_details, m_tree})
            if (t && o == t->viewport()) adjustColumns(t);
    if (o == m_title && e->type() == QEvent::ContextMenu) {
        emit activated(this);
        showFolderMenu(static_cast<QContextMenuEvent *>(e)->globalPos());
        return true;
    }
    if (o == m_title && e->type() == QEvent::Resize) updateTitle();
    if (o == m_title && e->type() == QEvent::MouseButtonPress) {
        emit activated(this);
        focusView();
    }

    // Mouse moves and paints of every viewport pass through here: find the
    // view by parent instead of searching all children.
    auto *v = qobject_cast<QAbstractItemView *>(o);
    QWidget *vp = nullptr;
    if (!v)
        if (auto *x = qobject_cast<QAbstractItemView *>(o->parent()); x && x->viewport() == o) {
            v = x;
            vp = x->viewport();
        }
    if (!v) return QFrame::eventFilter(o, e);

    switch (e->type()) {
    case QEvent::FocusIn:
        emit activated(this);
        break;
    case QEvent::MouseButtonPress: {
        emit activated(this);
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() == Qt::BackButton) {
            back();
            return true;
        }
        if (me->button() == Qt::ForwardButton) {
            forward();
            return true;
        }
        if (vp && me->button() == Qt::MiddleButton && Settings::get().middleClickNewTab) {
            QModelIndex ix = v->indexAt(me->position().toPoint());
            if (ix.isValid() && ix.data(FsModel::IsDirRole).toBool()) {
                emit openInNewTab(ix.data(FsModel::PathRole).toString());
                return true;
            }
        }
        break;
    }
    case QEvent::Wheel: {
        auto *we = static_cast<QWheelEvent *>(e);
        if (vp && (we->modifiers() & Qt::ControlModifier)) {
            zoom(we->angleDelta().y() > 0 ? 1 : -1);
            return true;
        }
        break;
    }
    case QEvent::KeyPress: {
        if (vp) break;
        auto *k = static_cast<QKeyEvent *>(e);
        const auto mods = k->modifiers() & ~Qt::KeypadModifier;
        if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && !(mods & Qt::AltModifier)) {
            if (mods & Qt::ControlModifier) openSelection(true);
            else openSelection();
            return true;
        }
        if (k->key() == Qt::Key_Backspace && mods == Qt::NoModifier) {
            up();
            return true;
        }
        if (k->key() == Qt::Key_Escape && (m_filterBar->isVisible() || !m_proxy->quickFilter().isEmpty())) {
            closeFilter();
            return true;
        }
        if (Settings::get().typeToSearch && (mods == Qt::NoModifier || mods == Qt::ShiftModifier) && !k->text().isEmpty() &&
            k->text().at(0).isPrint() && !k->text().at(0).isSpace()) {
            showFilter(k->text());
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QFrame::eventFilter(o, e);
}
