cmake_minimum_required(VERSION 3.22)

project(yay LANGUAGES CXX)

#
# Language
#

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

#
# IPO / LTO
#

include(CheckIPOSupported)

check_ipo_supported(
    RESULT IPO_SUPPORTED
    OUTPUT IPO_ERROR
)

if(IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

#
# Dependencies
#

# nlohmann/json — pulled via FetchContent so `cmake --build` is fully
# self-contained regardless of how it's invoked (build.sh, a CI workflow
# calling cmake directly, an IDE, etc). Previously this lived in
# src/vendor/json.hpp, downloaded by a curl step inside build.sh — that
# worked but meant the build silently depended on build.sh always running
# first; anything that called cmake directly (e.g. a CI step that skips
# build.sh) would fail with a missing header. FetchContent makes the
# dependency a normal part of the CMake graph instead of an out-of-band
# shell step, and pins to an exact tagged release rather than `develop`
# (which has no compatibility/versioning guarantees and can change under
# us at any time).
include(FetchContent)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(nlohmann_json)

#
# Sources
#

include_directories(
    src
)

set(COMMON_SOURCES
    src/hash_util.cpp
    src/logger.cpp
    src/json_config.cpp
    src/exec_util.cpp
    src/io_scheduler.cpp
    src/rules_engine.cpp
    src/game_mode.cpp
)

add_executable(yay_apply
    src/main_apply.cpp
    ${COMMON_SOURCES}
)

add_executable(yay_watch
    src/main_watch.cpp
    ${COMMON_SOURCES}
)

# yay_dns: Auto Private DNS daemon. Deliberately NOT linked against
# COMMON_SOURCES — it needs none of the I/O-scheduler / rules / game-mode
# machinery, and leaving those out keeps this long-lived resident process
# as small as possible. dns_policy.cpp is the pure decision logic and is
# also what tests/dns_policy_test.cpp exercises on a host machine.
add_executable(yay_dns
    src/main_dns.cpp
    src/dns_policy.cpp
    src/logger.cpp
    src/json_config.cpp
    src/exec_util.cpp
)

# yay_inspect: on-demand manifest/appops browser for the webui's "add
# component" / "add appops" pickers. Separate binary rather than folded
# into yay_apply — it pulls in zlib (for DEFLATE-compressed manifest
# entries) and a fair amount of parsing logic that boot-time apply has no
# use for, and it's invoked interactively/lazily rather than on every boot.
add_executable(yay_inspect
    src/main_inspect.cpp
    src/zip_reader.cpp
    src/axml_parser.cpp
    src/logger.cpp
    src/exec_util.cpp
)

#
# Common compiler options
#

foreach(target
    yay_apply
    yay_watch
    yay_inspect
    yay_dns)

    target_compile_features(${target}
        PRIVATE
            cxx_std_20
    )

    target_compile_options(${target}
        PRIVATE

        #
        # Optimization
        #

        $<$<CONFIG:Release>:-O2>

        #
        # Warnings
        #

        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wformat=2
        -Wnull-dereference
        -Wimplicit-fallthrough
        -Wunused
        -Wcast-qual
        -Wmissing-declarations

        #
        # Security
        #

        -fstack-protector-strong
        -fvisibility=hidden
        -ffunction-sections
        -fdata-sections

        -D_FORTIFY_SOURCE=2

        #
        # Silence
        #

        -Wno-unused-result
    )

    target_link_options(${target} PRIVATE
        -Wl,--gc-sections
        -Wl,-z,relro
        -Wl,-z,now
        $<$<CONFIG:Release>:-Wl,--strip-all>
    )

    target_link_libraries(${target}
        PRIVATE
            log
            nlohmann_json::nlohmann_json
    )

endforeach()

target_link_libraries(yay_inspect PRIVATE z)

# yay_dns stays resident for the whole uptime, so favour size over the last
# few percent of speed (it is asleep in poll() virtually all the time).
# Appended after the per-target -O2 above, so it wins for this target only.
target_compile_options(yay_dns PRIVATE $<$<CONFIG:Release>:-Os>)

#
# SystemUI MediaMetadata NPE Fix (Zygisk module)
#
# Originally a standalone ndk-build/Android.mk project; migrated into this
# CMake build so the whole repo builds through one toolchain invocation
# instead of two. Every optimization and hardening flag below is carried
# over unchanged from the original Android.mk/Application.mk — nothing here
# is a new choice, it's a like-for-like translation so the resulting binary
# is equivalent (same compiler, same flags, same ABI/API target as before).
#
# This module is Zygisk-only and firmware-fingerprint-locked at runtime
# (see media_fix/module.cpp's isSupportedBuild()) — it self-disables on any
# device that isn't the exact target firmware, so bundling it alongside
# yay's device-agnostic tweaks is safe: on every other device it loads,
# checks the fingerprint, and does nothing.
#

add_library(systemui_media_fix SHARED
    src/media_fix/module.cpp
    src/media_fix/binder.cpp
)

# No STL: the source uses zero std:: (pure C-style/POSIX + the Zygisk C++
# API, which itself avoids the STL). Matches Application.mk's
# `APP_STL := none`.
#
# This is NOT done via the ANDROID_STL_TYPE target property — that
# property belongs to CMake's own built-in Android cross-compilation
# support (CMAKE_SYSTEM_NAME=Android with no external toolchain file) and
# is silently ignored by android.toolchain.cmake, which is what build.sh
# actually uses as CMAKE_TOOLCHAIN_FILE. android.toolchain.cmake reads the
# ANDROID_STL *variable* exactly once, globally, at configure time (set to
# c++_static in build.sh for the three binaries above) and bakes the
# resulting flags straight into the project-wide CMAKE_CXX_FLAGS /
# CMAKE_EXE_LINKER_FLAGS — there is no per-target hook into that path.
#
# What android.toolchain.cmake itself does for `ANDROID_STL=none` is just
# two flags: `-nostdinc++` (compile) and `-nostdlib++` (link). Applying
# those two flags directly to this one target reproduces that exact
# behavior locally, on top of whatever the global c++_static flags already
# added — -nostdinc++/-nostdlib++ are unconditional "don't use the STL at
# all" flags, so their effect isn't something the earlier global STL flags
# can partially reassert.
target_compile_options(systemui_media_fix PRIVATE
    -nostdinc++
)
target_link_options(systemui_media_fix PRIVATE
    -nostdlib++
)

# LTO is mandatory for this target regardless of build type (the original
# Android.mk applied -flto unconditionally, not just in release) — small
# hot-path surface where cross-TU inlining meaningfully matters (the PLT
# hook trampoline calling into the original transact()).
set_target_properties(systemui_media_fix PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "systemui_media_fix"
    PREFIX "lib"
    INTERPROCEDURAL_OPTIMIZATION TRUE
)

target_compile_options(systemui_media_fix PRIVATE
    # Original Application.mk APP_CPPFLAGS: tuned for this device's CPU
    # (Cortex-A55, per Infinix X6815B's SoC) — safe to keep hardcoded since
    # this library only ever runs on that one fingerprint-locked device.
    -march=armv8.2-a+lse+rcpc
    -mtune=cortex-a55

    -DNDEBUG
    -fno-plt
    -fno-strict-aliasing
    -fomit-frame-pointer
    -fmerge-all-constants
    -fstack-clash-protection
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -fno-semantic-interposition
    -fstack-protector-strong
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -ffunction-sections
    -fdata-sections

    -Wall
    -Wextra
    -Wpedantic
)

target_link_options(systemui_media_fix PRIVATE
    -Wl,--strip-debug
    -Wl,-z,separate-code
    -Wl,--pack-dyn-relocs=android
    -Wl,--gc-sections
    -Wl,--icf=all
    -Wl,--as-needed
    -Wl,--exclude-libs,ALL
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,-z,noexecstack
    -Wl,--build-id=sha1
)

target_link_libraries(systemui_media_fix PRIVATE
    log
    android
    dl
)

#
# Sanitizer (Debug only)
#

foreach(target
    yay_apply
    yay_watch
    yay_inspect
    yay_dns)

    target_link_options(${target}
        PRIVATE

        $<$<CONFIG:Debug>:
            -fsanitize=address
            -fsanitize=undefined
        >
    )
endforeach()

# Install
install(TARGETS yay_apply yay_watch yay_inspect yay_dns RUNTIME DESTINATION bin)

install(CODE [[
execute_process(
    COMMAND ${CMAKE_STRIP} --strip-all ${CMAKE_INSTALL_PREFIX}/bin/yay_apply
)
execute_process(
    COMMAND ${CMAKE_STRIP} --strip-all ${CMAKE_INSTALL_PREFIX}/bin/yay_watch
)
execute_process(
    COMMAND ${CMAKE_STRIP} --strip-all ${CMAKE_INSTALL_PREFIX}/bin/yay_inspect
)
execute_process(
    COMMAND ${CMAKE_STRIP} --strip-all ${CMAKE_INSTALL_PREFIX}/bin/yay_dns
)
]]
)

# Zygisk loaders (Magisk/KernelSU/APatch/Zygisk Next/ReZygisk) expect one
# .so per ABI directly under a top-level zygisk/ directory, named after the
# ABI itself — not lib<name>.so under bin/ like the three binaries above.
install(FILES $<TARGET_FILE:systemui_media_fix>
    DESTINATION zygisk
    RENAME arm64-v8a.so
)
