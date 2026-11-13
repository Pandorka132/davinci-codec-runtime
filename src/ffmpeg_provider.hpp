#pragma once

#include <davinci_codec_runtime/api.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/buffer.h>
}

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Explicit, one-shot initialization. `private_ffmpeg_root` contains the
 * libavcodec/ and libavutil/ subdirectories of the private FFmpeg 6.1.6 build.
 * The original Resolve FFmpeg60 provider is discovered in RTLD_DEFAULT.
 */
DAVINCI_CODEC_RUNTIME_API int davinci_codec_runtime_initialize_ffmpeg(
    const char *private_ffmpeg_root) noexcept;
DAVINCI_CODEC_RUNTIME_API int davinci_codec_runtime_ffmpeg_ready() noexcept;
DAVINCI_CODEC_RUNTIME_API const char *davinci_codec_runtime_ffmpeg_error() noexcept;
DAVINCI_CODEC_RUNTIME_API unsigned int davinci_codec_runtime_private_avcodec_version() noexcept;

/* Exact C ABI wrappers used by the eight generic-video-decoder relay stubs. */
DAVINCI_CODEC_RUNTIME_API const AVCodec *davinci_avcodec_find_decoder(
    enum AVCodecID id) noexcept;
DAVINCI_CODEC_RUNTIME_API AVCodecContext *davinci_avcodec_alloc_context3(
    const AVCodec *codec) noexcept;
DAVINCI_CODEC_RUNTIME_API int davinci_avcodec_open2(
    AVCodecContext *context, const AVCodec *codec,
    AVDictionary **options) noexcept;
DAVINCI_CODEC_RUNTIME_API int davinci_avcodec_close(
    AVCodecContext *context) noexcept;
DAVINCI_CODEC_RUNTIME_API void davinci_avcodec_free_context(
    AVCodecContext **context) noexcept;
DAVINCI_CODEC_RUNTIME_API int davinci_avcodec_send_packet(
    AVCodecContext *context, const AVPacket *packet) noexcept;
DAVINCI_CODEC_RUNTIME_API int davinci_avcodec_receive_frame(
    AVCodecContext *context, AVFrame *frame) noexcept;
DAVINCI_CODEC_RUNTIME_API void davinci_avcodec_flush_buffers(
    AVCodecContext *context) noexcept;

#ifdef __cplusplus
}
#endif
