cmake_minimum_required(VERSION 3.28)

# On macOS, rockbottom + maya build with Homebrew GCC — the same toolchain
# maya's own CI uses (macos-gcc / g++-14). If the user hasn't picked a compiler
# and the default is Apple Clang (which CMake won't accept for cxx_std_26),
# steer to Homebrew's newest g++ so a plain `cmake -B build` just works. The
# Apple SDK headers parse under GCC thanks to the _Static_assert shim in
# src/core/platform/darwin/mach_util.hpp.
if(APPLE AND NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    file(GLOB _rb_gxx /opt/homebrew/bin/g++-1[0-9] /usr/local/bin/g++-1[0-9])
    list(SORT _rb_gxx ORDER DESCENDING)
    if(_rb_gxx)
        list(GET _rb_gxx 0 _rb_gxx0)
        set(CMAKE_CXX_COMPILER "${_rb_gxx0}" CACHE FILEPATH "Homebrew g++ for macOS" FORCE)
    endif()
endif()

# The ONE place the version is written. `rb --version`, the generated
# core/version.hpp, the release asset names and the git tag all derive from it;
# nothing else hardcodes a version string.
project(rockbottom VERSION 0.1.4 LANGUAGES CXX)

# rockbottom targets the same modern standard maya uses.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CMake < 3.30 mishandles the cxx_std_26 compile-feature (Ubuntu 24 LTS ships
# CMake 3.28 — see issue #1 / CMake 3.30 release notes). On such a CMake the
# feature isn't advertised in CMAKE_CXX_COMPILE_FEATURES even when the compiler
# supports C++26, so requesting it below aborts configuration. Mirror maya's
# guard: if cxx_std_26 isn't advertised, fall back to C++23 (which every
# supported compiler + CMake here handles), keeping `cmake -B build` working
# out of the box on stock distro CMake.
if(NOT MSVC AND NOT "cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES
            AND "cxx_std_23" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    set(CMAKE_CXX_STANDARD 23)
    message(STATUS "rockbottom: C++26 not advertised by "
                   "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
                   "(CMake ${CMAKE_VERSION}) — building at C++23.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING
        "rockbottom on macOS is built + tested with Homebrew GCC (like maya's CI). "
        "Apple/LLVM Clang here uses a libc++ that lacks std::move_only_function, "
        "which maya requires. Install it with `brew install gcc` and configure "
        "with `-DCMAKE_CXX_COMPILER=g++-15` (or set CXX).")
endif()

# ── maya (vendored as a git submodule) ───────────────────────────────────────
# We only want the library, not maya's own examples/tests/fuzzers.
set(MAYA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MAYA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(MAYA_BUILD_FUZZERS  OFF CACHE BOOL "" FORCE)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/maya/CMakeLists.txt")
    message(FATAL_ERROR
        "maya submodule missing — run: git submodule update --init --recursive")
endif()
add_subdirectory(third_party/maya)

# ── version ─────────────────────────────────────────────────────────────────
# Generate core/version.hpp from the project() version above, into the build
# tree. Both the app and the tests add ${CMAKE_CURRENT_BINARY_DIR} to their
# include path, so `#include "core/version.hpp"` resolves to the generated file
# and a stale hand-edited copy can never shadow it.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/core/version.hpp"
    @ONLY)

# ── rockbottom ──────────────────────────────────────────────────────────────
# The sampler is split into an OS-agnostic core (orchestration + verdict) and a
# per-platform backend. Exactly ONE backend compiles, chosen here by the host
# OS; adding an OS means adding a platform/<os>/ directory and a branch below.
# Termux reports CMAKE_SYSTEM_NAME as "Android", but it is Linux underneath
# (procfs, sysfs, the same syscalls) — so the Linux backend serves it directly.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(RB_PLATFORM_DIR src/core/platform/linux)
    file(GLOB RB_PLATFORM_SRC CONFIGURE_DEPENDS ${RB_PLATFORM_DIR}/*.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(RB_PLATFORM_DIR src/core/platform/darwin)
    file(GLOB RB_PLATFORM_SRC CONFIGURE_DEPENDS ${RB_PLATFORM_DIR}/*.cpp)
else()
    message(FATAL_ERROR
        "rockbottom has no platform backend for '${CMAKE_SYSTEM_NAME}'. "
        "Supported: Linux, Android (Termux), Darwin (macOS).")
endif()
message(STATUS "rockbottom platform backend: ${CMAKE_SYSTEM_NAME} (${RB_PLATFORM_DIR})")

add_executable(rockbottom
    src/main.cpp
    src/core/sampler.cpp
    src/core/verdict.cpp
    ${RB_PLATFORM_SRC}
)
target_link_libraries(rockbottom PRIVATE maya)
target_include_directories(rockbottom PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
# Request cxx_std_26 only when CMake actually advertises it (CMake >= 3.30 with a
# C++26-capable compiler). On older CMake the CMAKE_CXX_STANDARD set above still
# applies the right -std flag; asking for the unknown feature here is what broke
# the Ubuntu-24 / CMake-3.28 build (issue #1). Same guard maya uses internally.
if("cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    target_compile_features(rockbottom PRIVATE cxx_std_26)
else()
    target_compile_features(rockbottom PRIVATE cxx_std_23)
endif()

# macOS backend needs the system frameworks that expose host/vm statistics,
# battery state (IOKit power sources), and GPU telemetry (IOAccelerator).
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(rockbottom PRIVATE
        "-framework CoreFoundation"
        "-framework IOKit")
endif()

# The binary is `rb` — short, one hand, the way you'll actually type it.
set_target_properties(rockbottom PROPERTIES OUTPUT_NAME rb)

# ── standalone linking ────────────────────────────────────────────
# We promise "standalone binary — no build, no deps". Getting there took three
# separate bugs, all the same shape: the published artifact depended on
# something that existed on the BUILD machine and nowhere else.
#
#   1. macOS/libstdc++. GCC links its OWN libstdc++, so v0.1.0's mac build
#      referenced /opt/homebrew/opt/gcc@14/lib/gcc/14/libstdc++.6.dylib and
#      died in dyld before main() on every machine but the runner's.
#
#   2. Linux/libstdc++. The quieter version of the same bug: the binary ran
#      only where the system libstdc++ was at least as new as the builder's.
#
#   3. Linux/GLIBC SYMBOL VERSIONS — the one that survived the first fix.
#      -static-libstdc++ leaves *glibc itself* dynamic, and the linker stamps
#      in the version tag of whatever glibc it built against. Built on
#      ubuntu-latest (glibc 2.39) the artifact demanded GLIBC_2.29 through
#      2.38, so on a stable enterprise host it aborted with a wall of
#      "version `GLIBC_2.xx' not found (required by rb)" — before main(),
#      with no hint that the download simply wasn't built for that machine.
#      A binary is not portable because it avoided libstdc++; it is portable
#      when it needs NO versioned symbol from the host at all.
#
# So on Linux: link EVERYTHING statically. `rb` reads /proc and issues
# syscalls — the kernel ABI is stable in a way the glibc symbol table is not,
# so a static image is the right shape for this program. On macOS full static
# linking is unsupported (no crt0.o, Apple ships no libc.a), so there we do the
# most that platform allows: static libstdc++/libgcc, with libSystem dynamic.
#
# ONE BEHAVIOURAL CONSEQUENCE, handled in sys_util.hpp: static glibc cannot
# dlopen NSS modules, so getpwuid() sees only /etc/passwd and directory
# (LDAP/AD/SSSD) accounts would render as bare numeric uids. user_of() falls
# back to the system's own `getent` for exactly that case — see the comment
# there before touching either half of this.
#
# Defaults ON so a plain `cmake -B build` yields a binary you can hand to
# someone. Turn it OFF for a distro package, where linking against the system
# libraries is the correct behaviour.
option(RB_STANDALONE "Statically link so `rb` runs on any host of its OS" ON)
if(RB_STANDALONE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(rockbottom PRIVATE -static)
    else()
        target_link_options(rockbottom PRIVATE -static-libstdc++ -static-libgcc)
    endif()
endif()

# Strip the Release binary. Unstripped, `rb` is ~33MB (94% debug symbols); the
# kernel mmaps the image at exec and pages in a lot of that on a cold start.
# Stripping drops it to ~2MB, cutting page-in / load latency at launch — the
# thing a system monitor is judged on. Single-config generators (Make/Ninja)
# read CMAKE_BUILD_TYPE; multi-config (Xcode/VS) use the per-config genexp.
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set_target_properties(rockbottom PROPERTIES LINK_FLAGS_RELEASE "-s")
    # Also strip via a post-build step so an already-linked binary gets trimmed
    # even when the linker flag is a no-op on this toolchain.
    add_custom_command(TARGET rockbottom POST_BUILD
        COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:rockbottom>"
        COMMENT "Stripping rb for a faster cold start"
        VERBATIM)
endif()

install(TARGETS rockbottom RUNTIME DESTINATION bin)

# ── tests ──────────────────────────────────────────────────
# Headless render assertions for the widgets whose bugs are invisible to a
# compiler — column alignment, cluster labelling. They render the REAL widget
# onto a real canvas and inspect the resulting cells, so a layout regression
# fails the build instead of shipping. Not built by default; `make rb_tests`.
add_executable(rb_tests EXCLUDE_FROM_ALL tests/core_grid_test.cpp)
target_link_libraries(rb_tests PRIVATE maya)
target_include_directories(rb_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if("cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    target_compile_features(rb_tests PRIVATE cxx_std_26)
else()
    target_compile_features(rb_tests PRIVATE cxx_std_23)
endif()

# ── render bench ────────────────────────────────────────────
# Headless per-frame cost breakdown: builds one synthetic Snapshot and loops
# view()+render_tree at a chosen pty size, timing each phase. This is how we
# localise the super-linear-with-area cost the pty benchmark only detects.
# Not built by default; `make rb_render_bench`.
add_executable(rb_render_bench EXCLUDE_FROM_ALL
    tests/render_bench.cpp
    src/core/sampler.cpp
    src/core/verdict.cpp
    ${RB_PLATFORM_SRC}
)
target_link_libraries(rb_render_bench PRIVATE maya)
target_include_directories(rb_render_bench PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if("cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    target_compile_features(rb_render_bench PRIVATE cxx_std_26)
else()
    target_compile_features(rb_render_bench PRIVATE cxx_std_23)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(rb_render_bench PRIVATE
        "-framework CoreFoundation"
        "-framework IOKit")
endif()
