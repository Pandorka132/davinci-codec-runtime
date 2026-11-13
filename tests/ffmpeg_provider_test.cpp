#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ffmpeg_provider.hpp"

#include <libavutil/error.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

using FindDecoderFn = const AVCodec *(*)(enum AVCodecID);
using FrameAllocFn = AVFrame *(*)(void);
using FrameFreeFn = void (*)(AVFrame **);
using FrameUnrefFn = void (*)(AVFrame *);
using PacketAllocFn = AVPacket *(*)(void);
using PacketFreeFn = void (*)(AVPacket **);
using PacketNewFn = int (*)(AVPacket *, int);
using PacketUnrefFn = void (*)(AVPacket *);

void *open_library(const std::string &path) {
    dlerror();
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
        const char *error = dlerror();
        std::fprintf(stderr, "dlopen %s failed: %s\n", path.c_str(),
                     error != nullptr ? error : "unknown error");
    }
    return handle;
}

template <typename Function>
Function versioned_symbol(void *handle, const char *name,
                          const char *version) {
    dlerror();
    void *address = dlvsym(handle, name, version);
    const char *error = dlerror();
    if (address == nullptr || error != nullptr) {
        std::fprintf(stderr, "%s@%s lookup failed: %s\n", name, version,
                     error != nullptr ? error : "null symbol");
        std::exit(2);
    }
    return reinterpret_cast<Function>(address);
}

bool accepted_drain_result(int result) {
    return result >= 0 || result == AVERROR_EOF || result == AVERROR(EAGAIN);
}

bool exercise_custom_context(const AVCodec *codec,
                             FrameAllocFn frame_alloc,
                             FrameFreeFn frame_free) {
    AVFrame *frame = frame_alloc();
    if (frame == nullptr) return false;
    AVCodecContext *context = davinci_avcodec_alloc_context3(codec);
    if (context == nullptr) {
        frame_free(&frame);
        return false;
    }

    bool okay = context->codec == codec;
    bool opened = false;
    if (okay) {
        okay = davinci_avcodec_open2(context, codec, nullptr) >= 0;
        opened = okay;
    }
    if (okay) {
        davinci_avcodec_flush_buffers(context);
        const int send_result = davinci_avcodec_send_packet(context, nullptr);
        const int receive_result = davinci_avcodec_receive_frame(context, frame);
        okay = accepted_drain_result(send_result) &&
               accepted_drain_result(receive_result);
    }
    if (opened && davinci_avcodec_close(context) < 0) okay = false;
    if (opened && context->codec != nullptr) okay = false;

    davinci_avcodec_free_context(&context);
    frame_free(&frame);
    return okay && context == nullptr && frame == nullptr;
}

bool decode_ivf_through_provider(const std::string &path,
                                 const AVCodec *codec,
                                 FrameAllocFn frame_alloc,
                                 FrameFreeFn frame_free,
                                 FrameUnrefFn frame_unref,
                                 PacketAllocFn packet_alloc,
                                 PacketFreeFn packet_free,
                                 PacketNewFn packet_new,
                                 PacketUnrefFn packet_unref) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "cannot open AV1 IVF sample: %s\n", path.c_str());
        return false;
    }

    uint8_t header[32]{};
    input.read(reinterpret_cast<char *>(header), sizeof(header));
    if (input.gcount() != static_cast<std::streamsize>(sizeof(header)) ||
        std::memcmp(header, "DKIF", 4) != 0 ||
        std::memcmp(header + 8, "AV01", 4) != 0) {
        std::fprintf(stderr, "invalid IVF/AV1 header: %s\n", path.c_str());
        return false;
    }

    const auto read_le16 = [](const uint8_t *p) -> unsigned int {
        return static_cast<unsigned int>(p[0]) |
               (static_cast<unsigned int>(p[1]) << 8U);
    };
    const auto read_le32 = [](const uint8_t *p) -> uint32_t {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8U) |
               (static_cast<uint32_t>(p[2]) << 16U) |
               (static_cast<uint32_t>(p[3]) << 24U);
    };
    const auto read_le64 = [&read_le32](const uint8_t *p) -> uint64_t {
        return static_cast<uint64_t>(read_le32(p)) |
               (static_cast<uint64_t>(read_le32(p + 4)) << 32U);
    };

    const unsigned int header_size = read_le16(header + 6);
    const unsigned int width = read_le16(header + 12);
    const unsigned int height = read_le16(header + 14);
    if (header_size < sizeof(header) || width == 0U || height == 0U) {
        std::fprintf(stderr, "invalid IVF header size/dimensions: %u x %u\n",
                     width, height);
        return false;
    }
    input.seekg(static_cast<std::streamoff>(header_size), std::ios::beg);
    if (!input) return false;

    AVCodecContext *context = davinci_avcodec_alloc_context3(codec);
    AVFrame *frame = frame_alloc();
    AVPacket *packet = packet_alloc();
    if (context == nullptr || frame == nullptr || packet == nullptr) {
        if (context != nullptr) davinci_avcodec_free_context(&context);
        if (frame != nullptr) frame_free(&frame);
        if (packet != nullptr) packet_free(&packet);
        return false;
    }

    context->width = static_cast<int>(width);
    context->height = static_cast<int>(height);
    context->thread_count = 1;
    context->thread_type = 0;
    if (davinci_avcodec_open2(context, codec, nullptr) < 0) {
        std::fprintf(stderr, "private AV1 open failed for %s\n", path.c_str());
        packet_free(&packet);
        frame_free(&frame);
        davinci_avcodec_free_context(&context);
        return false;
    }

    unsigned int packet_count = 0;
    unsigned int frame_count = 0;
    bool okay = true;
    const auto drain = [&]() -> bool {
        for (;;) {
            const int result = davinci_avcodec_receive_frame(context, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
            if (result < 0) return false;
            ++frame_count;
            if (frame->width != static_cast<int>(width) ||
                frame->height != static_cast<int>(height) ||
                frame->format == AV_PIX_FMT_NONE) {
                std::fprintf(stderr, "decoded AVFrame has invalid properties: %dx%d format=%d\n",
                             frame->width, frame->height, frame->format);
                return false;
            }
            frame_unref(frame);
        }
    };

    for (;;) {
        uint8_t record_header[12];
        input.read(reinterpret_cast<char *>(record_header), sizeof(record_header));
        const std::streamsize header_read = input.gcount();
        if (header_read == 0 && input.eof()) break;
        if (header_read != static_cast<std::streamsize>(sizeof(record_header))) {
            okay = false;
            break;
        }

        const uint32_t packet_size = read_le32(record_header);
        const uint64_t timestamp = read_le64(record_header + 4);
        if (packet_size == 0U || packet_size > 16U * 1024U * 1024U ||
            packet_size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
            okay = false;
            break;
        }
        if (packet_new(packet, static_cast<int>(packet_size)) < 0) {
            okay = false;
            break;
        }
        input.read(reinterpret_cast<char *>(packet->data),
                   static_cast<std::streamsize>(packet_size));
        if (input.gcount() != static_cast<std::streamsize>(packet_size)) {
            packet_unref(packet);
            okay = false;
            break;
        }
        packet->pts = static_cast<int64_t>(timestamp);
        packet->dts = static_cast<int64_t>(timestamp);
        packet->stream_index = 0;
        packet->duration = 1;
        if (packet_count == 0U) packet->flags |= AV_PKT_FLAG_KEY;

        int send_result = davinci_avcodec_send_packet(context, packet);
        if (send_result == AVERROR(EAGAIN)) {
            if (!drain()) {
                packet_unref(packet);
                okay = false;
                break;
            }
            send_result = davinci_avcodec_send_packet(context, packet);
        }
        packet_unref(packet);
        if (send_result < 0 || !drain()) {
            okay = false;
            break;
        }
        ++packet_count;
    }

    if (okay && packet_count != 0U) {
        const int flush_result = davinci_avcodec_send_packet(context, nullptr);
        if (flush_result < 0 && flush_result != AVERROR_EOF)
            okay = false;
        if (okay && !drain()) okay = false;
    }

    if (davinci_avcodec_close(context) < 0) okay = false;
    packet_free(&packet);
    frame_free(&frame);
    davinci_avcodec_free_context(&context);
    if (packet != nullptr || frame != nullptr || context != nullptr)
        okay = false;
    if (packet_count == 0U || frame_count == 0U) okay = false;

    if (!okay) {
        std::fprintf(stderr, "IVF decode failed: %s packets=%u frames=%u\n",
                     path.c_str(), packet_count, frame_count);
        return false;
    }
    std::printf("PASS: %s decoded %u packets into %u frames (%ux%u)\n",
                path.c_str(), packet_count, frame_count, width, height);
    return true;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: %s <resolve-libs-dir> <private-ffmpeg-root> <av1.ivf>\n",
                     argv[0]);
        return 2;
    }

    const std::string resolve_root(argv[1]);
    const std::string private_root(argv[2]);
    const std::string sample_path(argv[3]);
    void *resolve_util = open_library(resolve_root + "/libavutil.so.58");
    if (resolve_util == nullptr) return 3;
    void *resolve_codec = open_library(resolve_root + "/libavcodec.so.60");
    if (resolve_codec == nullptr) return 4;

    const auto resolve_find = versioned_symbol<FindDecoderFn>(
        resolve_codec, "avcodec_find_decoder", "LIBAVCODEC_60");
    const auto resolve_frame_alloc = versioned_symbol<FrameAllocFn>(
        resolve_util, "av_frame_alloc", "LIBAVUTIL_58");
    const auto resolve_frame_free = versioned_symbol<FrameFreeFn>(
        resolve_util, "av_frame_free", "LIBAVUTIL_58");
    const auto resolve_frame_unref = versioned_symbol<FrameUnrefFn>(
        resolve_util, "av_frame_unref", "LIBAVUTIL_58");
    const auto resolve_packet_alloc = versioned_symbol<PacketAllocFn>(
        resolve_codec, "av_packet_alloc", "LIBAVCODEC_60");
    const auto resolve_packet_free = versioned_symbol<PacketFreeFn>(
        resolve_codec, "av_packet_free", "LIBAVCODEC_60");
    const auto resolve_packet_new = versioned_symbol<PacketNewFn>(
        resolve_codec, "av_new_packet", "LIBAVCODEC_60");
    const auto resolve_packet_unref = versioned_symbol<PacketUnrefFn>(
        resolve_codec, "av_packet_unref", "LIBAVCODEC_60");

    if (davinci_codec_runtime_initialize_ffmpeg(private_root.c_str()) != 0) {
        std::fprintf(stderr, "provider initialization failed: %s\n",
                     davinci_codec_runtime_ffmpeg_error());
        return 5;
    }
    if (davinci_codec_runtime_ffmpeg_ready() != 1) return 6;
    const unsigned int private_version =
        davinci_codec_runtime_private_avcodec_version();
    if (private_version != 3940198U) {
        std::fprintf(stderr, "unexpected private libavcodec version: %u\n",
                     private_version);
        return 7;
    }

    const AVCodec *custom_av1 = davinci_avcodec_find_decoder(AV_CODEC_ID_AV1);
    const AVCodec *resolve_vp9 = resolve_find(AV_CODEC_ID_VP9);
    const AVCodec *dispatched_vp9 = davinci_avcodec_find_decoder(AV_CODEC_ID_VP9);
    if (custom_av1 == nullptr || custom_av1->id != AV_CODEC_ID_AV1 ||
        std::string(custom_av1->name) != "libdav1d") {
        std::fprintf(stderr, "AV1 did not resolve to the private libdav1d decoder\n");
        return 8;
    }
    if (resolve_vp9 == nullptr || dispatched_vp9 != resolve_vp9) {
        std::fprintf(stderr, "non-AV1 lookup did not preserve Resolve provider identity\n");
        return 9;
    }

    if (!exercise_custom_context(custom_av1, resolve_frame_alloc,
                                 resolve_frame_free)) {
        std::fprintf(stderr, "custom AV1 context lifecycle failed\n");
        return 10;
    }

    if (!decode_ivf_through_provider(sample_path, custom_av1,
                                     resolve_frame_alloc, resolve_frame_free,
                                     resolve_frame_unref, resolve_packet_alloc,
                                     resolve_packet_free, resolve_packet_new,
                                     resolve_packet_unref))
        return 10;

    AVFrame *resolve_frame = resolve_frame_alloc();
    if (resolve_frame == nullptr) return 11;

    AVCodecContext *resolve_context = davinci_avcodec_alloc_context3(resolve_vp9);
    if (resolve_context == nullptr || resolve_context->codec != resolve_vp9 ||
        davinci_avcodec_open2(resolve_context, resolve_vp9, nullptr) < 0) {
        if (resolve_context != nullptr)
            davinci_avcodec_free_context(&resolve_context);
        resolve_frame_free(&resolve_frame);
        return 12;
    }
    davinci_avcodec_flush_buffers(resolve_context);
    const int resolve_send = davinci_avcodec_send_packet(resolve_context, nullptr);
    const int resolve_receive = davinci_avcodec_receive_frame(resolve_context, resolve_frame);
    if (!accepted_drain_result(resolve_send) ||
        !accepted_drain_result(resolve_receive) ||
        davinci_avcodec_close(resolve_context) < 0) {
        davinci_avcodec_free_context(&resolve_context);
        resolve_frame_free(&resolve_frame);
        return 13;
    }
    davinci_avcodec_free_context(&resolve_context);
    if (resolve_context != nullptr) {
        resolve_frame_free(&resolve_frame);
        return 14;
    }

    std::atomic<unsigned int> failures{0};
    std::vector<std::thread> workers;
    for (unsigned int worker = 0; worker < 4U; ++worker) {
        workers.emplace_back([&]() {
            for (unsigned int iteration = 0; iteration < 8U; ++iteration) {
                if (!exercise_custom_context(custom_av1, resolve_frame_alloc,
                                             resolve_frame_free))
                    failures.fetch_add(1U, std::memory_order_relaxed);
            }
        });
    }
    for (auto &worker : workers) worker.join();
    resolve_frame_free(&resolve_frame);
    if (failures.load(std::memory_order_relaxed) != 0U) {
        std::fprintf(stderr, "concurrent custom-context failures: %u\n",
                     failures.load(std::memory_order_relaxed));
        return 15;
    }

    std::printf("PASS: private FFmpeg version %u routes AV1 to %s\n",
                private_version, custom_av1->name);
    std::puts("PASS: VP9 codec identity and context operations stay on Resolve FFmpeg60");
    std::puts("PASS: AV1 allocation/open/flush/send/receive/close/free use the private provider");
    std::puts("PASS: Resolve-allocated AVFrame crosses the private decoder API boundary");
    std::puts("PASS: 4-thread custom-context lifecycle stress (32 contexts)");
    std::puts("LIMIT: direct provider-layer test only; Resolve call-site patches and callback are not installed");

    (void)dlclose(resolve_codec);
    (void)dlclose(resolve_util);
    return 0;
}
