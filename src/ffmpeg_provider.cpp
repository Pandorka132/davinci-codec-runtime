#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ffmpeg_provider.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>
#include <atomic>
#include <dirent.h>
#include <unistd.h>
#include <libavutil/hwcontext.h>

namespace {

using FindDecoderFn = const AVCodec *(*)(enum AVCodecID);
using FindDecoderByNameFn = const AVCodec *(*)(const char *);
using AllocContextFn = AVCodecContext *(*)(const AVCodec *);
using OpenFn = int (*)(AVCodecContext *, const AVCodec *, AVDictionary **);
using CloseFn = int (*)(AVCodecContext *);
using FreeContextFn = void (*)(AVCodecContext **);
using SendPacketFn = int (*)(AVCodecContext *, const AVPacket *);
using ReceiveFrameFn = int (*)(AVCodecContext *, AVFrame *);
using FlushFn = void (*)(AVCodecContext *);
using VersionFn = unsigned int (*)(void);
using FrameAllocFn = AVFrame *(*)(void);
using FrameFreeFn = void (*)(AVFrame **);
using FrameUnrefFn = void (*)(AVFrame *);
using FrameCopyPropsFn = int (*)(AVFrame *, const AVFrame *);
using FrameGetBufferFn = int (*)(AVFrame *, int);
using FrameCopyFn = int (*)(AVFrame *, const AVFrame *);
using FrameNewSideDataFn = AVFrameSideData *(*)(AVFrame *, enum AVFrameSideDataType, size_t);
using MalloczFn = void *(*)(size_t);
using FreepFn = void (*)(void *);
using DictGetFn = const AVDictionaryEntry *(*)(const AVDictionary *, const char *,
                                               const AVDictionaryEntry *, int);
using DictSetFn = int (*)(AVDictionary **, const char *, const char *, int);
using DictFreeFn = void (*)(AVDictionary **);
using HwDeviceCreateFn = int (*)(AVBufferRef **, enum AVHWDeviceType,
                                  const char *, AVDictionary *, int);
using BufferRefFn = AVBufferRef *(*)(const AVBufferRef *);
using BufferUnrefFn = void (*)(AVBufferRef **);
using HwFrameTransferFn = int (*)(AVFrame *, const AVFrame *, int);

struct ProviderApi {
    FindDecoderFn find_decoder;
    FindDecoderByNameFn find_decoder_by_name;
    AllocContextFn alloc_context;
    OpenFn open;
    CloseFn close;
    FreeContextFn free_context;
    SendPacketFn send_packet;
    ReceiveFrameFn receive_frame;
    FlushFn flush;
    VersionFn version;
    FrameAllocFn frame_alloc;
    FrameFreeFn frame_free;
    FrameUnrefFn frame_unref;
    FrameCopyPropsFn frame_copy_props;
    FrameGetBufferFn frame_get_buffer;
    FrameCopyFn frame_copy;
    FrameNewSideDataFn frame_new_side_data;
    MalloczFn mallocz;
    FreepFn freep;
    DictGetFn dict_get;
    DictSetFn dict_set;
    DictFreeFn dict_free;
    HwDeviceCreateFn hw_device_create;
    BufferRefFn buffer_ref;
    BufferUnrefFn buffer_unref;
    HwFrameTransferFn hw_frame_transfer;
};

struct ContextNode {
    AVCodecContext *context;
    unsigned int active_leases;
    bool closing;
    AVFrame *private_frame;
    AVFrame *software_frame;
    uint8_t *private_extradata;
    pthread_mutex_t operation_mutex;
    ContextNode *next;
};

pthread_mutex_t initialization_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t contexts_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t contexts_idle = PTHREAD_COND_INITIALIZER;

unsigned int active_wrapper_calls;
bool shutdown_in_progress;
int initialization_complete;
int initialization_failed;
char initialization_error[384];
void *private_util_handle;
void *private_codec_handle;
ProviderApi resolve_api{};
ProviderApi private_api{};
const AVCodec *private_av1_qsv_codec;
const AVCodec *private_av1_software_codec;
std::atomic<bool> av1_software_only{false};
AVBufferRef *private_qsv_device;
bool private_qsv_enabled;
ContextNode *contexts;


bool enter_wrapper_call() noexcept {
    if (pthread_mutex_lock(&contexts_mutex) != 0) return false;
    const bool ready =
        __atomic_load_n(&initialization_complete, __ATOMIC_ACQUIRE) != 0;
    if (!ready || shutdown_in_progress) {
        (void)pthread_mutex_unlock(&contexts_mutex);
        return false;
    }
    ++active_wrapper_calls;
    (void)pthread_mutex_unlock(&contexts_mutex);
    return true;
}

void leave_wrapper_call() noexcept {
    if (pthread_mutex_lock(&contexts_mutex) != 0) return;
    if (active_wrapper_calls > 0U) --active_wrapper_calls;
    if (active_wrapper_calls == 0U)
        (void)pthread_cond_broadcast(&contexts_idle);
    (void)pthread_mutex_unlock(&contexts_mutex);
}

class WrapperCallGuard {
public:
    WrapperCallGuard() noexcept : active_(enter_wrapper_call()) {}
    ~WrapperCallGuard() {
        if (active_) leave_wrapper_call();
    }
    WrapperCallGuard(const WrapperCallGuard &) = delete;
    WrapperCallGuard &operator=(const WrapperCallGuard &) = delete;
    explicit operator bool() const noexcept { return active_; }

private:
    bool active_;
};

void set_error(const char *message) noexcept {
    if (message == nullptr) message = "unknown FFmpeg provider initialization error";
    std::snprintf(initialization_error, sizeof(initialization_error), "%s", message);
}

void set_dl_error(const char *operation) noexcept {
    const char *detail = dlerror();
    std::snprintf(initialization_error, sizeof(initialization_error),
                  "%s: %s", operation,
                  detail != nullptr ? detail : "dynamic loader returned no detail");
}

template <typename Function>
Function resolve_versioned(void *handle, const char *symbol,
                           const char *version) noexcept {
    dlerror();
    void *address = dlvsym(handle, symbol, version);
    if (dlerror() != nullptr) return nullptr;
    return reinterpret_cast<Function>(address);
}

bool load_provider_api(void *handle, const char *codec_version,
                       ProviderApi *api, bool require_decoder_name) noexcept {
    if (api == nullptr) return false;
    api->find_decoder = resolve_versioned<FindDecoderFn>(
        handle, "avcodec_find_decoder", codec_version);
    api->alloc_context = resolve_versioned<AllocContextFn>(
        handle, "avcodec_alloc_context3", codec_version);
    api->open = resolve_versioned<OpenFn>(handle, "avcodec_open2", codec_version);
    api->close = resolve_versioned<CloseFn>(handle, "avcodec_close", codec_version);
    api->free_context = resolve_versioned<FreeContextFn>(
        handle, "avcodec_free_context", codec_version);
    api->send_packet = resolve_versioned<SendPacketFn>(
        handle, "avcodec_send_packet", codec_version);
    api->receive_frame = resolve_versioned<ReceiveFrameFn>(
        handle, "avcodec_receive_frame", codec_version);
    api->flush = resolve_versioned<FlushFn>(
        handle, "avcodec_flush_buffers", codec_version);
    api->version = resolve_versioned<VersionFn>(
        handle, "avcodec_version", codec_version);
    if (require_decoder_name) {
        api->find_decoder_by_name = resolve_versioned<FindDecoderByNameFn>(
            handle, "avcodec_find_decoder_by_name", codec_version);
    }

    return api->find_decoder != nullptr && api->alloc_context != nullptr &&
           api->open != nullptr && api->close != nullptr &&
           api->free_context != nullptr && api->send_packet != nullptr &&
           api->receive_frame != nullptr && api->flush != nullptr &&
           api->version != nullptr &&
           (!require_decoder_name || api->find_decoder_by_name != nullptr);
}

bool load_frame_api(void *handle, const char *util_version,
                    ProviderApi *api) noexcept {
    if (api == nullptr) return false;
    api->frame_alloc = resolve_versioned<FrameAllocFn>(
        handle, "av_frame_alloc", util_version);
    api->frame_free = resolve_versioned<FrameFreeFn>(
        handle, "av_frame_free", util_version);
    api->frame_unref = resolve_versioned<FrameUnrefFn>(
        handle, "av_frame_unref", util_version);
    api->frame_copy_props = resolve_versioned<FrameCopyPropsFn>(
        handle, "av_frame_copy_props", util_version);
    api->frame_get_buffer = resolve_versioned<FrameGetBufferFn>(
        handle, "av_frame_get_buffer", util_version);
    api->frame_copy = resolve_versioned<FrameCopyFn>(
        handle, "av_frame_copy", util_version);
    api->frame_new_side_data = resolve_versioned<FrameNewSideDataFn>(
        handle, "av_frame_new_side_data", util_version);
    api->mallocz = resolve_versioned<MalloczFn>(
        handle, "av_mallocz", util_version);
    api->freep = resolve_versioned<FreepFn>(
        handle, "av_freep", util_version);
    api->dict_get = resolve_versioned<DictGetFn>(
        handle, "av_dict_get", util_version);
    api->dict_set = resolve_versioned<DictSetFn>(
        handle, "av_dict_set", util_version);
    api->dict_free = resolve_versioned<DictFreeFn>(
        handle, "av_dict_free", util_version);
    api->hw_device_create = resolve_versioned<HwDeviceCreateFn>(
        handle, "av_hwdevice_ctx_create", util_version);
    api->buffer_ref = resolve_versioned<BufferRefFn>(
        handle, "av_buffer_ref", util_version);
    api->buffer_unref = resolve_versioned<BufferUnrefFn>(
        handle, "av_buffer_unref", util_version);
    api->hw_frame_transfer = resolve_versioned<HwFrameTransferFn>(
        handle, "av_hwframe_transfer_data", util_version);
    return api->frame_alloc != nullptr && api->frame_free != nullptr &&
           api->frame_unref != nullptr && api->frame_copy_props != nullptr &&
           api->frame_get_buffer != nullptr && api->frame_copy != nullptr &&
           api->frame_new_side_data != nullptr && api->mallocz != nullptr &&
           api->freep != nullptr && api->dict_get != nullptr &&
           api->dict_set != nullptr;
}

bool load_qsv_api(void *handle, ProviderApi *api) noexcept {
    if (api == nullptr) return false;
    api->dict_free = resolve_versioned<DictFreeFn>(
        handle, "av_dict_free", "LIBAVUTIL_58");
    api->hw_device_create = resolve_versioned<HwDeviceCreateFn>(
        handle, "av_hwdevice_ctx_create", "LIBAVUTIL_58");
    api->buffer_ref = resolve_versioned<BufferRefFn>(
        handle, "av_buffer_ref", "LIBAVUTIL_58");
    api->buffer_unref = resolve_versioned<BufferUnrefFn>(
        handle, "av_buffer_unref", "LIBAVUTIL_58");
    api->hw_frame_transfer = resolve_versioned<HwFrameTransferFn>(
        handle, "av_hwframe_transfer_data", "LIBAVUTIL_58");
    return api->dict_set != nullptr && api->dict_free != nullptr &&
           api->hw_device_create != nullptr && api->buffer_ref != nullptr &&
           api->buffer_unref != nullptr && api->hw_frame_transfer != nullptr;
}

int close_partial_private_handles() noexcept {
    int result = 0;
    if (private_codec_handle != nullptr) {
        dlerror();
        if (dlclose(private_codec_handle) != 0) {
            set_dl_error("dlclose private libavcodec.so.60");
            result = -1;
        } else {
            private_codec_handle = nullptr;
        }
    }
    if (private_util_handle != nullptr) {
        dlerror();
        if (dlclose(private_util_handle) != 0) {
            set_dl_error("dlclose private libavutil.so.58");
            result = -1;
        } else {
            private_util_handle = nullptr;
        }
    }
    return result;
}

ContextNode *find_context_locked(AVCodecContext *context) noexcept {
    for (ContextNode *node = contexts; node != nullptr; node = node->next) {
        if (node->context == context) return node;
    }
    return nullptr;
}

/* 1 = leased private context, 0 = Resolve context, -1 = closing. */
int lease_context(AVCodecContext *context, ContextNode **out) noexcept {
    if (out == nullptr) return -1;
    *out = nullptr;
    (void)pthread_mutex_lock(&contexts_mutex);
    ContextNode *node = find_context_locked(context);
    int status = 0;
    if (node != nullptr) {
        if (node->closing) {
            status = -1;
        } else {
            ++node->active_leases;
            *out = node;
            status = 1;
        }
    }
    (void)pthread_mutex_unlock(&contexts_mutex);
    return status;
}

void release_context(ContextNode *node) noexcept {
    if (node == nullptr) return;
    (void)pthread_mutex_lock(&contexts_mutex);
    if (node->active_leases > 0U) --node->active_leases;
    if (node->active_leases == 0U)
        (void)pthread_cond_broadcast(&contexts_idle);
    (void)pthread_mutex_unlock(&contexts_mutex);
}

bool register_context(AVCodecContext *context) noexcept {
    if (context == nullptr) return false;
    auto *node = static_cast<ContextNode *>(std::calloc(1, sizeof(ContextNode)));
    if (node == nullptr) return false;
    node->context = context;
    if (pthread_mutex_init(&node->operation_mutex, nullptr) != 0) {
        std::free(node);
        return false;
    }
    node->private_frame = private_api.frame_alloc();
    node->software_frame = private_api.frame_alloc();
    if (node->private_frame == nullptr || node->software_frame == nullptr) {
        private_api.frame_free(&node->private_frame);
        private_api.frame_free(&node->software_frame);
        (void)pthread_mutex_destroy(&node->operation_mutex);
        std::free(node);
        return false;
    }

    (void)pthread_mutex_lock(&contexts_mutex);
    if (find_context_locked(context) != nullptr) {
        (void)pthread_mutex_unlock(&contexts_mutex);
        private_api.frame_free(&node->private_frame);
        private_api.frame_free(&node->software_frame);
        (void)pthread_mutex_destroy(&node->operation_mutex);
        std::free(node);
        return false;
    }
    node->next = contexts;
    contexts = node;
    (void)pthread_mutex_unlock(&contexts_mutex);
    return true;
}

int rehome_resolve_extradata(AVCodecContext *context,
                             ContextNode *node) noexcept {
    if (context == nullptr || node == nullptr) return AVERROR(EINVAL);

    if (context->extradata == node->private_extradata) return 0;

    if (context->extradata == nullptr) {
        if (node->private_extradata != nullptr)
            private_api.freep(&node->private_extradata);
        return 0;
    }

    if (context->extradata_size < 0) return AVERROR(EINVAL);
    const size_t payload_size = static_cast<size_t>(context->extradata_size);
    if (payload_size > SIZE_MAX - AV_INPUT_BUFFER_PADDING_SIZE)
        return AVERROR(EINVAL);

    auto *replacement = static_cast<uint8_t *>(private_api.mallocz(
        payload_size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (replacement == nullptr) return AVERROR(ENOMEM);

    if (payload_size != 0U)
        std::memcpy(replacement, context->extradata, payload_size);

    /* Resolve's generic factory allocated the original buffer with its own
       libavutil. Release it through that owner before publishing the private
       libavutil allocation in the private AVCodecContext. */
    resolve_api.freep(&context->extradata);
    if (node->private_extradata != nullptr)
        private_api.freep(&node->private_extradata);
    context->extradata = replacement;
    node->private_extradata = replacement;
    return 0;
}

void release_unrehomeable_extradata(AVCodecContext *context,
                                    ContextNode *node) noexcept {
    if (context == nullptr || node == nullptr) return;

    if (node->private_extradata != nullptr &&
        context->extradata == node->private_extradata) {
        node->private_extradata = nullptr;
        return; // private_api.free_context owns this pointer
    }

    if (node->private_extradata != nullptr)
        private_api.freep(&node->private_extradata);
    if (context->extradata != nullptr)
        resolve_api.freep(&context->extradata);
}

int find_unique_intel_render_node(char *path, size_t path_size) noexcept {
    if (path == nullptr || path_size == 0U) return -1;
    DIR *directory = opendir("/dev/dri");
    if (directory == nullptr) return -1;

    unsigned int matches = 0;
    char candidate[PATH_MAX]{};
    struct dirent *entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (std::strncmp(entry->d_name, "renderD", 7U) != 0 ||
            entry->d_name[7] == '\0')
            continue;
        bool numeric = true;
        for (const char *cursor = entry->d_name + 7; *cursor != '\0'; ++cursor)
            if (*cursor < '0' || *cursor > '9') numeric = false;
        if (!numeric) continue;

        char vendor_path[PATH_MAX]{};
        const int vendor_length = std::snprintf(
            vendor_path, sizeof(vendor_path),
            "/sys/class/drm/%s/device/vendor", entry->d_name);
        if (vendor_length < 0 ||
            static_cast<size_t>(vendor_length) >= sizeof(vendor_path))
            continue;
        FILE *vendor_file = std::fopen(vendor_path, "r");
        if (vendor_file == nullptr) continue;
        char vendor[32]{};
        const bool read_vendor = std::fgets(vendor, sizeof(vendor), vendor_file) != nullptr;
        (void)std::fclose(vendor_file);
        if (!read_vendor || std::strncmp(vendor, "0x8086", 6U) != 0) continue;

        const int candidate_length = std::snprintf(
            candidate, sizeof(candidate), "/dev/dri/%s", entry->d_name);
        if (candidate_length < 0 ||
            static_cast<size_t>(candidate_length) >= sizeof(candidate))
            continue;
        if (access(candidate, R_OK | W_OK) != 0) continue;
        ++matches;
    }
    (void)closedir(directory);
    if (matches != 1U) return matches == 0U ? -1 : -2;
    const size_t length = std::strlen(candidate);
    if (length + 1U > path_size) return -1;
    std::memcpy(path, candidate, length + 1U);
    return 0;
}

bool initialize_qsv_decoder() noexcept {
    if (!load_qsv_api(private_util_handle, &private_api)) return false;
    const AVCodec *codec = private_api.find_decoder_by_name("av1_qsv");
    if (codec == nullptr || codec->id != AV_CODEC_ID_AV1) return false;

    char render_node[PATH_MAX]{};
    if (find_unique_intel_render_node(render_node, sizeof(render_node)) != 0)
        return false;

    AVDictionary *options = nullptr;
    int result = private_api.dict_set(
        &options, "child_device_type", "vaapi", 0);
    if (result >= 0)
        result = private_api.dict_set(
            &options, "child_device", render_node, 0);
    if (result >= 0)
        result = private_api.hw_device_create(
            &private_qsv_device, AV_HWDEVICE_TYPE_QSV, "hw", options, 0);
    private_api.dict_free(&options);
    if (result < 0 || private_qsv_device == nullptr) {
        if (private_qsv_device != nullptr)
            private_api.buffer_unref(&private_qsv_device);
        return false;
    }

    private_av1_qsv_codec = codec;
    private_qsv_enabled = true;
    return true;
}

int copy_dictionary_from_private(AVDictionary **destination,
                                 const AVDictionary *source) noexcept {
    if (destination == nullptr) return AVERROR(EINVAL);
    if (source == nullptr) return 0;

    const AVDictionaryEntry *entry = nullptr;
    while ((entry = private_api.dict_get(
                source, "", entry, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
        const int result = resolve_api.dict_set(
            destination, entry->key, entry->value, AV_DICT_MULTIKEY);
        if (result < 0) return result;
    }
    return 0;
}

int copy_private_frame_to_resolve(AVFrame *destination,
                                  AVFrame *source) noexcept {
    if (destination == nullptr || source == nullptr ||
        source->width <= 0 || source->height <= 0 ||
        source->format < 0 || source->hw_frames_ctx != nullptr ||
        source->private_ref != nullptr || source->opaque_ref != nullptr)
        return AVERROR(EINVAL);

    /* Never pass the private dictionary, side-data, or AVBufferRef objects
       into Resolve libavutil. Let the matching owner copy each object. */
    resolve_api.frame_unref(destination);
    destination->format = source->format;
    destination->width = source->width;
    destination->height = source->height;

    AVDictionary *source_metadata = source->metadata;
    AVFrameSideData **source_side_data = source->side_data;
    const int source_side_data_count = source->nb_side_data;
    source->metadata = nullptr;
    source->side_data = nullptr;
    source->nb_side_data = 0;
    const int props_result = resolve_api.frame_copy_props(destination, source);
    source->metadata = source_metadata;
    source->side_data = source_side_data;
    source->nb_side_data = source_side_data_count;
    if (props_result < 0) {
        resolve_api.frame_unref(destination);
        return props_result;
    }

    int result = copy_dictionary_from_private(
        &destination->metadata, source_metadata);
    if (result < 0) {
        resolve_api.frame_unref(destination);
        return result;
    }

    for (int index = 0; index < source_side_data_count; ++index) {
        const AVFrameSideData *source_entry = source_side_data[index];
        if (source_entry == nullptr ||
            (source_entry->data == nullptr && source_entry->size != 0U)) {
            resolve_api.frame_unref(destination);
            return AVERROR(EINVAL);
        }
        AVFrameSideData *destination_entry = resolve_api.frame_new_side_data(
            destination, source_entry->type, source_entry->size);
        if (destination_entry == nullptr) {
            resolve_api.frame_unref(destination);
            return AVERROR(ENOMEM);
        }
        if (source_entry->size != 0U)
            std::memcpy(destination_entry->data, source_entry->data,
                        source_entry->size);
        result = copy_dictionary_from_private(
            &destination_entry->metadata, source_entry->metadata);
        if (result < 0) {
            resolve_api.frame_unref(destination);
            return result;
        }
    }

    result = resolve_api.frame_get_buffer(destination, 32);
    if (result >= 0)
        result = resolve_api.frame_copy(destination, source);
    if (result < 0) resolve_api.frame_unref(destination);
    return result;
}

void unlink_context_locked(ContextNode *target) noexcept {
    ContextNode **link = &contexts;
    while (*link != nullptr && *link != target) link = &(*link)->next;
    if (*link == target) *link = target->next;
}

} // namespace

extern "C" DAVINCI_CODEC_RUNTIME_API int
 davinci_codec_runtime_initialize_ffmpeg(const char *private_ffmpeg_root) noexcept {
    if (private_ffmpeg_root == nullptr || private_ffmpeg_root[0] == 0)
        return -1;

    (void)pthread_mutex_lock(&initialization_mutex);
    if (__atomic_load_n(&initialization_complete, __ATOMIC_ACQUIRE) != 0) {
        (void)pthread_mutex_unlock(&initialization_mutex);
        return 0;
    }
    if (initialization_failed != 0) {
        (void)pthread_mutex_unlock(&initialization_mutex);
        return -1;
    }

    /* Capture the original provider before the private namespace exists. */
    if (!load_provider_api(RTLD_DEFAULT, "LIBAVCODEC_60", &resolve_api, false) ||
        !load_frame_api(RTLD_DEFAULT, "LIBAVUTIL_58", &resolve_api)) {
        set_dl_error("resolve original FFmpeg60 codec/util API");
        initialization_failed = 1;
        (void)pthread_mutex_unlock(&initialization_mutex);
        return -1;
    }
    if (resolve_api.version() != 3933028U) {
        set_error("Resolve FFmpeg provider is not the inspected 60.3.100 build");
        initialization_failed = 1;
        (void)pthread_mutex_unlock(&initialization_mutex);
        return -1;
    }

    char util_path[PATH_MAX];
    char codec_path[PATH_MAX];
    const int util_length = std::snprintf(
        util_path, sizeof(util_path), "%s/lib/libavutil.so.58",
        private_ffmpeg_root);
    const int codec_length = std::snprintf(
        codec_path, sizeof(codec_path), "%s/lib/libavcodec.so.60",
        private_ffmpeg_root);
    if (util_length < 0 || static_cast<size_t>(util_length) >= sizeof(util_path) ||
        codec_length < 0 || static_cast<size_t>(codec_length) >= sizeof(codec_path)) {
        set_error("private FFmpeg root path exceeds PATH_MAX");
        initialization_failed = 1;
        (void)pthread_mutex_unlock(&initialization_mutex);
        return -1;
    }

    /*
     * The private DSOs carry libdavinci_* SONAMEs and libavcodec has an
     * $ORIGIN RUNPATH to its matching libavutil. Keep them in the base linker
     * namespace: VA-API/oneVPL must be able to load the Intel driver's plugin
     * DSOs there. RTLD_DEEPBIND keeps private FFmpeg's own symbol references
     * bound to its explicitly loaded dependency rather than Resolve's copies.
     */
    dlerror();
    private_util_handle = dlopen(
        util_path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (private_util_handle == nullptr) {
        set_dl_error("dlopen private libdavinci_avutil.so.58");
        initialization_failed = 1;
        (void)pthread_mutex_unlock(&initialization_mutex);
        return -1;
    }

    dlerror();
    private_codec_handle = dlopen(
        codec_path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (private_codec_handle == nullptr) {
        set_dl_error("dlopen private libdavinci_avcodec.so.60");
        close_partial_private_handles();
        initialization_failed = 1;
        (void)pthread_mutex_unlock(&initialization_mutex);
        return -1;
    }

    if (!load_provider_api(private_codec_handle, "LIBAVCODEC_60",
                           &private_api, true) ||
        !load_frame_api(private_util_handle, "LIBAVUTIL_58", &private_api)) {
        set_error("private FFmpeg 6.1.6 is missing required LIBAVCODEC_60 exports");
        close_partial_private_handles();
        initialization_failed = 1;
        (void)pthread_mutex_unlock(&initialization_mutex);
        return -1;
    }

    if (private_api.version() != 3940198U) {
        set_error("private FFmpeg provider is not the inspected 60.31.102 build");
        close_partial_private_handles();
        private_api = {};
        initialization_failed = 1;
        (void)pthread_mutex_unlock(&initialization_mutex);
        return -1;
    }

    private_av1_software_codec = private_api.find_decoder_by_name("libdav1d");
    if (private_av1_software_codec == nullptr ||
        private_av1_software_codec->id != AV_CODEC_ID_AV1) {
        set_error("private FFmpeg 6.1.6 did not expose libdav1d as the AV1 fallback");
        close_partial_private_handles();
        private_api = {};
        initialization_failed = 1;
        (void)pthread_mutex_unlock(&initialization_mutex);
        return -1;
    }

    private_qsv_enabled = initialize_qsv_decoder();

    initialization_error[0] = 0;
    __atomic_store_n(&initialization_complete, 1, __ATOMIC_RELEASE);
    (void)pthread_mutex_unlock(&initialization_mutex);
    return 0;
}

extern "C" DAVINCI_CODEC_RUNTIME_API int
 davinci_codec_runtime_shutdown_ffmpeg() noexcept {
    if (pthread_mutex_lock(&initialization_mutex) != 0) return -1;

    /*
     * Shutdown is a lifecycle boundary: the caller must quiesce every thread
     * that can enter the eight wrapper APIs. The registry check prevents
     * unloading FFmpeg while any private AVCodecContext is still registered.
     */
    if (pthread_mutex_lock(&contexts_mutex) != 0) {
        (void)pthread_mutex_unlock(&initialization_mutex);
        return -1;
    }

    if (shutdown_in_progress) {
        (void)pthread_mutex_unlock(&contexts_mutex);
        (void)pthread_mutex_unlock(&initialization_mutex);
        return -4;
    }

    /*
     * Close the wrapper-entry gate before checking the context registry.
     * Calls which entered before this point drain through active_wrapper_calls;
     * new entries are rejected until shutdown is complete or safely refused.
     */
    shutdown_in_progress = true;
    while (active_wrapper_calls != 0U)
        (void)pthread_cond_wait(&contexts_idle, &contexts_mutex);

    if (contexts != nullptr) {
        shutdown_in_progress = false;
        (void)pthread_cond_broadcast(&contexts_idle);
        (void)pthread_mutex_unlock(&contexts_mutex);
        (void)pthread_mutex_unlock(&initialization_mutex);
        return -2;
    }
    (void)pthread_mutex_unlock(&contexts_mutex);

    __atomic_store_n(&initialization_complete, 0, __ATOMIC_RELEASE);
    if (private_qsv_device != nullptr)
        private_api.buffer_unref(&private_qsv_device);
    private_qsv_enabled = false;
    if (close_partial_private_handles() != 0) {
        /* Fail closed: leave the wrapper gate shut if any DSO close failed. */
        initialization_failed = 1;
        (void)pthread_mutex_unlock(&initialization_mutex);
        return -3;
    }

    private_api = {};
    resolve_api = {};
    private_av1_qsv_codec = nullptr;
    private_av1_software_codec = nullptr;
    av1_software_only.store(false, std::memory_order_release);
    initialization_failed = 0;
    initialization_error[0] = 0;

    (void)pthread_mutex_lock(&contexts_mutex);
    shutdown_in_progress = false;
    (void)pthread_cond_broadcast(&contexts_idle);
    (void)pthread_mutex_unlock(&contexts_mutex);

    (void)pthread_mutex_unlock(&initialization_mutex);
    return 0;
}

extern "C" DAVINCI_CODEC_RUNTIME_API int
 davinci_codec_runtime_ffmpeg_ready() noexcept {
    return __atomic_load_n(&initialization_complete, __ATOMIC_ACQUIRE);
}

extern "C" DAVINCI_CODEC_RUNTIME_API const char *
 davinci_codec_runtime_ffmpeg_error() noexcept {
    return initialization_error;
}

extern "C" DAVINCI_CODEC_RUNTIME_API unsigned int
 davinci_codec_runtime_private_avcodec_version() noexcept {
    WrapperCallGuard call;
    if (!call || private_api.version == nullptr) return 0U;
    return private_api.version();
}

extern "C" DAVINCI_CODEC_RUNTIME_API void
 davinci_codec_runtime_set_av1_software_only(int software_only) noexcept {
    av1_software_only.store(software_only != 0, std::memory_order_release);
}

extern "C" DAVINCI_CODEC_RUNTIME_API const AVCodec *
 davinci_avcodec_find_decoder(enum AVCodecID id) noexcept {
    WrapperCallGuard call;
    if (!call) return nullptr;
    if (id == AV_CODEC_ID_AV1) {
        if (private_qsv_enabled && private_av1_qsv_codec != nullptr &&
            !av1_software_only.load(std::memory_order_acquire))
            return private_av1_qsv_codec;
        return private_av1_software_codec;
    }
    return resolve_api.find_decoder(id);
}

extern "C" DAVINCI_CODEC_RUNTIME_API AVCodecContext *
 davinci_avcodec_alloc_context3(const AVCodec *codec) noexcept {
    WrapperCallGuard call;
    if (!call) return nullptr;
    if (codec != private_av1_software_codec &&
        codec != private_av1_qsv_codec)
        return resolve_api.alloc_context(codec);

    AVCodecContext *context = private_api.alloc_context(codec);
    if (context == nullptr) return nullptr;
    if (!register_context(context)) {
        private_api.free_context(&context);
        return nullptr;
    }
    return context;
}

extern "C" DAVINCI_CODEC_RUNTIME_API int
 davinci_avcodec_open2(AVCodecContext *context, const AVCodec *codec,
                       AVDictionary **options) noexcept {
    WrapperCallGuard call;
    if (!call) return AVERROR(EINVAL);
    ContextNode *node = nullptr;
    const int owner = lease_context(context, &node);
    if (owner == 0) return resolve_api.open(context, codec, options);
    if (owner < 0) return AVERROR(EINVAL);
    if (codec != private_av1_software_codec &&
        codec != private_av1_qsv_codec) {
        release_context(node);
        return AVERROR(EINVAL);
    }
    if (pthread_mutex_lock(&node->operation_mutex) != 0) {
        release_context(node);
        return AVERROR(EINVAL);
    }
    const int ownership_result = rehome_resolve_extradata(context, node);
    int result = ownership_result;
    if (ownership_result >= 0 && private_qsv_enabled &&
        codec == private_av1_qsv_codec && context->hw_device_ctx == nullptr) {
        context->hw_device_ctx = private_api.buffer_ref(private_qsv_device);
        if (context->hw_device_ctx == nullptr) result = AVERROR(ENOMEM);
    }
    if (result >= 0) {
        result = private_api.open(context, codec, options);
        /* open2 may normalize or replace extradata; after the call the field
           itself is authoritative and any replacement belongs to private
           libavutil. */
        node->private_extradata = context->extradata;
    }
    (void)pthread_mutex_unlock(&node->operation_mutex);
    release_context(node);
    return result;
}

extern "C" DAVINCI_CODEC_RUNTIME_API int
 davinci_avcodec_close(AVCodecContext *context) noexcept {
    WrapperCallGuard call;
    if (!call) return AVERROR(EINVAL);
    ContextNode *node = nullptr;
    const int owner = lease_context(context, &node);
    if (owner == 0) return resolve_api.close(context);
    if (owner < 0) return AVERROR(EINVAL);
    if (pthread_mutex_lock(&node->operation_mutex) != 0) {
        release_context(node);
        return AVERROR(EINVAL);
    }
    const int result = private_api.close(context);
    (void)pthread_mutex_unlock(&node->operation_mutex);
    release_context(node);
    return result;
}

extern "C" DAVINCI_CODEC_RUNTIME_API void
 davinci_avcodec_free_context(AVCodecContext **context) noexcept {
    WrapperCallGuard call;
    if (!call) return;
    if (context == nullptr || *context == nullptr) {
        resolve_api.free_context(context);
        return;
    }

    (void)pthread_mutex_lock(&contexts_mutex);
    ContextNode *node = find_context_locked(*context);
    if (node == nullptr) {
        (void)pthread_mutex_unlock(&contexts_mutex);
        resolve_api.free_context(context);
        return;
    }
    if (node->closing) {
        *context = nullptr;
        (void)pthread_mutex_unlock(&contexts_mutex);
        return;
    }
    node->closing = true;
    while (node->active_leases != 0U)
        (void)pthread_cond_wait(&contexts_idle, &contexts_mutex);
    (void)pthread_mutex_unlock(&contexts_mutex);

    /* Keep the closing node linked until both private objects are released. */
    private_api.frame_free(&node->private_frame);
    private_api.frame_free(&node->software_frame);
    release_unrehomeable_extradata(*context, node);
    private_api.free_context(context);

    (void)pthread_mutex_lock(&contexts_mutex);
    unlink_context_locked(node);
    (void)pthread_cond_broadcast(&contexts_idle);
    (void)pthread_mutex_unlock(&contexts_mutex);
    (void)pthread_mutex_destroy(&node->operation_mutex);
    std::free(node);
}

extern "C" DAVINCI_CODEC_RUNTIME_API int
 davinci_avcodec_send_packet(AVCodecContext *context,
                             const AVPacket *packet) noexcept {
    WrapperCallGuard call;
    if (!call) return AVERROR(EINVAL);
    ContextNode *node = nullptr;
    const int owner = lease_context(context, &node);
    if (owner == 0) return resolve_api.send_packet(context, packet);
    if (owner < 0) return AVERROR(EINVAL);
    if (pthread_mutex_lock(&node->operation_mutex) != 0) {
        release_context(node);
        return AVERROR(EINVAL);
    }
    const int result = private_api.send_packet(context, packet);
    (void)pthread_mutex_unlock(&node->operation_mutex);
    release_context(node);
    return result;
}

extern "C" DAVINCI_CODEC_RUNTIME_API int
 davinci_avcodec_receive_frame(AVCodecContext *context, AVFrame *frame) noexcept {
    WrapperCallGuard call;
    if (!call || frame == nullptr) return AVERROR(EINVAL);
    ContextNode *node = nullptr;
    const int owner = lease_context(context, &node);
    if (owner == 0) return resolve_api.receive_frame(context, frame);
    if (owner < 0) return AVERROR(EINVAL);

    if (pthread_mutex_lock(&node->operation_mutex) != 0) {
        release_context(node);
        return AVERROR(EINVAL);
    }
    const int decode_result = private_api.receive_frame(
        context, node->private_frame);
    int result = decode_result;
    if (decode_result >= 0) {
        AVFrame *output = node->private_frame;
        if (node->private_frame->hw_frames_ctx != nullptr ||
            node->private_frame->format == AV_PIX_FMT_QSV) {
            result = private_api.hw_frame_transfer(
                node->software_frame, node->private_frame, 0);
            if (result >= 0) output = node->software_frame;
        }
        if (result >= 0)
            result = copy_private_frame_to_resolve(frame, output);
    }
    private_api.frame_unref(node->private_frame);
    private_api.frame_unref(node->software_frame);
    (void)pthread_mutex_unlock(&node->operation_mutex);
    release_context(node);
    return result;
}

extern "C" DAVINCI_CODEC_RUNTIME_API void
 davinci_avcodec_flush_buffers(AVCodecContext *context) noexcept {
    WrapperCallGuard call;
    if (!call) return;
    ContextNode *node = nullptr;
    const int owner = lease_context(context, &node);
    if (owner == 0) {
        resolve_api.flush(context);
        return;
    }
    if (owner < 0) return;
    if (pthread_mutex_lock(&node->operation_mutex) != 0) {
        release_context(node);
        return;
    }
    private_api.flush(context);
    (void)pthread_mutex_unlock(&node->operation_mutex);
    release_context(node);
}
