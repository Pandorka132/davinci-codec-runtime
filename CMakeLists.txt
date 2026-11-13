cmake_minimum_required(VERSION 3.24)
project(davinci_codec_runtime VERSION 0.1.0 LANGUAGES CXX)

add_library(davinci_codec_runtime SHARED
    src/runtime.cpp
    src/codecs/av1.cpp
    src/backend_providers.cpp
    src/runtime_conditions.cpp
)

target_compile_features(davinci_codec_runtime PRIVATE cxx_std_20)
target_include_directories(davinci_codec_runtime
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
set_target_properties(davinci_codec_runtime PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
)

include(CTest)
if(BUILD_TESTING)
    add_executable(codec_definition_test tests/codec_definition_test.cpp)
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
        target_compile_options(av1_backend_policy_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME codec_definition_test COMMAND codec_definition_test)
    add_test(NAME av1_backend_policy_test COMMAND av1_backend_policy_test)
endif()
