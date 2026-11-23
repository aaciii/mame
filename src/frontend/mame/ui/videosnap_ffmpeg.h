// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <string>
extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#define VIDEOSNAP_FF_UTIL(X) X(avutil_version) X(av_strerror) X(av_dict_set) X(av_dict_free) X(av_frame_alloc) X(av_frame_free) X(av_frame_unref) X(av_rescale_rnd) X(av_sample_fmt_is_planar)
#define VIDEOSNAP_FF_SWR(X) X(swresample_version) X(swr_alloc_set_opts2) X(swr_init) X(swr_free) X(swr_convert) X(swr_get_delay)
#define VIDEOSNAP_FF_SWS(X) X(swscale_version) X(sws_getCachedContext) X(sws_scale) X(sws_freeContext)
#define VIDEOSNAP_FF_CODEC(X) X(avcodec_version) X(avcodec_find_decoder) X(avcodec_alloc_context3) X(avcodec_parameters_to_context) X(avcodec_open2) X(avcodec_free_context) X(avcodec_send_packet) X(avcodec_receive_frame) X(avcodec_flush_buffers) X(av_packet_alloc) X(av_packet_free) X(av_packet_unref)
#define VIDEOSNAP_FF_FORMAT(X) X(avformat_version) X(avformat_alloc_context) X(avformat_open_input) X(avformat_find_stream_info) X(avformat_close_input) X(av_find_best_stream) X(av_read_frame) X(av_seek_frame) X(av_guess_frame_rate) X(av_guess_sample_aspect_ratio)
namespace videosnap {
struct ffmpeg_api {
#define VIDEOSNAP_FF_MEMBER(name) decltype(&::name) name = nullptr;
    VIDEOSNAP_FF_UTIL(VIDEOSNAP_FF_MEMBER)
    VIDEOSNAP_FF_SWR(VIDEOSNAP_FF_MEMBER)
    VIDEOSNAP_FF_SWS(VIDEOSNAP_FF_MEMBER)
    VIDEOSNAP_FF_CODEC(VIDEOSNAP_FF_MEMBER)
    VIDEOSNAP_FF_FORMAT(VIDEOSNAP_FF_MEMBER)
#undef VIDEOSNAP_FF_MEMBER
};
// Loads the FFmpeg shared libraries on first use. Returns nullptr when they are unavailable.
ffmpeg_api const *ffmpeg(std::string const &folder);
std::string ffmpeg_error();
}
