#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ffmpeg_provider.hpp"

#include <libavutil/error.h>

#include <atomic>
#include <cstdio>
#include <cstring>
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
using MalloczFn = void *(*)(size_t);
using FreepFn = void (*)(void *);
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

bool exercise_resolve_owned_extradata(const AVCodec *codec,
                                       MalloczFn resolve_mallocz,
                                       FreepFn resolve_freep) {
    constexpr size_t payload_size = 18U;

    AVCodecContext *unopened = davinci_avcodec_alloc_context3(codec);
    auto *unopened_foreign = static_cast<uint8_t *>(resolve_mallocz(
        payload_size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (unopened == nullptr || unopened_foreign == nullptr) {
        if (unopened != nullptr) davinci_avcodec_free_context(&unopened);
        if (unopened_foreign != nullptr) resolve_freep(&unopened_foreign);
        return false;
    }
    unopened->extradata = unopened_foreign;
    unopened->extradata_size = static_cast<int>(payload_size);
    davinci_avcodec_free_context(&unopened);
    if (unopened != nullptr) return false;

    uint8_t expected[payload_size]{};
    auto *foreign = static_cast<uint8_t *>(resolve_mallocz(
        payload_size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (foreign == nullptr) return false;
    for (size_t index = 0; index < payload_size; ++index)
        expected[index] = static_cast<uint8_t>(index * 7U + 3U);
    std::memcpy(foreign, expected, payload_size);

    AVCodecContext *context = davinci_avcodec_alloc_context3(codec);
    if (context == nullptr) {
        resolve_freep(&foreign);
        return false;
    }

    context->width = 128;
    context->height = 72;
    context->thread_count = 1;
    context->thread_type = 0;
    context->time_base = AVRational{1, 30};
    context->pkt_timebase = context->time_base;
    context->extradata = foreign;
    context->extradata_size = static_cast<int>(payload_size);

    (void)davinci_avcodec_open2(context, codec, nullptr);
    const bool pointer_rehomed =
        context->extradata != nullptr && context->extradata != foreign;
    bool payload_preserved = pointer_rehomed &&
        std::memcmp(context->extradata, expected, payload_size) == 0;
    if (payload_preserved) {
        for (size_t index = 0; index < AV_INPUT_BUFFER_PADDING_SIZE; ++index) {
            if (context->extradata[payload_size + index] != 0U) {
                payload_preserved = false;
                break;
            }
        }
    }

    (void)davinci_avcodec_close(context);
    davinci_avcodec_free_context(&context);
    return pointer_rehomed && payload_preserved && context == nullptr;
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

    context->width = 128;
    context->height = 72;
    context->thread_count = 1;
    context->thread_type = 0;
    context->time_base = AVRational{1, 30};
    context->pkt_timebase = context->time_base;
    bool okay = context->codec == codec;
    if (okay && std::string(codec->name) == "av1_qsv") {
        davinci_avcodec_free_context(&context);
        frame_free(&frame);
        return context == nullptr && frame == nullptr;
    }
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



bool exercise_shared_context_concurrency(
    const AVCodec *codec, FrameAllocFn frame_alloc,
    FrameFreeFn frame_free, FrameUnrefFn frame_unref) {
    AVCodecContext *context = davinci_avcodec_alloc_context3(codec);
    if (context == nullptr) return false;
    context->width = 128;
    context->height = 72;
    context->thread_count = 1;
    context->thread_type = 0;
    context->time_base = AVRational{1, 30};
    context->pkt_timebase = context->time_base;
    if (davinci_avcodec_open2(context, codec, nullptr) < 0) {
        davinci_avcodec_free_context(&context);
        return false;
    }

    constexpr unsigned int worker_count = 4U;
    constexpr unsigned int iterations = 64U;
    std::atomic<unsigned int> ready{0U};
    std::atomic<bool> start{false};
    std::atomic<unsigned int> failures{0U};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (unsigned int worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&]() {
            AVFrame *frame = frame_alloc();
            if (frame == nullptr) {
                failures.fetch_add(1U, std::memory_order_relaxed);
                ready.fetch_add(1U, std::memory_order_release);
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                return;
            }

            ready.fetch_add(1U, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (unsigned int iteration = 0; iteration < iterations; ++iteration) {
                davinci_avcodec_flush_buffers(context);
                const int send_result =
                    davinci_avcodec_send_packet(context, nullptr);
                const int receive_result =
                    davinci_avcodec_receive_frame(context, frame);
                if (!accepted_drain_result(send_result) ||
                    !accepted_drain_result(receive_result)) {
                    failures.fetch_add(1U, std::memory_order_relaxed);
                }
                frame_unref(frame);
            }
            frame_free(&frame);
        });
    }

    while (ready.load(std::memory_order_acquire) != worker_count)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto &worker : workers) worker.join();

    bool okay = failures.load(std::memory_order_relaxed) == 0U;
    if (davinci_avcodec_close(context) < 0) okay = false;
    davinci_avcodec_free_context(&context);
    return okay && context == nullptr;
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
    context->time_base = AVRational{1, 30};
    context->pkt_timebase = context->time_base;
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
            if (frame_count == 1U) {
                std::printf("FRAME_LAYOUT: %dx%d format=%d linesize=[%d,%d,%d] opaque=%p\n",
                            frame->width, frame->height, frame->format,
                            frame->linesize[0], frame->linesize[1],
                            frame->linesize[2], frame->opaque);
            }
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
    const auto resolve_mallocz = versioned_symbol<MalloczFn>(
        resolve_util, "av_mallocz", "LIBAVUTIL_58");
    const auto resolve_freep = versioned_symbol<FreepFn>(
        resolve_util, "av_freep", "LIBAVUTIL_58");
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
    const std::string custom_decoder_name =
        custom_av1 != nullptr && custom_av1->name != nullptr
            ? custom_av1->name : "";
    davinci_codec_runtime_set_av1_software_only(1);
    const AVCodec *software_av1 = davinci_avcodec_find_decoder(AV_CODEC_ID_AV1);
    davinci_codec_runtime_set_av1_software_only(0);
    if (software_av1 == nullptr ||
        std::string(software_av1->name) != "libdav1d" ||
        davinci_avcodec_find_decoder(AV_CODEC_ID_AV1) != custom_av1) {
        std::fprintf(stderr, "software-only AV1 selection did not bypass QSV\n");
        return 8;
    }
    const AVCodec *resolve_vp9 = resolve_find(AV_CODEC_ID_VP9);
    const AVCodec *dispatched_vp9 = davinci_avcodec_find_decoder(AV_CODEC_ID_VP9);
    if (custom_av1 == nullptr || custom_av1->id != AV_CODEC_ID_AV1 ||
        (std::string(custom_av1->name) != "libdav1d" &&
         std::string(custom_av1->name) != "av1_qsv")) {
        std::fprintf(stderr, "AV1 did not resolve to a supported private decoder\n");
        return 8;
    }
    if (resolve_vp9 == nullptr || dispatched_vp9 != resolve_vp9) {
        std::fprintf(stderr, "non-AV1 lookup did not preserve Resolve provider identity\n");
        return 9;
    }

    AVCodecContext *mismatched_context =
        davinci_avcodec_alloc_context3(custom_av1);
    if (mismatched_context == nullptr ||
        davinci_avcodec_open2(mismatched_context, resolve_vp9, nullptr) !=
            AVERROR(EINVAL)) {
        if (mismatched_context != nullptr)
            davinci_avcodec_free_context(&mismatched_context);
        std::fprintf(stderr, "private context accepted a Resolve codec object\n");
        return 10;
    }
    davinci_avcodec_free_context(&mismatched_context);
    if (mismatched_context != nullptr) {
        std::fprintf(stderr, "mismatched private context did not free cleanly\n");
        return 10;
    }

    if (!exercise_resolve_owned_extradata(
            software_av1, resolve_mallocz, resolve_freep)) {
        std::fprintf(stderr, "Resolve-owned AV1 extradata was not safely rehomed\n");
        return 10;
    }
    std::printf("PASS: Resolve-owned extradata is released by its owner and rehomed before private open\n");

    if (!exercise_custom_context(custom_av1, resolve_frame_alloc,
                                 resolve_frame_free)) {
        std::fprintf(stderr, "custom AV1 context lifecycle failed\n");
        return 10;
    }

    if (software_av1 != custom_av1 &&
        !decode_ivf_through_provider(sample_path, software_av1,
                                     resolve_frame_alloc, resolve_frame_free,
                                     resolve_frame_unref, resolve_packet_alloc,
                                     resolve_packet_free, resolve_packet_new,
                                     resolve_packet_unref))
        return 10;
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
                if (!exercise_custom_context(software_av1, resolve_frame_alloc,
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

    if (!exercise_shared_context_concurrency(
            software_av1, resolve_frame_alloc, resolve_frame_free,
            resolve_frame_unref)) {
        std::fprintf(stderr,
                     "shared-context concurrent send/receive/flush stress failed\\n");
        return 16;
    }

    AVCodecContext *shutdown_guard =
        davinci_avcodec_alloc_context3(custom_av1);
    if (shutdown_guard == nullptr ||
        davinci_codec_runtime_shutdown_ffmpeg() != -2 ||
        davinci_codec_runtime_ffmpeg_ready() != 1) {
        if (shutdown_guard != nullptr)
            davinci_avcodec_free_context(&shutdown_guard);
        std::fprintf(stderr, "shutdown did not reject an active private context\n");
        return 16;
    }
    davinci_avcodec_free_context(&shutdown_guard);
    if (shutdown_guard != nullptr) {
        std::fprintf(stderr, "shutdown guard context did not free cleanly\\n");
        return 17;
    }

    std::atomic<unsigned int> shutdown_workers_ready{0U};
    std::atomic<bool> shutdown_workers_start{false};
    std::atomic<unsigned int> shutdown_lookup_failures{0U};
    std::vector<std::thread> shutdown_workers;
    for (unsigned int worker = 0; worker < 8U; ++worker) {
        shutdown_workers.emplace_back([&]() {
            const unsigned int initial =
                davinci_codec_runtime_private_avcodec_version();
            if (initial != 3940198U)
                shutdown_lookup_failures.fetch_add(1U, std::memory_order_relaxed);
            shutdown_workers_ready.fetch_add(1U, std::memory_order_release);
            while (!shutdown_workers_start.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (;;) {
                const unsigned int version =
                    davinci_codec_runtime_private_avcodec_version();
                if (version == 0U) break;
                if (version != 3940198U)
                    shutdown_lookup_failures.fetch_add(
                        1U, std::memory_order_relaxed);
            }
        });
    }
    while (shutdown_workers_ready.load(std::memory_order_acquire) != 8U)
        std::this_thread::yield();
    shutdown_workers_start.store(true, std::memory_order_release);

    const int concurrent_shutdown_result =
        davinci_codec_runtime_shutdown_ffmpeg();
    for (auto &worker : shutdown_workers) worker.join();
    if (concurrent_shutdown_result != 0 ||
        shutdown_lookup_failures.load(std::memory_order_relaxed) != 0U) {
        std::fprintf(stderr,
                     "concurrent provider shutdown gate failed: result=%d failures=%u\\n",
                     concurrent_shutdown_result,
                     shutdown_lookup_failures.load(std::memory_order_relaxed));
        return 17;
    }
    if (davinci_codec_runtime_shutdown_ffmpeg() != 0) {
        std::fprintf(stderr, "private FFmpeg shutdown failed after context cleanup\n");
        return 17;
    }
    if (davinci_codec_runtime_ffmpeg_ready() != 0 ||
        davinci_codec_runtime_private_avcodec_version() != 0U ||
        davinci_avcodec_find_decoder(AV_CODEC_ID_AV1) != nullptr) {
        std::fprintf(stderr, "provider API remained ready after shutdown\n");
        return 18;
    }

    if (davinci_codec_runtime_initialize_ffmpeg(private_root.c_str()) != 0 ||
        davinci_codec_runtime_ffmpeg_ready() != 1 ||
        davinci_codec_runtime_private_avcodec_version() != 3940198U ||
        davinci_avcodec_find_decoder(AV_CODEC_ID_AV1) == nullptr ||
        davinci_codec_runtime_shutdown_ffmpeg() != 0) {
        std::fprintf(stderr, "private FFmpeg reinitialization cycle failed: %s\n",
                     davinci_codec_runtime_ffmpeg_error());
        return 19;
    }

    std::printf("PASS: private FFmpeg version %u routes AV1 to %s\n",
                private_version, custom_decoder_name.c_str());
    std::puts("PASS: VP9 codec identity and context operations stay on Resolve FFmpeg60");
    std::puts("PASS: AV1 allocation/open/flush/send/receive/close/free use the private provider");
    std::puts("PASS: Resolve-allocated AVFrame crosses the private decoder API boundary");
    std::puts("PASS: 4-thread software-context lifecycle stress (32 contexts)");
    std::puts("PASS: 4-thread shared-context send/receive/flush serialization (256 rounds)");
    std::puts("PASS: provider wrapper-entry shutdown gate and context-free barrier");
    std::puts("LIMIT: direct provider-layer test only; Resolve call-site patches and callback are not installed");

    (void)dlclose(resolve_codec);
    (void)dlclose(resolve_util);
    return 0;
}
