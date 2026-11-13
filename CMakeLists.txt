cmake_minimum_required(VERSION 3.24)
project(davinci_codec_runtime VERSION 0.1.0 LANGUAGES CXX)

set(DAVINCI_FFMPEG_616_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../reverse_engineering/sources/ffmpeg-6.1.6-software-src"
    CACHE PATH "FFmpeg 6.1.6 source/build tree used for ABI headers and private runtime")

if(NOT EXISTS "${DAVINCI_FFMPEG_616_ROOT}/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg 6.1.6 headers not found at DAVINCI_FFMPEG_616_ROOT")
endif()

find_package(Threads REQUIRED)

add_library(davinci_codec_runtime SHARED
    src/runtime.cpp
    src/codecs/av1.cpp
    src/backend_providers.cpp
    src/runtime_conditions.cpp
    src/ffmpeg_provider.cpp
)

target_compile_features(davinci_codec_runtime PRIVATE cxx_std_20)
target_include_directories(davinci_codec_runtime
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${DAVINCI_FFMPEG_616_ROOT}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(davinci_codec_runtime PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
set_target_properties(davinci_codec_runtime PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
)

include(CTest)
if(BUILD_TESTING)
    set(DAVINCI_RESOLVE_FFMPEG_DIR "/opt/resolve/libs" CACHE PATH
        "Resolve FFmpeg60 library directory used by the local provider test")
    add_executable(codec_definition_test tests/codec_definition_test.cpp)
    add_executable(ffmpeg_provider_test tests/ffmpeg_provider_test.cpp)
    target_include_directories(ffmpeg_provider_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${DAVINCI_FFMPEG_616_ROOT}
    )
    target_link_libraries(ffmpeg_provider_test PRIVATE
        davinci_codec_runtime ${CMAKE_DL_LIBS} Threads::Threads
    )
    target_compile_features(ffmpeg_provider_test PRIVATE cxx_std_20)
    add_executable(av1_backend_policy_test
        tests/av1_backend_policy_test.cpp
        src/codecs/av1.cpp
        src/backend_providers.cpp
        src/runtime_conditions.cpp
    )
    target_compile_features(codec_definition_test PRIVATE cxx_std_20)
    target_include_directories(codec_definition_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(av1_backend_policy_test PRIVATE cxx_std_20)
    target_include_directories(av1_backend_policy_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(davinci_codec_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(codec_definition_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(ffmpeg_provider_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(av1_backend_policy_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME codec_definition_test COMMAND codec_definition_test)
    add_test(NAME av1_backend_policy_test COMMAND av1_backend_policy_test)
    add_test(NAME ffmpeg_provider_test_8bit
        COMMAND ffmpeg_provider_test
            "${DAVINCI_RESOLVE_FFMPEG_DIR}"
            "${DAVINCI_FFMPEG_616_ROOT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../reverse_engineering/smoke/av1_probe/test.ivf")
    add_test(NAME ffmpeg_provider_test_10bit
        COMMAND ffmpeg_provider_test
            "${DAVINCI_RESOLVE_FFMPEG_DIR}"
            "${DAVINCI_FFMPEG_616_ROOT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../reverse_engineering/smoke/av1_probe/test-10bit.ivf")
endif()
