#include <davinci_codec_runtime/api.hpp>

extern "C" unsigned int davinci_codec_runtime_abi_version() noexcept {
    return 1;
}

extern "C" const char* davinci_codec_runtime_name() noexcept {
    return "davinci-codec-runtime";
}
