#define _GNU_SOURCE

#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define PROBE_EXPORT __attribute__((visibility("default")))
#else
#define PROBE_EXPORT
#endif

typedef const AVCodec *(*FindDecoderFn)(enum AVCodecID);
typedef AVCodecContext *(*AllocContextFn)(const AVCodec *);
typedef int (*OpenContextFn)(AVCodecContext *, const AVCodec *, AVDictionary **);
typedef int (*CloseContextFn)(AVCodecContext *);
typedef void (*FreeContextFn)(AVCodecContext **);
typedef int (*SendPacketFn)(AVCodecContext *, const AVPacket *);
typedef int (*ReceiveFrameFn)(AVCodecContext *, AVFrame *);
typedef void (*FlushBuffersFn)(AVCodecContext *);
typedef AVFrame *(*FrameAllocFn)(void);
typedef void (*FrameFreeFn)(AVFrame **);
typedef void (*FrameUnrefFn)(AVFrame *);
typedef AVPacket *(*PacketAllocFn)(void);
typedef void (*PacketFreeFn)(AVPacket **);
typedef int (*NewPacketFn)(AVPacket *, int);
typedef void (*PacketUnrefFn)(AVPacket *);

static void *provider_handle;

typedef struct ProbeApi {
    FindDecoderFn resolve_find_decoder;
    AllocContextFn resolve_alloc_context;
    FrameAllocFn frame_alloc;
    FrameFreeFn frame_free;
    FrameUnrefFn frame_unref;
    PacketAllocFn packet_alloc;
    PacketFreeFn packet_free;
    NewPacketFn packet_new;
    PacketUnrefFn packet_unref;
    FindDecoderFn routed_find_decoder;
    AllocContextFn routed_alloc_context;
    OpenContextFn routed_open;
    CloseContextFn routed_close;
    FreeContextFn routed_free;
    SendPacketFn routed_send;
    ReceiveFrameFn routed_receive;
    FlushBuffersFn routed_flush;
} ProbeApi;

_Static_assert(sizeof(FindDecoderFn) == sizeof(void *),
               "POSIX function and object pointers must have equal size");

static void load_function(void *raw, void *destination, size_t destination_size)
{
    if (destination_size == sizeof(raw))
        memcpy(destination, &raw, sizeof(raw));
}

static void *versioned_symbol(const char *name, const char *version)
{
    dlerror();
    return dlvsym(RTLD_DEFAULT, name, version);
}

PROBE_EXPORT int davinci_resolve_live_probe_bind_provider(void *handle)
{
    if (handle == NULL)
        return -1;
    provider_handle = handle;
    return 0;
}

static int load_probe_api(ProbeApi *api)
{
    void *raw;
    if (provider_handle == NULL)
        return 0;
    memset(api, 0, sizeof(*api));

#define LOAD_VERSIONED(field, name, version) do { \
    raw = versioned_symbol((name), (version)); \
    load_function(raw, &api->field, sizeof(api->field)); \
} while (0)
#define LOAD_DEFAULT(field, name) do { \
    raw = dlsym(provider_handle, (name)); \
    load_function(raw, &api->field, sizeof(api->field)); \
} while (0)

    LOAD_VERSIONED(resolve_find_decoder, "avcodec_find_decoder", "LIBAVCODEC_60");
    LOAD_VERSIONED(resolve_alloc_context, "avcodec_alloc_context3", "LIBAVCODEC_60");
    LOAD_VERSIONED(frame_alloc, "av_frame_alloc", "LIBAVUTIL_58");
    LOAD_VERSIONED(frame_free, "av_frame_free", "LIBAVUTIL_58");
    LOAD_VERSIONED(frame_unref, "av_frame_unref", "LIBAVUTIL_58");
    LOAD_VERSIONED(packet_alloc, "av_packet_alloc", "LIBAVCODEC_60");
    LOAD_VERSIONED(packet_free, "av_packet_free", "LIBAVCODEC_60");
    LOAD_VERSIONED(packet_new, "av_new_packet", "LIBAVCODEC_60");
    LOAD_VERSIONED(packet_unref, "av_packet_unref", "LIBAVCODEC_60");

    LOAD_DEFAULT(routed_find_decoder, "davinci_avcodec_find_decoder");
    LOAD_DEFAULT(routed_alloc_context, "davinci_avcodec_alloc_context3");
    LOAD_DEFAULT(routed_open, "davinci_avcodec_open2");
    LOAD_DEFAULT(routed_close, "davinci_avcodec_close");
    LOAD_DEFAULT(routed_free, "davinci_avcodec_free_context");
    LOAD_DEFAULT(routed_send, "davinci_avcodec_send_packet");
    LOAD_DEFAULT(routed_receive, "davinci_avcodec_receive_frame");
    LOAD_DEFAULT(routed_flush, "davinci_avcodec_flush_buffers");

#undef LOAD_VERSIONED
#undef LOAD_DEFAULT

    return api->resolve_find_decoder != NULL &&
           api->resolve_alloc_context != NULL &&
           api->frame_alloc != NULL && api->frame_free != NULL &&
           api->frame_unref != NULL && api->packet_alloc != NULL &&
           api->packet_free != NULL && api->packet_new != NULL &&
           api->packet_unref != NULL && api->routed_find_decoder != NULL &&
           api->routed_alloc_context != NULL && api->routed_open != NULL &&
           api->routed_close != NULL && api->routed_free != NULL &&
           api->routed_send != NULL && api->routed_receive != NULL &&
           api->routed_flush != NULL;
}

static int accepted_drain_result(int result)
{
    return result >= 0 || result == AVERROR_EOF || result == AVERROR(EAGAIN);
}

static int drain_frames(const ProbeApi *api, AVCodecContext *context,
                        AVFrame *frame, unsigned int width,
                        unsigned int height, unsigned int *frame_count)
{
    for (;;) {
        const int result = api->routed_receive(context, frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return 0;
        if (result < 0)
            return result;

        if (frame->width != (int)width || frame->height != (int)height ||
            frame->format == AV_PIX_FMT_NONE || frame->data[0] == NULL) {
            api->frame_unref(frame);
            return AVERROR_INVALIDDATA;
        }

        ++(*frame_count);
        api->frame_unref(frame);
    }
}

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t read_le64(const uint8_t *bytes)
{
    return (uint64_t)read_le32(bytes) |
           ((uint64_t)read_le32(bytes + 4) << 32U);
}

static int decode_ivf_in_resolve(const ProbeApi *api, const char *path)
{
    FILE *input = NULL;
    uint8_t *file_data = NULL;
    AVCodecContext *context = NULL;
    AVFrame *frame = NULL;
    AVPacket *packet = NULL;
    int opened = 0;
    int okay = 0;
    int failure = 0;
    unsigned int packet_count = 0;
    unsigned int frame_count = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    size_t file_size = 0;
    size_t offset = 0;

    input = fopen(path, "rb");
    if (input == NULL || fseek(input, 0, SEEK_END) != 0) {
        failure = 1;
        goto cleanup;
    }
    {
        const long end = ftell(input);
        if (end < 44 || end > 16L * 1024L * 1024L ||
            fseek(input, 0, SEEK_SET) != 0) {
            failure = 2;
            goto cleanup;
        }
        file_size = (size_t)end;
    }

    file_data = (uint8_t *)malloc(file_size);
    if (file_data == NULL ||
        fread(file_data, 1, file_size, input) != file_size) {
        failure = 3;
        goto cleanup;
    }
    if (memcmp(file_data, "DKIF", 4) != 0 ||
        memcmp(file_data + 8, "AV01", 4) != 0) {
        failure = 4;
        goto cleanup;
    }

    {
        const unsigned int header_size = read_le16(file_data + 6);
        width = read_le16(file_data + 12);
        height = read_le16(file_data + 14);
        if (header_size < 32U || header_size > file_size ||
            width == 0U || height == 0U) {
            failure = 5;
            goto cleanup;
        }
        offset = header_size;
    }

    {
        const AVCodec *codec = api->routed_find_decoder(AV_CODEC_ID_AV1);
        if (codec == NULL || codec->id != AV_CODEC_ID_AV1 ||
            codec->name == NULL || strcmp(codec->name, "libdav1d") != 0) {
            failure = 6;
            goto cleanup;
        }
        context = api->routed_alloc_context(codec);
    }
    frame = api->frame_alloc();
    packet = api->packet_alloc();
    if (context == NULL || frame == NULL || packet == NULL) {
        failure = 7;
        goto cleanup;
    }
    context->width = (int)width;
    context->height = (int)height;
    context->thread_count = 1;
    context->thread_type = 0;
    if (api->routed_open(context, context->codec, NULL) < 0) {
        failure = 8;
        goto cleanup;
    }
    opened = 1;

    while (offset < file_size) {
        uint32_t packet_size;
        uint64_t timestamp;
        int send_result;

        if (file_size - offset < 12U) {
            failure = 9;
            goto cleanup;
        }
        packet_size = read_le32(file_data + offset);
        timestamp = read_le64(file_data + offset + 4U);
        offset += 12U;
        if (packet_size == 0U || packet_size > 16U * 1024U * 1024U ||
            (size_t)packet_size > file_size - offset ||
            packet_size > (uint32_t)INT32_MAX) {
            failure = 10;
            goto cleanup;
        }

        api->packet_unref(packet);
        if (api->packet_new(packet, (int)packet_size) < 0) {
            failure = 11;
            goto cleanup;
        }
        memcpy(packet->data, file_data + offset, packet_size);
        packet->pts = (int64_t)timestamp;
        packet->dts = (int64_t)timestamp;
        packet->stream_index = 0;
        packet->duration = 1;
        if (packet_count == 0U)
            packet->flags |= AV_PKT_FLAG_KEY;
        offset += packet_size;

        send_result = api->routed_send(context, packet);
        if (send_result == AVERROR(EAGAIN)) {
            if (drain_frames(api, context, frame, width, height,
                             &frame_count) < 0) {
                failure = 12;
                goto cleanup;
            }
            send_result = api->routed_send(context, packet);
        }
        api->packet_unref(packet);
        if (send_result < 0 ||
            drain_frames(api, context, frame, width, height,
                         &frame_count) < 0) {
            failure = 13;
            goto cleanup;
        }
        ++packet_count;
    }

    {
        int flush_result = api->routed_send(context, NULL);
        if (flush_result == AVERROR(EAGAIN)) {
            if (drain_frames(api, context, frame, width, height,
                             &frame_count) < 0) {
                failure = 14;
                goto cleanup;
            }
            flush_result = api->routed_send(context, NULL);
        }
        if ((flush_result < 0 && flush_result != AVERROR_EOF) ||
            drain_frames(api, context, frame, width, height,
                         &frame_count) < 0) {
            failure = 15;
            goto cleanup;
        }
    }

    if (packet_count == 0U || frame_count == 0U) {
        failure = 16;
        goto cleanup;
    }
    okay = 1;

cleanup:
    if (opened && api->routed_close(context) < 0) {
        okay = 0;
        if (failure == 0)
            failure = 17;
    }
    if (packet != NULL)
        api->packet_free(&packet);
    if (frame != NULL)
        api->frame_free(&frame);
    if (context != NULL)
        api->routed_free(&context);
    if (packet != NULL || frame != NULL || context != NULL) {
        okay = 0;
        if (failure == 0)
            failure = 18;
    }
    if (input != NULL)
        fclose(input);
    free(file_data);

    if (okay) {
        printf("RESOLVE_AV1_DECODE_SAMPLE_PASS %s %ux%u packets=%u frames=%u\n",
               path, width, height, packet_count, frame_count);
        fflush(stdout);
        return 0;
    }
    fprintf(stderr, "RESOLVE_AV1_DECODE_SAMPLE_FAIL %s stage=%d packets=%u frames=%u\n",
            path, failure, packet_count, frame_count);
    fflush(stderr);
    return failure != 0 ? failure : 19;
}

PROBE_EXPORT int davinci_resolve_live_vp9_context_probe(void)
{
    ProbeApi api;
    const AVCodec *codec;
    AVCodecContext *context = NULL;
    AVFrame *frame = NULL;
    int opened = 0;
    int okay = 0;

    if (!load_probe_api(&api))
        return 1;
    codec = api.resolve_find_decoder(AV_CODEC_ID_VP9);
    context = codec != NULL ? api.resolve_alloc_context(codec) : NULL;
    frame = api.frame_alloc();
    if (context == NULL || frame == NULL || context->codec != codec)
        goto cleanup;
    context->thread_count = 1;
    context->thread_type = 0;
    if (api.routed_open(context, codec, NULL) < 0)
        goto cleanup;
    opened = 1;
    api.routed_flush(context);
    if (!accepted_drain_result(api.routed_send(context, NULL)) ||
        !accepted_drain_result(api.routed_receive(context, frame)))
        goto cleanup;
    if (api.routed_close(context) < 0)
        goto cleanup;
    opened = 0;
    if (context->codec != NULL)
        goto cleanup;
    okay = 1;

cleanup:
    if (opened)
        (void)api.routed_close(context);
    if (context != NULL)
        api.routed_free(&context);
    if (frame != NULL)
        api.frame_free(&frame);
    if (okay && context == NULL && frame == NULL) {
        puts("RESOLVE_NATIVE_VP9_CONTEXT_PASS context-open/flush/drain/close/free");
        fflush(stdout);
        return 0;
    }
    fprintf(stderr, "RESOLVE_NATIVE_VP9_CONTEXT_FAIL\n");
    fflush(stderr);
    return 2;
}

PROBE_EXPORT int davinci_resolve_live_av1_decode_probe(const char *ivf_path)
{
    ProbeApi api;
    if (ivf_path == NULL || !load_probe_api(&api))
        return 1;
    return decode_ivf_in_resolve(&api, ivf_path);
}
