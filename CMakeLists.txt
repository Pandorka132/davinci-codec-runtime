cmake_minimum_required(VERSION 3.24)
project(davinci_codec_runtime VERSION 0.1.0 LANGUAGES C CXX)

set(DAVINCI_FFMPEG_616_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/build/private-ffmpeg"
    CACHE PATH "Private FFmpeg 6.1.6 install prefix used for ABI headers and runtime")

if(NOT EXISTS "${DAVINCI_FFMPEG_616_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR
        "FFmpeg 6.1.6 headers not found. Run scripts/build.sh to build the pinned private FFmpeg.")
endif()

set(DAVINCI_EXPECTED_BOOTSTRAP_SHA256
    "0000000000000000000000000000000000000000000000000000000000000000"
    CACHE STRING "SHA-256 of the built bootstrap DSO")
set(DAVINCI_EXPECTED_RUNTIME_SHA256
    "0000000000000000000000000000000000000000000000000000000000000000"
    CACHE STRING "SHA-256 of the built codec runtime DSO")
set(DAVINCI_EXPECTED_AVUTIL_SHA256
    "0000000000000000000000000000000000000000000000000000000000000000"
    CACHE STRING "SHA-256 of the private FFmpeg libavutil DSO")
set(DAVINCI_EXPECTED_AVCODEC_SHA256
    "0000000000000000000000000000000000000000000000000000000000000000"
    CACHE STRING "SHA-256 of the private FFmpeg libavcodec DSO")

foreach(hash_name IN ITEMS
        DAVINCI_EXPECTED_BOOTSTRAP_SHA256
        DAVINCI_EXPECTED_RUNTIME_SHA256
        DAVINCI_EXPECTED_AVUTIL_SHA256
        DAVINCI_EXPECTED_AVCODEC_SHA256)
    string(LENGTH "${${hash_name}}" hash_length)
    if(NOT hash_length EQUAL 64 OR NOT "${${hash_name}}" MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "${hash_name} must be a 64-character SHA-256 hex string")
    endif()
endforeach()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    installer/build_artifact_hashes.hpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/build_artifact_hashes.hpp"
    @ONLY
)

find_package(Threads REQUIRED)

add_library(davinci_codec_runtime SHARED
    src/runtime.cpp
    src/av1_callback.cpp
    src/codecs/av1.cpp
    src/backend_providers.cpp
    src/runtime_conditions.cpp
    src/ffmpeg_provider.cpp
)

target_compile_features(davinci_codec_runtime PRIVATE cxx_std_20)
target_include_directories(davinci_codec_runtime
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${DAVINCI_FFMPEG_616_ROOT}/include
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
        ${DAVINCI_FFMPEG_616_ROOT}/include
    )
    target_link_libraries(ffmpeg_provider_test PRIVATE
        davinci_codec_runtime ${CMAKE_DL_LIBS} Threads::Threads
    )
    target_compile_features(ffmpeg_provider_test PRIVATE cxx_std_20)
    add_library(resolve_av1_live_decode_probe MODULE
        tests/resolve_av1_live_decode_probe.c
    )
    target_include_directories(resolve_av1_live_decode_probe PRIVATE
        ${DAVINCI_FFMPEG_616_ROOT}/include
    )
    target_link_libraries(resolve_av1_live_decode_probe PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(resolve_av1_live_decode_probe PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS YES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
    add_executable(av1_callback_policy_test
        tests/av1_callback_policy_test.cpp
        src/av1_callback.cpp
    )
    target_compile_features(av1_callback_policy_test PRIVATE cxx_std_20)
    target_include_directories(av1_callback_policy_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_executable(install_authorization_test
        tests/install_authorization_test.c
    )
    target_include_directories(install_authorization_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/installer
        ${DAVINCI_INSTALLER_COMMON_DIR}
    )
    target_compile_features(install_authorization_test PRIVATE c_std_11)
    add_executable(session_controller_api_test
        tests/session_controller_api_test.cpp
    )
    target_include_directories(session_controller_api_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/installer
        ${DAVINCI_INSTALLER_COMMON_DIR}
    )
    target_compile_features(session_controller_api_test PRIVATE cxx_std_20)
    target_link_libraries(session_controller_api_test PRIVATE
        davinci_av1_session_controller)
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
        target_compile_options(resolve_av1_live_decode_probe PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(av1_backend_policy_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(av1_callback_policy_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(session_controller_api_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(install_authorization_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME codec_definition_test COMMAND codec_definition_test)
    add_test(NAME av1_backend_policy_test COMMAND av1_backend_policy_test)
    add_test(NAME av1_callback_policy_test COMMAND av1_callback_policy_test)
    add_test(NAME session_controller_api_test COMMAND session_controller_api_test)
    add_test(NAME install_authorization_test COMMAND install_authorization_test)
    add_test(NAME ffmpeg_provider_test_8bit
        COMMAND ffmpeg_provider_test
            "${DAVINCI_RESOLVE_FFMPEG_DIR}"
            "${DAVINCI_FFMPEG_616_ROOT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/av1-8bit.ivf")
    add_test(NAME ffmpeg_provider_test_10bit
        COMMAND ffmpeg_provider_test
            "${DAVINCI_RESOLVE_FFMPEG_DIR}"
            "${DAVINCI_FFMPEG_616_ROOT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/av1-10bit.ivf")
endif()

# Resolve-side bootstrap: intentionally read-only until a separately reviewed
# target-specific transaction is implemented. Keep it a C DSO, independent of
# the codec provider's C++/FFmpeg dependency graph.
set(DAVINCI_INSTALLER_COMMON_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/installer/common")
add_library(davinci_av1_bootstrap SHARED
    installer/bootstrap.c
    installer/transaction_executor.c
    installer/resolve_allocator_boundary.cpp
    ${DAVINCI_INSTALLER_COMMON_DIR}/target_identity.c
    ${DAVINCI_INSTALLER_COMMON_DIR}/sha256.c
)
target_include_directories(davinci_av1_bootstrap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/installer
    ${DAVINCI_INSTALLER_COMMON_DIR}
)
set_target_properties(davinci_av1_bootstrap PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    OUTPUT_NAME davinci_av1_bootstrap
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(davinci_av1_bootstrap PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()

# Same-process session controller. It owns bootstrap/provider/private-FFmpeg
# loading, verifies fixed Resolve target ownership, binds the AV1 callback and
# exposes a read-only prepare gate. It deliberately has no commit entrypoint.
add_library(davinci_av1_session_controller SHARED
    installer/session_controller.cpp
    ${DAVINCI_INSTALLER_COMMON_DIR}/sha256.c
    ${DAVINCI_INSTALLER_COMMON_DIR}/target_identity.c
)
target_include_directories(davinci_av1_session_controller PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/installer
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${DAVINCI_INSTALLER_COMMON_DIR}
)
target_compile_features(davinci_av1_session_controller PRIVATE cxx_std_20)
target_link_libraries(davinci_av1_session_controller PRIVATE
    ${CMAKE_DL_LIBS} Threads::Threads)
set_target_properties(davinci_av1_session_controller PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
    OUTPUT_NAME davinci_av1_session_controller
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(davinci_av1_session_controller PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING)
    add_executable(davinci_av1_bootstrap_test
        installer/tests/bootstrap_test.c
        ${DAVINCI_INSTALLER_COMMON_DIR}/target_identity.c
        ${DAVINCI_INSTALLER_COMMON_DIR}/sha256.c
    )
    add_executable(target_identity_range_test
        tests/target_identity_range_test.c
        ${DAVINCI_INSTALLER_COMMON_DIR}/target_identity.c
        ${DAVINCI_INSTALLER_COMMON_DIR}/sha256.c
    )
    add_executable(transaction_executor_test
        tests/transaction_executor_test.c
        installer/transaction_executor.c
        installer/resolve_allocator_boundary.cpp
        ${DAVINCI_INSTALLER_COMMON_DIR}/target_identity.c
        ${DAVINCI_INSTALLER_COMMON_DIR}/sha256.c
    )
    target_include_directories(transaction_executor_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/installer
        ${DAVINCI_INSTALLER_COMMON_DIR}
    )
    target_compile_features(transaction_executor_test PRIVATE c_std_11 cxx_std_20)
    target_compile_definitions(transaction_executor_test PRIVATE
        DAVINCI_AV1_EXECUTOR_TESTING=1
        DAVINCI_TARGET_IDENTITY_TEST=1
    )
    target_link_libraries(transaction_executor_test PRIVATE
        Threads::Threads ${CMAKE_DL_LIBS}
    )
    set_target_properties(transaction_executor_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS YES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
    )
    target_include_directories(target_identity_range_test PRIVATE
        ${DAVINCI_INSTALLER_COMMON_DIR}
    )
    target_compile_features(target_identity_range_test PRIVATE c_std_11)
    target_include_directories(davinci_av1_bootstrap_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/installer
        ${DAVINCI_INSTALLER_COMMON_DIR}
    )
    target_compile_features(davinci_av1_bootstrap_test PRIVATE c_std_11)
    target_link_libraries(davinci_av1_bootstrap_test PRIVATE
        davinci_av1_bootstrap)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(davinci_av1_bootstrap_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(target_identity_range_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(transaction_executor_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME davinci_av1_bootstrap_test
        COMMAND davinci_av1_bootstrap_test)
    add_test(NAME target_identity_range_test
        COMMAND target_identity_range_test)
    add_test(NAME transaction_executor_test
        COMMAND transaction_executor_test)
endif()
