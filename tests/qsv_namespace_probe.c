#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>

typedef int (*HwDeviceCreateFn)(AVBufferRef **, enum AVHWDeviceType,
                                const char *, AVDictionary *, int);
typedef AVBufferRef *(*BufferRefFn)(const AVBufferRef *);
typedef void (*BufferUnrefFn)(AVBufferRef **);
typedef const AVCodec *(*FindDecoderByNameFn)(const char *);
typedef AVCodecContext *(*AllocContextFn)(const AVCodec *);
typedef int (*OpenFn)(AVCodecContext *, const AVCodec *, AVDictionary **);
typedef void (*FreeContextFn)(AVCodecContext **);
typedef AVPacket *(*PacketAllocFn)(void);
typedef int (*PacketNewFn)(AVPacket *, int);
typedef void (*PacketFreeFn)(AVPacket **);
typedef void (*PacketUnrefFn)(AVPacket *);
typedef AVFrame *(*FrameAllocFn)(void);
typedef void (*FrameFreeFn)(AVFrame **);
typedef void (*FrameUnrefFn)(AVFrame *);
typedef int (*SendPacketFn)(AVCodecContext *, const AVPacket *);
typedef int (*ReceiveFrameFn)(AVCodecContext *, AVFrame *);
typedef int (*FrameTransferFn)(AVFrame *, const AVFrame *, int);
typedef int (*DictSetFn)(AVDictionary **, const char *, const char *, int);
typedef void (*DictFreeFn)(AVDictionary **);
typedef const char *(*PixelFormatNameFn)(enum AVPixelFormat);
typedef unsigned int (*VersionFn)(void);

static void *versioned(void *handle, const char *name, const char *version) {
    dlerror();
    void *p = dlvsym(handle, name, version);
    const char *e = dlerror();
    if (!p || e) {
        fprintf(stderr, "%s@%s: %s\n", name, version,
                e ? e : "null symbol");
        exit(2);
    }
    return p;
}

#define LOAD(dst, handle, name, version) \
    dst = (__typeof__(dst))versioned(handle, name, version)

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int drain(AVCodecContext *ctx, AVFrame *hw, AVFrame *sw,
                 ReceiveFrameFn receive, FrameTransferFn transfer,
                 FrameUnrefFn unref, PixelFormatNameFn pixel_format_name,
                 unsigned *frames) {
    for (;;) {
        int ret = receive(ctx, hw);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
        if (ret < 0) {
            fprintf(stderr, "receive_frame: %d\n", ret);
            return -1;
        }
        AVFrame *output = hw;
        if (hw->hw_frames_ctx || hw->format == AV_PIX_FMT_QSV) {
            ret = transfer(sw, hw, 0);
            if (ret < 0) {
                fprintf(stderr, "hwframe transfer: %d\n", ret);
                unref(hw);
                return -1;
            }
            output = sw;
        }
        printf("frame=%u format=%s size=%dx%d hw=%s\n", *frames,
               pixel_format_name((enum AVPixelFormat)output->format),
               output->width, output->height,
               output->hw_frames_ctx ? "yes" : "no");
        ++*frames;
        unref(hw);
        unref(sw);
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <private-ffmpeg-root> <render-node> <av1.ivf>\n",
                argv[0]);
        return 2;
    }

    char util_path[4096], codec_path[4096];
    snprintf(util_path, sizeof(util_path), "%s/lib/libavutil.so.58", argv[1]);
    snprintf(codec_path, sizeof(codec_path), "%s/lib/libavcodec.so.60", argv[1]);

    void *resolve_util = dlopen("/opt/resolve/libs/libavutil.so.58", RTLD_NOW | RTLD_GLOBAL);
    void *resolve_codec = dlopen("/opt/resolve/libs/libavcodec.so.60", RTLD_NOW | RTLD_GLOBAL);
    if (!resolve_util || !resolve_codec) {
        fprintf(stderr, "loading Resolve FFmpeg: %s\n", dlerror()); return 2;
    }
    void *util = dlopen(util_path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!util) { fprintf(stderr, "dlopen private util: %s\n", dlerror()); return 2; }
    void *codec = dlopen(codec_path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!codec) { fprintf(stderr, "dlopen private codec: %s\n", dlerror()); return 2; }

    HwDeviceCreateFn hw_create;
    BufferRefFn buffer_ref;
    BufferUnrefFn buffer_unref;
    FindDecoderByNameFn find_decoder;
    AllocContextFn alloc_context;
    OpenFn open_codec;
    FreeContextFn free_context;
    PacketAllocFn packet_alloc;
    PacketNewFn packet_new;
    PacketFreeFn packet_free;
    PacketUnrefFn packet_unref;
    FrameAllocFn frame_alloc;
    FrameFreeFn frame_free;
    FrameUnrefFn frame_unref;
    SendPacketFn send_packet;
    ReceiveFrameFn receive_frame;
    FrameTransferFn transfer_frame;
    DictSetFn dict_set;
    DictFreeFn dict_free;
    PixelFormatNameFn pixel_format_name;
    VersionFn codec_version;

    LOAD(hw_create, util, "av_hwdevice_ctx_create", "LIBAVUTIL_58");
    LOAD(buffer_ref, util, "av_buffer_ref", "LIBAVUTIL_58");
    LOAD(buffer_unref, util, "av_buffer_unref", "LIBAVUTIL_58");
    LOAD(transfer_frame, util, "av_hwframe_transfer_data", "LIBAVUTIL_58");
    LOAD(dict_set, util, "av_dict_set", "LIBAVUTIL_58");
    LOAD(dict_free, util, "av_dict_free", "LIBAVUTIL_58");
    LOAD(pixel_format_name, util, "av_get_pix_fmt_name", "LIBAVUTIL_58");
    LOAD(codec_version, codec, "avcodec_version", "LIBAVCODEC_60");
    LOAD(find_decoder, codec, "avcodec_find_decoder_by_name", "LIBAVCODEC_60");
    LOAD(alloc_context, codec, "avcodec_alloc_context3", "LIBAVCODEC_60");
    LOAD(open_codec, codec, "avcodec_open2", "LIBAVCODEC_60");
    LOAD(free_context, codec, "avcodec_free_context", "LIBAVCODEC_60");
    LOAD(packet_alloc, codec, "av_packet_alloc", "LIBAVCODEC_60");
    LOAD(packet_new, codec, "av_new_packet", "LIBAVCODEC_60");
    LOAD(packet_free, codec, "av_packet_free", "LIBAVCODEC_60");
    LOAD(packet_unref, codec, "av_packet_unref", "LIBAVCODEC_60");
    LOAD(frame_alloc, util, "av_frame_alloc", "LIBAVUTIL_58");
    LOAD(frame_free, util, "av_frame_free", "LIBAVUTIL_58");
    LOAD(frame_unref, util, "av_frame_unref", "LIBAVUTIL_58");
    LOAD(send_packet, codec, "avcodec_send_packet", "LIBAVCODEC_60");
    LOAD(receive_frame, codec, "avcodec_receive_frame", "LIBAVCODEC_60");

    AVBufferRef *device = NULL;
    int ret = 0;
    if (strcmp(argv[2], "-") != 0) {
        printf("private avcodec version=%u\n", codec_version());
        AVDictionary *opts = NULL;
        dict_set(&opts, "child_device_type", "vaapi", 0);
        dict_set(&opts, "child_device", argv[2], 0);
        ret = hw_create(&device, AV_HWDEVICE_TYPE_QSV, "hw", opts, 0);
        dict_free(&opts);
        if (ret < 0 || !device) {
            fprintf(stderr, "QSV device creation failed: %d\n", ret);
            return 3;
        }
        puts("QSV device created in private FFmpeg namespace");
    } else {
        puts("QSV device auto-selection mode");
    }

    const AVCodec *decoder = find_decoder("av1_qsv");
    if (!decoder) { fprintf(stderr, "av1_qsv decoder unavailable\n"); return 3; }
    AVCodecContext *ctx = alloc_context(decoder);
    AVPacket *packet = packet_alloc();
    AVFrame *hw = frame_alloc();
    AVFrame *sw = frame_alloc();
    if (!ctx || !packet || !hw || !sw) return 3;
    if (device) ctx->hw_device_ctx = buffer_ref(device);
    ctx->thread_count = 1;

    FILE *input = fopen(argv[3], "rb");
    if (!input) { perror(argv[3]); return 2; }
    uint8_t ivf[32];
    if (fread(ivf, 1, sizeof(ivf), input) != sizeof(ivf) ||
        memcmp(ivf, "DKIF", 4) || memcmp(ivf + 8, "AV01", 4)) {
        fprintf(stderr, "invalid IVF/AV1 input\n"); return 2;
    }
    ctx->width = ivf[12] | ((int)ivf[13] << 8);
    ctx->height = ivf[14] | ((int)ivf[15] << 8);
    ctx->time_base.num = 1;
    ctx->time_base.den = ivf[24] | ((int)ivf[25] << 8) |
                         ((int)ivf[26] << 16) | ((int)ivf[27] << 24);
    ctx->pkt_timebase = ctx->time_base;

    ret = open_codec(ctx, decoder, NULL);
    if (ret < 0) {
        fprintf(stderr, "avcodec_open2 av1_qsv failed: %d\n", ret);
        return 4;
    }

    unsigned frames = 0;
    uint8_t frame_header[12];
    while (fread(frame_header, 1, sizeof(frame_header), input) == sizeof(frame_header)) {
        uint32_t size = read_le32(frame_header);
        if (size > 64U * 1024U * 1024U) { fprintf(stderr, "invalid IVF packet size\n"); return 2; }
        ret = packet_new(packet, (int)size);
        if (ret < 0 || fread(packet->data, 1, size, input) != size) {
            fprintf(stderr, "packet read/allocation failed\n"); return 2;
        }
        ret = send_packet(ctx, packet);
        packet_unref(packet);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            fprintf(stderr, "send_packet: %d\n", ret); return 4;
        }
        if (drain(ctx, hw, sw, receive_frame, transfer_frame,
                  frame_unref, pixel_format_name, &frames) != 0) return 4;
    }
    ret = send_packet(ctx, NULL);
    if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
        fprintf(stderr, "send EOF: %d\n", ret); return 4;
    }
    if (drain(ctx, hw, sw, receive_frame, transfer_frame,
              frame_unref, pixel_format_name, &frames) != 0) return 4;

    printf("QSV private-namespace decode complete: %u frames\n", frames);
    fclose(input);
    packet_free(&packet);
    frame_free(&hw);
    frame_free(&sw);
    free_context(&ctx);
    buffer_unref(&device);
    return frames == 6 ? 0 : 5;
}
