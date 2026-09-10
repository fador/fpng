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
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm|ARM64|arm64")
        target_compile_definitions(fpng_core PRIVATE FPNG_HAS_NEON)
        set(FPNG_PRIVATE_DEFS "${FPNG_PRIVATE_DEFS} FPNG_HAS_NEON")
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|i686")
        # Enable the arch flags the intrinsics require. Without these the
        # FPNG_HAS_* macros are inert (and GCC rejects the AVX2 intrinsics).
        if(MSVC)
            check_cxx_compiler_flag("/arch:AVX2" FPNG_AVX2_FLAG)
        else()
            check_cxx_compiler_flag("-mavx2" FPNG_AVX2_FLAG)
        endif()
        if(FPNG_AVX2_FLAG)
            target_compile_definitions(fpng_core PRIVATE FPNG_HAS_AVX2)
            if(MSVC)
                target_compile_options(fpng_core PRIVATE /arch:AVX2)
            else()
                target_compile_options(fpng_core PRIVATE -mavx2)
            endif()
            set(FPNG_PRIVATE_DEFS "${FPNG_PRIVATE_DEFS} FPNG_HAS_AVX2")
        else()
            # Fall back to SSE4.2 if available.
            if(MSVC)
                check_cxx_compiler_flag("/arch:SSE2" FPNG_SSE42_FLAG)
            else()
                check_cxx_compiler_flag("-msse4.2" FPNG_SSE42_FLAG)
            endif()
            if(FPNG_SSE42_FLAG)
                target_compile_definitions(fpng_core PRIVATE FPNG_HAS_SSE4_2)
                if(NOT MSVC)
                    target_compile_options(fpng_core PRIVATE -msse4.2)
                endif()
                set(FPNG_PRIVATE_DEFS "${FPNG_PRIVATE_DEFS} FPNG_HAS_SSE4_2")
            endif()
        endif()
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
