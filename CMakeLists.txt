cmake_minimum_required(VERSION 3.22)
project(ktrie LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler flags ────────────────────────────────────────────
if(MSVC)
    # Strip /RTC1 from default Debug flags — incompatible with /O2
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

    add_compile_options(
        /O2                     # full optimization
        /EHsc                   # standard exception handling
        /Zc:__cplusplus         # report correct __cplusplus value
        /W4                     # high warning level
        /bigobj                 # allow large object files (heavy template instantiation)
        /guard:cf-              # DISABLE Control Flow Guard (C++20)
    )

    # AVX-512: closest MSVC equivalent to -march=x86-64-v4
    # Requires CPU support — comment out and use /arch:AVX2 if unsupported
    option(USE_AVX512 "Enable AVX-512 (/arch:AVX512)" ON)
    if(USE_AVX512)
        add_compile_options(/arch:AVX512)
    else()
        add_compile_options(/arch:AVX2)
    endif()
else()
    # GCC / Clang fallback (container, CI, etc.)
    add_compile_options(-O2 -march=x86-64-v4)
endif()

# ASAN: enable with -DSANITIZE=ON
option(SANITIZE "Enable Address Sanitizer" OFF)
if(SANITIZE)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# ── Include paths ─────────────────────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/kntrie
    ${CMAKE_SOURCE_DIR}/kstrie
    ${CMAKE_SOURCE_DIR}/ktoken
)

# ── Executables ───────────────────────────────────────────────
add_executable(bench_kntrie      kntrie/bench_kntrie.cpp)
add_executable(bench_kntrie_same kntrie/bench_kntrie_same.cpp)
add_executable(bench_kstrie      kstrie/bench_kstrie.cpp)
add_executable(bench_words       kstrie/bench_words.cpp)
add_executable(poc_encode        ktoken/poc_encode.cpp)