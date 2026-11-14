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
 * The private DSOs use unique libdavinci_* SONAMEs and are loaded with
 * RTLD_DEEPBIND in the base linker namespace so VA-API/oneVPL driver plugins
 * can load normally. Resolve's original FFmpeg60 provider is discovered in
 * RTLD_DEFAULT. QSV is preferred when one accessible Intel render node exists;
 * otherwise AV1 falls back to private libdav1d.
 */
DAVINCI_CODEC_RUNTIME_API int davinci_codec_runtime_initialize_ffmpeg(
    const char *private_ffmpeg_root) noexcept;
/*
 * Call only after the owning application has quiesced every caller and
 * released every AVCodec* returned by find_decoder, and all contexts have been
 * freed. The entry gate drains wrappers already executing and rejects new ones,
 * but cannot revoke a raw AVCodec* pointer after find_decoder returns. Returns
 * -2 while any private AVCodecContext remains registered, and -3 if a private
 * dlclose fails. A successful shutdown is not a substitute for caller-side
 * global quiescence.
 */
DAVINCI_CODEC_RUNTIME_API int davinci_codec_runtime_shutdown_ffmpeg() noexcept;
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
