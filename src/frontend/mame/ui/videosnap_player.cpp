#include "emu.h"
// SPDX-License-Identifier: BSD-3-Clause
#include "videosnap_player.h"
#include "videosnap_audio.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

namespace videosnap {
namespace {
void check(int code, char const *operation) {
    if (code >= 0) return;
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    throw std::runtime_error(std::string(operation) + ": " + text);
}
struct resources {
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *decoded = nullptr;
    SwsContext *scale = nullptr;
    ~resources() {
        sws_freeContext(scale);
        av_frame_free(&decoded);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }
};
}
player::~player() { stop(); }
void player::configure_audio(bool enabled, int volume) {
    stop();
    m_audio_enabled = enabled;
    m_audio_volume = std::clamp(volume, 0, 100);
}
std::string player::audio_error() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_audio_error;
}
void player::stop() {
    m_cancel.store(true);
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ready = false;
    m_latest = frame{};
    m_state = state::stopped;
    m_error.clear();
    m_audio_error.clear();
}
void player::start(std::string path, std::chrono::milliseconds delay) {
    stop();
    m_cancel.store(false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = state::opening;
    }
    try {
        m_thread = std::thread([this, path = std::move(path), delay] { run(path, delay); });
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = state::failed;
        m_error = "Cannot start video worker";
    }
}
bool player::take_frame(frame &result) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready) return false;
    result = std::move(m_latest);
    m_ready = false;
    return true;
}
state player::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}
std::string player::error() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}
bool player::wait_until(std::chrono::steady_clock::time_point time) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_wake.wait_until(lock, time, [this] { return m_cancel.load(); });
    return !m_cancel.load();
}
void player::run(std::string const &path, std::chrono::milliseconds delay) noexcept {
    using clock = std::chrono::steady_clock;
    try {
        if (!wait_until(clock::now() + std::max(delay, std::chrono::milliseconds(0)))) return;
        // Only local, regular files should be passed by the menu integration.
        resources r;
        r.format = avformat_alloc_context();
        if (!r.format) throw std::bad_alloc();
        r.format->interrupt_callback.callback = [](void *opaque) -> int {
            return static_cast<player *>(opaque)->m_cancel.load() ? 1 : 0;
        };
        r.format->interrupt_callback.opaque = this;
        AVDictionary *options = nullptr;
        av_dict_set(&options, "protocol_whitelist", "file", 0);
        int opened = avformat_open_input(&r.format, path.c_str(), nullptr, &options);
        av_dict_free(&options);
        check(opened, "Open video");
        check(avformat_find_stream_info(r.format, nullptr), "Read stream information");
        int stream_index = av_find_best_stream(r.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        check(stream_index, "Find video stream");
        AVStream *stream = r.format->streams[stream_index];
        AVCodec const *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) throw std::runtime_error("No installed decoder for this video");
        r.codec = avcodec_alloc_context3(decoder);
        if (!r.codec) throw std::bad_alloc();
        check(avcodec_parameters_to_context(r.codec, stream->codecpar), "Copy codec parameters");
        r.codec->pkt_timebase = stream->time_base;
        r.codec->thread_count = 2;
        r.codec->max_pixels = 8192LL * 8192LL;
        check(avcodec_open2(r.codec, decoder, nullptr), "Open decoder");
        r.packet = av_packet_alloc();
        r.decoded = av_frame_alloc();
        if (!r.packet || !r.decoded) throw std::bad_alloc();
        AVRational rate = av_guess_frame_rate(r.format, stream, nullptr);
        double nominal = rate.num > 0 && rate.den > 0 ? av_q2d(av_inv_q(rate)) : 1.0 / 30.0;
        nominal = std::clamp(nominal, 1.0 / 240.0, 1.0);
        std::uint64_t sequence = 0, loop = 0;
        for (;;) {
            if (m_cancel.load()) return;
            bool first = true;
            double base_pts = 0, previous = -nominal;
            auto epoch = clock::now();
            std::atomic<double> audio_epoch{0.0};
            std::unique_ptr<audio_output> audio;
            auto report_audio_failure = [this](std::string const &message) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_audio_error = message;
                }
#if defined(MAME_VIDEO_SNAPS)
                osd_printf_warning("Video preview audio: %s\n", message);
#endif
            };
            std::uint64_t loop_frames = 0;
            auto receive = [&]() {
                for (;;) {
                    if (m_cancel.load()) return;
                    int got = avcodec_receive_frame(r.codec, r.decoded);
                    if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return;
                    check(got, "Decode video frame");
                    AVFrame *f = r.decoded;
                    if (f->width <= 0 || f->height <= 0 || f->width > 8192 || f->height > 8192)
                        throw std::runtime_error("Video dimensions outside preview limits");
                    double raw = f->best_effort_timestamp == AV_NOPTS_VALUE
                        ? (first ? 0.0 : base_pts + previous + nominal)
                        : f->best_effort_timestamp * av_q2d(stream->time_base);
                    if (first) {
                        base_pts = raw;
                        if (m_audio_enabled && m_audio_volume > 0) {
                            try {
                                audio = std::make_unique<audio_output>(path, base_pts,
                                    m_audio_volume, m_cancel, audio_epoch);
                                if (!audio->wait_ready(std::chrono::milliseconds(3000))) {
                                    if (!m_cancel.load()) {
                                        report_audio_failure("Audio startup timed out after 3000 ms: " + path);
                                    }
                                    audio.reset();
                                } else if (!audio->error().empty()) {
                                    report_audio_failure(audio->error());
                                    audio.reset();
                                }
                            } catch (std::exception const &e) {
                                report_audio_failure(e.what());
                                audio.reset();
                            }
                        }
                        if (m_cancel.load()) return;
                        double const lead = audio ? audio->lead_seconds() : 0.0;
                        epoch = clock::now() + std::chrono::duration_cast<clock::duration>(
                            std::chrono::duration<double>(lead));
                        audio_epoch.store(std::chrono::duration<double>(
                            epoch.time_since_epoch()).count(), std::memory_order_release);
                        first = false;
                    }
                    double pts = raw - base_pts;
                    if (!std::isfinite(pts) || pts < 0 || pts <= previous)
                        pts = previous + nominal;
                    if (pts - previous > 10.0)
                        throw std::runtime_error("Invalid preview timestamp gap");
                    previous = pts;
                    auto deadline = epoch + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(pts));
                    if (!wait_until(deadline)) return;
                    double factor = std::min(1.0, 960.0 / std::max(f->width, f->height));
                    frame output;
                    output.width = std::max(1, int(f->width * factor));
                    output.height = std::max(1, int(f->height * factor));
                    AVRational sar = av_guess_sample_aspect_ratio(r.format, stream, f);
                    output.display_aspect = double(f->width) / f->height;
                    if (sar.num > 0 && sar.den > 0) output.display_aspect *= av_q2d(sar);
                    output.sequence = ++sequence;
                    output.loop = loop;
                    std::vector<std::uint8_t> rgba(std::size_t(output.width) * output.height * 4);
                    r.scale = sws_getCachedContext(r.scale, f->width, f->height,
                        static_cast<AVPixelFormat>(f->format), output.width, output.height,
                        AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (!r.scale) throw std::runtime_error("Cannot create pixel converter");
                    std::uint8_t *planes[4] = {rgba.data(), nullptr, nullptr, nullptr};
                    int strides[4] = {output.width * 4, 0, 0, 0};
                    int rows = sws_scale(r.scale, f->data, f->linesize, 0, f->height, planes, strides);
                    if (rows != output.height) throw std::runtime_error("Incomplete converted frame");
                    output.argb.resize(std::size_t(output.width) * output.height);
                    for (std::size_t i = 0; i < output.argb.size(); ++i)
                        output.argb[i] = 0xff000000U | (std::uint32_t(rgba[i*4]) << 16)
                            | (std::uint32_t(rgba[i*4+1]) << 8) | rgba[i*4+2];
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_latest = std::move(output);
                        m_ready = true;
                        m_state = state::playing;
                    }
                    ++loop_frames;
                    av_frame_unref(f);
                }
            };
            for (;;) {
                if (m_cancel.load()) return;
                int read = av_read_frame(r.format, r.packet);
                if (read == AVERROR_EOF) break;
                check(read, "Read video packet");
                if (r.packet->stream_index == stream_index) {
                    check(avcodec_send_packet(r.codec, r.packet), "Send compressed video packet");
                    receive();
                }
                av_packet_unref(r.packet);
            }
            check(avcodec_send_packet(r.codec, nullptr), "Drain video decoder");
            receive();
            if (!loop_frames) throw std::runtime_error("Video contains no decodable frames");
            if (!wait_until(epoch + std::chrono::duration_cast<clock::duration>(
                    std::chrono::duration<double>(previous + nominal)))) return;
            check(av_seek_frame(r.format, stream_index,
                stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time,
                AVSEEK_FLAG_BACKWARD), "Loop video");
            avcodec_flush_buffers(r.codec);
            ++loop;
        }
    } catch (std::exception const &e) {
        if (m_cancel.load()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = e.what();
        m_ready = false;
        m_latest = frame{};
        m_state = state::failed;
    } catch (...) {
        if (m_cancel.load()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = "Unknown video worker error";
        m_ready = false;
        m_latest = frame{};
        m_state = state::failed;
    }
}
}
