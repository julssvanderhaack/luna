// Quick look (Space): a floating window showing the current file —
// pictures, PDF pages, text, a frame or cover of videos and songs with a few
// facts, or a big icon. Heavy work (decoding, rendering) runs on a thread;
// ←/→ walk the folder, Intro opens, Espacio/Esc close.
#pragma once
#include <QDialog>
#include <QImage>
#include <functional>
#include <memory>

class AudioClip;
class QLabel;
class QProgressBar;
class QTimer;
class QToolButton;
class QPlainTextEdit;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
namespace Archive { struct Listing; }

class Preview : public QDialog {
    Q_OBJECT
public:
    // step(±1) moves the selection in the view and returns the new path
    // (empty at either end).
    Preview(const QString &path, std::function<QString(int)> step, QWidget *parent);
    ~Preview() override;

    static bool isOpen();

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    void load(const QString &path);
    void showImage(const QImage &img);
    void renderPdfPage();
    void showFacts(const QList<QPair<QString, QString>> &facts);
    void showArchive(const Archive::Listing &l);
    void openArchiveMember(QTreeWidgetItem *item);
    QSize imageTarget() const;
    void playClip();          // 30 s from the middle of a song (or play/pause it)
    void updatePlayer();

    std::function<QString(int)> m_step;
    QString m_path, m_mime;
    QString m_pdfPath;   // what renderPdfPage draws: the file, or an office document's PDF
    int m_request = 0;   // results of an older file are dropped
    int m_page = 0, m_pages = 0;
    QImage m_image;
    QLabel *m_title, *m_sub, *m_picture, *m_facts, *m_hint;
    QPlainTextEdit *m_text;
    class QTextBrowser *m_rich;
    QTreeWidget *m_tree;   // inside an archive
    QStackedWidget *m_stack;
    std::unique_ptr<AudioClip> m_clip;
    QWidget *m_player;   // ▶/⏸, progress and times of the fragment
    QToolButton *m_playBtn;
    QProgressBar *m_playBar;
    QLabel *m_playTime;
    QTimer *m_playTick;
};
