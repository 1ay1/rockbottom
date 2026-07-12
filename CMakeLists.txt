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

project(rockbottom VERSION 1.0.0 LANGUAGES CXX)

# rockbottom targets the same modern standard maya uses.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
target_compile_features(rockbottom PRIVATE cxx_std_26)

# macOS backend needs the system frameworks that expose host/vm statistics,
# battery state (IOKit power sources), and GPU telemetry (IOAccelerator).
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(rockbottom PRIVATE
        "-framework CoreFoundation"
        "-framework IOKit")
endif()

# The binary is `rb` — short, one hand, the way you'll actually type it.
set_target_properties(rockbottom PROPERTIES OUTPUT_NAME rb)

install(TARGETS rockbottom RUNTIME DESTINATION bin)
