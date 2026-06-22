cmake_minimum_required(VERSION 3.20)
project(fpng VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /std:c++20 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
endif()

option(FPNG_ENABLE_SIMD "Enable SIMD intrinsics (NEON/AVX2/SSE4.2)" ON)
option(FPNG_BUILD_TESTS "Build test framework" ON)
option(FPNG_BUILD_BENCHMARKS "Build benchmark executable" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(FPNG_SOURCES
    src/util/crc32.cpp
    src/util/file.cpp
    src/png/filter.cpp
    src/png/reader.cpp
    src/png/writer.cpp
    src/image/image.cpp
    src/preprocess/alpha_optimizer.cpp
    src/preprocess/color_reducer.cpp
    src/preprocess/palette_sorter.cpp
    src/preprocess/content_analyzer.cpp
    src/preprocess/preprocessor.cpp
    src/compress/filter_optimizer.cpp
    src/compress/compressor.cpp
    src/compress/deflate/bit_writer.cpp
    src/compress/deflate/huffman.cpp
    src/compress/deflate/match_finder.cpp
    src/compress/deflate/lz77.cpp
    src/compress/deflate/block_splitter.cpp
    src/compress/deflate/deflater.cpp
    src/compress/deflate/inflate.cpp
    src/compress/deflate/zlib.cpp
)

add_library(fpng_core STATIC ${FPNG_SOURCES})
target_include_directories(fpng_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(FPNG_PRIVATE_DEFS "")
if(FPNG_ENABLE_SIMD)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-mfpu=neon FPNG_NEON_FLAG)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm")
        target_compile_definitions(fpng_core PRIVATE FPNG_HAS_NEON)
        set(FPNG_PRIVATE_DEFS "${FPNG_PRIVATE_DEFS} FPNG_HAS_NEON")
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|i686")
        target_compile_definitions(fpng_core PRIVATE FPNG_HAS_SSE4_2 FPNG_HAS_AVX2)
        set(FPNG_PRIVATE_DEFS "${FPNG_PRIVATE_DEFS} FPNG_HAS_SSE4_2 FPNG_HAS_AVX2")
    endif()
endif()

add_executable(fpng src/main.cpp)
target_link_libraries(fpng PRIVATE fpng_core)

if(FPNG_BUILD_TESTS)
    add_executable(fpng_test
        src/test/test_framework.cpp
        src/test/test_main.cpp
        src/test/test_png.cpp
        src/test/test_deflate.cpp
        src/test/test_filter.cpp
    )
    target_link_libraries(fpng_test PRIVATE fpng_core)
endif()

if(FPNG_BUILD_BENCHMARKS)
    add_executable(fpng_bench
        src/test/benchmark.cpp
    )
    target_link_libraries(fpng_bench PRIVATE fpng_core)
endif()

install(TARGETS fpng RUNTIME DESTINATION bin)
