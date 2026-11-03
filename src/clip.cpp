#include "clip.h"
#include "i18n.h"

#ifndef LUNA_AUDIO
struct AudioClip::Impl {};
bool AudioClip::available() { return false; }
AudioClip::AudioClip(const QString &, double, QObject *parent) : QObject(parent) {}
AudioClip::~AudioClip() = default;
void AudioClip::setPaused(bool) {}
bool AudioClip::playing() const { return false; }
double AudioClip::position() const { return 0; }
#else

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

namespace {
// Single-producer/single-consumer ring of float samples: the decoder thread
// writes, the PipeWire realtime callback reads (Sonata's, without seeking).
class Ring {
public:
    explicit Ring(size_t capacityPow2) : buf_(capacityPow2), mask_(capacityPow2 - 1) {}
    size_t capacity() const { return buf_.size(); }
    size_t filled() const { return size_t(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire)); }
    size_t space() const { return buf_.size() - filled(); }
    size_t write(const float *src, size_t n)
    {
        n = std::min(n, space());
        const uint64_t w = w_.load(std::memory_order_relaxed);
        const size_t at = size_t(w & mask_), first = std::min(n, buf_.size() - at);
        std::memcpy(buf_.data() + at, src, first * sizeof(float));
        std::memcpy(buf_.data(), src + first, (n - first) * sizeof(float));
        w_.store(w + n, std::memory_order_release);
        return n;
    }
    size_t read(float *dst, size_t n)
    {
        const uint64_t r = r_.load(std::memory_order_relaxed);
        n = std::min<size_t>(n, size_t(w_.load(std::memory_order_acquire) - r));
        const size_t at = size_t(r & mask_), first = std::min(n, buf_.size() - at);
        std::memcpy(dst, buf_.data() + at, first * sizeof(float));
        std::memcpy(dst + first, buf_.data(), (n - first) * sizeof(float));
        r_.store(r + n, std::memory_order_release);
        return n;
    }

private:
    std::vector<float> buf_;
    const size_t mask_;
    alignas(64) std::atomic<uint64_t> w_{0};
    alignas(64) std::atomic<uint64_t> r_{0};
};

// FFmpeg → interleaved float stereo at the file's own rate (PipeWire
// resamples if it must). Sonata's decoder without ReplayGain or surround.
class Decoder {
public:
    ~Decoder()
    {
        av_channel_layout_uninit(&out_);
        av_packet_free(&pkt_);
        av_frame_free(&frm_);
        swr_free(&swr_);
        avcodec_free_context(&cc_);
        avformat_close_input(&fmt_);
    }

    bool open(const QString &path, QString &err)
    {
        int r = avformat_open_input(&fmt_, path.toUtf8().constData(), nullptr, nullptr);
        if (r < 0 || (r = avformat_find_stream_info(fmt_, nullptr)) < 0) return fail(r, err);
        const AVCodec *codec = nullptr;
        stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (stream_ < 0 || !codec) {
            err = i18n("No tiene audio");
            return false;
        }
        for (unsigned i = 0; i < fmt_->nb_streams; ++i)   // the cover picture and the rest are never read
            if (int(i) != stream_) fmt_->streams[i]->discard = AVDISCARD_ALL;
        AVStream *st = fmt_->streams[stream_];
        cc_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(cc_, st->codecpar);
        cc_->pkt_timebase = st->time_base;
        if ((r = avcodec_open2(cc_, codec, nullptr)) < 0) return fail(r, err);
        if (cc_->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            const int n = cc_->ch_layout.nb_channels;
            av_channel_layout_uninit(&cc_->ch_layout);
            av_channel_layout_default(&cc_->ch_layout, n);
        }
        rate = cc_->sample_rate;
        if (rate <= 0 || cc_->ch_layout.nb_channels <= 0) {
            err = i18n("Formato de audio desconocido");
            return false;
        }
        av_channel_layout_default(&out_, 2);
        if (!setupSwr(err)) return false;
        if (fmt_->duration > 0) duration = fmt_->duration / double(AV_TIME_BASE);
        else if (st->duration > 0) duration = st->duration * av_q2d(st->time_base);
        tb_ = av_q2d(st->time_base);
        pkt_ = av_packet_alloc();
        frm_ = av_frame_alloc();
        return true;
    }

    void seek(double sec)
    {
        const int64_t ts = int64_t(sec * AV_TIME_BASE);
        if (avformat_seek_file(fmt_, -1, INT64_MIN, ts, ts, 0) < 0 && av_seek_frame(fmt_, -1, ts, AVSEEK_FLAG_BACKWARD) < 0) return;
        avcodec_flush_buffers(cc_);
        skipTo_ = sec;   // the seek lands on a packet before: drop up to the exact time
    }

    // Decodes about one packet, appending interleaved stereo floats.
    void decode(std::vector<float> &out)
    {
        for (;;) {
            int r = avcodec_receive_frame(cc_, frm_);
            if (r == 0) {
                convert(out);
                av_frame_unref(frm_);
                return;
            }
            if (r == AVERROR_EOF || flushing_) {
                eof = true;
                return;
            }
            if (av_read_frame(fmt_, pkt_) < 0) {
                avcodec_send_packet(cc_, nullptr);
                flushing_ = true;
                continue;
            }
            if (pkt_->stream_index == stream_) avcodec_send_packet(cc_, pkt_);
            av_packet_unref(pkt_);
        }
    }

    int rate = 0;
    double duration = 0;
    bool eof = false;

private:
    bool fail(int r, QString &err)
    {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(r, buf, sizeof buf);
        err = QString::fromUtf8(buf);
        return false;
    }
    bool setupSwr(QString &err)
    {
        int r = swr_alloc_set_opts2(&swr_, &out_, AV_SAMPLE_FMT_FLT, rate, &cc_->ch_layout, cc_->sample_fmt, cc_->sample_rate, 0, nullptr);
        if (r < 0 || (r = swr_init(swr_)) < 0) return fail(r, err);
        return true;
    }
    void convert(std::vector<float> &out)
    {
        const int n = frm_->nb_samples;
        if (n <= 0) return;
        if (frm_->format != cc_->sample_fmt || frm_->sample_rate != cc_->sample_rate) {   // chained Ogg and the like
            cc_->sample_fmt = AVSampleFormat(frm_->format);
            QString e;
            swr_free(&swr_);
            setupSwr(e);
        }
        int drop = 0;
        if (skipTo_ >= 0) {
            if (const int64_t ts = frm_->best_effort_timestamp; ts != AV_NOPTS_VALUE) {
                const double t = ts * tb_;
                if (t + double(n) / frm_->sample_rate <= skipTo_) return;
                if (t < skipTo_) drop = int((skipTo_ - t) * frm_->sample_rate);
            }
            skipTo_ = -1;
        }
        const int cap = swr_get_out_samples(swr_, n);
        const size_t base = out.size();
        out.resize(base + size_t(cap) * 2);
        auto *dst = reinterpret_cast<uint8_t *>(out.data() + base);
        int got = std::max(0, swr_convert(swr_, &dst, cap, const_cast<const uint8_t **>(frm_->extended_data), n));
        drop = std::min(drop, got);
        if (drop) std::memmove(out.data() + base, out.data() + base + size_t(drop) * 2, size_t(got - drop) * 2 * sizeof(float));
        out.resize(base + size_t(got - drop) * 2);
    }

    AVFormatContext *fmt_ = nullptr;
    AVCodecContext *cc_ = nullptr;
    SwrContext *swr_ = nullptr;
    AVPacket *pkt_ = nullptr;
    AVFrame *frm_ = nullptr;
    AVChannelLayout out_{};
    int stream_ = -1;
    double tb_ = 0, skipTo_ = -1;
    bool flushing_ = false;
};
}

// The thread never calls back into the GUI: it publishes its state here and
// the clip's timer reads it, so the Impl can outlive the AudioClip (it is
// torn down on its own thread after the preview has moved on).
struct AudioClip::Impl {
    QString path;
    double seconds;
    Ring ring{1u << 18};   // ~2.7 s of 48 kHz stereo: the decoder wakes about once a second
    std::thread thread;
    std::mutex mx;
    std::condition_variable cv;
    bool quit = false;
    std::atomic<bool> decoded{false}, opened{false}, fadeOut{false}, silent{false};
    double start = 0, length = 0, duration = 0;   // written before `opened`
    QString error;                                 // written before `decoded`
    std::atomic<uint64_t> played{0};   // frames handed to PipeWire
    std::atomic<int> rate{0};
    pw_thread_loop *loop = nullptr;
    pw_stream *stream = nullptr;

    void run()
    {
        QString err;
        Decoder dec;
        if (!dec.open(path, err)) {
            error = err.isEmpty() ? QStringLiteral("?") : err;
            decoded = true;
            return;
        }
        // the middle of the song; all of it when it is shorter
        duration = dec.duration;
        start = duration > seconds + 2 ? duration / 2 - seconds / 2 : 0;
        length = duration > 0 ? std::min(seconds, duration - start) : seconds;
        if (start > 0) dec.seek(start);
        rate = dec.rate;
        {
            std::lock_guard g(mx);
            if (quit) return;   // closed while opening
        }
        openOutput(dec.rate);
        opened = true;

        const uint64_t total = uint64_t(length * dec.rate);
        const uint64_t fadeIn = uint64_t(0.25 * dec.rate), fadeOutLen = std::min<uint64_t>(uint64_t(2.0 * dec.rate), total / 4);
        const auto nap = std::chrono::milliseconds(std::clamp(int(500.0 * ring.capacity() / (2.0 * dec.rate)), 20, 1500));
        uint64_t frame = 0;   // frames decoded so far
        std::vector<float> pend;
        size_t off = 0;
        std::unique_lock lk(mx);
        while (!quit) {
            if (off < pend.size()) {
                off += ring.write(pend.data() + off, pend.size() - off);
                if (off < pend.size()) cv.wait_for(lk, nap);   // ring full: sleep, PipeWire is playing
                continue;
            }
            pend.clear();
            off = 0;
            if (frame >= total || dec.eof) break;
            if (ring.space() < ring.capacity() / 4) {
                cv.wait_for(lk, nap);
                continue;
            }
            lk.unlock();
            dec.decode(pend);
            size_t frames = pend.size() / 2;
            if (frame + frames > total) frames = size_t(total - frame), pend.resize(frames * 2);
            for (size_t i = 0; i < frames; ++i) {   // fade in and out so it neither starts nor ends abruptly
                const uint64_t at = frame + i;
                float g = 1;
                if (at < fadeIn) g = float(at) / fadeIn;
                if (at + fadeOutLen > total) g = std::min(g, float(total - at) / fadeOutLen);
                if (g < 1) pend[2 * i] *= g, pend[2 * i + 1] *= g;
            }
            frame += frames;
            lk.lock();
        }
        decoded = true;
    }

    void openOutput(int r)
    {
        static const bool inited = [] {
            pw_init(nullptr, nullptr);
            return true;
        }();
        Q_UNUSED(inited);
        loop = pw_thread_loop_new("luna-clip", nullptr);
        pw_thread_loop_start(loop);
        pw_thread_loop_lock(loop);
        const QByteArray lat = "1024/" + QByteArray::number(r);
        pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
                                                 PW_KEY_APP_NAME, "Luna", PW_KEY_APP_ID, "luna", PW_KEY_APP_ICON_NAME, "luna",
                                                 PW_KEY_NODE_NAME, "luna-preview", PW_KEY_NODE_DESCRIPTION, "Luna",
                                                 PW_KEY_NODE_LATENCY, lat.constData(), nullptr);
#ifdef LUNA_TEST_HOOKS
        if (const QByteArray t = qgetenv("LUNA_AUDIO_TARGET"); !t.isEmpty())   // tests: into a null sink
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, t.constData());
#endif
        static const pw_stream_events events = [] {
            pw_stream_events e{};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.process = &Impl::onProcess;
            return e;
        }();
        stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "luna-preview", props, &events, this);
        if (stream) {
            uint8_t buffer[512];
            spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
            spa_audio_info_raw info{};
            info.format = SPA_AUDIO_FORMAT_F32;
            info.rate = uint32_t(r);
            info.channels = 2;
            info.position[0] = SPA_AUDIO_CHANNEL_FL;
            info.position[1] = SPA_AUDIO_CHANNEL_FR;
            const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
            pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);
        }
        pw_thread_loop_unlock(loop);
    }

    static void onProcess(void *data)
    {
        auto *o = static_cast<Impl *>(data);
        pw_buffer *b = pw_stream_dequeue_buffer(o->stream);
        if (!b) return;
        spa_data &sd = b->buffer->datas[0];
        auto *dst = static_cast<float *>(sd.data);
        if (!dst) return;
        uint32_t frames = sd.maxsize / (2 * sizeof(float));
        if (b->requested) frames = std::min<uint32_t>(frames, uint32_t(b->requested));
        size_t got = o->silent ? 0 : o->ring.read(dst, size_t(frames) * 2);
        if (o->fadeOut && !o->silent) {   // closing: ramp this quantum down to nothing
            for (size_t i = 0; i < got / 2; ++i) {
                const float g = 1.0f - float(i) / (got / 2);
                dst[2 * i] *= g, dst[2 * i + 1] *= g;
            }
            o->silent = true;
        }
        std::memset(dst + got, 0, (size_t(frames) * 2 - got) * sizeof(float));
        o->played.fetch_add(got / 2, std::memory_order_relaxed);
        sd.chunk->offset = 0;
        sd.chunk->stride = 2 * sizeof(float);
        sd.chunk->size = frames * 2 * sizeof(float);
        pw_stream_queue_buffer(o->stream, b);
    }

    void teardown()
    {
        {
            std::lock_guard g(mx);
            quit = true;
        }
        cv.notify_all();
        if (thread.joinable()) thread.join();
        if (!loop) return;
        pw_thread_loop_lock(loop);
        if (stream) pw_stream_destroy(stream);
        stream = nullptr;
        pw_thread_loop_unlock(loop);
        pw_thread_loop_stop(loop);
        pw_thread_loop_destroy(loop);
        loop = nullptr;
    }
};

bool AudioClip::available() { return true; }

AudioClip::AudioClip(const QString &path, double seconds, QObject *parent) : QObject(parent), d(std::make_unique<Impl>())
{
    d->path = path;
    d->seconds = seconds;
    Impl *impl = d.get();
    d->thread = std::thread([impl] { impl->run(); });
    // only until the fragment is over: has it opened, failed, finished?
    m_poll.setInterval(100);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        if (!m_started && d->opened) {
            m_started = true;
            m_start = d->start;
            m_length = d->length;
            m_poll.setInterval(250);
            emit started(m_start, m_length, d->duration);
        }
        if (!d->decoded || d->ring.filled() > 0) return;
        m_poll.stop();
        m_done = true;
        d->teardown();   // the thread has ended: nothing to wait for
        if (!d->error.isEmpty()) emit failed(d->error);
        else emit finished();
    });
    m_poll.start();
}

AudioClip::~AudioClip()
{
    if (m_done) return;
    // Never wait here (a song on a slow network share may still be opening):
    // fade the last quantum (~20 ms) and let go of PipeWire on another thread.
    if (!m_paused) d->fadeOut = true;
    std::thread([impl = d.release()] {
        for (int i = 0; i < 12 && impl->opened && impl->fadeOut && !impl->silent; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        impl->teardown();
        delete impl;
    }).detach();
}

void AudioClip::setPaused(bool paused)
{
    if (m_done || !d->opened || paused == m_paused) return;
    m_paused = paused;
    pw_thread_loop_lock(d->loop);
    if (d->stream) pw_stream_set_active(d->stream, !paused);
    pw_thread_loop_unlock(d->loop);
}

bool AudioClip::playing() const { return !m_done; }

double AudioClip::position() const
{
    const int r = d->rate.load();
    return r > 0 ? std::min(m_length, double(d->played.load()) / r) : 0;
}
#endif
