cmake_minimum_required(VERSION 3.16)
project(GaloisBipartiteFilter VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(GBF_BASE_FLAGS -O3 -march=native -Wall -Wextra -Wpedantic)

# Optional AVX-512 detection (currently no AVX-512 query path is shipped;
# the macro is set so a future implementation can opt in without re-plumbing).
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-mavx512f -mavx512vpopcntdq")
check_cxx_source_compiles("
#include <immintrin.h>
int main() {
    __m512i a = _mm512_setzero_si512();
    __m512i b = _mm512_popcnt_epi64(a);
    (void)b;
    return 0;
}
" HAVE_AVX512VPOPCNTDQ)
unset(CMAKE_REQUIRED_FLAGS)

set(GBF_AVX512_FLAGS "")
if(HAVE_AVX512VPOPCNTDQ)
    message(STATUS "AVX-512 VPOPCNTDQ detected")
    set(GBF_AVX512_FLAGS -mavx512f -mavx512vpopcntdq -D__AVX512VPOPCNTDQ__)
else()
    message(STATUS "AVX-512 VPOPCNTDQ not available — using scalar / NEON path")
endif()

find_package(Threads REQUIRED)

# Header-only library
add_library(gbf_headers INTERFACE)
target_include_directories(gbf_headers INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(gbf_headers INTERFACE ${GBF_BASE_FLAGS} ${GBF_AVX512_FLAGS})
target_link_libraries(gbf_headers INTERFACE Threads::Threads)

# Standalone benchmark
add_executable(gbf_bench src/bench.cpp)
target_link_libraries(gbf_bench PRIVATE gbf_headers)
set_target_properties(gbf_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Correctness tests
add_executable(gbf_test tests/test_correctness.cpp)
target_link_libraries(gbf_test PRIVATE gbf_headers)
set_target_properties(gbf_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Head-to-head vs competent Blocked Bloom (on the same machine)
add_executable(bench_vs src/bench_vs.cpp)
target_link_libraries(bench_vs PRIVATE gbf_headers)
set_target_properties(bench_vs PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# FastFilter-compatible C API library
add_library(gbf_c STATIC src/gbf_c.cpp)
target_link_libraries(gbf_c PUBLIC gbf_headers)
target_include_directories(gbf_c PUBLIC ${CMAKE_SOURCE_DIR}/src)

# C API test (compiled as C)
enable_language(C)
set(CMAKE_C_STANDARD 11)
add_executable(test_c_api tests/test_c_api.c)
target_link_libraries(test_c_api PRIVATE gbf_c)
set_target_properties(test_c_api PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# libFuzzer harness (opt-in: -DGBF_FUZZ=ON, ASan-incompatible with Release build)
option(GBF_FUZZ "Build libFuzzer harness" OFF)
if(GBF_FUZZ)
    add_executable(fuzz_gbf fuzz/fuzz_gbf.cpp)
    target_compile_options(fuzz_gbf PRIVATE -O1 -g -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_gbf PRIVATE -fsanitize=fuzzer,address,undefined)
    target_include_directories(fuzz_gbf PRIVATE src)
    target_link_libraries(fuzz_gbf PRIVATE Threads::Threads)
    set_target_properties(fuzz_gbf PROPERTIES
        CXX_STANDARD 17
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

# CTest
enable_testing()
add_test(NAME correctness COMMAND gbf_test)
add_test(NAME c_api       COMMAND test_c_api)
