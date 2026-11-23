// SPDX-License-Identifier: BSD-3-Clause
#include "emu.h"
#include "videosnap_audio.h"
#include "videosnap_ffmpeg.h"
#include <portaudio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace videosnap {
namespace {
double steady_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void ffcheck(ffmpeg_api const &ff, int result, char const *where) {
    if (result >= 0) return;
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    ff.av_strerror(result, text, sizeof(text));
    throw std::runtime_error(std::string(where) + ": " + text);
}
void pacheck(PaError result, char const *where) {
    if (result != paNoError)
        throw std::runtime_error(std::string(where) + ": " + Pa_GetErrorText(result));
}
struct audio_resources {
    explicit audio_resources(ffmpeg_api const &api) : ff(api) { }
    ffmpeg_api const &ff;
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    SwrContext *swr = nullptr;
    PaStream *stream = nullptr;
    bool pa_initialized = false;
    ~audio_resources() {
        if (stream) { Pa_AbortStream(stream); Pa_CloseStream(stream); }
        if (pa_initialized) Pa_Terminate();
        ff.swr_free(&swr);
        ff.av_frame_free(&frame);
        ff.av_packet_free(&packet);
        ff.avcodec_free_context(&codec);
        ff.avformat_close_input(&format);
    }
};
}
struct audio_output::implementation {
    ffmpeg_api const &ff;
    struct sample { std::int64_t index; float left, right; };
    static constexpr std::uint64_t capacity = 192000;
    std::vector<sample> ring;
    std::atomic<std::uint64_t> read{0}, written{0};
    std::atomic<bool> stopped{false}, ready{false};
    std::atomic<bool> &owner_cancel;
    std::atomic<double> &epoch;
    std::thread worker;
    mutable std::mutex mutex;
    std::string failure;
    int sample_rate = 48000, channels = 2;
    float gain;
    double origin;
    double latency = 0.1;
    bool has_output = false;
    bool mapped = false;
    double pa_to_steady = 0;

    implementation(ffmpeg_api const &api, std::string filename, double media_origin, int volume,
        std::atomic<bool> &cancel, std::atomic<double> &start)
        : ff(api), ring(capacity), owner_cancel(cancel), epoch(start),
          gain(float(std::clamp(volume, 0, 100)) / 100.0F), origin(media_origin) {
        worker = std::thread([this, filename = std::move(filename)] { run(filename); });
    }
    ~implementation() {
        stopped.store(true);
        if (worker.joinable()) worker.join();
    }
    bool cancelled() const { return stopped.load() || owner_cancel.load(); }
    static int callback(void const *, void *output, unsigned long count,
        PaStreamCallbackTimeInfo const *time, PaStreamCallbackFlags, void *opaque) {
        auto &self = *static_cast<implementation *>(opaque);
        auto *dst = static_cast<float *>(output);
        std::fill_n(dst, std::size_t(count) * self.channels, 0.0F);
        if (self.cancelled()) return paAbort;
        if (!self.mapped) {
            self.pa_to_steady = steady_seconds() - time->currentTime;
            self.mapped = true;
        }
        double const start = self.epoch.load(std::memory_order_acquire);
        if (!(start > 0)) return paContinue;
        double dac = time->outputBufferDacTime;
        if (!std::isfinite(dac) || dac < time->currentTime)
            dac = time->currentTime + self.latency;
        double const desired = (dac + self.pa_to_steady - start) * self.sample_rate;
        if (!std::isfinite(desired)) return paContinue;
        std::int64_t const first = std::llround(desired);
        auto rd = self.read.load(std::memory_order_relaxed);
        auto const wr = self.written.load(std::memory_order_acquire);
        for (unsigned long i = 0; i < count; ++i) {
            auto const target = first + std::int64_t(i);
            if (target < 0) continue;
            while (rd < wr && self.ring[rd % capacity].index < target) ++rd;
            if (rd == wr) continue;
            auto const &s = self.ring[rd % capacity];
            if (s.index > target + 1) continue;
            float const left = std::clamp(s.left * self.gain, -1.0F, 1.0F);
            float const right = std::clamp(s.right * self.gain, -1.0F, 1.0F);
            if (self.channels == 1) dst[i] = (left + right) * 0.5F;
            else { dst[i * 2] = left; dst[i * 2 + 1] = right; }
            ++rd;
        }
        self.read.store(rd, std::memory_order_release);
        return paContinue;
    }
    void push(float const *samples, int count, std::int64_t &position) {
        for (int i = 0; i < count && !cancelled();) {
            auto wr = written.load(std::memory_order_relaxed);
            auto const rd = read.load(std::memory_order_acquire);
            auto const space = capacity - (wr - rd);
            if (!space) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            int const batch = std::min<int>(int(space), count - i);
            for (int j = 0; j < batch; ++j, ++i, ++position, ++wr)
                ring[wr % capacity] = sample{position, samples[i * 2], samples[i * 2 + 1]};
            written.store(wr, std::memory_order_release);
            if (wr >= std::uint64_t(sample_rate / 5)) ready.store(true, std::memory_order_release);
        }
    }
    void run(std::string const &filename) noexcept {
        try {
            audio_resources r(ff);
            r.format = ff.avformat_alloc_context();
            if (!r.format) throw std::bad_alloc();
            r.format->interrupt_callback.callback = [](void *p) -> int {
                return static_cast<implementation *>(p)->cancelled() ? 1 : 0;
            };
            r.format->interrupt_callback.opaque = this;
            AVDictionary *options = nullptr;
            ff.av_dict_set(&options, "protocol_whitelist", "file", 0);
            int const opened = ff.avformat_open_input(&r.format, filename.c_str(), nullptr, &options);
            ff.av_dict_free(&options);
            ffcheck(ff, opened, "Open preview audio");
            ffcheck(ff, ff.avformat_find_stream_info(r.format, nullptr), "Read audio streams");
            int const index = ff.av_find_best_stream(r.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (index == AVERROR_STREAM_NOT_FOUND) {
                ready.store(true, std::memory_order_release);
                return; // A video-only file is not an error.
            }
            ffcheck(ff, index, "Find preview audio");
            AVStream *stream = r.format->streams[index];
            auto const *decoder = ff.avcodec_find_decoder(stream->codecpar->codec_id);
            if (!decoder) throw std::runtime_error("No decoder for preview audio");
            r.codec = ff.avcodec_alloc_context3(decoder);
            if (!r.codec) throw std::bad_alloc();
            ffcheck(ff, ff.avcodec_parameters_to_context(r.codec, stream->codecpar), "Copy audio codec");
            r.codec->pkt_timebase = stream->time_base;
            r.codec->thread_count = 1;
            ffcheck(ff, ff.avcodec_open2(r.codec, decoder, nullptr), "Open audio decoder");
            if (r.codec->sample_rate <= 0 || r.codec->ch_layout.nb_channels <= 0)
                throw std::runtime_error("Invalid preview audio format");
            pacheck(Pa_Initialize(), "Initialize preview audio output");
            r.pa_initialized = true;
            PaDeviceIndex const device = Pa_GetDefaultOutputDevice();
            if (device == paNoDevice) throw std::runtime_error("No default audio output device");
            auto const *info = Pa_GetDeviceInfo(device);
            if (!info || info->maxOutputChannels < 1)
                throw std::runtime_error("Default audio output is unavailable");
            channels = std::min(2, info->maxOutputChannels);
            PaStreamParameters output{};
            output.device = device;
            output.channelCount = channels;
            output.sampleFormat = paFloat32;
            output.suggestedLatency = info->defaultLowOutputLatency;
            if (Pa_IsFormatSupported(nullptr, &output, sample_rate) != paFormatIsSupported)
                sample_rate = int(info->defaultSampleRate);
            pacheck(Pa_IsFormatSupported(nullptr, &output, sample_rate), "Preview output format");
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            ffcheck(ff, ff.swr_alloc_set_opts2(&r.swr, &stereo, AV_SAMPLE_FMT_FLT, sample_rate,
                &r.codec->ch_layout, r.codec->sample_fmt, r.codec->sample_rate, 0, nullptr), "Allocate audio resampler");
            ffcheck(ff, ff.swr_init(r.swr), "Initialize audio resampler");
            pacheck(Pa_OpenStream(&r.stream, nullptr, &output, sample_rate,
                paFramesPerBufferUnspecified, paNoFlag, callback, this), "Open preview audio output");
            if (auto const *stream_info = Pa_GetStreamInfo(r.stream))
                latency = stream_info->outputLatency;
            pacheck(Pa_StartStream(r.stream), "Start preview audio output");
            has_output = true;
            r.packet = ff.av_packet_alloc();
            r.frame = ff.av_frame_alloc();
            if (!r.packet || !r.frame) throw std::bad_alloc();
            bool have_position = false;
            std::int64_t position = 0;
            std::vector<float> converted;
            auto convert = [&](AVFrame *frame) {
                int const input_count = frame ? frame->nb_samples : 0;
                auto const delay = ff.swr_get_delay(r.swr, r.codec->sample_rate);
                int const output_count = int(ff.av_rescale_rnd(delay + input_count,
                    sample_rate, r.codec->sample_rate, AV_ROUND_UP));
                if (output_count <= 0) return 0;
                converted.resize(std::size_t(output_count) * 2);
                auto *bytes = reinterpret_cast<std::uint8_t *>(converted.data());
                std::uint8_t *planes[] = {bytes};
                int count = 0;
                if (frame) {
                    std::vector<std::uint8_t const *> input(std::size_t(r.codec->ch_layout.nb_channels));
                    if (ff.av_sample_fmt_is_planar(r.codec->sample_fmt)) {
                        for (std::size_t i = 0; i < input.size(); ++i) input[i] = frame->extended_data[i];
                    } else input[0] = frame->extended_data[0];
                    count = ff.swr_convert(r.swr, planes, output_count, input.data(), input_count);
                } else count = ff.swr_convert(r.swr, planes, output_count, nullptr, 0);
                ffcheck(ff, count, "Resample preview audio");
                push(converted.data(), count, position);
                return count;
            };
            auto receive = [&]() {
                for (;;) {
                    if (cancelled()) return;
                    int const got = ff.avcodec_receive_frame(r.codec, r.frame);
                    if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return;
                    ffcheck(ff, got, "Decode preview audio");
                    if (!have_position) {
                        if (r.frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                            double const pts = r.frame->best_effort_timestamp * av_q2d(stream->time_base);
                            position = std::llround((pts - origin) * sample_rate);
                        }
                        have_position = true;
                    }
                    convert(r.frame);
                    ff.av_frame_unref(r.frame);
                }
            };
            for (;;) {
                if (cancelled()) return;
                int const got = ff.av_read_frame(r.format, r.packet);
                if (got == AVERROR_EOF) break;
                ffcheck(ff, got, "Read preview audio packet");
                if (r.packet->stream_index == index) {
                    ffcheck(ff, ff.avcodec_send_packet(r.codec, r.packet), "Send audio packet");
                    receive();
                }
                ff.av_packet_unref(r.packet);
            }
            ffcheck(ff, ff.avcodec_send_packet(r.codec, nullptr), "Drain audio decoder");
            receive();
            while (!cancelled() && convert(nullptr) > 0) { }
            ready.store(true, std::memory_order_release);
            // Keep the callback alive while its queued tail drains.
            while (!cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } catch (std::exception const &e) {
            if (!cancelled()) { std::lock_guard<std::mutex> lock(mutex); failure = e.what(); }
            ready.store(true, std::memory_order_release);
        } catch (...) {
            if (!cancelled()) { std::lock_guard<std::mutex> lock(mutex); failure = "Unknown preview audio failure"; }
            ready.store(true, std::memory_order_release);
        }
    }
};
audio_output::audio_output(ffmpeg_api const &ff, std::string filename, double origin, int volume,
    std::atomic<bool> &cancel, std::atomic<double> &epoch)
    : m_impl(std::make_unique<implementation>(ff, std::move(filename), origin, volume, cancel, epoch)) { }
audio_output::~audio_output() = default;
bool audio_output::wait_ready(std::chrono::milliseconds timeout) {
    auto const end = std::chrono::steady_clock::now() + timeout;
    while (!m_impl->ready.load(std::memory_order_acquire)) {
        if (m_impl->cancelled() || std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}
std::string audio_output::error() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->failure;
}
double audio_output::lead_seconds() const {
    return m_impl->has_output ? std::max(0.15, m_impl->latency + 0.05) : 0.0;
}
}
