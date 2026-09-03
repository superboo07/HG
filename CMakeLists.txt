cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VITASDK})
    set(PS2X_VITA_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake")
    if(EXISTS "${PS2X_VITA_TOOLCHAIN_FILE}")
        set(CMAKE_TOOLCHAIN_FILE "${PS2X_VITA_TOOLCHAIN_FILE}" CACHE PATH
            "Toolchain file used for cross-compiling" FORCE)
        message(STATUS "Using VitaSDK toolchain from VITASDK: ${CMAKE_TOOLCHAIN_FILE}")
    endif()
endif()

project(PS2RetroX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optional compiler cache.  The generated game target is tens of thousands of
# translation units, so a warm cache turns a multi-hour rebuild into minutes.
option(PS2X_ENABLE_CCACHE "Use ccache for compilation when it is available" ON)
if(PS2X_ENABLE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(PS2X_CCACHE_PROGRAM NAMES ccache sccache)
    if(PS2X_CCACHE_PROGRAM)
        if(CMAKE_GENERATOR MATCHES "Visual Studio")
            # The Visual Studio generator ignores CMAKE_<LANG>_COMPILER_LAUNCHER.
            message(STATUS "ccache found but the Visual Studio generator ignores compiler launchers; skipping")
        else()
            # ps2EntryRunner precompiles its heavy runtime headers.  Without
            # these sloppiness settings ccache declines to cache any translation
            # unit that consumes a precompiled header, which is exactly the
            # expensive generated-game target we want cached.
            set(PS2X_CCACHE_LAUNCHER
                "${CMAKE_COMMAND}" -E env
                "CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_ctime,include_file_mtime"
                "${PS2X_CCACHE_PROGRAM}")
            set(CMAKE_C_COMPILER_LAUNCHER ${PS2X_CCACHE_LAUNCHER})
            set(CMAKE_CXX_COMPILER_LAUNCHER ${PS2X_CCACHE_LAUNCHER})
            message(STATUS "Using compiler cache: ${PS2X_CCACHE_PROGRAM}")
        endif()
    endif()
endif()

option(PS2X_BUILD_RECOMP "Build ps2xRecomp" ON)
option(PS2X_BUILD_RUNTIME "Build ps2xRuntime" ON)
option(PS2X_BUILD_ANALYZER "Build ps2xAnalyzer" ON)
option(PS2X_BUILD_TEST "Build ps2xTest" ON)
option(PS2X_BUILD_STUDIO "Build ps2xStudio" ON)

if(ANDROID)
    message(STATUS "Android target detected, building runtime only")
    set(PS2X_BUILD_RECOMP OFF)
    set(PS2X_BUILD_ANALYZER OFF)
    set(PS2X_BUILD_TEST OFF)
    set(PS2X_BUILD_STUDIO OFF)
endif()

set(PS2X_IS_ARM_TARGET OFF)
set(PS2X_IS_AARCH64_TARGET OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    set(PS2X_IS_ARM_TARGET ON)
    set(PS2X_IS_AARCH64_TARGET ON)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm|^ARM")
    set(PS2X_IS_ARM_TARGET ON)
endif()

if((CMAKE_C_COMPILER MATCHES "arm-vita-eabi") OR
   (CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi"))
    set(PS2X_IS_ARM_TARGET ON)
endif()

if(PS2X_IS_ARM_TARGET)
    message(STATUS "ARM target detected, fetching sse2neon")

    include(FetchContent)
    FetchContent_Declare(
        sse2neon
        GIT_REPOSITORY https://github.com/DLTcollab/sse2neon.git
        GIT_TAG v1.9.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(sse2neon)

    include_directories(${sse2neon_SOURCE_DIR})
    add_compile_definitions(USE_SSE2NEON)

    if(PS2X_IS_AARCH64_TARGET AND APPLE)
        # macOS ARM64 already uses optimal defaults
        message(STATUS "macOS ARM64 - using default compiler flags")
    elseif(PS2X_IS_AARCH64_TARGET AND MSVC)
        # Windows ARM64 - MSVC already uses optimal defaults
        message(STATUS "Windows ARM64 (MSVC) - using default compiler flags")
    elseif(PS2X_IS_AARCH64_TARGET)
        # Linux ARM64 - add NEON flags
        message(STATUS "Non-Apple ARM64 - adding NEON compiler flags")
        add_compile_options(-march=armv8-a+fp+simd)

        # Try to enable crypto and CRC extensions if supported
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-march=armv8-a+fp+simd+crypto+crc" COMPILER_SUPPORTS_CRYPTO_CRC)
        if(COMPILER_SUPPORTS_CRYPTO_CRC)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+fp+simd+crypto+crc")
            message(STATUS "Crypto and CRC extensions enabled")
        else()
            add_compile_options(-march=armv8-a+fp+simd)
        endif()
    endif()
endif()

# x86-64 hosts: the runtime's guest-register helpers call SSE4.1 intrinsics
# (_mm_extract_epi32/_mm_extract_epi64, _mm_blendv_*, _mm_insert_epi8,
# _mm_min_epi32/_mm_max_epi32, _mm_mullo_epi32).  MSVC exposes every intrinsic
# unconditionally, but GCC and Clang reject them as an "inlining failed ...
# target specific option mismatch" error unless the ISA is enabled explicitly.
# SSE4.1 is the exact required baseline; no AVX intrinsics are used.
if(NOT PS2X_IS_ARM_TARGET AND NOT MSVC AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86|x86)$")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-msse4.1" PS2X_COMPILER_SUPPORTS_SSE41)
    if(PS2X_COMPILER_SUPPORTS_SSE41)
        add_compile_options(-msse4.1)
        message(STATUS "x86 target - enabling SSE4.1 required by the runtime intrinsics")
    else()
        message(FATAL_ERROR
            "The PS2 runtime requires SSE4.1 intrinsics but the compiler rejected -msse4.1")
    endif()
endif()

if(PS2X_BUILD_RECOMP)
    add_subdirectory("ps2xRecomp")
endif()

if(PS2X_BUILD_RUNTIME)
    add_subdirectory("ps2xIOP")
    add_subdirectory("ps2xRuntime")
endif()

if(PS2X_BUILD_ANALYZER)
    add_subdirectory("ps2xAnalyzer")
endif()

if(PS2X_BUILD_TEST)
    add_subdirectory("ps2xTest")
endif()

if(PS2X_BUILD_STUDIO)
    add_subdirectory("ps2xStudio")
endif()
