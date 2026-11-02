// Plays a fragment from the middle of a song, for the Space preview.
// Trimmed from Sonata's engine: one thread opens the file with FFmpeg, seeks
// and decodes into a lock-free ring a couple of seconds ahead, then sleeps; a
// PipeWire stream pulls from the ring. When the fragment ends the thread and
// the stream are gone, so a finished or closed preview costs nothing.
#pragma once
#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>

class AudioClip : public QObject {
    Q_OBJECT
public:
    static bool available();   // built with FFmpeg + PipeWire
    explicit AudioClip(const QString &path, double seconds = 30, QObject *parent = nullptr);
    ~AudioClip() override;   // returns at once; fades out (no click) and closes on its own thread

    void setPaused(bool paused);
    bool paused() const { return m_paused; }
    bool playing() const;     // started and not finished
    double position() const;  // seconds heard since the fragment started
    double start() const { return m_start; }     // where the fragment is in the song
    double length() const { return m_length; }

signals:
    void started(double start, double length, double songDuration);
    void finished();
    void failed(const QString &message);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    QTimer m_poll;
    double m_start = 0, m_length = 0;
    bool m_started = false, m_paused = false, m_done = false;
};
