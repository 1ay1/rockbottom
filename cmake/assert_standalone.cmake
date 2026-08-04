# assert_standalone.cmake — run at build time (cmake -P) to PROVE the freshly
# linked `rb` is actually standalone, not merely requested to be.
#
# The link flags in the top-level CMakeLists ASK for a standalone binary, but
# both toolchains can quietly hand back a dependent one:
#   * GNU ld downgrades a missing static library to a WARNING and falls back to
#     the dynamic variant, so `-static` can emit a dynamic ELF.
#   * Homebrew GCC on macOS can still leave a /opt/homebrew libgcc_s / libstdc++
#     dylib in the load commands despite -static-libgcc/-static-libstdc++.
# Either one ships a binary that runs on the build host and dies on the user's.
# This script turns that into a hard build failure.
#
# Invoked as:  cmake -DRB_BIN=<path> -DRB_OS=<Linux|Darwin> -P assert_standalone.cmake

if(NOT DEFINED RB_BIN OR NOT EXISTS "${RB_BIN}")
    message(FATAL_ERROR "assert_standalone: RB_BIN not set or missing (${RB_BIN})")
endif()

# ── Linux: must be a static ELF with no interpreter and no versioned glibc ──
if(RB_OS STREQUAL "Linux")
    set(_fail "")

    # 1. `file` must call it statically linked. Portable and toolchain-agnostic.
    find_program(_file file)
    if(_file)
        execute_process(COMMAND "${_file}" "${RB_BIN}"
                        OUTPUT_VARIABLE _f OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _f MATCHES "statically linked")
            string(APPEND _fail
                "  - not statically linked (-static silently fell back to dynamic?)\n"
                "    file said: ${_f}\n")
        endif()
    endif()

    # 2. A dynamic INTERP program header is the definitive tell: a truly static
    #    binary has none. `readelf -l` is in binutils on every builder.
    find_program(_readelf readelf)
    if(_readelf)
        execute_process(COMMAND "${_readelf}" -l "${RB_BIN}"
                        OUTPUT_VARIABLE _ph OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_ph MATCHES "INTERP" OR _ph MATCHES "Requesting program interpreter")
            string(APPEND _fail
                "  - has a dynamic-loader INTERP segment (it is not static)\n")
        endif()
    endif()

    # 3. No versioned glibc symbol imports. This is the one that shipped: a
    #    binary can be 'mostly static' yet still demand GLIBC_2.34 from the host
    #    and abort before main() on an older enterprise distro.
    find_program(_objdump objdump)
    if(_objdump)
        execute_process(COMMAND "${_objdump}" -T "${RB_BIN}"
                        OUTPUT_VARIABLE _dt ERROR_QUIET
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCHALL "GLIBC_[0-9.]+" _globs "${_dt}")
        if(_globs)
            list(REMOVE_DUPLICATES _globs)
            list(JOIN _globs " " _globs_j)
            string(APPEND _fail
                "  - imports versioned glibc symbols: ${_globs_j}\n")
        endif()
    endif()

    if(_fail)
        message(FATAL_ERROR
            "rb is NOT standalone on Linux — it will fail on hosts unlike this builder:\n"
            "${_fail}"
            "Fixes: ensure a static glibc/libstdc++ is installed for -static "
            "(e.g. `glibc-static libstdc++-static` on RHEL, or build the "
            "musl/dockcross image), or set -DRB_STANDALONE=OFF for a "
            "system-linked distro package.")
    endif()
    message(STATUS "standalone OK: rb is fully static, no versioned glibc imports")
    return()
endif()

# ── macOS: only OS-provided dylibs, and a low deployment floor ──────────────
if(RB_OS STREQUAL "Darwin")
    set(_fail "")

    # 1. Every linked dylib must live under /usr/lib or /System/Library — the
    #    two directories Apple guarantees on every Mac. Anything under
    #    /opt/homebrew, /usr/local, or a @rpath is a build-host leak that dyld
    #    cannot satisfy on the user's machine.
    find_program(_otool otool)
    if(_otool)
        execute_process(COMMAND "${_otool}" -L "${RB_BIN}"
                        OUTPUT_VARIABLE _L OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REPLACE "\n" ";" _lines "${_L}")
        foreach(_ln IN LISTS _lines)
            string(STRIP "${_ln}" _ln)
            # Skip the header line (the binary's own path) and blank lines.
            if(_ln STREQUAL "" OR _ln MATCHES "${RB_BIN}:")
                continue()
            endif()
            # First token up to the first space/paren is the dylib path.
            string(REGEX MATCH "^[^ (]+" _dylib "${_ln}")
            if(_dylib AND NOT _dylib MATCHES "^/usr/lib/" AND NOT _dylib MATCHES "^/System/Library/")
                string(APPEND _fail "  - links a non-OS library: ${_dylib}\n")
            endif()
        endforeach()
    endif()

    # 2. Deployment floor: the LC_BUILD_VERSION / minos must be <= our target,
    #    so the binary launches on older Macs instead of dyld rejecting it.
    if(_otool)
        execute_process(COMMAND "${_otool}" -l "${RB_BIN}"
                        OUTPUT_VARIABLE _lc OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "minos ([0-9]+\\.[0-9]+)" _m "${_lc}")
        if(CMAKE_MATCH_1)
            if(CMAKE_MATCH_1 VERSION_GREATER "11.0")
                string(APPEND _fail
                    "  - minimum macOS is ${CMAKE_MATCH_1}, above the 11.0 floor "
                    "(set CMAKE_OSX_DEPLOYMENT_TARGET)\n")
            endif()
        endif()
    endif()

    if(_fail)
        message(FATAL_ERROR
            "rb is NOT standalone on macOS — it will fail in dyld on other Macs:\n"
            "${_fail}"
            "Fixes: link with Homebrew GCC + -static-libstdc++ -static-libgcc "
            "(RB_STANDALONE=ON does this), and set CMAKE_OSX_DEPLOYMENT_TARGET.")
    endif()
    message(STATUS "standalone OK: rb links only OS libraries, deploys to macOS 11.0+")
    return()
endif()

message(WARNING "assert_standalone: unknown RB_OS='${RB_OS}', skipping check")
