#include "terminal.h"
#include "theme.h"
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QKeyEvent>
#include <QPainter>
#include <QSocketNotifier>
#include <QTimer>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {
constexpr int kHistory = 5000;   // lines kept above the screen
constexpr int kPadX = 6, kPadY = 4;

VTermScreenCell blankCell()
{
    VTermScreenCell c;
    std::memset(&c, 0, sizeof c);
    c.width = 1;
    c.fg.type = VTERM_COLOR_DEFAULT_FG;
    c.bg.type = VTERM_COLOR_DEFAULT_BG;
    return c;
}

QString cellText(const VTermScreenCell &c)
{
    QString s;
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i]; ++i) s += QString::fromUcs4(reinterpret_cast<const char32_t *>(&c.chars[i]), 1);
    return s;
}

// The shell may leave background jobs holding the pty: reap it without
// blocking the window, retrying for a while.
void reap(pid_t pid, int tries = 20)
{
    if (waitpid(pid, nullptr, WNOHANG) != 0 || tries == 0) return;
    QTimer::singleShot(250, qApp, [pid, tries] { reap(pid, tries - 1); });
}
}

const VTermScreenCallbacks Terminal::s_callbacks = {
    &Terminal::onDamage, &Terminal::onMoveRect, &Terminal::onMoveCursor, &Terminal::onTermProp,
    nullptr,   // bell
    nullptr,   // resize
    &Terminal::onPushLine, &Terminal::onPopLine, &Terminal::onClearHistory,
};

Terminal::Terminal(const QString &dir, QWidget *parent) : QWidget(parent), m_dir(dir)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::IBeamCursor);
    setMinimumHeight(3 * 16);

    QFont f(Theme::get().mono);
    f.setStyleHint(QFont::Monospace);
    f.setPointSizeF(QApplication::font().pointSizeF());
    f.setKerning(false);
    f.setFeature(QFont::Tag("liga"), 0);   // cells must stay one glyph wide
    f.setFeature(QFont::Tag("calt"), 0);
    setFont(f);
    const QFontMetrics fm(f);
    m_cw = std::max(1, fm.horizontalAdvance(QLatin1Char('M')));
    m_ch = std::max(1, fm.height());
    m_ascent = fm.ascent();

    m_vt = vterm_new(24, 80);
    m_rows = 24;
    m_cols = 80;
    vterm_set_utf8(m_vt, 1);
    vterm_output_set_callback(m_vt, &Terminal::onOutput, this);
    m_screen = vterm_obtain_screen(m_vt);
    vterm_screen_set_callbacks(m_screen, &s_callbacks, this);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
    applyTheme();
    vterm_screen_reset(m_screen, 1);

    // Everything the child needs is built here: after fork() in a threaded
    // process only async-signal-safe calls are allowed (chdir, execve).
    QByteArray shell = qgetenv("SHELL");
    if (shell.isEmpty() || access(shell.constData(), X_OK) != 0) shell = "/bin/bash";
    QByteArray cwd = QFile::encodeName(QDir(dir).exists() ? dir : QDir::homePath());
    QList<QByteArray> env;
    for (char **e = environ; *e; ++e)
        if (!QByteArray(*e).startsWith("TERM=") && !QByteArray(*e).startsWith("COLORTERM=")) env << QByteArray(*e);
    env << "TERM=xterm-256color" << "COLORTERM=truecolor" << "TERM_PROGRAM=luna";
    std::vector<char *> envp;
    for (QByteArray &e : env) envp.push_back(e.data());
    envp.push_back(nullptr);
    char *argv[] = {shell.data(), nullptr};

    winsize ws{};
    ws.ws_row = m_rows;
    ws.ws_col = m_cols;
    m_pid = forkpty(&m_fd, nullptr, nullptr, &ws);
    if (m_pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        if (chdir(cwd.constData()) != 0) (void)chdir("/");
        execve(shell.constData(), argv, envp.data());
        _exit(127);
    }
    if (m_pid < 0) {
        m_fd = -1;
        QTimer::singleShot(0, this, &Terminal::finished);
        return;
    }
    fcntl(m_fd, F_SETFL, fcntl(m_fd, F_GETFL) | O_NONBLOCK);
    fcntl(m_fd, F_SETFD, FD_CLOEXEC);
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Terminal::readPty);
}

Terminal::~Terminal()
{
    if (m_fd >= 0) {
        kill(m_pid, SIGHUP);
        ::close(m_fd);
        reap(m_pid);
    }
    vterm_free(m_vt);
}

void Terminal::applyTheme()
{
    const Theme &t = Theme::get();
    VTermState *state = vterm_obtain_state(m_vt);
    auto rgb = [](const QColor &q) {
        VTermColor c;
        vterm_color_rgb(&c, q.red(), q.green(), q.blue());
        return c;
    };
    const VTermColor fg = rgb(t.fg), bg = rgb(t.bg);
    vterm_state_set_default_colors(state, &fg, &bg);
    for (int i = 0; i < 16; ++i) {
        const VTermColor c = rgb(t.ansi[i]);
        vterm_state_set_palette_color(state, i, &c);
    }
    update();
}

void Terminal::setDirectory(const QString &dir)
{
    if (!dir.startsWith('/') || dir == m_dir) return;
    m_dir = dir;
    if (m_fd < 0 || tcgetpgrp(m_fd) != m_pid) return;   // a program is running: leave it alone
    QString quoted = dir;
    quoted.replace('\'', "'\\''");
    // Ctrl+U clears whatever was half typed; the leading space keeps the cd
    // out of the history (HISTCONTROL=ignorespace/ignoreboth).
    const QByteArray cmd = "\x15 cd -- '" + quoted.toUtf8() + "'\r";
    writePty(cmd.constData(), size_t(cmd.size()));
}

// ---- pty

void Terminal::readPty()
{
    char buf[16384];
    for (;;) {
        const ssize_t n = read(m_fd, buf, sizeof buf);
        if (n > 0) {
            vterm_input_write(m_vt, buf, size_t(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) break;
        // EOF or EIO: the shell is gone
        m_notifier->setEnabled(false);
        ::close(m_fd);
        m_fd = -1;
        reap(m_pid);
        vterm_screen_flush_damage(m_screen);
        emit finished();
        return;
    }
    vterm_screen_flush_damage(m_screen);
}

void Terminal::writePty(const char *data, size_t len)
{
    while (len && m_fd >= 0) {
        const ssize_t n = write(m_fd, data, len);
        if (n > 0) {
            data += n;
            len -= size_t(n);
        } else if (n < 0 && errno == EAGAIN) {
            pollfd p{m_fd, POLLOUT, 0};
            poll(&p, 1, 100);   // a big paste can fill the pty buffer
        } else if (!(n < 0 && errno == EINTR)) {
            return;
        }
    }
}

void Terminal::onOutput(const char *s, size_t len, void *self) { static_cast<Terminal *>(self)->writePty(s, len); }

// ---- libvterm callbacks

int Terminal::onDamage(VTermRect r, void *self)
{
    auto *t = static_cast<Terminal *>(self);
    t->update(t->cellRect(r.start_row + t->m_scroll, r.start_col, r.end_col - r.start_col, r.end_row - r.start_row));
    return 1;
}

int Terminal::onMoveRect(VTermRect, VTermRect, void *) { return 0; }   // repaint as damage

int Terminal::onMoveCursor(VTermPos pos, VTermPos old, int visible, void *self)
{
    auto *t = static_cast<Terminal *>(self);
    t->update(t->cellRect(old.row + t->m_scroll, old.col, 2));
    t->m_cursor = {pos.col, pos.row};
    t->m_cursorVisible = visible;
    t->update(t->cellRect(pos.row + t->m_scroll, pos.col, 2));
    return 1;
}

int Terminal::onTermProp(VTermProp prop, VTermValue *val, void *self)
{
    auto *t = static_cast<Terminal *>(self);
    if (prop == VTERM_PROP_CURSORVISIBLE) t->m_cursorVisible = val->boolean;
    else if (prop == VTERM_PROP_ALTSCREEN) t->m_altScreen = val->boolean;
    else return 0;
    t->update();
    return 1;
}

int Terminal::onPushLine(int cols, const VTermScreenCell *cells, void *self)
{
    auto *t = static_cast<Terminal *>(self);
    t->m_history.emplace_back(cells, cells + cols);
    if (int(t->m_history.size()) > kHistory) t->m_history.pop_front();
    if (t->m_scroll > 0) t->m_scroll = std::min<int>(t->m_scroll + 1, int(t->m_history.size()));   // the view stays put
    t->m_selA.ry()--;   // the selection moves with its text
    t->m_selB.ry()--;
    return 1;
}

int Terminal::onPopLine(int cols, VTermScreenCell *cells, void *self)
{
    auto *t = static_cast<Terminal *>(self);
    if (t->m_history.empty()) return 0;
    const std::vector<VTermScreenCell> &line = t->m_history.back();
    for (int i = 0; i < cols; ++i) cells[i] = i < int(line.size()) ? line[i] : blankCell();
    t->m_history.pop_back();
    t->m_scroll = std::min<int>(t->m_scroll, int(t->m_history.size()));
    return 1;
}

int Terminal::onClearHistory(void *self)
{
    auto *t = static_cast<Terminal *>(self);
    t->m_history.clear();
    t->m_scroll = 0;
    t->update();
    return 1;
}

// ---- geometry and cells

QRect Terminal::cellRect(int row, int col, int w, int h) const { return {kPadX + col * m_cw, kPadY + row * m_ch, w * m_cw, h * m_ch}; }

QPoint Terminal::cellFromPos(const QPoint &p) const
{
    const int col = std::clamp((p.x() - kPadX + m_cw / 2) / m_cw, 0, m_cols);
    const int row = std::clamp((p.y() - kPadY) / m_ch, 0, m_rows - 1);
    return {col, row - m_scroll};
}

bool Terminal::cellAt(int line, int col, VTermScreenCell &cell) const
{
    if (line >= 0) return vterm_screen_get_cell(m_screen, {line, col}, &cell);
    const int i = int(m_history.size()) + line;
    if (i < 0) return false;
    const std::vector<VTermScreenCell> &l = m_history[size_t(i)];
    cell = col < int(l.size()) ? l[size_t(col)] : blankCell();
    return true;
}

QColor Terminal::color(VTermColor c, bool fg) const
{
    if (fg && VTERM_COLOR_IS_DEFAULT_FG(&c)) return Theme::get().fg;
    if (!fg && VTERM_COLOR_IS_DEFAULT_BG(&c)) return {};   // the widget's own background
    if (VTERM_COLOR_IS_INDEXED(&c)) {
        if (c.indexed.idx < 16) return Theme::get().ansi[c.indexed.idx];
        vterm_screen_convert_color_to_rgb(m_screen, &c);
    }
    return QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
}

void Terminal::updateSize()
{
    const int cols = std::max(2, (width() - 2 * kPadX) / m_cw);
    const int rows = std::max(1, (height() - 2 * kPadY) / m_ch);
    if (cols == m_cols && rows == m_rows) return;
    m_cols = cols;
    m_rows = rows;
    vterm_set_size(m_vt, rows, cols);
    vterm_screen_flush_damage(m_screen);
    if (m_fd >= 0) {
        winsize ws{};
        ws.ws_row = rows;
        ws.ws_col = cols;
        ioctl(m_fd, TIOCSWINSZ, &ws);
    }
    update();
}

void Terminal::scrollTo(int linesBack)
{
    linesBack = std::clamp(linesBack, 0, int(m_history.size()));
    if (linesBack == m_scroll) return;
    m_scroll = linesBack;
    update();
}

// ---- selection

bool Terminal::isSelected(int line, int col) const
{
    if (m_selA == m_selB) return false;
    auto key = [](QPoint p) { return qint64(p.y()) * 100000 + p.x(); };
    const qint64 a = std::min(key(m_selA), key(m_selB)), b = std::max(key(m_selA), key(m_selB));
    const qint64 k = qint64(line) * 100000 + col;
    return k >= a && k < b;
}

QString Terminal::selectedText() const
{
    if (m_selA == m_selB) return {};
    QPoint a = m_selA, b = m_selB;
    if (std::make_pair(b.y(), b.x()) < std::make_pair(a.y(), a.x())) std::swap(a, b);
    QStringList lines;
    for (int line = a.y(); line <= b.y(); ++line) {
        QString text;
        const int from = line == a.y() ? a.x() : 0, to = line == b.y() ? b.x() : m_cols;
        VTermScreenCell cell;
        for (int col = from; col < to; ++col) {
            if (!cellAt(line, col, cell)) break;
            if (cell.chars[0] == uint32_t(-1)) continue;   // right half of a wide character
            const QString s = cellText(cell);
            text += s.isEmpty() ? QStringLiteral(" ") : s;
        }
        while (text.endsWith(' ')) text.chop(1);
        lines << text;
    }
    return lines.join('\n');
}

// ---- painting

void Terminal::paintEvent(QPaintEvent *e)
{
    const Theme &t = Theme::get();
    QPainter p(this);
    p.fillRect(e->rect(), t.bg);
    QFont normal = font(), bold = font(), italic = font();
    bold.setBold(true);
    italic.setItalic(true);

    const QRect r = e->rect();
    const int row0 = std::max(0, (r.top() - kPadY) / m_ch), row1 = std::min(m_rows - 1, (r.bottom() - kPadY) / m_ch);
    const int col0 = std::max(0, (r.left() - kPadX) / m_cw - 1), col1 = std::min(m_cols - 1, (r.right() - kPadX) / m_cw);
    const bool showCursor = m_cursorVisible && m_scroll == 0;
    VTermScreenCell cell;
    for (int row = row0; row <= row1; ++row) {
        const int line = row - m_scroll;
        for (int col = col0; col <= col1; ++col) {
            if (!cellAt(line, col, cell) || cell.chars[0] == uint32_t(-1)) continue;
            QColor fg = color(cell.fg, true), bg = color(cell.bg, false);
            if (cell.attrs.reverse) {
                std::swap(fg, bg);
                if (!fg.isValid()) fg = t.bg;
            }
            const int w = std::max(1, int(cell.width));
            const bool cursorHere = showCursor && hasFocus() && line == m_cursor.y() && col == m_cursor.x();
            if (isSelected(line, col)) {
                bg = t.selection;
                fg = t.fg;
            }
            if (cursorHere) {
                bg = t.fg;
                fg = t.bg;
            }
            const QRect cr = cellRect(row, col, w);
            if (bg.isValid()) p.fillRect(cr, bg);
            const QString s = cellText(cell);
            if (s.isEmpty() || s == QLatin1String(" ")) continue;
            p.setFont(cell.attrs.bold ? bold : cell.attrs.italic ? italic : normal);
            p.setPen(fg);
            p.drawText(cr.left(), cr.top() + m_ascent, s);
            if (cell.attrs.underline) p.drawLine(cr.left(), cr.top() + m_ascent + 2, cr.right(), cr.top() + m_ascent + 2);
            if (cell.attrs.strike) p.drawLine(cr.left(), cr.top() + m_ch / 2, cr.right(), cr.top() + m_ch / 2);
            col += w - 1;
        }
    }
    if (showCursor && !hasFocus()) {   // hollow block when the focus is elsewhere
        p.setPen(t.fgDim);
        p.drawRect(cellRect(m_cursor.y(), m_cursor.x()).adjusted(0, 0, -1, -1));
    }
}

void Terminal::resizeEvent(QResizeEvent *) { updateSize(); }

void Terminal::focusInEvent(QFocusEvent *) { update(cellRect(m_cursor.y() + m_scroll, m_cursor.x(), 2)); }

void Terminal::focusOutEvent(QFocusEvent *) { update(cellRect(m_cursor.y() + m_scroll, m_cursor.x(), 2)); }

// ---- input

bool Terminal::event(QEvent *e)
{
    // The window's shortcuts (Ctrl+H, Supr, Retroceso…) must not eat the
    // shell's keys; only F4 (this panel) and Mayús+F4 still reach them.
    if (e->type() == QEvent::ShortcutOverride) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() != Qt::Key_F4) {
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

void Terminal::sendKey(VTermKey key, VTermModifier mod)
{
    vterm_keyboard_key(m_vt, key, mod);
}

void Terminal::keyPressEvent(QKeyEvent *e)
{
    const Qt::KeyboardModifiers m = e->modifiers();
    int mod = VTERM_MOD_NONE;
    if (m & Qt::ShiftModifier) mod |= VTERM_MOD_SHIFT;
    if (m & Qt::AltModifier) mod |= VTERM_MOD_ALT;
    if (m & Qt::ControlModifier) mod |= VTERM_MOD_CTRL;
    const bool ctrlShift = (m & Qt::ControlModifier) && (m & Qt::ShiftModifier);
    if (ctrlShift && e->key() == Qt::Key_C) {
        if (const QString s = selectedText(); !s.isEmpty()) QApplication::clipboard()->setText(s);
        return;
    }
    if (ctrlShift && e->key() == Qt::Key_V) {
        QString s = QApplication::clipboard()->text();
        s.replace("\r\n", "\r").replace('\n', '\r');
        vterm_keyboard_start_paste(m_vt);
        for (char32_t c : s.toUcs4()) vterm_keyboard_unichar(m_vt, c, VTERM_MOD_NONE);
        vterm_keyboard_end_paste(m_vt);
        scrollTo(0);
        return;
    }
    if (m == Qt::ShiftModifier && (e->key() == Qt::Key_PageUp || e->key() == Qt::Key_PageDown)) {
        scrollTo(m_scroll + (e->key() == Qt::Key_PageUp ? 1 : -1) * std::max(1, m_rows - 1));
        return;
    }
    scrollTo(0);
    const VTermModifier vm = VTermModifier(mod);
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter: return sendKey(VTERM_KEY_ENTER, vm);
    case Qt::Key_Tab: return sendKey(VTERM_KEY_TAB, vm);
    case Qt::Key_Backtab: return sendKey(VTERM_KEY_TAB, VTermModifier(mod | VTERM_MOD_SHIFT));
    case Qt::Key_Backspace: return sendKey(VTERM_KEY_BACKSPACE, vm);
    case Qt::Key_Escape: return sendKey(VTERM_KEY_ESCAPE, vm);
    case Qt::Key_Up: return sendKey(VTERM_KEY_UP, vm);
    case Qt::Key_Down: return sendKey(VTERM_KEY_DOWN, vm);
    case Qt::Key_Left: return sendKey(VTERM_KEY_LEFT, vm);
    case Qt::Key_Right: return sendKey(VTERM_KEY_RIGHT, vm);
    case Qt::Key_Insert: return sendKey(VTERM_KEY_INS, vm);
    case Qt::Key_Delete: return sendKey(VTERM_KEY_DEL, vm);
    case Qt::Key_Home: return sendKey(VTERM_KEY_HOME, vm);
    case Qt::Key_End: return sendKey(VTERM_KEY_END, vm);
    case Qt::Key_PageUp: return sendKey(VTERM_KEY_PAGEUP, vm);
    case Qt::Key_PageDown: return sendKey(VTERM_KEY_PAGEDOWN, vm);
    default: break;
    }
    if (e->key() >= Qt::Key_F1 && e->key() <= Qt::Key_F35) return sendKey(VTermKey(VTERM_KEY_FUNCTION(e->key() - Qt::Key_F1 + 1)), vm);
    const QString text = e->text();
    if (!text.isEmpty()) {
        for (char32_t c : text.toUcs4()) {
            int cm = mod & ~VTERM_MOD_SHIFT;                 // already in the text
            if (c < 0x20 || c == 0x7f) cm &= ~VTERM_MOD_CTRL;   // Ctrl+letter arrives as a control byte
            vterm_keyboard_unichar(m_vt, c, VTermModifier(cm));
        }
    } else if ((m & Qt::ControlModifier) && e->key() >= Qt::Key_A && e->key() <= Qt::Key_Z) {
        vterm_keyboard_unichar(m_vt, char32_t('a' + e->key() - Qt::Key_A), VTermModifier(mod & ~VTERM_MOD_SHIFT));
    } else if ((m & Qt::ControlModifier) && e->key() == Qt::Key_Space) {
        vterm_keyboard_unichar(m_vt, ' ', VTERM_MOD_CTRL);
    }
}

void Terminal::wheelEvent(QWheelEvent *e)
{
    const int steps = e->angleDelta().y() / 40;   // 3 lines per notch
    if (!steps) return;
    if (m_altScreen) {   // less, man, vim: they scroll with the arrows
        for (int i = 0; i < std::abs(steps); ++i) sendKey(steps > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
        return;
    }
    scrollTo(m_scroll + steps);
}

void Terminal::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    if (e->button() == Qt::MiddleButton) {   // paste the primary selection, as terminals do
        QString s = QApplication::clipboard()->text(QClipboard::Selection);
        s.replace('\n', '\r');
        vterm_keyboard_start_paste(m_vt);
        for (char32_t c : s.toUcs4()) vterm_keyboard_unichar(m_vt, c, VTERM_MOD_NONE);
        vterm_keyboard_end_paste(m_vt);
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    m_selA = m_selB = cellFromPos(e->position().toPoint());
    m_selecting = true;
    update();
}

void Terminal::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_selecting) return;
    m_selB = cellFromPos(e->position().toPoint());
    update();
}

void Terminal::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || !m_selecting) return;
    m_selecting = false;
    if (const QString s = selectedText(); !s.isEmpty()) QApplication::clipboard()->setText(s, QClipboard::Selection);
}
