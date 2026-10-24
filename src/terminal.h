// Small terminal docked under the panes (F4): the user's shell on a pty,
// parsed by libvterm and painted cell by cell. Only damaged cells are
// repainted and nothing ticks while it is idle (no cursor blink).
#pragma once
#include <QString>
#include <QWidget>
#include <deque>
#include <vector>
#include <vterm.h>

class QSocketNotifier;

class Terminal : public QWidget {
    Q_OBJECT
public:
    explicit Terminal(const QString &dir, QWidget *parent = nullptr);
    ~Terminal() override;

    // Follows the active folder: "cd" into it, but only while the shell is
    // waiting at its prompt (never typed into a running program).
    void setDirectory(const QString &dir);
    void applyTheme();

signals:
    void finished();   // the shell exited

protected:
    bool event(QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    bool focusNextPrevChild(bool) override { return false; }   // Tab goes to the shell

private:
    static const VTermScreenCallbacks s_callbacks;
    static int onDamage(VTermRect r, void *self);
    static int onMoveRect(VTermRect dest, VTermRect src, void *self);
    static int onMoveCursor(VTermPos pos, VTermPos old, int visible, void *self);
    static int onTermProp(VTermProp prop, VTermValue *val, void *self);
    static int onPushLine(int cols, const VTermScreenCell *cells, void *self);
    static int onPopLine(int cols, VTermScreenCell *cells, void *self);
    static int onClearHistory(void *self);
    static void onOutput(const char *s, size_t len, void *self);

    void readPty();
    void writePty(const char *data, size_t len);
    void updateSize();
    void scrollTo(int linesBack);
    bool cellAt(int line, int col, VTermScreenCell &cell) const;   // line: -history .. rows-1
    QPoint cellFromPos(const QPoint &p) const;                       // (col, line)
    QRect cellRect(int row, int col, int w = 1, int h = 1) const;   // row on screen
    QString selectedText() const;
    bool isSelected(int line, int col) const;
    QColor color(VTermColor c, bool fg) const;
    void sendKey(VTermKey key, VTermModifier mod);

    VTerm *m_vt = nullptr;
    VTermScreen *m_screen = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    int m_fd = -1;
    int m_pid = -1;
    int m_rows = 0, m_cols = 0;
    int m_cw = 8, m_ch = 16, m_ascent = 12;
    int m_scroll = 0;   // lines scrolled back into the history
    QPoint m_cursor;    // (col, row)
    bool m_cursorVisible = true, m_altScreen = false;
    std::deque<std::vector<VTermScreenCell>> m_history;
    QPoint m_selA{0, 0}, m_selB{0, 0};   // (col, line); empty when equal
    bool m_selecting = false;
    QString m_dir;
};
