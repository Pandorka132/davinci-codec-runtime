#pragma once

#if defined(__GNUC__)
#define DAVINCI_CODEC_RUNTIME_API __attribute__((visibility("default")))
#else
#define DAVINCI_CODEC_RUNTIME_API
#endif

extern "C" {
DAVINCI_CODEC_RUNTIME_API unsigned int davinci_codec_runtime_abi_version() noexcept;
DAVINCI_CODEC_RUNTIME_API const char* davinci_codec_runtime_name() noexcept;
}
